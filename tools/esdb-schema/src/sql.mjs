/*
 * esdb-schema: identifier validation, SQL emission, migration packaging.
 *
 * Every identifier reaches this module only after IR validation; emission
 * quotes identifiers and binds all values. There is no code path that splices
 * caller-controlled text into SQL.
 */

import { canonicalize, sha256Hex } from "./canonical.mjs";
import { columnByName, deriveBindParams, effectiveDefault, tableByName } from "./ir.mjs";

/* SQLite keyword list (from the SQLite documentation's keyword table, minus
 * values that are only contextual in ways that still parse as identifiers).
 * The list is intentionally conservative: rejecting a name costs a rename,
 * accepting a keyword risks ambiguous SQL. */
export const RESERVED_WORDS = new Set([
    "ABORT", "ACTION", "ADD", "AFTER", "ALL", "ALTER", "ALWAYS", "ANALYZE", "AND", "AS", "ASC", "ATTACH",
    "AUTOINCREMENT", "BEFORE", "BEGIN", "BETWEEN", "BY", "CASCADE", "CASE", "CAST", "CHECK", "COLLATE",
    "COLUMN", "COMMIT", "CONFLICT", "CONSTRAINT", "CREATE", "CROSS", "CURRENT", "CURRENT_DATE",
    "CURRENT_TIME", "CURRENT_TIMESTAMP", "DATABASE", "DEFAULT", "DEFERRABLE", "DEFERRED", "DELETE", "DESC",
    "DETACH", "DISTINCT", "DO", "DROP", "EACH", "ELSE", "END", "ESCAPE", "EXCEPT", "EXCLUDE", "EXCLUSIVE",
    "EXISTS", "EXPLAIN", "FAIL", "FILTER", "FIRST", "FOLLOWING", "FOR", "FOREIGN", "FROM", "FULL",
    "GENERATED", "GLOB", "GROUP", "GROUPS", "HAVING", "IF", "IGNORE", "IMMEDIATE", "IN", "INDEX", "INDEXED",
    "INITIALLY", "INNER", "INSERT", "INSTEAD", "INTERSECT", "INTO", "IS", "ISNULL", "JOIN", "KEY", "LAST",
    "LEFT", "LIKE", "LIMIT", "MATCH", "MATERIALIZED", "NATURAL", "NO", "NOT", "NOTHING", "NOTNULL", "NULL",
    "NULLS", "OF", "OFFSET", "ON", "OR", "ORDER", "OTHERS", "OUTER", "OVER", "PARTITION", "PLAN", "PRAGMA",
    "PRECEDING", "PRIMARY", "QUERY", "RAISE", "RANGE", "RECURSIVE", "REFERENCES", "REGEXP", "REINDEX",
    "RELEASE", "RENAME", "REPLACE", "RESTRICT", "RETURNING", "RIGHT", "ROLLBACK", "ROW", "ROWS", "SAVEPOINT",
    "SELECT", "SET", "TABLE", "TEMP", "TEMPORARY", "THEN", "TIES", "TO", "TRANSACTION", "TRIGGER",
    "UNBOUNDED", "UNION", "UNIQUE", "UPDATE", "USING", "VACUUM", "VALUES", "VIEW", "VIRTUAL", "WHEN", "WHERE",
    "WINDOW", "WITH", "WITHOUT"
]);

export function isReservedWord(name) {
    return RESERVED_WORDS.has(String(name).toUpperCase());
}

export function quoteIdentifier(name) {
    return `"${name}"`;
}

export function sqlType(type) {
    switch (type) {
        case "integer":
            return "INTEGER";
        case "text":
            return "TEXT";
        case "real":
            return "REAL";
        case "blob":
            return "BLOB";
        default:
            throw new Error(`unsupported IR scalar type "${type}"`);
    }
}

export function literalToSql(literal) {
    switch (literal.type) {
        case "integer":
        case "real":
            return literal.value;
        case "text":
            return `'${literal.value.replace(/'/g, "''")}'`;
        case "blob":
            return `X'${literal.value}'`;
        case "null":
            return "NULL";
        default:
            throw new Error(`unsupported literal type "${literal.type}"`);
    }
}

export function emitCreateTableSql(table, ir = null) {
    const lines = [`CREATE TABLE ${quoteIdentifier(table.sql_name)} (`];
    const parts = [];
    for (const column of table.columns) {
        let part = `  ${quoteIdentifier(column.sql_name)} ${sqlType(column.type)}`;
        if (column.autoincrement === true) {
            part += " PRIMARY KEY AUTOINCREMENT";
        }
        if (column.nullable !== true) {
            part += " NOT NULL";
        }
        const literal = effectiveDefault(table, column);
        if (literal !== null) {
            part += ` DEFAULT ${literalToSql(literal)}`;
        }
        parts.push(part);
    }
    const singleAutoincrement = table.primary_key.length === 1 &&
        (table.columns.find((c) => c.name === table.primary_key[0]) || {}).autoincrement === true;
    if (!singleAutoincrement) {
        const keyColumns = table.primary_key.map((name) => quoteIdentifier(columnByName(table, name).sql_name));
        parts.push(`  PRIMARY KEY (${keyColumns.join(", ")})`);
    }
    for (const key of table.uniques) {
        const keyColumns = key.map((name) => quoteIdentifier(columnByName(table, name).sql_name));
        parts.push(`  UNIQUE (${keyColumns.join(",")})`);
    }
    for (const foreignKey of table.foreign_keys || []) {
        const target = ir === null ? null : tableByName(ir, foreignKey.references.table);
        if (target === null) {
            throw new Error(`foreign key target table "${foreignKey.references.table}" is unavailable while emitting "${table.name}"`);
        }
        const localColumns = foreignKey.columns.map((name) => quoteIdentifier(columnByName(table, name).sql_name));
        const targetColumns = foreignKey.references.columns.map((name) => quoteIdentifier(columnByName(target, name).sql_name));
        const constraint = typeof foreignKey.name === "string" ? `CONSTRAINT ${quoteIdentifier(foreignKey.name)} ` : "";
        parts.push(
            `  ${constraint}FOREIGN KEY (${localColumns.join(",")}) REFERENCES ${quoteIdentifier(target.sql_name)} (${targetColumns.join(",")})` +
            ` ON UPDATE ${foreignKey.on_update.toUpperCase()} ON DELETE ${foreignKey.on_delete.toUpperCase()}`
        );
    }
    for (const check of table.checks || []) {
        parts.push(`  CONSTRAINT ${quoteIdentifier(check.name)} CHECK (${check.sql})`);
    }
    lines.push(parts.join(",\n"));
    lines.push(");");
    return lines.join("\n");
}

export function emitCreateIndexSql(table, index) {
    const terms = index.terms.map((term) => {
        if (term.kind === "column") {
            return `${quoteIdentifier(columnByName(table, term.column).sql_name)} ${term.direction.toUpperCase()}`;
        }
        return term.sql;
    });
    const unique = index.unique === true ? "UNIQUE " : "";
    const where = typeof index.where === "string" ? ` WHERE ${index.where}` : "";
    return `CREATE ${unique}INDEX ${quoteIdentifier(index.name)} ON ${quoteIdentifier(table.sql_name)} (${terms.join(", ")})${where};`;
}

export function emitSchemaSql(ir) {
    const statements = [];
    for (const table of ir.tables) {
        statements.push(emitCreateTableSql(table, ir));
    }
    for (const table of ir.tables) {
        for (const index of table.indexes || []) {
            statements.push(emitCreateIndexSql(table, index));
        }
    }
    return statements;
}

export function emitQuerySql(table, query, ir = null) {
    const params = deriveBindParams(table, query, ir);
    const hasJoins = Array.isArray(query.joins) && query.joins.length > 0;
    const projection = table.columns
        .map((column) => hasJoins
            ? `${quoteIdentifier(table.sql_name)}.${quoteIdentifier(column.sql_name)}`
            : quoteIdentifier(column.sql_name))
        .join(",");
    const tableName = quoteIdentifier(table.sql_name);
    let sql;
    if (query.kind === "select") {
        const conditions = Array.isArray(query.where) ? query.where : [];
        const where = conditions
            .map((condition) => {
                const owner = condition.table === undefined
                    ? table
                    : (ir === null ? null : tableByName(ir, condition.table));
                if (owner === null) {
                    throw new Error(`where condition references unavailable table "${condition.table}" while emitting "${query.name}"`);
                }
                const column = quoteIdentifier(columnByName(owner, condition.column).sql_name);
                return hasJoins
                    ? `${quoteIdentifier(owner.sql_name)}.${column} = ?`
                    : `${column} = ?`;
            })
            .join(" AND ");
        let text = `SELECT ${projection} FROM ${tableName}`;
        for (const join of query.joins || []) {
            const joined = ir === null ? null : tableByName(ir, join.table);
            if (joined === null) {
                throw new Error(`joined table "${join.table}" is unavailable while emitting "${query.name}"`);
            }
            const on = join.on.map((condition) => {
                const leftTable = tableByName(ir, condition.left.table);
                const rightTable = tableByName(ir, condition.right.table);
                if (leftTable === null || rightTable === null) {
                    throw new Error(`join condition references an unavailable table while emitting "${query.name}"`);
                }
                return `${quoteIdentifier(leftTable.sql_name)}.${quoteIdentifier(columnByName(leftTable, condition.left.column).sql_name)} = ${quoteIdentifier(rightTable.sql_name)}.${quoteIdentifier(columnByName(rightTable, condition.right.column).sql_name)}`;
            }).join(" AND ");
            text += ` ${join.kind === "left" ? "LEFT JOIN" : "INNER JOIN"} ${quoteIdentifier(joined.sql_name)} ON ${on}`;
        }
        text += where.length > 0 ? ` WHERE ${where}` : "";
        if (Array.isArray(query.order_by) && query.order_by.length > 0) {
            const order = query.order_by
                .map((term) => {
                    const orderTable = term.table === undefined ? table : (ir === null ? null : tableByName(ir, term.table));
                    if (orderTable === null) throw new Error(`order term references unavailable table "${term.table}"`);
                    return `${quoteIdentifier(orderTable.sql_name)}.${quoteIdentifier(columnByName(orderTable, term.column).sql_name)} ${term.direction.toUpperCase()}`;
                })
                .join(", ");
            text += ` ORDER BY ${order}`;
        }
        if (typeof query.limit === "string") {
            text += " LIMIT ?";
        }
        if (typeof query.offset === "string") {
            text += " OFFSET ?";
        }
        sql = text;
    } else if (query.kind === "insert" || query.kind === "upsert") {
        const columns = query.values.map((value) => quoteIdentifier(columnByName(table, value.column).sql_name));
        const placeholders = query.values.map(() => "?").join(",");
        sql = `INSERT INTO ${tableName} (${columns.join(",")}) VALUES (${placeholders})`;
        if (query.kind === "upsert") {
            const target = query.on_conflict.target
                .map((name) => quoteIdentifier(columnByName(table, name).sql_name))
                .join(",");
            if (query.on_conflict.action === "nothing") {
                sql += ` ON CONFLICT (${target}) DO NOTHING`;
            } else {
                const sets = query.on_conflict.set
                    .map((assignment) => `${quoteIdentifier(columnByName(table, assignment.column).sql_name)} = ?`)
                    .join(", ");
                sql += ` ON CONFLICT (${target}) DO UPDATE SET ${sets}`;
            }
        }
    } else if (query.kind === "update") {
        const sets = query.set
            .map((assignment) => `${quoteIdentifier(columnByName(table, assignment.column).sql_name)} = ?`)
            .join(", ");
        const where = query.where
            .map((condition) => `${quoteIdentifier(columnByName(table, condition.column).sql_name)} = ?`)
            .join(" AND ");
        sql = `UPDATE ${tableName} SET ${sets} WHERE ${where}`;
    } else if (query.kind === "delete") {
        const where = query.where
            .map((condition) => `${quoteIdentifier(columnByName(table, condition.column).sql_name)} = ?`)
            .join(" AND ");
        sql = `DELETE FROM ${tableName} WHERE ${where}`;
    } else {
        throw new Error(`unsupported query kind "${query.kind}"`);
    }
    return { sql, params };
}

export function buildMigrationPackage(ir, ctx) {
    const schemaName = ir.schema.name;
    const fileName = `0001_${schemaName}_init.sql`;
    const header = [
        `-- generated by ${ctx.compilerName} ${ctx.compilerVersion}`,
        `-- IR ${ir.ir_version} sha256:${ctx.irHash}`,
        `-- schema: ${schemaName}`,
        `-- migration: 0001_${schemaName}_init`
    ].join("\n");
    const statements = emitSchemaSql(ir).join("\n\n");
    const content = `${header}\n${statements}\n`;
    const manifest = {
        ir_version: ir.ir_version,
        ir_hash: ctx.irHash,
        schema: schemaName,
        migrations: [
            {
                ordinal: "0001",
                version: "1",
                name: `${schemaName}_init`,
                file: fileName,
                sha256: sha256Hex(content)
            }
        ]
    };
    return {
        files: [
            { relPath: `migrations/${fileName}`, content },
            { relPath: "migrations/manifest.json", content: canonicalize(manifest) }
        ],
        manifest
    };
}
