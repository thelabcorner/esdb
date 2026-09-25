/*
 * SQLite-semantic physical schema model for ESDB.
 *
 * Drizzle Kit remains the production DDL/diff authority. This module compares
 * schemas only after SQLite has realized them. Structured PRAGMA metadata is
 * the primary contract; normalized CREATE-token signatures conservatively
 * retain semantics that SQLite's PRAGMAs do not expose directly (CHECK
 * expressions, generated expressions, partial-index predicates, DEFERRABLE
 * clauses, conflict clauses, etc.).
 *
 * The token normalizer deliberately does not attempt algebraic equivalence.
 * False mismatches for differently-written-but-equivalent expressions are
 * preferable to silently treating materially different schemas as identical.
 */

import { createRequire } from "node:module";

import { canonicalize, sha256Hex } from "./canonical.mjs";
import { effectiveDefault } from "./ir.mjs";
import { emitSchemaSql, isReservedWord, literalToSql } from "./sql.mjs";

const require = createRequire(import.meta.url);

export const PHYSICAL_SCHEMA_FORMAT = "esdb.physical-schema/v1";
export const PHYSICAL_NORMALIZER_VERSION = "sqlite-introspection-v1";
export const PHYSICAL_DIFF_FORMAT = "esdb.physical-schema-diff/v1";
export const IR_PHYSICAL_DIFF_FORMAT = "esdb.ir-physical-diff/v1";
export const PHYSICAL_VERIFICATION_FORMAT = "esdb.physical-verification/v1";

export class PhysicalSchemaError extends Error {
    constructor(code, message) {
        super(`${code} ${message}`);
        this.name = "PhysicalSchemaError";
        this.code = code;
    }
}

function sqliteModule() {
    try {
        return require("node:sqlite");
    } catch (error) {
        throw new PhysicalSchemaError(
            "PHY001",
            `node:sqlite is required for physical schema inspection: ${error.message}`
        );
    }
}

function asciiLower(value) {
    return String(value).replace(/[A-Z]/g, (ch) => ch.toLowerCase());
}

export function normalizeIdentifier(value) {
    return asciiLower(value);
}

function integerText(value, label) {
    if (typeof value === "bigint") return value.toString();
    if (typeof value === "number" && Number.isSafeInteger(value)) return String(value);
    if (typeof value === "string" && /^-?(0|[1-9][0-9]*)$/.test(value)) return value;
    throw new PhysicalSchemaError("PHY002", `${label} is not an exact integer`);
}

export function sqliteAffinity(declaredType) {
    const type = String(declaredType || "").toUpperCase();
    if (type.includes("INT")) return "integer";
    if (type.includes("CHAR") || type.includes("CLOB") || type.includes("TEXT")) return "text";
    if (type.length === 0 || type.includes("BLOB")) return "blob";
    if (type.includes("REAL") || type.includes("FLOA") || type.includes("DOUB")) return "real";
    return "numeric";
}

function decodeQuoted(sql, start, quote, doubledQuote) {
    let out = "";
    let index = start + 1;
    while (index < sql.length) {
        const ch = sql[index];
        if (ch === quote) {
            if (doubledQuote && sql[index + 1] === quote) {
                out += quote;
                index += 2;
                continue;
            }
            return { value: out, next: index + 1 };
        }
        out += ch;
        index += 1;
    }
    throw new PhysicalSchemaError("PHY003", "unterminated quoted SQL token");
}

function decodeBracket(sql, start) {
    const end = sql.indexOf("]", start + 1);
    if (end === -1) {
        throw new PhysicalSchemaError("PHY003", "unterminated bracket-quoted identifier");
    }
    return { value: sql.slice(start + 1, end), next: end + 1 };
}

function isSpace(ch) {
    return ch === " " || ch === "\t" || ch === "\r" || ch === "\n" || ch === "\f";
}

function isSymbolStart(ch) {
    return "(),.;+-*/%~&|<>=!".includes(ch);
}

function operatorAt(sql, index) {
    const three = sql.slice(index, index + 3);
    if (three === "->>") return three;
    const two = sql.slice(index, index + 2);
    if (["!=", "==", "<=", ">=", "<>", "||", "<<", ">>", "->"].includes(two)) return two;
    return sql[index];
}

function isNumericStart(sql, index) {
    const ch = sql[index];
    if (ch >= "0" && ch <= "9") return true;
    return ch === "." && sql[index + 1] >= "0" && sql[index + 1] <= "9";
}

function readNumeric(sql, start) {
    const match = /^(?:0[xX][0-9A-Fa-f]+|(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)/.exec(sql.slice(start));
    if (!match) return null;
    return { value: asciiLower(match[0]), next: start + match[0].length };
}

function wordToken(value) {
    const normalized = asciiLower(value);
    if (
        isReservedWord(normalized) ||
        ["null", "true", "false", "current_date", "current_time", "current_timestamp"].includes(normalized)
    ) {
        return `kw:${normalized}`;
    }
    return `id:${normalized}`;
}

/*
 * Formatting-insensitive, deliberately conservative SQL tokenization.
 * Comments/whitespace/case/identifier quote style are normalized. String/blob
 * literal payloads and operator/punctuation boundaries remain exact.
 */
export function canonicalSqlTokens(input) {
    const sql = String(input || "");
    const tokens = [];
    let index = 0;

    while (index < sql.length) {
        const ch = sql[index];

        if (isSpace(ch)) {
            index += 1;
            continue;
        }
        if (ch === "-" && sql[index + 1] === "-") {
            index += 2;
            while (index < sql.length && sql[index] !== "\n") index += 1;
            continue;
        }
        if (ch === "/" && sql[index + 1] === "*") {
            const end = sql.indexOf("*/", index + 2);
            if (end === -1) throw new PhysicalSchemaError("PHY003", "unterminated SQL block comment");
            index = end + 2;
            continue;
        }

        if ((ch === "x" || ch === "X") && sql[index + 1] === "'") {
            const quoted = decodeQuoted(sql, index + 1, "'", true);
            if (!/^(?:[0-9A-Fa-f]{2})*$/.test(quoted.value)) {
                throw new PhysicalSchemaError("PHY003", "invalid SQLite blob literal");
            }
            tokens.push(`blob:${quoted.value.toUpperCase()}`);
            index = quoted.next;
            continue;
        }

        if (ch === "'") {
            const quoted = decodeQuoted(sql, index, "'", true);
            tokens.push(`str:${quoted.value}`);
            index = quoted.next;
            continue;
        }
        if (ch === '"') {
            const quoted = decodeQuoted(sql, index, '"', true);
            tokens.push(`id:${normalizeIdentifier(quoted.value)}`);
            index = quoted.next;
            continue;
        }
        if (ch === "`") {
            const quoted = decodeQuoted(sql, index, "`", true);
            tokens.push(`id:${normalizeIdentifier(quoted.value)}`);
            index = quoted.next;
            continue;
        }
        if (ch === "[") {
            const quoted = decodeBracket(sql, index);
            tokens.push(`id:${normalizeIdentifier(quoted.value)}`);
            index = quoted.next;
            continue;
        }

        if (isNumericStart(sql, index)) {
            const numeric = readNumeric(sql, index);
            if (numeric !== null) {
                tokens.push(`num:${numeric.value}`);
                index = numeric.next;
                continue;
            }
        }

        if (isSymbolStart(ch)) {
            const operator = operatorAt(sql, index);
            tokens.push(`sym:${operator}`);
            index += operator.length;
            continue;
        }

        let end = index + 1;
        while (
            end < sql.length &&
            !isSpace(sql[end]) &&
            !isSymbolStart(sql[end]) &&
            !["'", '"', "`", "[", "]"].includes(sql[end])
        ) {
            end += 1;
        }
        tokens.push(wordToken(sql.slice(index, end)));
        index = end;
    }
    return tokens;
}

function definitionTokens(sql) {
    return sql === null || sql === undefined ? [] : canonicalSqlTokens(sql);
}

function compareCanonical(left, right) {
    const a = canonicalize(left);
    const b = canonicalize(right);
    return a < b ? -1 : a > b ? 1 : 0;
}

function sortCanonical(values) {
    return values.slice().sort(compareCanonical);
}

function tableRows(db, tableName) {
    return db.prepare(
        "SELECT cid, name, type, \"notnull\", dflt_value, pk, hidden " +
        "FROM pragma_table_xinfo(?) ORDER BY cid"
    ).all(tableName);
}

function tableColumns(db, tableName) {
    return tableRows(db, tableName).map((row) => ({
        cid: integerText(row.cid, "column cid"),
        name: normalizeIdentifier(row.name),
        declared_type: canonicalSqlTokens(row.type || ""),
        affinity: sqliteAffinity(row.type || ""),
        not_null: integerText(row.notnull, "column notnull") !== "0",
        default: row.dflt_value === null ? null : canonicalSqlTokens(row.dflt_value),
        primary_key_ordinal: integerText(row.pk, "column pk ordinal"),
        hidden: integerText(row.hidden, "column hidden")
    }));
}

function foreignKeys(db, tableName) {
    const rows = db.prepare(
        "SELECT id, seq, \"table\", \"from\", \"to\", on_update, on_delete, match " +
        "FROM pragma_foreign_key_list(?) ORDER BY id, seq"
    ).all(tableName);

    const groups = new Map();
    for (const row of rows) {
        const id = integerText(row.id, "foreign key id");
        if (!groups.has(id)) {
            groups.set(id, {
                target_table: normalizeIdentifier(row.table),
                on_update: asciiLower(row.on_update),
                on_delete: asciiLower(row.on_delete),
                match: asciiLower(row.match),
                columns: []
            });
        }
        groups.get(id).columns.push({
            ordinal: integerText(row.seq, "foreign key sequence"),
            from: row.from === null ? null : normalizeIdentifier(row.from),
            to: row.to === null ? null : normalizeIdentifier(row.to)
        });
    }
    return sortCanonical(Array.from(groups.values()));
}

function indexKeyColumns(db, indexName) {
    return db.prepare(
        "SELECT seqno, cid, name, \"desc\", coll, \"key\" " +
        "FROM pragma_index_xinfo(?) ORDER BY seqno"
    ).all(indexName)
        .filter((row) => integerText(row.key, "index key flag") !== "0")
        .map((row) => ({
            ordinal: integerText(row.seqno, "index sequence"),
            cid: integerText(row.cid, "index column id"),
            column: row.name === null ? null : normalizeIdentifier(row.name),
            descending: integerText(row.desc, "index descending flag") !== "0",
            collation: row.coll === null ? null : normalizeIdentifier(row.coll)
        }));
}

function indexesForTable(db, tableName, schemaSqlByName) {
    const rows = db.prepare(
        "SELECT seq, name, \"unique\", origin, partial FROM pragma_index_list(?)"
    ).all(tableName);

    const explicit = [];
    const automatic = [];
    for (const row of rows) {
        const base = {
            unique: integerText(row.unique, "index unique flag") !== "0",
            partial: integerText(row.partial, "index partial flag") !== "0",
            origin: asciiLower(row.origin),
            columns: indexKeyColumns(db, row.name)
        };
        if (base.origin === "c") {
            const schemaRow = schemaSqlByName.get(normalizeIdentifier(row.name));
            explicit.push({
                name: normalizeIdentifier(row.name),
                table: normalizeIdentifier(tableName),
                ...base,
                definition: definitionTokens(schemaRow ? schemaRow.sql : null)
            });
        } else {
            automatic.push(base);
        }
    }
    return {
        explicit: explicit.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0),
        automatic: sortCanonical(automatic)
    };
}

function viewColumns(db, viewName) {
    return tableColumns(db, viewName);
}

function schemaRows(db) {
    return db.prepare(
        "SELECT type, name, tbl_name, sql FROM sqlite_schema " +
        "WHERE name NOT LIKE 'sqlite_%' AND type IN ('table','index','view','trigger') " +
        "ORDER BY type, name"
    ).all();
}

export function snapshotDatabase(db) {
    if (!db || typeof db.prepare !== "function") {
        throw new PhysicalSchemaError("PHY004", "snapshotDatabase requires an open SQLite database");
    }

    const rows = schemaRows(db);
    const schemaSqlByName = new Map();
    for (const row of rows) schemaSqlByName.set(normalizeIdentifier(row.name), row);

    const tableList = db.prepare(
        "SELECT name, type, ncol, wr, strict FROM pragma_table_list " +
        "WHERE schema='main' AND name NOT LIKE 'sqlite_%'"
    ).all();
    const tableMeta = new Map();
    for (const row of tableList) tableMeta.set(normalizeIdentifier(row.name), row);

    const tables = [];
    const indexes = [];
    const views = [];
    const triggers = [];

    for (const row of rows) {
        const type = asciiLower(row.type);
        const name = normalizeIdentifier(row.name);
        if (type === "table") {
            const meta = tableMeta.get(name);
            const indexModel = indexesForTable(db, row.name, schemaSqlByName);
            indexes.push(...indexModel.explicit);
            tables.push({
                name,
                table_kind: meta ? asciiLower(meta.type) : "table",
                without_rowid: meta ? integerText(meta.wr, "table without-rowid flag") !== "0" : false,
                strict: meta ? integerText(meta.strict, "table strict flag") !== "0" : false,
                columns: tableColumns(db, row.name),
                foreign_keys: foreignKeys(db, row.name),
                automatic_indexes: indexModel.automatic,
                definition: definitionTokens(row.sql)
            });
        } else if (type === "view") {
            views.push({
                name,
                columns: viewColumns(db, row.name),
                definition: definitionTokens(row.sql)
            });
        } else if (type === "trigger") {
            triggers.push({
                name,
                table: normalizeIdentifier(row.tbl_name),
                definition: definitionTokens(row.sql)
            });
        }
    }

    tables.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    indexes.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    views.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    triggers.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);

    return {
        format: PHYSICAL_SCHEMA_FORMAT,
        normalizer: PHYSICAL_NORMALIZER_VERSION,
        tables,
        indexes,
        views,
        triggers
    };
}

export function physicalSchemaHash(snapshot) {
    if (!snapshot || snapshot.format !== PHYSICAL_SCHEMA_FORMAT) {
        throw new PhysicalSchemaError("PHY004", "physicalSchemaHash requires an ESDB physical schema snapshot");
    }
    return sha256Hex(canonicalize(snapshot));
}

export function snapshotSql(sql) {
    const { DatabaseSync } = sqliteModule();
    const db = new DatabaseSync(":memory:");
    try {
        db.exec("PRAGMA foreign_keys=ON");
        if (Array.isArray(sql)) {
            for (const part of sql) db.exec(String(part));
        } else {
            db.exec(String(sql));
        }
        return snapshotDatabase(db);
    } finally {
        db.close();
    }
}

export function snapshotIr(ir) {
    if (!ir || !Array.isArray(ir.tables)) {
        throw new PhysicalSchemaError("PHY004", "snapshotIr requires a validated IR document");
    }
    return snapshotSql(emitSchemaSql(ir));
}

export function inspectDatabasePath(path) {
    const { DatabaseSync } = sqliteModule();
    const db = new DatabaseSync(path, { readOnly: true });
    let ownTransaction = false;
    try {
        db.exec("PRAGMA query_only=ON");
        if (!db.isTransaction) {
            db.exec("BEGIN DEFERRED");
            ownTransaction = true;
        }
        const versionRow = db.prepare("PRAGMA user_version").get();
        const userVersion = integerText(versionRow.user_version, "PRAGMA user_version");
        const snapshot = snapshotDatabase(db);
        if (ownTransaction) {
            db.exec("COMMIT");
            ownTransaction = false;
        }
        return { user_version: userVersion, snapshot };
    } catch (error) {
        if (ownTransaction && db.isTransaction) {
            try {
                db.exec("ROLLBACK");
            } catch {
                /* Preserve the original inspection failure. */
            }
        }
        throw error;
    } finally {
        db.close();
    }
}

export function snapshotDatabasePath(path) {
    return inspectDatabasePath(path).snapshot;
}

function pushValueDiff(mismatches, object, path, expected, actual) {
    if (expected === actual) return;

    const expectedArray = Array.isArray(expected);
    const actualArray = Array.isArray(actual);
    if (expectedArray || actualArray) {
        if (!(expectedArray && actualArray)) {
            mismatches.push({ kind: "value_mismatch", object, path, expected, actual });
            return;
        }
        if (expected.length !== actual.length) {
            mismatches.push({
                kind: "value_mismatch",
                object,
                path: `${path}.length`,
                expected: String(expected.length),
                actual: String(actual.length)
            });
        }
        const count = Math.min(expected.length, actual.length);
        for (let i = 0; i < count; i += 1) {
            pushValueDiff(mismatches, object, `${path}[${i}]`, expected[i], actual[i]);
        }
        return;
    }

    const expectedObject = expected !== null && typeof expected === "object";
    const actualObject = actual !== null && typeof actual === "object";
    if (expectedObject || actualObject) {
        if (!(expectedObject && actualObject)) {
            mismatches.push({ kind: "value_mismatch", object, path, expected, actual });
            return;
        }
        const keys = Array.from(new Set([...Object.keys(expected), ...Object.keys(actual)])).sort();
        for (const key of keys) {
            if (!(key in expected)) {
                mismatches.push({
                    kind: "value_mismatch",
                    object,
                    path: `${path}.${key}`,
                    expected: null,
                    actual: actual[key]
                });
            } else if (!(key in actual)) {
                mismatches.push({
                    kind: "value_mismatch",
                    object,
                    path: `${path}.${key}`,
                    expected: expected[key],
                    actual: null
                });
            } else {
                pushValueDiff(mismatches, object, `${path}.${key}`, expected[key], actual[key]);
            }
        }
        return;
    }

    mismatches.push({ kind: "value_mismatch", object, path, expected, actual });
}

function diffNamedCollection(mismatches, label, expected, actual) {
    const expectedMap = new Map(expected.map((item) => [item.name, item]));
    const actualMap = new Map(actual.map((item) => [item.name, item]));
    const names = Array.from(new Set([...expectedMap.keys(), ...actualMap.keys()])).sort();
    for (const name of names) {
        const object = `${label}:${name}`;
        if (!actualMap.has(name)) {
            mismatches.push({ kind: "missing_object", object });
            continue;
        }
        if (!expectedMap.has(name)) {
            mismatches.push({ kind: "extra_object", object });
            continue;
        }
        pushValueDiff(mismatches, object, "$", expectedMap.get(name), actualMap.get(name));
    }
}

export function diffPhysicalSchemas(expected, actual) {
    if (
        !expected || !actual ||
        expected.format !== PHYSICAL_SCHEMA_FORMAT ||
        actual.format !== PHYSICAL_SCHEMA_FORMAT
    ) {
        throw new PhysicalSchemaError("PHY004", "diffPhysicalSchemas requires two physical schema snapshots");
    }

    const mismatches = [];
    diffNamedCollection(mismatches, "table", expected.tables, actual.tables);
    diffNamedCollection(mismatches, "index", expected.indexes, actual.indexes);
    diffNamedCollection(mismatches, "view", expected.views, actual.views);
    diffNamedCollection(mismatches, "trigger", expected.triggers, actual.triggers);

    const expectedHash = physicalSchemaHash(expected);
    const actualHash = physicalSchemaHash(actual);
    return {
        format: PHYSICAL_DIFF_FORMAT,
        ok: mismatches.length === 0,
        expected_hash: expectedHash,
        actual_hash: actualHash,
        mismatches
    };
}

function physicalTable(snapshot, name) {
    const normalized = normalizeIdentifier(name);
    return snapshot.tables.find((table) => table.name === normalized) || null;
}

function physicalColumn(table, name) {
    const normalized = normalizeIdentifier(name);
    return table.columns.find((column) => column.name === normalized) || null;
}

function keyColumns(index) {
    return index.columns
        .slice()
        .sort((a, b) => Number(a.ordinal) - Number(b.ordinal))
        .map((column) => column.column);
}

function arraysEqual(left, right) {
    if (left.length !== right.length) return false;
    for (let i = 0; i < left.length; i += 1) {
        if (left[i] !== right[i]) return false;
    }
    return true;
}

function physicalUniqueKeys(snapshot, table) {
    const keys = [];
    for (const index of table.automatic_indexes) {
        if (index.unique && index.origin !== "pk") keys.push(keyColumns(index));
    }
    for (const index of snapshot.indexes) {
        if (index.table === table.name && index.unique) keys.push(keyColumns(index));
    }
    return keys;
}

function irDefaultTokens(table, column) {
    const literal = effectiveDefault(table, column);
    return literal === null ? null : canonicalSqlTokens(literalToSql(literal));
}

function indexDefinitionParts(definition) {
    const tokens = Array.isArray(definition) ? definition : [];
    const onIndex = tokens.indexOf("kw:on");
    let open = -1;
    for (let index = onIndex === -1 ? 0 : onIndex + 1; index < tokens.length; index += 1) {
        if (tokens[index] === "sym:(") { open = index; break; }
    }
    if (open === -1) return { terms: [], where: [] };
    let depth = 1;
    let cursor = open + 1;
    const terms = [];
    while (cursor < tokens.length && depth > 0) {
        const token = tokens[cursor];
        if (token === "sym:(") depth += 1;
        if (token === "sym:)") depth -= 1;
        if (depth > 0 && token !== "kw:asc") terms.push(token);
        cursor += 1;
    }
    const whereIndex = tokens.indexOf("kw:where", cursor);
    const where = whereIndex === -1 ? [] : tokens.slice(whereIndex + 1);
    return { terms, where };
}

function indexSemantic(index) {
    const definition = indexDefinitionParts(index.definition);
    const hasExpression = index.columns.some((column) => column.column === null);
    return {
        unique: index.unique,
        partial: index.partial,
        columns: index.columns,
        expression_terms: hasExpression ? definition.terms : [],
        where: definition.where
    };
}
function physicalChecks(definition) {
    const checks = [];
    const tokens = Array.isArray(definition) ? definition : [];
    for (let index = 0; index < tokens.length; index += 1) {
        if (tokens[index] !== "kw:check" || tokens[index + 1] !== "sym:(") continue;
        let name = null;
        if (index >= 2 && tokens[index - 2] === "kw:constraint" && String(tokens[index - 1]).startsWith("id:")) {
            name = String(tokens[index - 1]).slice(3);
        }
        let depth = 1;
        let cursor = index + 2;
        const expression = [];
        while (cursor < tokens.length && depth > 0) {
            const token = tokens[cursor];
            if (token === "sym:(") {
                depth += 1;
                expression.push(token);
            } else if (token === "sym:)") {
                depth -= 1;
                if (depth > 0) expression.push(token);
            } else {
                expression.push(token);
            }
            cursor += 1;
        }
        checks.push({ name, expression });
        index = cursor - 1;
    }
    return sortCanonical(checks);
}
function irPhysicalMismatch(mismatches, object, path, expected, actual) {
    mismatches.push({
        kind: "ir_semantic_mismatch",
        object,
        path,
        expected,
        actual
    });
}

function irPhysicalUnmodeled(unmodeled, kind, object, detail) {
    unmodeled.push({ kind, object, detail });
}

/*
 * One-way compatibility differential: every semantic fact represented in IR
 * must be present in the realized physical schema. Physical details that IR v1
 * cannot yet express are returned separately as "unmodeled" rather than being
 * silently discarded or treated as proof of equivalence.
 */
export function diffIrAgainstPhysical(ir, actual) {
    if (!ir || !Array.isArray(ir.tables)) {
        throw new PhysicalSchemaError("PHY004", "diffIrAgainstPhysical requires an IR document");
    }
    if (!actual || actual.format !== PHYSICAL_SCHEMA_FORMAT) {
        throw new PhysicalSchemaError("PHY004", "diffIrAgainstPhysical requires a physical schema snapshot");
    }

    const mismatches = [];
    const unmodeled = [];
    const irTableNames = new Set(ir.tables.map((table) => normalizeIdentifier(table.sql_name)));
    const expectedModeled = snapshotIr(ir);
    const modeledIndexNames = new Set();

    for (const table of ir.tables) {
        const expectedTableName = normalizeIdentifier(table.sql_name);
        const actualTable = physicalTable(actual, expectedTableName);
        const object = `table:${expectedTableName}`;
        if (actualTable === null) {
            mismatches.push({ kind: "missing_object", object });
            continue;
        }

        const expectedColumns = new Set();
        for (const column of table.columns) {
            const columnName = normalizeIdentifier(column.sql_name);
            expectedColumns.add(columnName);
            const actualColumn = physicalColumn(actualTable, columnName);
            const columnObject = `${object}.column:${columnName}`;
            if (actualColumn === null) {
                mismatches.push({ kind: "missing_object", object: columnObject });
                continue;
            }

            const expectedAffinity = column.type;
            if (actualColumn.affinity !== expectedAffinity) {
                irPhysicalMismatch(
                    mismatches,
                    columnObject,
                    "$.affinity",
                    expectedAffinity,
                    actualColumn.affinity
                );
            }
            if (actualColumn.not_null !== (column.nullable !== true)) {
                irPhysicalMismatch(
                    mismatches,
                    columnObject,
                    "$.not_null",
                    column.nullable !== true,
                    actualColumn.not_null
                );
            }

            const pkIndex = table.primary_key.indexOf(column.name);
            const expectedPkOrdinal = pkIndex === -1 ? "0" : String(pkIndex + 1);
            if (actualColumn.primary_key_ordinal !== expectedPkOrdinal) {
                irPhysicalMismatch(
                    mismatches,
                    columnObject,
                    "$.primary_key_ordinal",
                    expectedPkOrdinal,
                    actualColumn.primary_key_ordinal
                );
            }

            const expectedDefault = irDefaultTokens(table, column);
            if (canonicalize(actualColumn.default) !== canonicalize(expectedDefault)) {
                irPhysicalMismatch(
                    mismatches,
                    columnObject,
                    "$.default",
                    expectedDefault,
                    actualColumn.default
                );
            }

            if (column.autoincrement === true && !actualTable.definition.includes("kw:autoincrement")) {
                irPhysicalMismatch(
                    mismatches,
                    columnObject,
                    "$.autoincrement",
                    true,
                    false
                );
            }
        }

        for (const column of actualTable.columns) {
            if (!expectedColumns.has(column.name)) {
                irPhysicalUnmodeled(unmodeled, "column", `${object}.column:${column.name}`, "not represented by IR");
            }
        }

        const uniqueKeys = physicalUniqueKeys(actual, actualTable);
        for (const key of table.uniques) {
            const expectedKey = key.map((columnName) => {
                const column = table.columns.find((candidate) => candidate.name === columnName);
                return column ? normalizeIdentifier(column.sql_name) : normalizeIdentifier(columnName);
            });
            if (!uniqueKeys.some((candidate) => arraysEqual(candidate, expectedKey))) {
                irPhysicalMismatch(
                    mismatches,
                    object,
                    "$.unique",
                    expectedKey,
                    uniqueKeys
                );
            }
        }

        const expectedPhysicalTable = physicalTable(expectedModeled, expectedTableName);
        if (Object.prototype.hasOwnProperty.call(table, "foreign_keys")) {
            const expectedForeignKeys = expectedPhysicalTable === null ? [] : expectedPhysicalTable.foreign_keys;
            if (canonicalize(expectedForeignKeys) !== canonicalize(actualTable.foreign_keys)) {
                irPhysicalMismatch(mismatches, object, "$.foreign_keys", expectedForeignKeys, actualTable.foreign_keys);
            }
        } else if (actualTable.foreign_keys.length > 0) {
            irPhysicalUnmodeled(unmodeled, "foreign_keys", object, actualTable.foreign_keys);
        }

        const actualChecks = physicalChecks(actualTable.definition);
        if (Object.prototype.hasOwnProperty.call(table, "checks")) {
            const expectedChecks = expectedPhysicalTable === null ? [] : physicalChecks(expectedPhysicalTable.definition);
            if (canonicalize(expectedChecks) !== canonicalize(actualChecks)) {
                irPhysicalMismatch(mismatches, object, "$.checks", expectedChecks, actualChecks);
            }
        } else if (actualChecks.length > 0) {
            irPhysicalUnmodeled(unmodeled, "checks", object, actualChecks);
        }

        for (const index of table.indexes || []) {
            const indexName = normalizeIdentifier(index.name);
            modeledIndexNames.add(indexName);
            const expectedIndex = expectedModeled.indexes.find((candidate) => candidate.name === indexName) || null;
            const actualIndex = actual.indexes.find((candidate) => candidate.name === indexName) || null;
            const indexObject = `index:${indexName}`;
            if (actualIndex === null) {
                mismatches.push({ kind: "missing_object", object: indexObject });
                continue;
            }
            const expectedSemantic = expectedIndex === null ? null : indexSemantic(expectedIndex);
            const actualSemantic = indexSemantic(actualIndex);
            if (canonicalize(expectedSemantic) !== canonicalize(actualSemantic)) {
                irPhysicalMismatch(mismatches, indexObject, "$", expectedSemantic, actualSemantic);
            }
        }
    }

    for (const table of actual.tables) {
        if (!irTableNames.has(table.name)) {
            irPhysicalUnmodeled(unmodeled, "table", `table:${table.name}`, "not represented by IR");
        }
    }
    for (const index of actual.indexes) {
        if (!modeledIndexNames.has(index.name)) {
            irPhysicalUnmodeled(unmodeled, "index", `index:${index.name}`, index);
        }
    }
    for (const view of actual.views) {
        irPhysicalUnmodeled(unmodeled, "view", `view:${view.name}`, view);
    }
    for (const trigger of actual.triggers) {
        irPhysicalUnmodeled(unmodeled, "trigger", `trigger:${trigger.name}`, trigger);
    }

    return {
        format: IR_PHYSICAL_DIFF_FORMAT,
        compatible: mismatches.length === 0,
        physical_hash: physicalSchemaHash(actual),
        mismatches,
        unmodeled
    };
}

export function diffKitSqlAgainstIr(ir, kitSql) {
    return diffIrAgainstPhysical(ir, snapshotSql(kitSql));
}
