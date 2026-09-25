/*
 * esdb-schema: project-specific ESABI bridge contract, reference stub, and
 * machine-readable manifest.
 *
 * The bridge is not the ESDB ExternalObject adapter. It is the named-operation
 * surface the generated ES3 facade calls; its contract forbids SQL text.
 */

import { columnByName, deriveApiParams, deriveBindParams, tableByName } from "./ir.mjs";
import { canonicalize } from "./canonical.mjs";
import { bridgeDefaultLibrary, bridgeFunctionName, querySurfaceName, toUpperSnake } from "./naming.mjs";

function lane(type) {
    switch (type) {
        case "integer":
            return "i";
        case "text":
            return "t";
        case "real":
            return "r";
        case "blob":
            return "b";
        default:
            throw new Error(`unsupported IR scalar type "${type}"`);
    }
}

function decorateParams(ir, table, params) {
    return params.map((param) => {
        const owner = param.table === null || param.table === table.name ? table : tableByName(ir, param.table);
        const column = param.column === null || owner === null ? null : columnByName(owner, param.column);
        return {
            name: param.name,
            table: param.table,
            type: param.type,
            lane: lane(param.type),
            nullable: column !== null && column.nullable === true
        };
    });
}

function planQuery(ir, schema, table, query) {
    return {
        method: querySurfaceName(schema, query.name),
        bridge: bridgeFunctionName(schema, query.name),
        kind: query.kind,
        cardinality: query.cardinality,
        params: decorateParams(ir, table, deriveApiParams(table, query, ir)),
        bindParams: decorateParams(ir, table, deriveBindParams(table, query, ir))
    };
}

function signatureFor(plan) {
    /* handle is I32 ("d"); every value parameter is a U1 lane string. */
    return `${plan.bridge}_d${"s".repeat(plan.params.length)}`;
}

function emitHeader(ir, ctx, plans, lines) {
    const schema = ir.schema.name;
    const guard = `ESDB_GENERATED_${toUpperSnake(schema)}_ORM_BRIDGE_H`;
    lines.push(`#ifndef ${guard}`);
    lines.push(`#define ${guard}`);
    lines.push(``);
    lines.push(`#include <esabi/esabi.h>`);
    lines.push(``);
    lines.push(`/*`);
    lines.push(` * Generated project-specific ESABI bridge contract for schema "${schema}".`);
    lines.push(` * IR: ${ir.ir_version} sha256:${ctx.irHash}`);
    if (ctx.source) {
        lines.push(` * Source: ${ctx.source}`);
    }
    lines.push(` *`);
    lines.push(` * This is NOT the ESDB ExternalObject adapter. It is the named-operation`);
    lines.push(` * contract a project bridge DLL implements; the generated ES3 facade calls`);
    lines.push(` * exactly these operations and never SQL.`);
    lines.push(` *`);
    lines.push(` * Handshake and lifecycle:`);
    lines.push(` *   ormPing(0)        -> 42 (load check)`);
    lines.push(` *   ormVersion(0)     -> "${ctx.irHash}" (drift check; raw hash string)`);
    lines.push(` *   ormOpen(0,pathHex)-> U1:H:<handle> | U1:E:...`);
    lines.push(` *   ormClose(handle)  -> U1:C:1 | U1:E:...`);
    lines.push(` *   ormLastError(0)   -> U1:O | U1:E:...`);
    lines.push(` *`);
    lines.push(` * Named operations (handle first; every parameter is a U1 lane string):`);
    for (const plan of plans) {
        const args = plan.params.map((param) => `${param.name}(${param.lane}${param.nullable ? "|n" : ""})`);
        lines.push(` *   ${plan.bridge}(${["handle", ...args].join(", ")})`);
    }
    lines.push(` *`);
    lines.push(` * U1 wire v1 (ASCII only):`);
    lines.push(` *   ok        "U1:O"`);
    lines.push(` *   none      "U1:N"`);
    lines.push(` *   error     "U1:E:<esabi_error_decimal>:<messageHex>"`);
    lines.push(` *   handle    "U1:H:<handle_decimal>"`);
    lines.push(` *   changes   "U1:C:<changes_decimal>"`);
    lines.push(` *   row       "U1:R:<fieldCount>"  then per field "|<tag>|<valueHex>"`);
    lines.push(` *   rows      "U1:L:<rowCount>"    then per row "|<fieldCount>" + fields`);
    lines.push(` *   tags: n null | i int64 decimal | e int64 decimal (exact mode)`);
    lines.push(` *         t UTF-8 | r real decimal | b blob`);
    lines.push(` *   valueHex: uppercase hex of the payload bytes (empty for null)`);
    lines.push(` *`);
    lines.push(` * Parameter lanes (ES3 -> native), one ESABI string per parameter:`);
    lines.push(` *   "i:<canonical decimal>" | "t:<hex>" | "r:<decimal>" | "b:<hex>" | "n"`);
    lines.push(` * Implementations MUST reject any parameter that does not match its lane`);
    lines.push(` * grammar; no parameter text may ever reach SQL.`);
    lines.push(` */`);
    lines.push(``);
    lines.push(`ESABI_DIRECT_FUNCTION(ormPing);`);
    lines.push(`ESABI_DIRECT_FUNCTION(ormVersion);`);
    lines.push(`ESABI_DIRECT_FUNCTION(ormOpen);`);
    lines.push(`ESABI_DIRECT_FUNCTION(ormClose);`);
    lines.push(`ESABI_DIRECT_FUNCTION(ormLastError);`);
    for (const plan of plans) {
        lines.push(`ESABI_DIRECT_FUNCTION(${plan.bridge});  /* ${signatureFor(plan)} */`);
    }
    lines.push(``);
    lines.push(`ESABI_INITIALIZE_FUNCTION;`);
    lines.push(`ESABI_VERSION_FUNCTION;`);
    lines.push(`ESABI_FREE_FUNCTION;`);
    lines.push(`ESABI_TERMINATE_FUNCTION;`);
    lines.push(``);
    lines.push(`#endif /* ${guard} */`);
    lines.push(``);
}

function emitStub(ir, ctx, plans, lines) {
    const schema = ir.schema.name;
    lines.push(`/*`);
    lines.push(` * Generated by ${ctx.compilerName} ${ctx.compilerVersion}. DO NOT EDIT.`);
    lines.push(` * IR: ${ir.ir_version} sha256:${ctx.irHash}`);
    lines.push(` *`);
    lines.push(` * Reference stub for the "${schema}" ESABI bridge contract. Lifecycle,`);
    lines.push(` * handshake, and lastError are functional so a host can load the library`);
    lines.push(` * and receive clean per-operation failures; every named operation returns`);
    lines.push(` * a well-formed U1 error with ESABI_ERR_NOT_IMPLEMENTED.`);
    lines.push(` *`);
    lines.push(` * A production bridge implements the named operations on top of the`);
    lines.push(` * generated C++ repository (cpp/${schema}_repository.hpp) behind an`);
    lines.push(` * esdb_database* handle table; it must never accept SQL text.`);
    lines.push(` */`);
    lines.push(``);
    lines.push(`#include "${schema}_orm_bridge.h"`);
    lines.push(``);
    lines.push(`#include <stdlib.h>`);
    lines.push(`#include <string.h>`);
    lines.push(``);
    lines.push(`#define ORM_IR_HASH ${JSON.stringify(ctx.irHash)}`);
    lines.push(``);
    lines.push(`static char g_last_error[256];`);
    lines.push(`static char g_signatures[] =`);
    lines.push(`    ESABI_SIGNATURE(ormPing, ESABI_SIG_DOUBLE)`);
    lines.push(`    ESABI_SIGNATURE_SEPARATOR`);
    lines.push(`    ESABI_SIGNATURE(ormVersion, ESABI_SIG_DOUBLE)`);
    lines.push(`    ESABI_SIGNATURE_SEPARATOR`);
    lines.push(`    ESABI_SIGNATURE(ormOpen, ESABI_SIG_DOUBLE ESABI_SIG_STRING)`);
    lines.push(`    ESABI_SIGNATURE_SEPARATOR`);
    lines.push(`    ESABI_SIGNATURE(ormClose, ESABI_SIG_I32)`);
    lines.push(`    ESABI_SIGNATURE_SEPARATOR`);
    lines.push(`    ESABI_SIGNATURE(ormLastError, ESABI_SIG_DOUBLE)`);
    plans.forEach((plan) => {
        const args = plan.params.map(() => "ESABI_SIG_STRING");
        lines.push(`    ESABI_SIGNATURE_SEPARATOR`);
        lines.push(`    ESABI_SIGNATURE(${plan.bridge}, ESABI_SIG_I32${args.length > 0 ? ` ${args.join(" ")}` : ""})`);
    });
    lines.push(`    ;`);
    lines.push(``);
    lines.push(`static void set_last_error(const char *message) {`);
    lines.push(`    size_t length = strlen(message);`);
    lines.push(`    if (length >= sizeof(g_last_error)) {`);
    lines.push(`        length = sizeof(g_last_error) - 1u;`);
    lines.push(`    }`);
    lines.push(`    memcpy(g_last_error, message, length);`);
    lines.push(`    g_last_error[length] = '\\0';`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`static char *duplicate_string(const char *text) {`);
    lines.push(`    const size_t length = strlen(text);`);
    lines.push(`    char *out = (char *)malloc(length + 1u);`);
    lines.push(`    if (out == NULL) {`);
    lines.push(`        return NULL;`);
    lines.push(`    }`);
    lines.push(`    memcpy(out, text, length + 1u);`);
    lines.push(`    return out;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`static esabi_error reply_ok(esabi_value *retval) {`);
    lines.push(`    char *out = duplicate_string("U1:O");`);
    lines.push(`    if (out == NULL) {`);
    lines.push(`        return ESABI_ERR_OUT_OF_MEMORY;`);
    lines.push(`    }`);
    lines.push(`    if (retval == NULL) {`);
    lines.push(`        free(out);`);
    lines.push(`        return ESABI_ERR_BAD_ARGUMENTS;`);
    lines.push(`    }`);
    lines.push(`    esabi_value_set_string(retval, out);`);
    lines.push(`    return ESABI_OK;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`static esabi_error reply_error(esabi_value *retval, esabi_error status, const char *message) {`);
    lines.push(`    static const char hex_digits[] = "0123456789ABCDEF";`);
    lines.push(`    char status_text[24];`);
    lines.push(`    char reversed[16];`);
    lines.push(`    char *out;`);
    lines.push(`    size_t status_length = 0u;`);
    lines.push(`    size_t digit_count = 0u;`);
    lines.push(`    size_t total;`);
    lines.push(`    size_t offset = 0u;`);
    lines.push(`    size_t index;`);
    lines.push(`    const size_t message_length = strlen(message);`);
    lines.push(`    const long long signed_status = (long long)status;`);
    lines.push(`    unsigned long long magnitude = signed_status < 0`);
    lines.push(`        ? (unsigned long long)(-(signed_status + 1LL)) + 1ULL`);
    lines.push(`        : (unsigned long long)signed_status;`);
    lines.push(``);
    lines.push(`    if (status < 0) {`);
    lines.push(`        status_text[status_length] = '-';`);
    lines.push(`        status_length += 1u;`);
    lines.push(`    }`);
    lines.push(`    do {`);
    lines.push(`        reversed[digit_count] = (char)('0' + (magnitude % 10));`);
    lines.push(`        magnitude /= 10;`);
    lines.push(`        digit_count += 1u;`);
    lines.push(`    } while (magnitude != 0 && digit_count < sizeof(reversed));`);
    lines.push(`    while (digit_count > 0u) {`);
    lines.push(`        digit_count -= 1u;`);
    lines.push(`        status_text[status_length] = reversed[digit_count];`);
    lines.push(`        status_length += 1u;`);
    lines.push(`    }`);
    lines.push(`    status_text[status_length] = '\\0';`);
    lines.push(``);
    lines.push(`    total = 5u + status_length + 1u + message_length * 2u;`);
    lines.push(`    out = (char *)malloc(total + 1u);`);
    lines.push(`    if (out == NULL) {`);
    lines.push(`        return ESABI_ERR_OUT_OF_MEMORY;`);
    lines.push(`    }`);
    lines.push(`    if (retval == NULL) {`);
    lines.push(`        free(out);`);
    lines.push(`        return ESABI_ERR_BAD_ARGUMENTS;`);
    lines.push(`    }`);
    lines.push(`    memcpy(out, "U1:E:", 5u);`);
    lines.push(`    offset = 5u;`);
    lines.push(`    memcpy(out + offset, status_text, status_length);`);
    lines.push(`    offset += status_length;`);
    lines.push(`    out[offset] = ':';`);
    lines.push(`    offset += 1u;`);
    lines.push(`    for (index = 0u; index < message_length; index += 1u) {`);
    lines.push(`        const unsigned char byte = (unsigned char)message[index];`);
    lines.push(`        out[offset] = hex_digits[(byte >> 4) & 0x0fu];`);
    lines.push(`        out[offset + 1u] = hex_digits[byte & 0x0fu];`);
    lines.push(`        offset += 2u;`);
    lines.push(`    }`);
    lines.push(`    out[offset] = '\\0';`);
    lines.push(`    esabi_value_set_string(retval, out);`);
    lines.push(`    return ESABI_OK;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`static esabi_error reply_not_implemented(esabi_value *retval) {`);
    lines.push(`    const char *message = "bridge stub: operation not implemented";`);
    lines.push(`    set_last_error(message);`);
    lines.push(`    return reply_error(retval, ESABI_ERR_NOT_IMPLEMENTED, message);`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`ESABI_DIRECT_FUNCTION(ormPing) {`);
    lines.push(`    (void)argv;`);
    lines.push(`    (void)argc;`);
    lines.push(`    if (retval != NULL) {`);
    lines.push(`        esabi_value_set_i32(retval, 42);`);
    lines.push(`    }`);
    lines.push(`    return ESABI_OK;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`ESABI_DIRECT_FUNCTION(ormVersion) {`);
    lines.push(`    char *out;`);
    lines.push(`    (void)argv;`);
    lines.push(`    (void)argc;`);
    lines.push(`    out = duplicate_string(ORM_IR_HASH);`);
    lines.push(`    if (out == NULL) {`);
    lines.push(`        return ESABI_ERR_OUT_OF_MEMORY;`);
    lines.push(`    }`);
    lines.push(`    if (retval == NULL) {`);
    lines.push(`        free(out);`);
    lines.push(`        return ESABI_ERR_BAD_ARGUMENTS;`);
    lines.push(`    }`);
    lines.push(`    esabi_value_set_string(retval, out);`);
    lines.push(`    return ESABI_OK;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`ESABI_DIRECT_FUNCTION(ormLastError) {`);
    lines.push(`    (void)argv;`);
    lines.push(`    (void)argc;`);
    lines.push(`    if (g_last_error[0] == '\\0') {`);
    lines.push(`        return reply_ok(retval);`);
    lines.push(`    }`);
    lines.push(`    return reply_error(retval, ESABI_ERR_INTERNAL, g_last_error);`);
    lines.push(`}`);
    lines.push(``);
    for (const name of ["ormOpen", "ormClose", ...plans.map((plan) => plan.bridge)]) {
        lines.push(`ESABI_DIRECT_FUNCTION(${name}) {`);
        lines.push(`    (void)argv;`);
        lines.push(`    (void)argc;`);
        lines.push(`    return reply_not_implemented(retval);`);
        lines.push(`}`);
        lines.push(``);
    }
    lines.push(`ESABI_INITIALIZE_FUNCTION {`);
    lines.push(`    (void)argv;`);
    lines.push(`    (void)argc;`);
    lines.push(`    return g_signatures;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`ESABI_VERSION_FUNCTION {`);
    lines.push(`    return 1;`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`ESABI_FREE_FUNCTION {`);
    lines.push(`    free(pointer);`);
    lines.push(`}`);
    lines.push(``);
    lines.push(`ESABI_TERMINATE_FUNCTION {`);
    lines.push(`    g_last_error[0] = '\\0';`);
    lines.push(`}`);
    lines.push(``);
}

export function generateBridge(ir, ctx) {
    const schema = ir.schema.name;
    const plans = ir.queries.map((query) => planQuery(ir, schema, ir.tables.find((table) => table.name === query.table), query));

    const headerLines = [];
    emitHeader(ir, ctx, plans, headerLines);
    const stubLines = [];
    emitStub(ir, ctx, plans, stubLines);

    const manifest = {
        ir_version: ir.ir_version,
        ir_hash: ctx.irHash,
        schema,
        wire: "U1",
        library: bridgeDefaultLibrary(schema),
        operations: plans.map((plan) => ({
            name: plan.bridge,
            signature: signatureFor(plan),
            kind: plan.kind,
            cardinality: plan.cardinality,
            params: plan.params.map((param) => ({
                name: param.name,
                type: param.type,
                lane: param.lane,
                nullable: param.nullable
            })),
            api_params: plan.params.map((param) => ({
                name: param.name,
                type: param.type,
                lane: param.lane,
                nullable: param.nullable
            })),
            bind_params: plan.bindParams.map((param) => ({
                name: param.name,
                type: param.type,
                lane: param.lane,
                nullable: param.nullable
            })),
            returns: plan.cardinality === "one" ? ["row", "none", "error"] : plan.cardinality === "many" ? ["rows", "error"] : ["changes", "error"]
        }))
    };

    return [
        { relPath: `bridge/${schema}_orm_bridge.h`, content: headerLines.join("\n") },
        { relPath: `bridge/${schema}_orm_bridge_stub.c`, content: stubLines.join("\n") },
        { relPath: `bridge/${schema}_orm_bridge.json`, content: canonicalize(manifest) }
    ];
}
