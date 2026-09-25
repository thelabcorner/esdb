/*
 * Drizzle adapter: the only module in this package that imports drizzle-orm.
 *
 * Extraction uses documented/public Drizzle metadata only:
 *   - getTableConfig(table)  (drizzle-orm/sqlite-core)
 *   - getTableColumns(table) (drizzle-orm)
 * No drizzle-kit internals, snapshots, or serializers are used anywhere.
 *
 * Unsupported constructs fail loudly instead of being dropped silently.
 */

import { getTableColumns, is } from "drizzle-orm";
import { getTableConfig, SQLiteSyncDialect, SQLiteTable } from "drizzle-orm/sqlite-core";
import type { SQLiteColumn } from "drizzle-orm/sqlite-core";

import type {
    IrCheck,
    IrColumn,
    IrForeignKey,
    IrForeignKeyAction,
    IrIndex,
    IrIndexTerm,
    IrLiteral,
    IrScalarType,
    IrTable
} from "./ir-types.ts";

export class DrizzleAdapterError extends Error {
    readonly code: string;
    readonly path: string;

    constructor(code: string, path: string, message: string) {
        super(`${code} ${path} ${message}`);
        this.name = "DrizzleAdapterError";
        this.code = code;
        this.path = path;
    }
}

const COLUMN_TYPE_MAP: Record<string, IrScalarType> = {
    SQLiteInteger: "integer",
    SQLiteText: "text",
    SQLiteReal: "real",
    SQLiteBlobBuffer: "blob"
};

function columnPath(exportKey: string, columnName: string): string {
    return `tables.${exportKey}.${columnName}`;
}

function defaultLiteral(column: SQLiteColumn, path: string): IrLiteral | null {
    const extended = column as SQLiteColumn & { onUpdateFn?: unknown };
    if (typeof extended.onUpdateFn === "function") {
        throw new DrizzleAdapterError(
            "ORM002",
            path,
            "runtime update functions ($onUpdate/$onUpdateFn) are not representable in IR v1"
        );
    }
    if (typeof column.defaultFn === "function") {
        throw new DrizzleAdapterError(
            "ORM002",
            path,
            "runtime default functions ($defaultFn) are not representable in IR v1"
        );
    }
    if (column.hasDefault !== true) {
        return null;
    }
    const value = column.default;
    if (value === undefined) {
        /* Primary keys report hasDefault without an explicit default; the
         * SQLite rowid alias is implicit, so IR records no default. */
        return null;
    }
    if (value === null) {
        return { kind: "literal", type: "null", value: "" };
    }
    if (typeof value === "number") {
        if (!Number.isFinite(value)) {
            throw new DrizzleAdapterError("ORM002", path, `default value ${String(value)} is not finite`);
        }
        if (Number.isInteger(value)) {
            if (!Number.isSafeInteger(value)) {
                throw new DrizzleAdapterError(
                    "ORM002",
                    path,
                    "integer defaults outside JavaScript's safe-integer range must be authored as bigint"
                );
            }
            return { kind: "literal", type: "integer", value: String(value) };
        }
        return { kind: "literal", type: "real", value: String(value) };
    }
    if (typeof value === "string") {
        return { kind: "literal", type: "text", value };
    }
    if (typeof value === "bigint") {
        return { kind: "literal", type: "integer", value: String(value) };
    }
    if (value instanceof Uint8Array) {
        return { kind: "literal", type: "blob", value: Buffer.from(value).toString("hex").toUpperCase() };
    }
    throw new DrizzleAdapterError(
        "ORM003",
        path,
        "only literal defaults (number/string/bigint/Uint8Array/null) are representable in IR v1; SQL expressions are not"
    );
}

function uniqueKeyFromColumns(
    columns: readonly unknown[],
    propertyBySqlName: ReadonlyMap<string, string>,
    path: string
): string[] {
    return columns.map((entry) => {
        const column = entry as SQLiteColumn;
        if (typeof column !== "object" || column === null || typeof column.name !== "string") {
            throw new DrizzleAdapterError(
                "ORM007",
                path,
                "SQL-expression index/constraint columns are not representable in IR v1; use plain column references"
            );
        }
        const property = propertyBySqlName.get(column.name.toLowerCase());
        if (property === undefined) {
            throw new DrizzleAdapterError(
                "ORM008",
                path,
                `constraint column "${column.name}" is absent from getTableColumns() metadata`
            );
        }
        return property;
    });
}

function dedupeKeys(keys: string[][]): string[][] {
    const seen = new Set<string>();
    const out: string[][] = [];
    for (const key of keys) {
        const id = key.join("\u0000");
        if (!seen.has(id)) {
            seen.add(id);
            out.push(key);
        }
    }
    return out;
}


function renderSchemaExpression(
    dialect: SQLiteSyncDialect,
    expression: unknown,
    _tableSqlName: string,
    path: string
): string {
    let rendered: { sql: string; params: unknown[] };
    try {
        /*
         * Drizzle's public "indexes" render mode deliberately emits bare column
         * identifiers. That is exactly the SQLite DDL expression shape needed
         * for CHECK constraints, expression indexes, and partial predicates.
         */
        rendered = dialect.sqlToQuery(
            expression as Parameters<SQLiteSyncDialect["sqlToQuery"]>[0],
            "indexes"
        );
    } catch (error) {
        throw new DrizzleAdapterError(
            "ORM012",
            path,
            `failed to render Drizzle SQL expression: ${error instanceof Error ? error.message : String(error)}`
        );
    }
    if (rendered.params.length !== 0) {
        throw new DrizzleAdapterError(
            "ORM012",
            path,
            "schema expressions must be fully static and may not contain bind parameters"
        );
    }
    if (/[;\0]|--|\/\*/.test(rendered.sql)) {
        throw new DrizzleAdapterError(
            "ORM012",
            path,
            "schema expression contains a statement separator, comment marker, or NUL"
        );
    }
    return rendered.sql.trim();
}

function asForeignKeyAction(value: unknown, path: string): IrForeignKeyAction {
    const action = value === undefined ? "no action" : String(value).toLowerCase();
    if (!["no action", "restrict", "cascade", "set null", "set default"].includes(action)) {
        throw new DrizzleAdapterError("ORM013", path, `unsupported foreign-key action "${String(value)}"`);
    }
    return action as IrForeignKeyAction;
}

function indexTerm(
    entry: unknown,
    dialect: SQLiteSyncDialect,
    tableSqlName: string,
    propertyBySqlName: ReadonlyMap<string, string>,
    path: string
): IrIndexTerm {
    const candidate = entry as Partial<SQLiteColumn>;
    if (candidate && typeof candidate === "object" && typeof candidate.name === "string" && typeof candidate.columnType === "string") {
        const property = propertyBySqlName.get(candidate.name.toLowerCase());
        if (property === undefined) {
            throw new DrizzleAdapterError("ORM008", path, `index column "${candidate.name}" is absent from getTableColumns() metadata`);
        }
        return { kind: "column", column: property, direction: "asc" };
    }

    const sql = renderSchemaExpression(dialect, entry, tableSqlName, path);
    const simple = /^"((?:[^"]|"")*)"\s+(asc|desc)$/i.exec(sql);
    if (simple) {
        const sqlName = simple[1].replace(/""/g, '"');
        const property = propertyBySqlName.get(sqlName.toLowerCase());
        if (property !== undefined) {
            return { kind: "column", column: property, direction: simple[2].toLowerCase() as "asc" | "desc" };
        }
    }
    return { kind: "expression", sql };
}

export type DrizzleTables = Record<string, SQLiteTable>;

/* Collects exported Drizzle SQLite tables from a schema module namespace. The
 * only drizzle-specific step here is `is(value, SQLiteTable)`; everything else
 * consumes the adapter's own metadata contract. */
export function collectTables(moduleExports: Record<string, unknown>): DrizzleTables {
    const tables: DrizzleTables = {};
    const seen = new Map<SQLiteTable, string>();
    for (const key of Object.keys(moduleExports).sort()) {
        const value = moduleExports[key];
        if (is(value, SQLiteTable)) {
            const table = value as SQLiteTable;
            const existing = seen.get(table);
            if (existing !== undefined) {
                throw new DrizzleAdapterError(
                    "ORM006",
                    `tables.${key}`,
                    `the same Drizzle table object is exported as both "${existing}" and "${key}"; export it exactly once so the IR logical table name is unambiguous`
                );
            }
            seen.set(table, key);
            tables[key] = table;
        }
    }
    return tables;
}

export function extractTables(tables: DrizzleTables): IrTable[] {
    const irTables: IrTable[] = [];
    const seenTableSqlNames = new Map<string, string>();

    /* The export key is the IR/logical table identity; Drizzle's
     * configured table name is the physical SQL identity. Sort by the logical
     * identity because IR v1 requires deterministic logical-name ordering. */
    const exportKeys = Object.keys(tables).sort((left, right) =>
        left < right ? -1 : left > right ? 1 : 0
    );
    const dialect = new SQLiteSyncDialect();
    const configs = new Map<string, ReturnType<typeof getTableConfig>>();
    const propertyMaps = new Map<string, Map<string, string>>();
    const logicalNameByTable = new Map<SQLiteTable, string>();

    /* Pre-index every table before extracting cross-table constraints. This is
     * deliberately a public-metadata two-pass: FK targets must resolve to a
     * table explicitly supplied to this schema build. */
    for (const exportKey of exportKeys) {
        const table = tables[exportKey];
        if (table === undefined) {
            continue;
        }
        const config = getTableConfig(table);
        const propertyColumns = getTableColumns(table) as unknown as Record<string, SQLiteColumn>;
        const propertyBySqlName = new Map<string, string>();
        for (const propertyKey of Object.keys(propertyColumns).sort()) {
            const column = propertyColumns[propertyKey];
            const columnLowered = column.name.toLowerCase();
            if (propertyBySqlName.has(columnLowered)) {
                throw new DrizzleAdapterError(
                    "ORM006",
                    columnPath(exportKey, column.name),
                    `column SQL name "${column.name}" is declared more than once`
                );
            }
            propertyBySqlName.set(columnLowered, propertyKey);
        }
        if (propertyBySqlName.size !== config.columns.length) {
            throw new DrizzleAdapterError(
                "ORM008",
                `tables.${exportKey}`,
                `Drizzle metadata mismatch: getTableColumns() exposes ${propertyBySqlName.size} column(s) but getTableConfig() lists ${config.columns.length}`
            );
        }
        configs.set(exportKey, config);
        propertyMaps.set(exportKey, propertyBySqlName);
        logicalNameByTable.set(table, exportKey);
    }

    for (const exportKey of exportKeys) {
        const table = tables[exportKey];
        if (table === undefined) {
            continue;
        }
        const config = configs.get(exportKey);
        const propertyBySqlName = propertyMaps.get(exportKey);
        if (config === undefined || propertyBySqlName === undefined) {
            throw new DrizzleAdapterError("ORM008", `tables.${exportKey}`, "pre-indexed Drizzle metadata is missing");
        }
        const tablePath = `tables.${exportKey}`;
        const lowered = config.name.toLowerCase();
        const existing = seenTableSqlNames.get(lowered);
        if (existing !== undefined) {
            throw new DrizzleAdapterError(
                "ORM006",
                tablePath,
                `table SQL name "${config.name}" collides (case-insensitively) with exported table "${existing}"`
            );
        }
        seenTableSqlNames.set(lowered, exportKey);

        let primaryKey: string[];
        if (config.primaryKeys.length > 1) {
            throw new DrizzleAdapterError("ORM006", tablePath, "IR v1 supports a single primary key per table");
        }
        if (config.primaryKeys.length === 1) {
            primaryKey = uniqueKeyFromColumns(
                config.primaryKeys[0].columns,
                propertyBySqlName,
                `${tablePath}.primaryKey`
            );
        } else {
            primaryKey = config.columns
                .filter((column) => column.primary === true)
                .map((column) => {
                    const property = propertyBySqlName.get(column.name.toLowerCase());
                    if (property === undefined) {
                        throw new DrizzleAdapterError(
                            "ORM008",
                            tablePath,
                            `primary-key column "${column.name}" is absent from getTableColumns() metadata`
                        );
                    }
                    return property;
                });
        }
        if (primaryKey.length === 0) {
            throw new DrizzleAdapterError("ORM006", tablePath, "IR v1 requires a primary key on every table");
        }

        const uniqueKeys: string[][] = [];
        for (const column of config.columns) {
            if (column.isUnique === true) {
                const property = propertyBySqlName.get(column.name.toLowerCase());
                if (property === undefined) {
                    throw new DrizzleAdapterError(
                        "ORM008",
                        tablePath,
                        `unique column "${column.name}" is absent from getTableColumns() metadata`
                    );
                }
                uniqueKeys.push([property]);
            }
        }
        for (const constraint of config.uniqueConstraints) {
            uniqueKeys.push(uniqueKeyFromColumns(constraint.columns, propertyBySqlName, `${tablePath}.unique`));
        }
        const indexes: IrIndex[] = config.indexes.map((index) => {
            const indexConfig = index.config as {
                unique?: boolean;
                where?: unknown;
                columns: readonly unknown[];
                name: string;
            };
            const indexPath = `${tablePath}.index.${indexConfig.name}`;
            const terms = indexConfig.columns.map((entry, termIndex) =>
                indexTerm(entry, dialect, config.name, propertyBySqlName, `${indexPath}.terms[${termIndex}]`)
            );
            const irIndex: IrIndex = {
                name: indexConfig.name,
                unique: indexConfig.unique === true,
                terms
            };
            if (indexConfig.where !== undefined) {
                irIndex.where = renderSchemaExpression(dialect, indexConfig.where, config.name, `${indexPath}.where`);
            }
            if (
                irIndex.unique &&
                irIndex.where === undefined &&
                terms.every((term) => term.kind === "column")
            ) {
                uniqueKeys.push(terms.map((term) => (term as { kind: "column"; column: string }).column));
            }
            return irIndex;
        }).sort((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0);

        const primarySet = new Set(primaryKey);
        const columns: IrColumn[] = config.columns.map((column) => {
            const logicalName = propertyBySqlName.get(column.name.toLowerCase());
            const path = columnPath(exportKey, column.name);
            if (logicalName === undefined) {
                throw new DrizzleAdapterError(
                    "ORM008",
                    path,
                    `column "${column.name}" is absent from getTableColumns() metadata`
                );
            }
            if (column.generated !== undefined) {
                throw new DrizzleAdapterError("ORM008", path, "generated columns are not representable in IR v1");
            }
            const type = COLUMN_TYPE_MAP[column.columnType];
            if (type === undefined) {
                throw new DrizzleAdapterError(
                    "ORM001",
                    path,
                    `unsupported Drizzle column type "${column.columnType}"; IR v1 supports SQLiteInteger, SQLiteText, SQLiteReal, SQLiteBlobBuffer`
                );
            }
            const literal = defaultLiteral(column, path);
            const irColumn: IrColumn = {
                name: logicalName,
                sql_name: column.name,
                type,
                nullable: column.notNull !== true,
                primary_key: primarySet.has(logicalName),
                /* autoIncrement is declared on integer columns only, so it is
                 * read structurally rather than through the base column type. */
                autoincrement: (column as { autoIncrement?: boolean }).autoIncrement === true,
                unique: column.isUnique === true
            };
            if (literal !== null) {
                irColumn.default = literal;
            }
            return irColumn;
        });

        const foreignKeys: IrForeignKey[] = config.foreignKeys.map((foreignKey, foreignKeyIndex) => {
            const reference = foreignKey.reference();
            const targetTable = logicalNameByTable.get(reference.foreignTable);
            if (targetTable === undefined) {
                throw new DrizzleAdapterError(
                    "ORM004",
                    `${tablePath}.foreignKeys[${foreignKeyIndex}]`,
                    "foreign-key target table must be exported as part of the same ESDB schema"
                );
            }
            const targetProperties = propertyMaps.get(targetTable);
            if (targetProperties === undefined) {
                throw new DrizzleAdapterError("ORM008", `${tablePath}.foreignKeys[${foreignKeyIndex}]`, "foreign-key target metadata is missing");
            }
            return {
                name: foreignKey.getName(),
                columns: uniqueKeyFromColumns(reference.columns, propertyBySqlName, `${tablePath}.foreignKeys[${foreignKeyIndex}].columns`),
                references: {
                    table: targetTable,
                    columns: uniqueKeyFromColumns(reference.foreignColumns, targetProperties, `${tablePath}.foreignKeys[${foreignKeyIndex}].references`)
                },
                on_update: asForeignKeyAction(foreignKey.onUpdate, `${tablePath}.foreignKeys[${foreignKeyIndex}].on_update`),
                on_delete: asForeignKeyAction(foreignKey.onDelete, `${tablePath}.foreignKeys[${foreignKeyIndex}].on_delete`)
            };
        }).sort((left, right) => {
            const leftName = left.name ?? "";
            const rightName = right.name ?? "";
            return leftName < rightName ? -1 : leftName > rightName ? 1 : 0;
        });

        const checks: IrCheck[] = config.checks.map((check) => ({
            name: check.name,
            sql: renderSchemaExpression(dialect, check.value, config.name, `${tablePath}.check.${check.name}`)
        })).sort((left, right) => left.name < right.name ? -1 : left.name > right.name ? 1 : 0);

        const irTable: IrTable = {
            name: exportKey,
            sql_name: config.name,
            columns,
            primary_key: primaryKey,
            uniques: dedupeKeys(uniqueKeys)
        };
        if (foreignKeys.length > 0) irTable.foreign_keys = foreignKeys;
        if (checks.length > 0) irTable.checks = checks;
        if (indexes.length > 0) irTable.indexes = indexes;
        irTables.push(irTable);
    }

    return irTables;
}
