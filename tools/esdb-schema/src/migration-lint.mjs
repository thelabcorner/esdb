/*
 * esdb-schema migration SQL policy.
 *
 * Drizzle Kit owns DDL/diff authoring. This module only validates the ordinary
 * SQL that Kit emitted before it is packaged for esdb_migrate().
 */

function syntaxError(message) {
    const error = new Error(`MIG007 ${message}`);
    error.code = "MIG007";
    return error;
}

function tokenize(sql) {
    const tokens = [];
    let i = 0;
    const pushWord = (word) => {
        if (word.length > 0) {
            tokens.push(word.toUpperCase());
        }
    };
    while (i < sql.length) {
        const ch = sql.charAt(i);
        const next = sql.charAt(i + 1);

        if (ch === "-" && next === "-") {
            i += 2;
            while (i < sql.length && sql.charAt(i) !== "\n") i += 1;
            continue;
        }
        if (ch === "/" && next === "*") {
            i += 2;
            while (i + 1 < sql.length && !(sql.charAt(i) === "*" && sql.charAt(i + 1) === "/")) i += 1;
            if (i + 1 >= sql.length) throw syntaxError("unterminated SQL block comment");
            i += 2;
            continue;
        }
        if (ch === "'") {
            let closed = false;
            i += 1;
            while (i < sql.length) {
                if (sql.charAt(i) === "'") {
                    if (sql.charAt(i + 1) === "'") {
                        i += 2;
                        continue;
                    }
                    i += 1;
                    closed = true;
                    break;
                }
                i += 1;
            }
            if (!closed) throw syntaxError("unterminated SQL string literal");
            continue;
        }
        if (ch === '"' || ch === "`" || ch === "[") {
            const close = ch === "[" ? "]" : ch;
            let word = "";
            let closed = false;
            i += 1;
            while (i < sql.length) {
                if (sql.charAt(i) === close) {
                    if (close !== "]" && sql.charAt(i + 1) === close) {
                        word += close;
                        i += 2;
                        continue;
                    }
                    i += 1;
                    closed = true;
                    break;
                }
                word += sql.charAt(i);
                i += 1;
            }
            if (!closed) throw syntaxError("unterminated quoted SQL identifier");
            tokens.push("Q:" + word.toUpperCase());
            continue;
        }
        if (/[A-Za-z0-9_]/.test(ch)) {
            let word = ch;
            i += 1;
            while (i < sql.length && /[A-Za-z0-9_]/.test(sql.charAt(i))) {
                word += sql.charAt(i);
                i += 1;
            }
            pushWord(word);
            continue;
        }
        if (ch === "." || ch === ";" || ch === "(" || ch === ")") {
            tokens.push(ch);
        }
        i += 1;
    }
    return tokens;
}

const TRANSACTION_CONTROL = new Set(["BEGIN", "COMMIT", "END", "ROLLBACK", "SAVEPOINT", "RELEASE"]);
const DANGEROUS_PRAGMAS = new Set([
    "USER_VERSION",
    "SCHEMA_VERSION",
    "JOURNAL_MODE",
    "LOCKING_MODE",
    "SYNCHRONOUS",
    "WAL_CHECKPOINT",
    "WRITABLE_SCHEMA",
    "FOREIGN_KEYS"
]);
const DIRECTLY_FORBIDDEN = new Set(["ATTACH", "DETACH", "VACUUM"]);
const INTERNAL_SCHEMA_NAMES = new Set(["SQLITE_SCHEMA", "SQLITE_MASTER"]);

function pragmaName(tokens, index) {
    let i = index + 1;
    let name = tokens[i] || "";
    if ((tokens[i + 1] || "") === ".") {
        name = tokens[i + 2] || "";
    }
    return name;
}

export function lintMigrationSql(sql) {
    const tokens = tokenize(String(sql));
    const violations = [];
    let destructive = false;
    const add = (code, message) => {
        if (!violations.some((item) => item.code === code && item.message === message)) {
            violations.push({ code, message });
        }
    };

    for (let i = 0; i < tokens.length; i += 1) {
        const token = tokens[i];
        const quotedIdentifier = token.startsWith("Q:");
        const identifier = quotedIdentifier ? token.substring(2) : token;
        if (!quotedIdentifier && TRANSACTION_CONTROL.has(token)) {
            add("MIG001", `transaction control "${token}" is owned by esdb_migrate()`);
        }
        if (!quotedIdentifier && DIRECTLY_FORBIDDEN.has(token)) {
            add("MIG002", `statement "${token}" is forbidden in packaged migrations`);
        }
        if (!quotedIdentifier && token === "PRAGMA") {
            const name = pragmaName(tokens, i);
            if (DANGEROUS_PRAGMAS.has(name)) {
                add("MIG003", `PRAGMA ${name.toLowerCase()} is owned by ESDB Runtime`);
            }
        }
        if (!quotedIdentifier && (token === "TEMP" || token === "TEMPORARY")) {
            add("MIG004", "temporary-schema objects are forbidden in packaged migrations");
        }
        if (INTERNAL_SCHEMA_NAMES.has(identifier)) {
            add("MIG005", `direct access to ${identifier.toLowerCase()} is forbidden in packaged migrations`);
        }
        if (!quotedIdentifier && (token === "DROP" || token === "RENAME" || token === "ALTER")) {
            destructive = true;
        }
    }

    return { ok: violations.length === 0, violations, destructive, tokens };
}

export function assertMigrationSqlSafe(sql) {
    const result = lintMigrationSql(sql);
    if (!result.ok) {
        const first = result.violations[0];
        const error = new Error(`${first.code} ${first.message}`);
        error.code = first.code;
        error.violations = result.violations;
        throw error;
    }
    return result;
}
