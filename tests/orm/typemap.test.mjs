/*
 * Type mapping and the IR v1 int64 policy.
 *
 * Mapping (orm/ir/esdb-ir-v1.md §4.1, §7):
 *   integer -> C++ int64_t / TS+ES3 number with safe-integer policy
 *   text    -> std::string / string
 *   real    -> double / number
 *   blob    -> std::vector<unsigned char> / uppercase hex string
 */

import { join } from "node:path";
import { pathToFileURL } from "node:url";

import {
    EXAMPLE_GENERATED,
    assert,
    assertEqual,
    assertThrows,
    freshWorkDir,
    loadFixture,
    readText,
    runCli,
    sealFixture
} from "./helpers.mjs";

export async function run() {
    /* Generated TS int64 helpers (imported through Node type stripping). */
    const tsModule = await import(pathToFileURL(join(EXAMPLE_GENERATED, "ts", "user.ts")).href);

    assertEqual(tsModule.toInt64Param(42), "42", "number -> decimal string");
    assertEqual(tsModule.toInt64Param(9007199254740991), "9007199254740991", "max safe integer accepted");
    assertEqual(tsModule.toInt64Param(-9007199254740991), "-9007199254740991", "min safe integer accepted");
    assertEqual(tsModule.toInt64Param("9007199254740993"), "9007199254740993", "exact decimal string accepted");
    assertEqual(tsModule.toInt64Param("-0"), "0", "-0 normalizes to 0");
    assertThrows(() => tsModule.toInt64Param(9007199254740992), "safe integer", "unsafe number rejected");
    assertThrows(() => tsModule.toInt64Param(1.5), "whole number", "fractional number rejected");
    assertThrows(() => tsModule.toInt64Param("007"), "canonical", "leading zeros rejected");
    assertThrows(() => tsModule.toInt64Param("9223372036854775808"), "signed 64-bit", "int64 overflow rejected");
    assertThrows(() => tsModule.toInt64Param("-9223372036854775809"), "signed 64-bit", "int64 underflow rejected");
    assertEqual(tsModule.fromInt64Param("9007199254740991"), 9007199254740991, "safe read returns a number");
    assertThrows(() => tsModule.fromInt64Param("9007199254740993"), "safe integer range", "unsafe read throws by default");
    assertEqual(tsModule.fromInt64Param("9007199254740993", true), "9007199254740993", "exact read returns the string");
    assertEqual(tsModule.canonicalInt64Text("-0"), "0", "canonical text normalizes -0");

    /* Generated TS type surface (User slice). */
    const userTs = readText(join(EXAMPLE_GENERATED, "ts", "user.ts"));
    assert(userTs.includes("id: number;"), "integer maps to number in TS rows");
    assert(userTs.includes("createdAt: number;"), "logical integer createdAt maps to number");
    assert(userTs.includes("email: string;"), "NOT NULL text maps to string");
    assert(/interface UserRowExact \{[\s\S]*?id: string;/.test(userTs), "exact row carries int64 as decimal string");
    assert(userTs.includes('FROM \\"user\\" WHERE \\"id\\" = ?'), "query SQL is parameterized");

    /* Types fixture: all four scalar types plus nullable paths. */
    const work = freshWorkDir("typemap");
    const typesSealed = sealFixture(loadFixture("types.ir.json"), work, "types");
    const outDir = join(work, "gen");
    const build = runCli(["build", typesSealed, "--out", outDir]);
    assertEqual(build.status, 0, `build types fixture: ${build.stderr}`);

    const cpp = readText(join(outDir, "cpp", "metric_repository.hpp"));
    assert(cpp.includes("std::int64_t id;"), "integer -> std::int64_t");
    assert(cpp.includes("double score;"), "real -> double");
    assert(cpp.includes("std::vector<unsigned char> payload;"), "blob -> std::vector<unsigned char>");
    assert(cpp.includes("bool payload_is_null;"), "nullable blob exposes a null flag");
    assert(cpp.includes("bool note_is_null;"), "nullable text exposes a null flag");
    assert(cpp.includes("struct MetricUpdateNoteParams"), "nullable update set uses a params struct");
    assert(cpp.includes("int update_note(std::int64_t id, const MetricUpdateNoteParams &values)"), "update signature");
    assert(cpp.includes("int find_by_id(std::int64_t id, MetricRow *out)"), "int64 find signature");
    assert(cpp.includes("sqlite3_bind_int64"), "int64 binds through sqlite3_bind_int64");
    assert(cpp.includes("sqlite3_bind_double"), "real binds through sqlite3_bind_double");
    assert(cpp.includes("sqlite3_bind_blob"), "non-empty blob binds through sqlite3_bind_blob");
    assert(
        cpp.includes("sqlite3_bind_zeroblob(statement, index, 0)"),
        "empty non-null blob preserves BLOB identity instead of binding SQL NULL"
    );
    assert(cpp.includes("sqlite3_column_int64"), "int64 decodes through sqlite3_column_int64");
    assert(cpp.includes("bind_text(statement, 2, values.label, false)"), "insert binds label text");

    const ts = readText(join(outDir, "ts", "metric.ts"));
    assert(ts.includes("payload: string | null;"), "nullable blob maps to hex string | null");
    assert(ts.includes("score: number;"), "real maps to number");
    assert(ts.includes("label: string;"), "text maps to string");
    assert(/interface MetricRowExact \{[\s\S]*?id: string;/.test(ts), "types fixture exact row");

    const es3 = readText(join(outDir, "es3", "metric_repository.jsx"));
    assert(
        es3.includes('"integer",\n        "text",\n        "real",\n        "blob",\n        "text"'),
        "ES3 column type table covers all four scalar types"
    );
    assert(es3.includes('{ name: "payload", type: "blob", nullable: true }'), "ES3 nullable blob param");
    assert(es3.includes('{ name: "note", type: "text", nullable: true }'), "ES3 nullable text param");
}
