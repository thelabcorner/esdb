/*
 * esdb-schema: IR v1 model + validation.
 *
 * The compiler is dependency-free ESM so it can run from a fresh checkout with
 * `node` alone. Validation is normative; orm/ir/esdb-ir-v1.schema.json is an
 * informative mirror for external tools.
 */

import { isReservedWord } from "./sql.mjs";
import { bridgeFunctionName, queryMethodName, toCamel, toPascal, toSnake } from "./naming.mjs";

export const IR_VERSION = "esdb.ir/v1";
export const CANONICAL_ID = "esdb-canonical-json-v1";
export const COMPILER_NAME = "esdb-schema";
export const COMPILER_VERSION = "0.2.0";

export const SCALAR_TYPES = ["integer", "text", "real", "blob"];

/* ES3 reserved words that cannot be bare property/method names in generated
 * ExtendScript bindings. Query method names are checked against this set. */
export const ES3_RESERVED_WORDS = new Set([
    "abstract", "boolean", "break", "byte", "case", "catch", "char", "class", "const", "continue",
    "debugger", "default", "delete", "do", "double", "else", "enum", "export", "extends", "false",
    "final", "finally", "float", "for", "function", "goto", "if", "implements", "import", "in",
    "instanceof", "int", "interface", "long", "native", "new", "null", "package", "private",
    "protected", "public", "return", "short", "static", "super", "switch", "synchronized", "this",
    "throw", "throws", "transient", "true", "try", "typeof", "var", "void", "volatile", "while", "with"
]);
export const QUERY_KINDS = ["select", "insert", "upsert", "update", "delete"];
export const CARDINALITIES = ["one", "many", "changes"];
export const LITERAL_TYPES = ["integer", "text", "real", "blob", "null"];
export const FOREIGN_KEY_ACTIONS = ["no action", "restrict", "cascade", "set null", "set default"];

const IDENTIFIER_RE = /^[A-Za-z_][A-Za-z0-9_]{0,63}$/;
const QUERY_NAME_RE = /^[A-Za-z_][A-Za-z0-9_]{0,63}(\.[A-Za-z_][A-Za-z0-9_]{0,63})?$/;
const INT64_RE = /^(0|-?[1-9][0-9]*)$/;
const REAL_RE = /^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$/;
const BLOB_RE = /^(?:[0-9A-F]{2})*$/;
const HASH_RE = /^[0-9a-f]{64}$/;
const RESERVED_SQL_PREFIX_RE = /^(?:sqlite_|__esdb_)/i;
const HAZARDOUS_LOGICAL_NAMES = new Set(["__proto__", "prototype", "constructor"]);
const MAX_NESTING_DEPTH = 128;
const CXX_RESERVED_WORDS = new Set([
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
    "char", "char16_t", "char32_t", "class", "compl", "const", "constexpr", "const_cast", "continue", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
    "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
    "reinterpret_cast", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
]);
const GENERATED_LIFECYCLE_NAMES = new Set([
    "constructor", "prototype", "__proto__", "valid", "lastError", "last_error", "lastErrorCode", "last_error_code",
    "open", "close", "load", "unload", "dispose", "repository", "handle", "invoke", "values", "esdbOptions"
]);

export const INT64_MIN = "-9223372036854775808";
export const INT64_MAX = "9223372036854775807";

export class GenerationError extends Error {
    constructor(code, message) {
        super(`${code} ${message}`);
        this.name = "GenerationError";
        this.code = code;
    }
}

export class IrValidationError extends Error {
    constructor(diagnostics) {
        const first = diagnostics[0];
        super(`IR validation failed: ${first.code} ${first.path} ${first.message}`);
        this.name = "IrValidationError";
        this.diagnostics = diagnostics;
    }
}

function diag(diagnostics, code, path, message) {
    diagnostics.push({ code, path, message });
}

function isPlainObject(value) {
    return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isIdentifier(value) {
    return typeof value === "string" && IDENTIFIER_RE.test(value);
}

function checkLogicalIdentifier(diagnostics, value, path, label) {
    if (!isIdentifier(value)) {
        return;
    }
    if (HAZARDOUS_LOGICAL_NAMES.has(value)) {
        diag(diagnostics, "IR032", path, `${label} "${value}" is unsafe for generated language bindings`);
    }
    const forms = [toCamel(value), toSnake(value), toPascal(value)];
    if (forms.some((form) => form.length === 0)) {
        diag(diagnostics, "IR032", path, `${label} "${value}" normalizes to an empty generated identifier`);
    }
    for (const form of forms) {
        if (ES3_RESERVED_WORDS.has(form) || CXX_RESERVED_WORDS.has(form)) {
            diag(diagnostics, "IR031", path, `${label} "${value}" normalizes to reserved identifier "${form}"`);
            break;
        }
    }
}

function checkSqlIdentifier(diagnostics, value, path, label) {
    if (typeof value !== "string" || !IDENTIFIER_RE.test(value)) {
        return;
    }
    if (RESERVED_SQL_PREFIX_RE.test(value)) {
        diag(diagnostics, "IR032", path, `${label} "${value}" uses reserved sqlite_/__esdb_ prefix`);
    }
}

function recordGeneratedForms(maps, value, path, diagnostics, label) {
    if (!isIdentifier(value)) {
        return;
    }
    const forms = {
        camel: toCamel(value),
        snake: toSnake(value),
        pascal: toPascal(value)
    };
    for (const [kind, form] of Object.entries(forms)) {
        const existing = maps[kind].get(form);
        if (existing && existing !== value) {
            diag(
                diagnostics,
                "IR032",
                path,
                `${label} "${value}" collides with "${existing}" after ${kind} normalization ("${form}")`
            );
        } else {
            maps[kind].set(form, value);
        }
    }
}

function generatedFormMaps() {
    return { camel: new Map(), snake: new Map(), pascal: new Map() };
}

function checkBooleanField(diagnostics, node, field, path) {
    if (typeof node[field] !== "boolean") {
        diag(diagnostics, "IR012", `${path}.${field}`, `"${field}" must be a boolean`);
    }
}

function checkStringField(diagnostics, node, field, path) {
    if (typeof node[field] !== "string") {
        diag(diagnostics, "IR012", `${path}.${field}`, `"${field}" must be a string`);
    }
}

function checkUnknownFields(diagnostics, node, allowed, path) {
    for (const key of Object.keys(node)) {
        if (!allowed.includes(key)) {
            diag(diagnostics, "IR023", `${path}.${key}`, `unknown field "${key}"`);
        }
    }
}

function isUnicodeScalarString(value) {
    for (let i = 0; i < value.length; i += 1) {
        const code = value.charCodeAt(i);
        if (code >= 0xd800 && code <= 0xdbff) {
            const low = value.charCodeAt(i + 1);
            if (!(low >= 0xdc00 && low <= 0xdfff)) {
                return false;
            }
            i += 1;
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            return false;
        }
    }
    return true;
}

function checkScalarWalk(value, path, diagnostics, depth = 0) {
    if (depth > MAX_NESTING_DEPTH) {
        diag(diagnostics, "IR024", path, `JSON nesting exceeds maximum depth ${MAX_NESTING_DEPTH}`);
        return;
    }
    if (typeof value === "number") {
        diag(
            diagnostics,
            "IR022",
            path,
            "IR v1 forbids JSON numbers; carry numeric data as canonical decimal strings"
        );
        return;
    }
    if (typeof value === "string") {
        if (!isUnicodeScalarString(value)) {
            diag(diagnostics, "IR024", path, "string contains an unpaired surrogate");
        }
        return;
    }
    if (value === null || typeof value === "boolean") {
        return;
    }
    if (Array.isArray(value)) {
        for (let i = 0; i < value.length; i += 1) {
            if (!(i in value)) {
                diag(diagnostics, "IR024", `${path}[${i}]`, "array holes are not representable in JSON");
                continue;
            }
            checkScalarWalk(value[i], `${path}[${i}]`, diagnostics, depth + 1);
        }
        return;
    }
    if (isPlainObject(value)) {
        for (const key of Object.keys(value)) {
            if (!isUnicodeScalarString(key)) {
                diag(diagnostics, "IR024", path, `object key "${key}" contains an unpaired surrogate`);
            }
            checkScalarWalk(value[key], `${path}.${key}`, diagnostics, depth + 1);
        }
        return;
    }
    diag(diagnostics, "IR024", path, `unsupported JSON value of type ${typeof value}`);
}

function checkLiteral(literal, columnType, nullable, path, diagnostics) {
    if (!isPlainObject(literal)) {
        diag(diagnostics, "IR016", path, "default must be a literal object");
        return;
    }
    checkUnknownFields(diagnostics, literal, ["kind", "type", "value"], path);
    if (literal.kind !== "literal") {
        diag(diagnostics, "IR016", `${path}.kind`, 'default "kind" must be "literal"');
    }
    if (!LITERAL_TYPES.includes(literal.type)) {
        diag(diagnostics, "IR016", `${path}.type`, `default type must be one of ${LITERAL_TYPES.join(", ")}`);
        return;
    }
    if (typeof literal.value !== "string") {
        diag(diagnostics, "IR016", `${path}.value`, "default value must be a string");
        return;
    }
    if (literal.type !== "null" && literal.type !== columnType) {
        diag(
            diagnostics,
            "IR016",
            `${path}.type`,
            `default type "${literal.type}" does not match column type "${columnType}"`
        );
        return;
    }
    if (literal.type === "null") {
        if (!nullable) {
            diag(diagnostics, "IR016", path, "NOT NULL columns may not declare DEFAULT NULL");
        }
        if (literal.value !== "") {
            diag(diagnostics, "IR016", `${path}.value`, 'null default value must be ""');
        }
        return;
    }
    if (literal.type === "integer") {
        if (!INT64_RE.test(literal.value) || !int64InRange(literal.value)) {
            diag(diagnostics, "IR016", `${path}.value`, "integer default must be a canonical signed 64-bit decimal");
        }
    } else if (literal.type === "real") {
        if (!REAL_RE.test(literal.value) || !Number.isFinite(Number(literal.value))) {
            diag(diagnostics, "IR016", `${path}.value`, "real default must be a finite canonical decimal token");
        }
    } else if (literal.type === "blob") {
        if (!BLOB_RE.test(literal.value)) {
            diag(diagnostics, "IR016", `${path}.value`, "blob default must be uppercase even-length hex");
        }
    } else if (literal.type === "text" && literal.value.indexOf("\0") !== -1) {
        diag(diagnostics, "IR016", `${path}.value`, "text default may not contain NUL");
    }
}

function int64InRange(decimal) {
    const digits = decimal.replace(/^-/, "");
    const limit = decimal.startsWith("-") ? INT64_MIN.slice(1) : INT64_MAX;
    const trimmed = digits.replace(/^0+/, "") || "0";
    if (trimmed.length !== limit.length) {
        return trimmed.length < limit.length;
    }
    return trimmed <= limit;
}

function checkColumn(column, table, index, diagnostics) {
    const path = `$.tables[${table.index}].columns[${index}]`;
    if (!isPlainObject(column)) {
        diag(diagnostics, "IR009", path, "column must be an object");
        return;
    }
    checkUnknownFields(
        diagnostics,
        column,
        ["name", "sql_name", "type", "nullable", "primary_key", "autoincrement", "unique", "default"],
        path
    );
    if (!isIdentifier(column.name)) {
        diag(diagnostics, "IR009", `${path}.name`, "column name must match [A-Za-z_][A-Za-z0-9_]{0,63}");
    } else {
        checkLogicalIdentifier(diagnostics, column.name, `${path}.name`, "column name");
    }
    if (!isIdentifier(column.sql_name)) {
        diag(diagnostics, "IR009", `${path}.sql_name`, "column sql_name must match [A-Za-z_][A-Za-z0-9_]{0,63}");
    } else if (isReservedWord(column.sql_name)) {
        diag(diagnostics, "IR028", `${path}.sql_name`, `column sql_name "${column.sql_name}" is a reserved SQL word`);
    }
    checkSqlIdentifier(diagnostics, column.sql_name, `${path}.sql_name`, "column sql_name");
    if (!SCALAR_TYPES.includes(column.type)) {
        diag(diagnostics, "IR011", `${path}.type`, `unknown column type "${String(column.type)}"`);
    }
    checkBooleanField(diagnostics, column, "nullable", path);
    checkBooleanField(diagnostics, column, "primary_key", path);
    checkBooleanField(diagnostics, column, "autoincrement", path);
    checkBooleanField(diagnostics, column, "unique", path);
    if (column.autoincrement === true) {
        if (column.primary_key !== true || column.type !== "integer") {
            diag(
                diagnostics,
                "IR013",
                `${path}.autoincrement`,
                "autoincrement is only valid on a single-column integer primary key"
            );
        } else if (
            !Array.isArray(table.primary_key) ||
            table.primary_key.length !== 1 ||
            table.primary_key[0] !== column.name
        ) {
            diag(
                diagnostics,
                "IR013",
                `${path}.autoincrement`,
                "autoincrement requires the table primary key to be exactly this column"
            );
        }
    }
    if ("default" in column) {
        checkLiteral(column.default, column.type, column.nullable === true, `${path}.default`, diagnostics);
    }
}

function checkStaticSqlExpression(value, path, diagnostics, label) {
    if (typeof value !== "string" || value.length === 0) {
        diag(diagnostics, "IR034", path, `${label} must be a non-empty SQL expression string`);
        return;
    }
    if (value.length > 8192) {
        diag(diagnostics, "IR034", path, `${label} exceeds the 8192-byte IR v1 expression limit`);
    }
    if (/[;\0?]|--|\/\*|\*\//.test(value)) {
        diag(diagnostics, "IR034", path, `${label} may not contain statement separators, comments, NULs, or bind placeholders`);
    }
}

function checkForeignKeys(table, columnNames, path, diagnostics) {
    if (!("foreign_keys" in table)) return;
    if (!Array.isArray(table.foreign_keys)) {
        diag(diagnostics, "IR035", `${path}.foreign_keys`, "foreign_keys must be an array");
        return;
    }
    const names = new Set();
    let previousName = null;
    for (let i = 0; i < table.foreign_keys.length; i += 1) {
        const foreignKey = table.foreign_keys[i];
        const fkPath = `${path}.foreign_keys[${i}]`;
        if (!isPlainObject(foreignKey)) {
            diag(diagnostics, "IR035", fkPath, "foreign key must be an object");
            continue;
        }
        checkUnknownFields(diagnostics, foreignKey, ["name", "columns", "references", "on_update", "on_delete"], fkPath);
        if (!isIdentifier(foreignKey.name)) {
            diag(diagnostics, "IR035", `${fkPath}.name`, "foreign-key name must be an identifier");
        } else {
            checkSqlIdentifier(diagnostics, foreignKey.name, `${fkPath}.name`, "foreign-key name");
            const folded = foreignKey.name.toLowerCase();
            if (names.has(folded)) diag(diagnostics, "IR035", `${fkPath}.name`, `duplicate foreign-key name "${foreignKey.name}"`);
            names.add(folded);
            if (previousName !== null && previousName > foreignKey.name) {
                diag(diagnostics, "IR033", `${fkPath}.name`, "foreign keys must be sorted by name");
            }
            previousName = foreignKey.name;
        }
        if (!Array.isArray(foreignKey.columns) || foreignKey.columns.length === 0) {
            diag(diagnostics, "IR035", `${fkPath}.columns`, "foreign-key columns must be a non-empty array");
        } else {
            const seen = new Set();
            foreignKey.columns.forEach((name, columnIndex) => {
                if (!isIdentifier(name) || !columnNames.has(name)) {
                    diag(diagnostics, "IR035", `${fkPath}.columns[${columnIndex}]`, `unknown foreign-key column "${String(name)}"`);
                }
                if (seen.has(name)) diag(diagnostics, "IR035", `${fkPath}.columns[${columnIndex}]`, `duplicate foreign-key column "${String(name)}"`);
                seen.add(name);
            });
        }
        if (!isPlainObject(foreignKey.references)) {
            diag(diagnostics, "IR035", `${fkPath}.references`, "references must be an object");
        } else {
            checkUnknownFields(diagnostics, foreignKey.references, ["table", "columns"], `${fkPath}.references`);
            if (!isIdentifier(foreignKey.references.table)) {
                diag(diagnostics, "IR035", `${fkPath}.references.table`, "referenced table must be a logical table identifier");
            }
            if (!Array.isArray(foreignKey.references.columns) || foreignKey.references.columns.length === 0) {
                diag(diagnostics, "IR035", `${fkPath}.references.columns`, "referenced columns must be a non-empty array");
            } else if (Array.isArray(foreignKey.columns) && foreignKey.references.columns.length !== foreignKey.columns.length) {
                diag(diagnostics, "IR035", `${fkPath}.references.columns`, "foreign-key local and referenced column counts must match");
            }
        }
        for (const field of ["on_update", "on_delete"]) {
            if (!FOREIGN_KEY_ACTIONS.includes(foreignKey[field])) {
                diag(diagnostics, "IR035", `${fkPath}.${field}`, `${field} must be one of ${FOREIGN_KEY_ACTIONS.join(", ")}`);
            }
        }
    }
}

function checkChecks(table, path, diagnostics) {
    if (!("checks" in table)) return;
    if (!Array.isArray(table.checks)) {
        diag(diagnostics, "IR036", `${path}.checks`, "checks must be an array");
        return;
    }
    const names = new Set();
    let previousName = null;
    table.checks.forEach((check, index) => {
        const checkPath = `${path}.checks[${index}]`;
        if (!isPlainObject(check)) {
            diag(diagnostics, "IR036", checkPath, "check constraint must be an object");
            return;
        }
        checkUnknownFields(diagnostics, check, ["name", "sql"], checkPath);
        if (!isIdentifier(check.name)) {
            diag(diagnostics, "IR036", `${checkPath}.name`, "check name must be an identifier");
        } else {
            checkSqlIdentifier(diagnostics, check.name, `${checkPath}.name`, "check name");
            const folded = check.name.toLowerCase();
            if (names.has(folded)) diag(diagnostics, "IR036", `${checkPath}.name`, `duplicate check name "${check.name}"`);
            names.add(folded);
            if (previousName !== null && previousName > check.name) diag(diagnostics, "IR033", `${checkPath}.name`, "checks must be sorted by name");
            previousName = check.name;
        }
        checkStaticSqlExpression(check.sql, `${checkPath}.sql`, diagnostics, "check expression");
    });
}

function checkIndexes(table, columnNames, path, diagnostics) {
    if (!("indexes" in table)) return;
    if (!Array.isArray(table.indexes)) {
        diag(diagnostics, "IR037", `${path}.indexes`, "indexes must be an array");
        return;
    }
    const names = new Set();
    let previousName = null;
    table.indexes.forEach((index, indexNumber) => {
        const indexPath = `${path}.indexes[${indexNumber}]`;
        if (!isPlainObject(index)) {
            diag(diagnostics, "IR037", indexPath, "index must be an object");
            return;
        }
        checkUnknownFields(diagnostics, index, ["name", "unique", "terms", "where"], indexPath);
        if (!isIdentifier(index.name)) {
            diag(diagnostics, "IR037", `${indexPath}.name`, "index name must be an identifier");
        } else {
            checkSqlIdentifier(diagnostics, index.name, `${indexPath}.name`, "index name");
            if (isReservedWord(index.name)) diag(diagnostics, "IR028", `${indexPath}.name`, `index name "${index.name}" is a reserved SQL word`);
            const folded = index.name.toLowerCase();
            if (names.has(folded)) diag(diagnostics, "IR037", `${indexPath}.name`, `duplicate index name "${index.name}"`);
            names.add(folded);
            if (previousName !== null && previousName > index.name) diag(diagnostics, "IR033", `${indexPath}.name`, "indexes must be sorted by name");
            previousName = index.name;
        }
        if (typeof index.unique !== "boolean") diag(diagnostics, "IR037", `${indexPath}.unique`, "index unique must be boolean");
        if (!Array.isArray(index.terms) || index.terms.length === 0) {
            diag(diagnostics, "IR037", `${indexPath}.terms`, "index terms must be a non-empty array");
        } else {
            index.terms.forEach((term, termIndex) => {
                const termPath = `${indexPath}.terms[${termIndex}]`;
                if (!isPlainObject(term)) {
                    diag(diagnostics, "IR037", termPath, "index term must be an object");
                    return;
                }
                if (term.kind === "column") {
                    checkUnknownFields(diagnostics, term, ["kind", "column", "direction"], termPath);
                    if (!isIdentifier(term.column) || !columnNames.has(term.column)) diag(diagnostics, "IR037", `${termPath}.column`, `unknown index column "${String(term.column)}"`);
                    if (term.direction !== "asc" && term.direction !== "desc") diag(diagnostics, "IR037", `${termPath}.direction`, "index column direction must be asc or desc");
                } else if (term.kind === "expression") {
                    checkUnknownFields(diagnostics, term, ["kind", "sql"], termPath);
                    checkStaticSqlExpression(term.sql, `${termPath}.sql`, diagnostics, "index expression");
                } else {
                    diag(diagnostics, "IR037", `${termPath}.kind`, "index term kind must be column or expression");
                }
            });
        }
        if ("where" in index) checkStaticSqlExpression(index.where, `${indexPath}.where`, diagnostics, "partial-index predicate");
    });
}
function checkTable(table, index, diagnostics) {
    const path = `$.tables[${index}]`;
    if (!isPlainObject(table)) {
        diag(diagnostics, "IR007", path, "table must be an object");
        return;
    }
    checkUnknownFields(diagnostics, table, ["name", "sql_name", "columns", "primary_key", "uniques", "defaults", "foreign_keys", "checks", "indexes"], path);
    if (!isIdentifier(table.name)) {
        diag(diagnostics, "IR007", `${path}.name`, "table name must match [A-Za-z_][A-Za-z0-9_]{0,63}");
    } else {
        checkLogicalIdentifier(diagnostics, table.name, `${path}.name`, "table name");
    }
    if (!isIdentifier(table.sql_name)) {
        diag(diagnostics, "IR007", `${path}.sql_name`, "table sql_name must match [A-Za-z_][A-Za-z0-9_]{0,63}");
    } else if (isReservedWord(table.sql_name)) {
        diag(diagnostics, "IR028", `${path}.sql_name`, `table sql_name "${table.sql_name}" is a reserved SQL word`);
    }
    checkSqlIdentifier(diagnostics, table.sql_name, `${path}.sql_name`, "table sql_name");
    if (!Array.isArray(table.columns) || table.columns.length === 0) {
        diag(diagnostics, "IR009", `${path}.columns`, "table must declare a non-empty columns array");
        return;
    }

    const columnNames = new Set();
    const sqlNames = new Set();
    const generatedColumnNames = generatedFormMaps();
    const indexed = { ...table, index };
    for (let i = 0; i < table.columns.length; i += 1) {
        const column = table.columns[i];
        checkColumn(column, indexed, i, diagnostics);
        if (isPlainObject(column) && typeof column.name === "string") {
            if (columnNames.has(column.name)) {
                diag(diagnostics, "IR010", `${path}.columns[${i}].name`, `duplicate column name "${column.name}"`);
            }
            columnNames.add(column.name);
            recordGeneratedForms(
                generatedColumnNames,
                column.name,
                `${path}.columns[${i}].name`,
                diagnostics,
                "column name"
            );
        }
        if (isPlainObject(column) && typeof column.sql_name === "string") {
            const foldedSqlName = column.sql_name.toLowerCase();
            if (sqlNames.has(foldedSqlName)) {
                diag(diagnostics, "IR010", `${path}.columns[${i}].sql_name`, `duplicate column sql_name "${column.sql_name}" (SQLite identifiers are case-insensitive)`);
            }
            sqlNames.add(foldedSqlName);
        }
    }

    const primaryKey = table.primary_key;
    if (!Array.isArray(primaryKey) || primaryKey.length === 0) {
        diag(diagnostics, "IR014", `${path}.primary_key`, "primary_key must be a non-empty array of column names");
    } else {
        const pkSet = new Set();
        for (let i = 0; i < primaryKey.length; i += 1) {
            const name = primaryKey[i];
            if (!isIdentifier(name)) {
                diag(diagnostics, "IR014", `${path}.primary_key[${i}]`, "primary_key entries must be column names");
                continue;
            }
            if (!columnNames.has(name)) {
                diag(diagnostics, "IR014", `${path}.primary_key[${i}]`, `unknown primary key column "${name}"`);
            }
            if (pkSet.has(name)) {
                diag(diagnostics, "IR014", `${path}.primary_key[${i}]`, `duplicate primary key column "${name}"`);
            }
            pkSet.add(name);
        }
        const flagged = table.columns
            .filter((c) => isPlainObject(c) && c.primary_key === true)
            .map((c) => c.name);
        if (flagged.length !== primaryKey.length || flagged.some((n) => !pkSet.has(n))) {
            diag(
                diagnostics,
                "IR014",
                `${path}.primary_key`,
                `primary_key [${primaryKey.join(", ")}] does not match column primary_key flags [${flagged.join(", ")}]`
            );
        }
    }

    const uniques = table.uniques;
    if (!Array.isArray(uniques)) {
        diag(diagnostics, "IR015", `${path}.uniques`, "uniques must be an array of column-name arrays");
    } else {
        const uniqueKeys = uniques.map((key) => (Array.isArray(key) ? key.slice() : null));
        for (let i = 0; i < uniques.length; i += 1) {
            const key = uniques[i];
            if (!Array.isArray(key) || key.length === 0) {
                diag(diagnostics, "IR015", `${path}.uniques[${i}]`, "unique key must be a non-empty array");
                continue;
            }
            for (let j = 0; j < key.length; j += 1) {
                if (!isIdentifier(key[j]) || !columnNames.has(key[j])) {
                    diag(diagnostics, "IR015", `${path}.uniques[${i}][${j}]`, `unknown unique column "${String(key[j])}"`);
                }
            }
        }
        const flagged = table.columns
            .filter((c) => isPlainObject(c) && c.unique === true)
            .map((c) => c.name);
        for (const name of flagged) {
            const covered = uniqueKeys.some((key) => key !== null && key.length === 1 && key[0] === name);
            if (!covered) {
                diag(
                    diagnostics,
                    "IR015",
                    `${path}.uniques`,
                    `column "${name}" is flagged unique but has no matching single-column unique key`
                );
            }
        }
        /* `column.unique` represents a column-level UNIQUE declaration. A
         * table-level unique constraint or unique index may legitimately create
         * a single-column unique key without setting that column metadata flag. */
    }

    if ("defaults" in table) {
        if (!isPlainObject(table.defaults)) {
            diag(diagnostics, "IR016", `${path}.defaults`, "defaults must be an object keyed by column name");
        } else {
            for (const key of Object.keys(table.defaults)) {
                if (!columnNames.has(key)) {
                    diag(diagnostics, "IR016", `${path}.defaults.${key}`, `unknown column "${key}" in defaults`);
                    continue;
                }
                const column = table.columns.find((c) => isPlainObject(c) && c.name === key);
                if (column && "default" in column) {
                    diag(
                        diagnostics,
                        "IR016",
                        `${path}.defaults.${key}`,
                        `column "${key}" declares a default in both the column and the defaults map`
                    );
                    continue;
                }
                if (column) {
                    checkLiteral(table.defaults[key], column.type, column.nullable === true, `${path}.defaults.${key}`, diagnostics);
                }
            }
        }
    }

    checkForeignKeys(table, columnNames, path, diagnostics);
    checkChecks(table, path, diagnostics);
    checkIndexes(table, columnNames, path, diagnostics);
}

function tableViewFor(table) {
    return {
        table,
        columnNames: new Set(table.columns.filter(isPlainObject).map((column) => column.name)),
        primaryKey: Array.isArray(table.primary_key) ? table.primary_key : [],
        uniques: Array.isArray(table.uniques) ? table.uniques.filter(Array.isArray) : []
    };
}

function scopedTableView(scope, baseName, requestedName) {
    return scope.get(requestedName === undefined ? baseName : requestedName) || null;
}

function checkWhere(scope, baseName, where, path, diagnostics, required) {
    if (where === undefined) {
        if (required) diag(diagnostics, "IR018", path, "where is required for this query");
        return;
    }
    if (!Array.isArray(where) || where.length === 0) {
        diag(diagnostics, "IR018", path, "where must be a non-empty array of conditions");
        return;
    }
    for (let i = 0; i < where.length; i += 1) {
        const condition = where[i];
        const conditionPath = `${path}[${i}]`;
        if (!isPlainObject(condition)) {
            diag(diagnostics, "IR018", conditionPath, "condition must be an object");
            continue;
        }
        checkUnknownFields(diagnostics, condition, ["table", "column", "op", "param"], conditionPath);
        if (condition.op !== "eq") {
            diag(diagnostics, "IR018", `${conditionPath}.op`, `unsupported operator "${String(condition.op)}" (v1 supports eq)`);
        }
        if ("table" in condition && !isIdentifier(condition.table)) {
            diag(diagnostics, "IR018", `${conditionPath}.table`, "condition table must be a logical table identifier");
        }
        const target = scopedTableView(scope, baseName, condition.table);
        if (target === null) {
            diag(diagnostics, "IR018", `${conditionPath}.table`, `table "${String(condition.table)}" is not in this query scope`);
        } else if (!isIdentifier(condition.column) || !target.columnNames.has(condition.column)) {
            diag(diagnostics, "IR018", `${conditionPath}.column`, `unknown column "${String(condition.column)}" on table "${target.table.name}"`);
        }
        if (!isIdentifier(condition.param)) {
            diag(diagnostics, "IR019", `${conditionPath}.param`, "param must be an identifier");
        }
    }
}

function checkAssignments(table, list, path, diagnostics, label) {
    if (!Array.isArray(list) || list.length === 0) {
        diag(diagnostics, "IR018", path, `${label} must be a non-empty array`);
        return;
    }
    const seen = new Set();
    for (let i = 0; i < list.length; i += 1) {
        const entry = list[i];
        const entryPath = `${path}[${i}]`;
        if (!isPlainObject(entry)) {
            diag(diagnostics, "IR018", entryPath, `${label} entry must be an object`);
            continue;
        }
        checkUnknownFields(diagnostics, entry, ["column", "param"], entryPath);
        if (!isIdentifier(entry.column) || !table.columnNames.has(entry.column)) {
            diag(diagnostics, "IR018", `${entryPath}.column`, `unknown column "${String(entry.column)}"`);
            continue;
        }
        if (seen.has(entry.column)) {
            diag(diagnostics, "IR019", `${entryPath}.column`, `column "${entry.column}" appears more than once`);
        }
        seen.add(entry.column);
        if (!isIdentifier(entry.param)) {
            diag(diagnostics, "IR019", `${entryPath}.param`, "param must be an identifier");
        }
    }
}

function checkJoinRef(ref, scope, path, diagnostics) {
    if (!isPlainObject(ref)) {
        diag(diagnostics, "IR039", path, "join column reference must be an object");
        return null;
    }
    checkUnknownFields(diagnostics, ref, ["table", "column"], path);
    if (!isIdentifier(ref.table)) {
        diag(diagnostics, "IR039", `${path}.table`, "join reference table must be an identifier");
        return null;
    }
    const tableView = scope.get(ref.table);
    if (!tableView) {
        diag(diagnostics, "IR039", `${path}.table`, `table "${ref.table}" is not available at this join step`);
        return null;
    }
    if (!isIdentifier(ref.column) || !tableView.columnNames.has(ref.column)) {
        diag(diagnostics, "IR039", `${path}.column`, `unknown column "${String(ref.column)}" on table "${ref.table}"`);
        return null;
    }
    return { tableView, column: columnByName(tableView.table, ref.column) };
}

function checkJoins(query, ir, baseTable, path, diagnostics) {
    const baseView = tableViewFor(baseTable);
    const scope = new Map([[baseTable.name, baseView]]);
    if (!("joins" in query)) return scope;
    if (!Array.isArray(query.joins) || query.joins.length === 0) {
        diag(diagnostics, "IR039", `${path}.joins`, "joins must be a non-empty array when present");
        return scope;
    }
    for (let joinIndex = 0; joinIndex < query.joins.length; joinIndex += 1) {
        const join = query.joins[joinIndex];
        const joinPath = `${path}.joins[${joinIndex}]`;
        if (!isPlainObject(join)) {
            diag(diagnostics, "IR039", joinPath, "join must be an object");
            continue;
        }
        checkUnknownFields(diagnostics, join, ["kind", "table", "on"], joinPath);
        if (join.kind !== "inner" && join.kind !== "left") {
            diag(diagnostics, "IR039", `${joinPath}.kind`, "join kind must be inner or left");
        }
        if (!isIdentifier(join.table)) {
            diag(diagnostics, "IR039", `${joinPath}.table`, "join table must be a logical table identifier");
            continue;
        }
        const targetTable = tableByName(ir, join.table);
        if (!targetTable) {
            diag(diagnostics, "IR039", `${joinPath}.table`, `unknown join table "${join.table}"`);
            continue;
        }
        if (scope.has(join.table)) {
            diag(diagnostics, "IR039", `${joinPath}.table`, `table "${join.table}" is already in query scope`);
            continue;
        }
        const priorScope = new Map(scope);
        scope.set(join.table, tableViewFor(targetTable));
        if (!Array.isArray(join.on) || join.on.length === 0) {
            diag(diagnostics, "IR039", `${joinPath}.on`, "join on must be a non-empty array");
            continue;
        }
        for (let onIndex = 0; onIndex < join.on.length; onIndex += 1) {
            const condition = join.on[onIndex];
            const onPath = `${joinPath}.on[${onIndex}]`;
            if (!isPlainObject(condition)) {
                diag(diagnostics, "IR039", onPath, "join condition must be an object");
                continue;
            }
            checkUnknownFields(diagnostics, condition, ["left", "right"], onPath);
            const left = checkJoinRef(condition.left, scope, `${onPath}.left`, diagnostics);
            const right = checkJoinRef(condition.right, scope, `${onPath}.right`, diagnostics);
            if (left && right) {
                const leftIsTarget = condition.left.table === join.table;
                const rightIsTarget = condition.right.table === join.table;
                if (leftIsTarget === rightIsTarget) {
                    diag(diagnostics, "IR039", onPath, "each join predicate must connect the newly joined table to a previously available table");
                } else {
                    const priorRef = leftIsTarget ? condition.right : condition.left;
                    if (!priorScope.has(priorRef.table)) {
                        diag(diagnostics, "IR039", onPath, "join predicate must connect to a table available before this join");
                    }
                }
                if (left.column && right.column && left.column.type !== right.column.type) {
                    diag(diagnostics, "IR039", onPath, `join column types must match (${left.column.type} vs ${right.column.type})`);
                }
            }
        }
    }
    return scope;
}

function checkConflict(tableView, query, path, diagnostics) {
    const policy = query.on_conflict;
    if (!isPlainObject(policy)) {
        diag(diagnostics, "IR040", `${path}.on_conflict`, "upsert requires an on_conflict object");
        return;
    }
    checkUnknownFields(diagnostics, policy, ["target", "action", "set"], `${path}.on_conflict`);
    if (!Array.isArray(policy.target) || policy.target.length === 0) {
        diag(diagnostics, "IR040", `${path}.on_conflict.target`, "conflict target must be a non-empty array");
    } else {
        const seen = new Set();
        policy.target.forEach((name, index) => {
            if (!isIdentifier(name) || !tableView.columnNames.has(name)) diag(diagnostics, "IR040", `${path}.on_conflict.target[${index}]`, `unknown conflict-target column "${String(name)}"`);
            if (seen.has(name)) diag(diagnostics, "IR040", `${path}.on_conflict.target[${index}]`, `duplicate conflict-target column "${String(name)}"`);
            seen.add(name);
        });
        if (![tableView.primaryKey, ...tableView.uniques].some((key) => sameColumnKey(key, policy.target))) {
            diag(diagnostics, "IR040", `${path}.on_conflict.target`, "conflict target must exactly match the primary key or a declared unique key");
        }
    }
    if (policy.action !== "nothing" && policy.action !== "update") {
        diag(diagnostics, "IR040", `${path}.on_conflict.action`, "conflict action must be nothing or update");
    } else if (policy.action === "nothing") {
        if ("set" in policy) diag(diagnostics, "IR040", `${path}.on_conflict.set`, "DO NOTHING may not declare set assignments");
    } else {
        checkAssignments(tableView, policy.set, `${path}.on_conflict.set`, diagnostics, "conflict set");
    }
}


function checkQuery(query, index, ir, diagnostics) {
    const path = `$.queries[${index}]`;
    if (!isPlainObject(query)) {
        diag(diagnostics, "IR017", path, "query must be an object");
        return;
    }
    checkUnknownFields(
        diagnostics,
        query,
        ["name", "kind", "table", "cardinality", "where", "joins", "order_by", "limit", "offset", "values", "set", "on_conflict"],
        path
    );
    if (typeof query.name !== "string" || !QUERY_NAME_RE.test(query.name)) {
        diag(diagnostics, "IR017", `${path}.name`, "query name must match identifier(.identifier)?");
    }
    if (!QUERY_KINDS.includes(query.kind)) {
        diag(diagnostics, "IR029", `${path}.kind`, `query kind must be one of ${QUERY_KINDS.join(", ")}`);
        return;
    }
    if (!CARDINALITIES.includes(query.cardinality)) {
        diag(diagnostics, "IR029", `${path}.cardinality`, `cardinality must be one of ${CARDINALITIES.join(", ")}`);
    }
    const expectedCardinality = query.kind === "select" ? ["one", "many"] : ["changes"];
    if (CARDINALITIES.includes(query.cardinality) && !expectedCardinality.includes(query.cardinality)) {
        diag(
            diagnostics,
            "IR029",
            `${path}.cardinality`,
            `${query.kind} queries require cardinality ${expectedCardinality.join(" or ")}`
        );
    }

    const table = ir.tables.find((t) => isPlainObject(t) && t.name === query.table);
    if (!table) {
        diag(diagnostics, "IR018", `${path}.table`, `unknown table "${String(query.table)}"`);
        return;
    }
    const tableView = tableViewFor(table);

    if (query.kind === "select") {
        if ("values" in query || "set" in query || "on_conflict" in query) {
            diag(diagnostics, "IR029", path, "select queries may not declare values/set/on_conflict");
        }
        const scope = checkJoins(query, ir, table, path, diagnostics);
        checkWhere(scope, table.name, query.where, `${path}.where`, diagnostics, query.cardinality === "one");
        if ("order_by" in query) {
            if (!Array.isArray(query.order_by)) {
                diag(diagnostics, "IR018", `${path}.order_by`, "order_by must be an array");
            } else {
                for (let i = 0; i < query.order_by.length; i += 1) {
                    const term = query.order_by[i];
                    const termPath = `${path}.order_by[${i}]`;
                    if (!isPlainObject(term)) {
                        diag(diagnostics, "IR018", termPath, "order term must be an object");
                        continue;
                    }
                    checkUnknownFields(diagnostics, term, ["table", "column", "direction"], termPath);
                    const orderView = scopedTableView(scope, table.name, term.table);
                    if (orderView === null) {
                        diag(diagnostics, "IR018", `${termPath}.table`, `table "${String(term.table)}" is not in this query scope`);
                    } else if (!isIdentifier(term.column) || !orderView.columnNames.has(term.column)) {
                        diag(diagnostics, "IR018", `${termPath}.column`, `unknown column "${String(term.column)}" on table "${orderView.table.name}"`);
                    }
                    if (term.direction !== "asc" && term.direction !== "desc") {
                        diag(diagnostics, "IR018", `${termPath}.direction`, 'direction must be "asc" or "desc"');
                    }
                }
            }
        }
        for (const key of ["limit", "offset"]) {
            if (key in query && !isIdentifier(query[key])) {
                diag(diagnostics, "IR026", `${path}.${key}`, `${key} must be a parameter identifier`);
            }
        }
        if (query.cardinality === "one") {
            const whereColumns = Array.isArray(query.where)
                ? query.where
                    .filter((entry) => isPlainObject(entry) && (entry.table === undefined || entry.table === table.name))
                    .map((entry) => entry.column)
                : [];
            if (!isKeyedWhere(tableView, whereColumns)) {
                diag(
                    diagnostics,
                    "IR021",
                    `${path}.where`,
                    "cardinality one requires the where clause to cover a primary key or declared unique key"
                );
            }
        }
    } else if (query.kind === "insert" || query.kind === "upsert") {
        if ("where" in query || "set" in query || "joins" in query || "order_by" in query || "limit" in query || "offset" in query) {
            diag(diagnostics, "IR029", path, "insert/upsert queries may not declare where/set/joins/order_by/limit/offset");
        }
        if (query.kind === "upsert") {
            checkConflict(tableView, query, path, diagnostics);
        } else if ("on_conflict" in query) {
            diag(diagnostics, "IR029", `${path}.on_conflict`, "insert queries may not declare on_conflict");
        }
        checkAssignments(tableView, query.values, `${path}.values`, diagnostics, "values");
        const provided = new Set(Array.isArray(query.values) ? query.values.filter(isPlainObject).map((v) => v.column) : []);
        for (const column of table.columns.filter(isPlainObject)) {
            const hasDefault = effectiveDefault(table, column) !== null;
            if (column.nullable !== true && !hasDefault && !provided.has(column.name)) {
                diag(
                    diagnostics,
                    "IR019",
                    `${path}.values`,
                    `required column "${column.name}" (not null, no default) is missing from insert values`
                );
            }
        }
    } else if (query.kind === "update") {
        if ("values" in query || "joins" in query || "on_conflict" in query || "order_by" in query || "limit" in query || "offset" in query) {
            diag(diagnostics, "IR029", path, "update queries may not declare values/joins/on_conflict/order_by/limit/offset");
        }
        checkAssignments(tableView, query.set, `${path}.set`, diagnostics, "set");
        checkWhere(new Map([[table.name, tableViewFor(table)]]), table.name, query.where, `${path}.where`, diagnostics, true);
        checkKeyedMutation(tableView, query, path, diagnostics);
    } else if (query.kind === "delete") {
        if ("values" in query || "set" in query || "joins" in query || "on_conflict" in query || "order_by" in query || "limit" in query || "offset" in query) {
            diag(diagnostics, "IR029", path, "delete queries may not declare values/set/joins/on_conflict/order_by/limit/offset");
        }
        checkWhere(new Map([[table.name, tableViewFor(table)]]), table.name, query.where, `${path}.where`, diagnostics, true);
        checkKeyedMutation(tableView, query, path, diagnostics);
    }

    if (typeof query.name === "string" && QUERY_NAME_RE.test(query.name)) {
        const methodName = queryMethodName(query.name);
        if (
            methodName.length === 0 ||
            ES3_RESERVED_WORDS.has(methodName) ||
            CXX_RESERVED_WORDS.has(methodName) ||
            GENERATED_LIFECYCLE_NAMES.has(methodName)
        ) {
            diag(
                diagnostics,
                "IR031",
                `${path}.name`,
                `query method name "${methodName}" collides with a reserved/generated binding identifier`
            );
        }
    }

    const params = deriveParams(table, query, ir);
    const seenParams = new Set();
    for (const param of params) {
        if (seenParams.has(param.name)) {
            diag(diagnostics, "IR019", `${path}`, `duplicate parameter "${param.name}"`);
        }
        seenParams.add(param.name);
        checkLogicalIdentifier(diagnostics, param.name, path, "parameter name");
        const paramCamel = toCamel(param.name);
        const paramSnake = toSnake(param.name);
        if (
            ES3_RESERVED_WORDS.has(paramCamel) ||
            CXX_RESERVED_WORDS.has(paramSnake) ||
            GENERATED_LIFECYCLE_NAMES.has(paramCamel) ||
            GENERATED_LIFECYCLE_NAMES.has(paramSnake)
        ) {
            diag(diagnostics, "IR031", `${path}`, `parameter name "${param.name}" collides with a reserved/generated binding identifier`);
        }
    }
}

function checkKeyedMutation(tableView, query, path, diagnostics) {
    const whereColumns = Array.isArray(query.where)
        ? query.where.filter(isPlainObject).map((w) => w.column)
        : [];
    if (!isKeyedWhere(tableView, whereColumns)) {
        diag(
            diagnostics,
            "IR020",
            `${path}.where`,
            "update/delete requires a where clause covering a primary key or declared unique key"
        );
    }
}

export function isKeyedWhere(tableView, whereColumns) {
    const columns = new Set(whereColumns);
    const keys = [tableView.primaryKey, ...tableView.uniques];
    return keys.some((key) => key.length > 0 && key.every((column) => columns.has(column)));
}

export function effectiveDefault(table, column) {
    if (isPlainObject(column) && Object.prototype.hasOwnProperty.call(column, "default")) {
        return column.default;
    }
    if (isPlainObject(table.defaults) && Object.prototype.hasOwnProperty.call(table.defaults, column.name)) {
        return table.defaults[column.name];
    }
    return null;
}

export function columnByName(table, name) {
    return table.columns.find((column) => isPlainObject(column) && column.name === name) || null;
}

export function tableByName(ir, name) {
    return ir.tables.find((table) => isPlainObject(table) && table.name === name) || null;
}

function deriveParamList(table, query, updateBindOrder, ir = null) {
    const params = [];
    const push = (paramName, columnName, logicalTableName = table.name) => {
        if (typeof paramName !== "string") return;
        if (columnName === null) {
            params.push({ name: paramName, table: null, column: null, type: "integer" });
            return;
        }
        const owner = logicalTableName === table.name
            ? table
            : (ir === null ? null : tableByName(ir, logicalTableName));
        const column = owner === null ? null : columnByName(owner, columnName);
        params.push({
            name: paramName,
            table: owner === null ? logicalTableName : owner.name,
            column: columnName,
            type: column ? column.type : "text"
        });
    };
    if (query.kind === "select") {
        for (const condition of query.where || []) {
            push(condition.param, condition.column, condition.table === undefined ? table.name : condition.table);
        }
        if (typeof query.limit === "string") push(query.limit, null);
        if (typeof query.offset === "string") push(query.offset, null);
    } else if (query.kind === "insert" || query.kind === "upsert") {
        for (const value of query.values || []) push(value.param, value.column);
        if (query.kind === "upsert" && query.on_conflict && query.on_conflict.action === "update") {
            for (const assignment of query.on_conflict.set || []) push(assignment.param, assignment.column);
        }
    } else if (query.kind === "update") {
        if (updateBindOrder) {
            for (const assignment of query.set || []) push(assignment.param, assignment.column);
            for (const condition of query.where || []) push(condition.param, condition.column);
        } else {
            for (const condition of query.where || []) push(condition.param, condition.column);
            for (const assignment of query.set || []) push(assignment.param, assignment.column);
        }
    } else if (query.kind === "delete") {
        for (const condition of query.where || []) push(condition.param, condition.column);
    }
    return params;
}

/* Public API parameter order. UPDATE remains ergonomic/key-first:
 * WHERE parameters first, then SET parameters. */
export function deriveApiParams(table, query, ir = null) {
    return deriveParamList(table, query, false, ir);
}

/* SQLite placeholder/bind order. UPDATE text is emitted as
 * SET ... WHERE ..., so SET parameters must be bound before WHERE parameters. */
export function deriveBindParams(table, query, ir = null) {
    return deriveParamList(table, query, true, ir);
}

/* Backward-compatible alias for the historical public/API order. */
export function deriveParams(table, query, ir = null) {
    return deriveApiParams(table, query, ir);
}

function checkIntegrity(integrity, diagnostics) {
    const path = "$.integrity";
    if (!isPlainObject(integrity)) {
        diag(diagnostics, "IR025", path, "integrity must be an object");
        return;
    }
    checkUnknownFields(diagnostics, integrity, ["algorithm", "canonical", "hash"], path);
    if (integrity.algorithm !== "sha256") {
        diag(diagnostics, "IR025", `${path}.algorithm`, 'integrity.algorithm must be "sha256"');
    }
    if (integrity.canonical !== CANONICAL_ID) {
        diag(diagnostics, "IR025", `${path}.canonical`, `integrity.canonical must be "${CANONICAL_ID}"`);
    }
    if (typeof integrity.hash !== "string" || !HASH_RE.test(integrity.hash)) {
        diag(diagnostics, "IR025", `${path}.hash`, "integrity.hash must be 64 lowercase hex characters");
    }
}

function checkGenerator(generator, diagnostics) {
    const path = "$.generator";
    if (!isPlainObject(generator)) {
        diag(diagnostics, "IR030", path, "generator must be an object");
        return;
    }
    checkUnknownFields(diagnostics, generator, ["name", "version", "source"], path);
    for (const key of Object.keys(generator)) {
        if (typeof generator[key] !== "string") {
            diag(diagnostics, "IR030", `${path}.${key}`, `generator.${key} must be a string`);
            continue;
        }
        if (/[\0\r\n\u2028\u2029]/.test(generator[key]) || generator[key].indexOf("*/") !== -1) {
            diag(diagnostics, "IR030", `${path}.${key}`, `generator.${key} must be safe single-line metadata`);
        }
    }
}

function sameColumnKey(left, right) {
    return Array.isArray(left) && Array.isArray(right) && left.length === right.length && left.every((value, index) => value === right[index]);
}

function checkRelationalSchema(doc, diagnostics) {
    if (!Array.isArray(doc.tables)) return;
    const tablesByName = new Map();
    const schemaObjectNames = new Map();
    for (let tableIndex = 0; tableIndex < doc.tables.length; tableIndex += 1) {
        const table = doc.tables[tableIndex];
        if (!isPlainObject(table)) continue;
        if (typeof table.name === "string") tablesByName.set(table.name, { table, tableIndex });
        if (typeof table.sql_name === "string") schemaObjectNames.set(table.sql_name.toLowerCase(), `table:${table.name}`);
    }

    for (let tableIndex = 0; tableIndex < doc.tables.length; tableIndex += 1) {
        const table = doc.tables[tableIndex];
        if (!isPlainObject(table) || !Array.isArray(table.columns)) continue;
        const path = `$.tables[${tableIndex}]`;
        if (Array.isArray(table.indexes)) {
            for (let indexNumber = 0; indexNumber < table.indexes.length; indexNumber += 1) {
                const index = table.indexes[indexNumber];
                if (!isPlainObject(index) || typeof index.name !== "string") continue;
                const folded = index.name.toLowerCase();
                const prior = schemaObjectNames.get(folded);
                if (prior !== undefined) {
                    diag(diagnostics, "IR037", `${path}.indexes[${indexNumber}].name`, `SQLite schema object name "${index.name}" collides with ${prior}`);
                } else {
                    schemaObjectNames.set(folded, `index:${index.name}`);
                }
                if (index.unique === true && !("where" in index) && Array.isArray(index.terms) && index.terms.every((term) => isPlainObject(term) && term.kind === "column")) {
                    const key = index.terms.map((term) => term.column);
                    const covered = Array.isArray(table.uniques) && table.uniques.some((candidate) => sameColumnKey(candidate, key));
                    if (!covered) {
                        diag(diagnostics, "IR037", `${path}.indexes[${indexNumber}]`, `plain unique index [${key.join(", ")}] must also appear in table.uniques`);
                    }
                }
            }
        }

        if (!Array.isArray(table.foreign_keys)) continue;
        for (let fkIndex = 0; fkIndex < table.foreign_keys.length; fkIndex += 1) {
            const foreignKey = table.foreign_keys[fkIndex];
            if (!isPlainObject(foreignKey) || !isPlainObject(foreignKey.references)) continue;
            const fkPath = `${path}.foreign_keys[${fkIndex}]`;
            const targetInfo = tablesByName.get(foreignKey.references.table);
            if (!targetInfo) {
                diag(diagnostics, "IR038", `${fkPath}.references.table`, `unknown referenced table "${String(foreignKey.references.table)}"`);
                continue;
            }
            const target = targetInfo.table;
            const localNames = Array.isArray(foreignKey.columns) ? foreignKey.columns : [];
            const targetNames = Array.isArray(foreignKey.references.columns) ? foreignKey.references.columns : [];
            const targetColumns = new Set(Array.isArray(target.columns) ? target.columns.filter(isPlainObject).map((column) => column.name) : []);
            targetNames.forEach((name, columnIndex) => {
                if (!targetColumns.has(name)) {
                    diag(diagnostics, "IR038", `${fkPath}.references.columns[${columnIndex}]`, `unknown referenced column "${String(name)}"`);
                }
            });
            const targetKeys = [
                Array.isArray(target.primary_key) ? target.primary_key : [],
                ...(Array.isArray(target.uniques) ? target.uniques.filter(Array.isArray) : [])
            ];
            if (targetNames.length > 0 && !targetKeys.some((key) => sameColumnKey(key, targetNames))) {
                diag(diagnostics, "IR038", `${fkPath}.references.columns`, "referenced columns must exactly match the target primary key or a declared unique key");
            }
            if (localNames.length === targetNames.length) {
                for (let columnIndex = 0; columnIndex < localNames.length; columnIndex += 1) {
                    const localColumn = columnByName(table, localNames[columnIndex]);
                    const targetColumn = columnByName(target, targetNames[columnIndex]);
                    if (localColumn && targetColumn && localColumn.type !== targetColumn.type) {
                        diag(diagnostics, "IR038", `${fkPath}.columns[${columnIndex}]`, `foreign-key type ${localColumn.type} does not match referenced ${targetColumn.type}`);
                    }
                    if (localColumn && (foreignKey.on_update === "set null" || foreignKey.on_delete === "set null") && localColumn.nullable !== true) {
                        diag(diagnostics, "IR038", `${fkPath}.columns[${columnIndex}]`, `SET NULL requires nullable child column "${localColumn.name}"`);
                    }
                    if (localColumn && (foreignKey.on_update === "set default" || foreignKey.on_delete === "set default") && effectiveDefault(table, localColumn) === null) {
                        diag(diagnostics, "IR038", `${fkPath}.columns[${columnIndex}]`, `SET DEFAULT requires a default on child column "${localColumn.name}"`);
                    }
                }
            }
        }
    }
}
export function validateDocument(doc) {
    const diagnostics = [];
    if (!isPlainObject(doc)) {
        return { ok: false, diagnostics: [{ code: "IR001", path: "$", message: "IR document must be a JSON object" }] };
    }

    checkScalarWalk(doc, "$", diagnostics);

    checkUnknownFields(
        diagnostics,
        doc,
        ["ir_version", "schema", "tables", "queries", "integrity", "generator", "annotations"],
        "$"
    );
    if (doc.ir_version !== IR_VERSION) {
        diag(diagnostics, "IR002", "$.ir_version", `ir_version must be "${IR_VERSION}"`);
    }
    if (!isPlainObject(doc.schema)) {
        diag(diagnostics, "IR003", "$.schema", "schema must be an object");
    } else {
        checkUnknownFields(diagnostics, doc.schema, ["name", "dialect", "namespace"], "$.schema");
        if (!isIdentifier(doc.schema.name)) {
            diag(diagnostics, "IR004", "$.schema.name", "schema name must match [A-Za-z_][A-Za-z0-9_]{0,63}");
        } else {
            checkLogicalIdentifier(diagnostics, doc.schema.name, "$.schema.name", "schema name");
        }
        if (doc.schema.dialect !== "sqlite") {
            diag(diagnostics, "IR005", "$.schema.dialect", 'schema.dialect must be "sqlite"');
        }
        if ("namespace" in doc.schema && (typeof doc.schema.namespace !== "string" || doc.schema.namespace.length === 0)) {
            diag(diagnostics, "IR003", "$.schema.namespace", "schema.namespace must be a non-empty string");
        }
    }
    if (!Array.isArray(doc.tables) || doc.tables.length === 0) {
        diag(diagnostics, "IR006", "$.tables", "tables must be a non-empty array");
    } else {
        const names = new Set();
        const sqlNames = new Set();
        const generatedTableNames = generatedFormMaps();
        let previousTableName = null;
        for (let i = 0; i < doc.tables.length; i += 1) {
            checkTable(doc.tables[i], i, diagnostics);
            const table = doc.tables[i];
            if (isPlainObject(table) && typeof table.name === "string") {
                if (names.has(table.name)) {
                    diag(diagnostics, "IR008", `$.tables[${i}].name`, `duplicate table name "${table.name}"`);
                }
                names.add(table.name);
                if (previousTableName !== null && previousTableName > table.name) {
                    diag(
                        diagnostics,
                        "IR033",
                        `$.tables[${i}].name`,
                        "tables must be sorted by logical name for deterministic IR v1"
                    );
                }
                previousTableName = table.name;
                recordGeneratedForms(generatedTableNames, table.name, `$.tables[${i}].name`, diagnostics, "table name");
            }
            if (isPlainObject(table) && typeof table.sql_name === "string") {
                const foldedSqlName = table.sql_name.toLowerCase();
                if (sqlNames.has(foldedSqlName)) {
                    diag(diagnostics, "IR008", `$.tables[${i}].sql_name`, `duplicate table sql_name "${table.sql_name}" (SQLite identifiers are case-insensitive)`);
                }
                sqlNames.add(foldedSqlName);
            }
        }
    }
    checkRelationalSchema(doc, diagnostics);
    if ("queries" in doc) {
        if (!Array.isArray(doc.queries)) {
            diag(diagnostics, "IR017", "$.queries", "queries must be an array");
        } else if (Array.isArray(doc.tables)) {
            const names = new Set();
            const generatedMethods = new Map();
            const generatedBridges = new Map();
            let previousQueryName = null;
            for (let i = 0; i < doc.queries.length; i += 1) {
                checkQuery(doc.queries[i], i, doc, diagnostics);
                const query = doc.queries[i];
                if (isPlainObject(query) && typeof query.name === "string") {
                    if (names.has(query.name)) {
                        diag(diagnostics, "IR017", `$.queries[${i}].name`, `duplicate query name "${query.name}"`);
                    }
                    names.add(query.name);
                    if (previousQueryName !== null && previousQueryName > query.name) {
                        diag(
                            diagnostics,
                            "IR033",
                            `$.queries[${i}].name`,
                            "queries must be sorted by name for deterministic IR v1"
                        );
                    }
                    previousQueryName = query.name;
                    if (QUERY_NAME_RE.test(query.name)) {
                        const method = queryMethodName(query.name);
                        const methodOwner = typeof query.table === "string" ? query.table : "";
                        const methodKey = `${methodOwner}.${method}`;
                        const priorMethod = generatedMethods.get(methodKey);
                        if (priorMethod && priorMethod !== query.name) {
                            diag(
                                diagnostics,
                                "IR032",
                                `$.queries[${i}].name`,
                                `query "${query.name}" collides with "${priorMethod}" after method-name normalization ("${method}")`
                            );
                        } else {
                            generatedMethods.set(methodKey, query.name);
                        }
                        if (isPlainObject(doc.schema) && isIdentifier(doc.schema.name)) {
                            const bridge = bridgeFunctionName(doc.schema.name, query.name);
                            const priorBridge = generatedBridges.get(bridge);
                            if (priorBridge && priorBridge !== query.name) {
                                diag(
                                    diagnostics,
                                    "IR032",
                                    `$.queries[${i}].name`,
                                    `query "${query.name}" collides with "${priorBridge}" after bridge-name normalization ("${bridge}")`
                                );
                            } else {
                                generatedBridges.set(bridge, query.name);
                            }
                        }
                    }
                }
            }
        }
    }
    if ("integrity" in doc) {
        checkIntegrity(doc.integrity, diagnostics);
    }
    if ("generator" in doc) {
        checkGenerator(doc.generator, diagnostics);
    }
    if ("annotations" in doc && !isPlainObject(doc.annotations)) {
        diag(diagnostics, "IR023", "$.annotations", "annotations must be an object");
    }
    return { ok: diagnostics.length === 0, diagnostics };
}

export function assertValidDocument(doc) {
    const result = validateDocument(doc);
    if (!result.ok) {
        throw new IrValidationError(result.diagnostics);
    }
    return doc;
}

export { INT64_RE, REAL_RE, BLOB_RE, IDENTIFIER_RE, QUERY_NAME_RE, int64InRange };
