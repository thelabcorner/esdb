/*
 * esdb-schema: C++ repository bindings.
 *
 * Header-only C++11 bindings that go through esdb_native_handle() and the
 * pinned SQLite prepared-statement API. SQL text is generated from the IR;
 * every value is bound. Callers never build SQL.
 */

import { columnByName, deriveApiParams, deriveBindParams, tableByName } from "./ir.mjs";
import { emitQuerySql } from "./sql.mjs";
import { cppNamespace, queryMethodName, toPascal, toSnake, toUpperSnake } from "./naming.mjs";

const CPP_TYPES = {
    integer: "std::int64_t",
    text: "std::string",
    real: "double",
    blob: "std::vector<unsigned char>"
};

function cppStringLiteral(text) {
    return `"${String(text).replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

function cppType(type) {
    return CPP_TYPES[type];
}

function argDeclaration(column, paramName) {
    if (column.type === "text") {
        return `const std::string &${toSnake(paramName)}`;
    }
    if (column.type === "blob") {
        return `const std::vector<unsigned char> &${toSnake(paramName)}`;
    }
    return `${cppType(column.type)} ${toSnake(paramName)}`;
}

function decorateParams(ir, table, params) {
    return params.map((param) => {
        const owner = param.table === null || param.table === table.name
            ? table
            : tableByName(ir, param.table);
        const column = param.column === null || owner === null ? null : columnByName(owner, param.column);
        return {
            name: param.name,
            table: param.table,
            column: param.column,
            type: param.type,
            nullable: column !== null && column.nullable === true
        };
    });
}

function planQuery(ir, table, query, insertStructName) {
    const apiParams = decorateParams(ir, table, deriveApiParams(table, query, ir));
    const bindParams = decorateParams(ir, table, deriveBindParams(table, query, ir));
    const plan = {
        method: toSnake(queryMethodName(query.name)),
        query,
        params: bindParams,
        apiParams,
        structName: null,
        structParams: [],
        argParams: []
    };
    if (query.kind === "insert" || query.kind === "upsert") {
        plan.structName = insertStructName;
        plan.structParams = apiParams;
    } else if (query.kind === "update") {
        const setParams = apiParams.filter((param) => query.set.some((entry) => entry.param === param.name));
        const whereParams = apiParams.filter((param) => query.where.some((entry) => entry.param === param.name));
        if (setParams.some((param) => param.nullable)) {
            plan.structName = `${toPascal(table.name)}${toPascal(queryMethodName(query.name))}Params`;
            plan.structParams = setParams;
        } else {
            plan.argParams = plan.argParams.concat(setParams);
        }
        plan.argParams = whereParams.concat(plan.argParams);
    } else {
        plan.argParams = apiParams;
    }
    return plan;
}

function accessor(plan, param) {
    const inStruct = plan.structParams.some((candidate) => candidate.name === param.name);
    return `${inStruct ? "values." : ""}${toSnake(param.name)}`;
}

function nullExpression(plan, param) {
    if (!param.nullable) {
        return "false";
    }
    const inStruct = plan.structParams.some((candidate) => candidate.name === param.name);
    return inStruct ? `${accessor(plan, param)}_is_null` : "false";
}

function bindCall(plan, param, index) {
    const value = accessor(plan, param);
    const isNull = nullExpression(plan, param);
    if (param.type === "integer") {
        return `bind_integer(statement, ${index}, ${value}, ${isNull})`;
    }
    if (param.type === "text") {
        return `bind_text(statement, ${index}, ${value}, ${isNull})`;
    }
    if (param.type === "real") {
        return `bind_real(statement, ${index}, ${value}, ${isNull})`;
    }
    if (param.type === "blob") {
        return `bind_blob(statement, ${index}, ${value}, ${isNull})`;
    }
    throw new Error(`unsupported IR scalar type "${param.type}"`);
}

function methodSignature(plan, rowStruct) {
    const args = plan.argParams.map((param) => {
        const column = { type: param.type };
        return argDeclaration(column, param.name);
    });
    if (plan.structName !== null) {
        args.push(`const ${plan.structName} &values`);
    }
    if (plan.query.kind === "select") {
        if (plan.query.cardinality === "one") {
            args.push(`${rowStruct} *out`);
        } else {
            args.push(`std::vector<${rowStruct}> *out`);
        }
    }
    return `${plan.method}(${args.join(", ")})`;
}

function emitMethod(lines, plan, rowStruct) {
    const method = plan.method;
    const hasOutput = plan.query.kind === "select";
    lines.push(`    int ${methodSignature(plan, rowStruct)} {`);
    if (hasOutput) {
        lines.push(`        if (!ensure_prepared()) {`);
        lines.push(`            return last_error_code_ != 0 ? -last_error_code_ : -SQLITE_MISUSE;`);
        lines.push(`        }`);
        lines.push(`        if (out == NULL) {`);
        lines.push(`            return misuse("${method} requires a non-null output");`);
        lines.push(`        }`);
    } else {
        lines.push(`        if (!ensure_prepared()) {`);
        lines.push(`            return last_error_code_ != 0 ? -last_error_code_ : -SQLITE_MISUSE;`);
        lines.push(`        }`);
    }
    lines.push(`        sqlite3_stmt *statement = stmt_${method}_;`);
    lines.push(`        // sqlite3_reset() returns the prior sqlite3_step() result; that`);
    lines.push(`        // result was already surfaced by the previous call, so do not`);
    lines.push(`        // turn it into a duplicate failure for this invocation.`);
    lines.push(`        (void)sqlite3_reset(statement);`);
    lines.push(`        int rc = sqlite3_clear_bindings(statement);`);
    lines.push(`        if (rc != SQLITE_OK) {`);
    lines.push(`            return fail(rc);`);
    lines.push(`        }`);
    plan.params.forEach((param, index) => {
        lines.push(`        rc = ${bindCall(plan, param, index + 1)};`);
        lines.push(`        if (rc != SQLITE_OK) {`);
        lines.push(`            return -rc;`);
        lines.push(`        }`);
    });
    if (plan.query.kind === "select" && plan.query.cardinality === "one") {
        lines.push(`        rc = sqlite3_step(statement);`);
        lines.push(`        if (rc == SQLITE_ROW) {`);
        lines.push(`            read_row(statement, out);`);
        lines.push(`            return 1;`);
        lines.push(`        }`);
        lines.push(`        if (rc == SQLITE_DONE) {`);
        lines.push(`            return 0;`);
        lines.push(`        }`);
        lines.push(`        return fail(rc);`);
    } else if (plan.query.kind === "select") {
        lines.push(`        int count = 0;`);
        lines.push(`        for (;;) {`);
        lines.push(`            rc = sqlite3_step(statement);`);
        lines.push(`            if (rc == SQLITE_ROW) {`);
        lines.push(`                ${rowStruct} row;`);
        lines.push(`                read_row(statement, &row);`);
        lines.push(`                out->push_back(row);`);
        lines.push(`                count += 1;`);
        lines.push(`                continue;`);
        lines.push(`            }`);
        lines.push(`            if (rc == SQLITE_DONE) {`);
        lines.push(`                return count;`);
        lines.push(`            }`);
        lines.push(`            return fail(rc);`);
        lines.push(`        }`);
    } else {
        lines.push(`        return finish_changes(statement);`);
    }
    lines.push(`    }`);
    lines.push(``);
}

function emitStruct(lines, name, fields) {
    lines.push(`struct ${name} {`);
    for (const field of fields) {
        if (field.nullable) {
            lines.push(`    bool ${toSnake(field.name)}_is_null;`);
        }
        lines.push(`    ${cppType(field.type)} ${toSnake(field.name)};`);
    }
    lines.push(`};`);
    lines.push(``);
}

function emitTable(ir, table, ctx, lines) {
    const pascal = toPascal(table.name);
    const queries = ir.queries.filter((query) => query.table === table.name);
    const insertQueries = queries.filter((query) => query.kind === "insert" || query.kind === "upsert");

    emitStruct(
        lines,
        `${pascal}Row`,
        table.columns.map((column) => ({ name: column.name, type: column.type, nullable: column.nullable === true }))
    );

    const insertStructNames = new Map();
    insertQueries.forEach((query) => {
        const structName = insertQueries.length === 1
            ? `${pascal}Insert`
            : `${toPascal(queryMethodName(query.name))}Insert`;
        insertStructNames.set(query.name, structName);
        const fields = deriveApiParams(table, query, ir).map((param) => {
            const column = columnByName(table, param.column);
            return { name: param.name, type: param.type, nullable: column.nullable === true };
        });
        emitStruct(lines, structName, fields);
    });

    const plans = queries.map((query) => planQuery(ir, table, query, insertStructNames.get(query.name) || null));

    for (const plan of plans) {
        if (plan.structName !== null && plan.query.kind === "update") {
            emitStruct(
                lines,
                plan.structName,
                plan.structParams.map((param) => ({ name: param.name, type: param.type, nullable: param.nullable }))
            );
        }
    }

    lines.push(`class ${pascal}Repository {`);
    lines.push(`public:`);
    lines.push(`    explicit ${pascal}Repository(esdb_database *database)`);
    lines.push(`        : database_(database),`);
    lines.push(`          native_(NULL),`);
    lines.push(`          prepared_(false),`);
    lines.push(`          last_error_code_(0),`);
    lines.push(`          last_error_("not initialized")${plans.length > 0 ? "," : ""}`);
    plans.forEach((plan, index) => {
        const comma = index + 1 < plans.length ? "," : "";
        lines.push(`          stmt_${plan.method}_(NULL)${comma}`);
    });
    lines.push(`    {`);
    lines.push(`        if (database_ != NULL) {`);
    lines.push(`            native_ = static_cast<sqlite3 *>(esdb_native_handle(database_));`);
    lines.push(`        }`);
    lines.push(`        if (native_ == NULL) {`);
    lines.push(`            last_error_code_ = SQLITE_MISUSE;`);
    lines.push(`            last_error_ = "esdb_native_handle returned NULL";`);
    lines.push(`        }`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    ~${pascal}Repository() {`);
    lines.push(`        reset_prepared_state();`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    ${pascal}Repository(const ${pascal}Repository &) = delete;`);
    lines.push(`    ${pascal}Repository &operator=(const ${pascal}Repository &) = delete;`);
    lines.push(``);
    lines.push(`    bool valid() { return ensure_prepared(); }`);
    lines.push(`    int last_error_code() const { return last_error_code_; }`);
    lines.push(`    const char *last_error_message() const { return last_error_.c_str(); }`);
    lines.push(``);
    plans.forEach((plan) => emitMethod(lines, plan, `${pascal}Row`));

    lines.push(`private:`);
    lines.push(`    bool ensure_prepared() {`);
    lines.push(`        if (prepared_) {`);
    lines.push(`            return true;`);
    lines.push(`        }`);
    lines.push(`        if (native_ == NULL) {`);
    lines.push(`            return false;`);
    lines.push(`        }`);
    plans.forEach((plan) => {
        const { sql } = emitQuerySql(table, plan.query, ir);
        const expectedColumns = plan.query.kind === "select" ? table.columns.length : -1;
        lines.push(`        if (!prepare(&stmt_${plan.method}_, ${cppStringLiteral(sql)}, ${plan.params.length}, ${expectedColumns})) {`);
        lines.push(`            reset_prepared_state();`);
        lines.push(`            return false;`);
        lines.push(`        }`);
    });
    lines.push(`        prepared_ = true;`);
    lines.push(`        return true;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    bool prepare(sqlite3_stmt **statement, const char *sql, int expected_params, int expected_columns) {`);
    lines.push(`        const int rc = sqlite3_prepare_v3(native_, sql, -1, SQLITE_PREPARE_PERSISTENT, statement, NULL);`);
    lines.push(`        if (rc != SQLITE_OK) {`);
    lines.push(`            set_error(rc);`);
    lines.push(`            return false;`);
    lines.push(`        }`);
    lines.push(`        if (sqlite3_bind_parameter_count(*statement) != expected_params) {`);
    lines.push(`            sqlite3_finalize(*statement);`);
    lines.push(`            *statement = NULL;`);
    lines.push(`            last_error_code_ = SQLITE_MISUSE;`);
    lines.push(`            last_error_ = "generated bind-parameter count mismatch";`);
    lines.push(`            return false;`);
    lines.push(`        }`);
    lines.push(`        if (expected_columns >= 0 && sqlite3_column_count(*statement) != expected_columns) {`);
    lines.push(`            sqlite3_finalize(*statement);`);
    lines.push(`            *statement = NULL;`);
    lines.push(`            last_error_code_ = SQLITE_MISUSE;`);
    lines.push(`            last_error_ = "generated result-column count mismatch";`);
    lines.push(`            return false;`);
    lines.push(`        }`);
    lines.push(`        return true;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    void reset_prepared_state() {`);
    plans.forEach((plan) => {
        lines.push(`        if (stmt_${plan.method}_ != NULL) {`);
        lines.push(`            sqlite3_finalize(stmt_${plan.method}_);`);
        lines.push(`            stmt_${plan.method}_ = NULL;`);
        lines.push(`        }`);
    });
    lines.push(`        prepared_ = false;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int bind_integer(sqlite3_stmt *statement, int index, std::int64_t value, bool is_null) {`);
    lines.push(`        const int rc = is_null`);
    lines.push(`            ? sqlite3_bind_null(statement, index)`);
    lines.push(`            : sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value));`);
    lines.push(`        if (rc != SQLITE_OK) {`);
    lines.push(`            set_error(rc);`);
    lines.push(`        }`);
    lines.push(`        return rc;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int bind_real(sqlite3_stmt *statement, int index, double value, bool is_null) {`);
    lines.push(`        const int rc = is_null`);
    lines.push(`            ? sqlite3_bind_null(statement, index)`);
    lines.push(`            : sqlite3_bind_double(statement, index, value);`);
    lines.push(`        if (rc != SQLITE_OK) {`);
    lines.push(`            set_error(rc);`);
    lines.push(`        }`);
    lines.push(`        return rc;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int bind_text(sqlite3_stmt *statement, int index, const std::string &value, bool is_null) {`);
    lines.push(`        if (!is_null && value.size() > static_cast<std::size_t>(INT_MAX)) {`);
    lines.push(`            last_error_code_ = SQLITE_TOOBIG;`);
    lines.push(`            last_error_ = "text parameter exceeds sqlite3_bind_text size limit";`);
    lines.push(`            return SQLITE_TOOBIG;`);
    lines.push(`        }`);
    lines.push(`        const int rc = is_null`);
    lines.push(`            ? sqlite3_bind_null(statement, index)`);
    lines.push(`            : sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);`);
    lines.push(`        if (rc != SQLITE_OK) {`);
    lines.push(`            set_error(rc);`);
    lines.push(`        }`);
    lines.push(`        return rc;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int bind_blob(sqlite3_stmt *statement, int index, const std::vector<unsigned char> &value, bool is_null) {`);
    lines.push(`        if (!is_null && value.size() > static_cast<std::size_t>(INT_MAX)) {`);
    lines.push(`            last_error_code_ = SQLITE_TOOBIG;`);
    lines.push(`            last_error_ = "blob parameter exceeds sqlite3_bind_blob size limit";`);
    lines.push(`            return SQLITE_TOOBIG;`);
    lines.push(`        }`);
    lines.push(`        int rc;`);
    lines.push(`        if (is_null) {`);
    lines.push(`            rc = sqlite3_bind_null(statement, index);`);
    lines.push(`        } else if (value.empty()) {`);
    lines.push(`            // sqlite3_bind_blob(..., NULL, 0, ...) binds SQL NULL, not an`);
    lines.push(`            // empty BLOB. zeroblob(0) preserves empty-BLOB vs NULL semantics.`);
    lines.push(`            rc = sqlite3_bind_zeroblob(statement, index, 0);`);
    lines.push(`        } else {`);
    lines.push(`            rc = sqlite3_bind_blob(`);
    lines.push(`                statement, index, &value[0],`);
    lines.push(`                static_cast<int>(value.size()), SQLITE_TRANSIENT);`);
    lines.push(`        }`);
    lines.push(`        if (rc != SQLITE_OK) {`);
    lines.push(`            set_error(rc);`);
    lines.push(`        }`);
    lines.push(`        return rc;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int finish_changes(sqlite3_stmt *statement) {`);
    lines.push(`        const int rc = sqlite3_step(statement);`);
    lines.push(`        if (rc != SQLITE_DONE) {`);
    lines.push(`            return fail(rc);`);
    lines.push(`        }`);
    lines.push(`        const sqlite3_int64 changes = sqlite3_changes64(native_);`);
    lines.push(`        if (changes > static_cast<sqlite3_int64>(INT_MAX)) {`);
    lines.push(`            last_error_code_ = SQLITE_TOOBIG;`);
    lines.push(`            last_error_ = "change count exceeds generated API int range";`);
    lines.push(`            return -SQLITE_TOOBIG;`);
    lines.push(`        }`);
    lines.push(`        return static_cast<int>(changes);`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int fail(int rc) {`);
    lines.push(`        set_error(rc);`);
    lines.push(`        return -rc;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    int misuse(const char *message) {`);
    lines.push(`        last_error_code_ = SQLITE_MISUSE;`);
    lines.push(`        last_error_ = message;`);
    lines.push(`        return -SQLITE_MISUSE;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    void set_error(int code) {`);
    lines.push(`        last_error_code_ = code;`);
    lines.push(`        last_error_ = native_ != NULL ? sqlite3_errmsg(native_) : "no native handle";`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    static std::string read_text(sqlite3_stmt *statement, int index) {`);
    lines.push(`        const unsigned char *text = sqlite3_column_text(statement, index);`);
    lines.push(`        const int bytes = sqlite3_column_bytes(statement, index);`);
    lines.push(`        if (text == NULL || bytes <= 0) {`);
    lines.push(`            return std::string();`);
    lines.push(`        }`);
    lines.push(`        return std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(bytes));`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    static std::vector<unsigned char> read_blob(sqlite3_stmt *statement, int index) {`);
    lines.push(`        const void *data = sqlite3_column_blob(statement, index);`);
    lines.push(`        const int bytes = sqlite3_column_bytes(statement, index);`);
    lines.push(`        std::vector<unsigned char> out;`);
    lines.push(`        if (data == NULL || bytes <= 0) {`);
    lines.push(`            return out;`);
    lines.push(`        }`);
    lines.push(`        const unsigned char *first = static_cast<const unsigned char *>(data);`);
    lines.push(`        out.assign(first, first + bytes);`);
    lines.push(`        return out;`);
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    static void read_row(sqlite3_stmt *statement, ${pascal}Row *out) {`);
    table.columns.forEach((column, index) => {
        const name = toSnake(column.name);
        if (column.nullable === true) {
            lines.push(`        out->${name}_is_null = sqlite3_column_type(statement, ${index}) == SQLITE_NULL;`);
        }
        if (column.type === "integer") {
            lines.push(`        out->${name} = static_cast<std::int64_t>(sqlite3_column_int64(statement, ${index}));`);
        } else if (column.type === "text") {
            if (column.nullable === true) {
                lines.push(`        out->${name} = out->${name}_is_null ? std::string() : read_text(statement, ${index});`);
            } else {
                lines.push(`        out->${name} = read_text(statement, ${index});`);
            }
        } else if (column.type === "real") {
            lines.push(`        out->${name} = sqlite3_column_double(statement, ${index});`);
        } else if (column.type === "blob") {
            if (column.nullable === true) {
                lines.push(`        out->${name} = out->${name}_is_null ? std::vector<unsigned char>() : read_blob(statement, ${index});`);
            } else {
                lines.push(`        out->${name} = read_blob(statement, ${index});`);
            }
        }
    });
    lines.push(`    }`);
    lines.push(``);
    lines.push(`    esdb_database *database_;`);
    lines.push(`    sqlite3 *native_;`);
    lines.push(`    bool prepared_;`);
    lines.push(`    int last_error_code_;`);
    lines.push(`    std::string last_error_;`);
    plans.forEach((plan) => {
        lines.push(`    sqlite3_stmt *stmt_${plan.method}_;`);
    });
    lines.push(`};`);
    lines.push(``);
}

export function generateCpp(ir, ctx) {
    const schema = ir.schema.name;
    const guard = `ESDB_GENERATED_${toUpperSnake(schema)}_REPOSITORY_HPP_INCLUDED`;
    const sqliteGuard = `ESDB_GENERATED_${toUpperSnake(schema)}_HAVE_SQLITE3`;
    const lines = [];

    lines.push(`// Generated by ${ctx.compilerName} ${ctx.compilerVersion}. DO NOT EDIT.`);
    lines.push(`// IR: ${ir.ir_version} sha256:${ctx.irHash}`);
    if (ctx.source) {
        lines.push(`// Source: ${ctx.source}`);
    }
    lines.push(`//`);
    lines.push(`// Thin C++ repository bindings for schema "${schema}".`);
    lines.push(`//`);
    lines.push(`// Contract:`);
    lines.push(`//   - callers never build SQL; every operation is a generated named query;`);
    lines.push(`//   - values are bound with sqlite3_bind_* and decoded with sqlite3_column_*;`);
    lines.push(`//   - return values: >= 0 means success (rows found / rows appended /`);
    lines.push(`//     changes), negative means -(sqlite3 primary result code);`);
    lines.push(`//     last_error_code()/last_error_message() carry detail;`);
    lines.push(`//   - integer columns are std::int64_t end to end (SQLite INTEGER is 64-bit);`);
    lines.push(`//   - v1 keyed lookups bind non-null values; SQL NULL equality matches nothing.`);
    lines.push(`//   - one repository instance is single-caller-at-a-time: SQLite FULLMUTEX`);
    lines.push(`//     does not make reset/bind/step sequences atomic across callers.`);
    lines.push(`//`);
    lines.push(`// Requires the ESDB Runtime header and the pinned SQLite header:`);
    lines.push(`// <esdb/sqlite3.h> from an ESDB install, or <sqlite3.h> from`);
    lines.push(`// third_party/sqlite. The database handle must outlive the repository.`);
    lines.push(``);
    lines.push(`#ifndef ${guard}`);
    lines.push(`#define ${guard}`);
    lines.push(``);
    lines.push(`#include <esdb/esdb.h>`);
    lines.push(``);
    lines.push(`#include <cstddef>`);
    lines.push(`#include <cstdint>`);
    lines.push(`#include <climits>`);
    lines.push(`#include <string>`);
    lines.push(`#include <vector>`);
    lines.push(``);
    lines.push(`#if defined(__has_include)`);
    lines.push(`#  if __has_include(<esdb/sqlite3.h>)`);
    lines.push(`#    include <esdb/sqlite3.h>`);
    lines.push(`#    define ${sqliteGuard} 1`);
    lines.push(`#  elif __has_include(<sqlite3.h>)`);
    lines.push(`#    include <sqlite3.h>`);
    lines.push(`#    define ${sqliteGuard} 1`);
    lines.push(`#  endif`);
    lines.push(`#else`);
    lines.push(`#  include <sqlite3.h>`);
    lines.push(`#  define ${sqliteGuard} 1`);
    lines.push(`#endif`);
    lines.push(``);
    lines.push(`#if !defined(${sqliteGuard})`);
    lines.push(`#  error "esdb generated bindings require <esdb/sqlite3.h> or <sqlite3.h>"`);
    lines.push(`#endif`);
    lines.push(``);
    lines.push(`namespace ${cppNamespace(schema)} {`);
    lines.push(``);
    lines.push(`inline const char *expected_ir_hash() { return ${cppStringLiteral(ctx.irHash)}; }`);
    lines.push(``);
    for (const table of ir.tables) {
        emitTable(ir, table, ctx, lines);
    }
    lines.push(`}  // namespace ${cppNamespace(schema)}`);
    lines.push(``);
    lines.push(`#endif  // ${guard}`);
    lines.push(``);

    return [
        {
            relPath: `cpp/${toSnake(schema)}_repository.hpp`,
            content: lines.join("\n")
        }
    ];
}
