/*
 * IR v1 validation: required fields, keyed mutations, identifier rules,
 * ES3 binding safety, and the no-numbers rule.
 */

import { join } from "node:path";

import {
    ESDB_ROOT,
    FIXTURES_DIR,
    assert,
    assertEqual,
    clone,
    expectDiagnostic,
    freshWorkDir,
    loadCompiler,
    loadFixture,
    readText,
    runCli,
    writeJson
} from "./helpers.mjs";

export async function run() {
    const { ir } = await loadCompiler();
    const base = loadFixture("user.ir.json");
    const types = loadFixture("types.ir.json");

    assert(ir.validateDocument(base).ok, "user fixture must validate");
    assert(ir.validateDocument(types).ok, "types fixture must validate");

    const cases = [
        ["wrong ir_version", "IR002", (d) => { d.ir_version = "esdb.ir/v2"; }],
        ["unknown field", "IR023", (d) => { d.tables[0].extra = true; }],
        ["unknown column type", "IR011", (d) => { d.tables[0].columns[1].type = "varchar"; }],
        ["non-boolean flag", "IR012", (d) => { d.tables[0].columns[0].nullable = "yes"; }],
        ["identifier grammar", "IR009", (d) => { d.tables[0].columns[1].sql_name = "na me"; }],
        ["identifier injection", "IR009", (d) => { d.tables[0].columns[1].sql_name = "name; DROP TABLE user; --"; }],
        ["identifier quote escape", "IR009", (d) => { d.tables[0].columns[1].sql_name = 'na"me'; }],
        ["identifier non-ascii", "IR009", (d) => { d.tables[0].columns[1].sql_name = "名前"; }],
        ["reserved SQL word", "IR028", (d) => { d.tables[0].columns[1].sql_name = "select"; }],
        ["reserved SQL table word", "IR028", (d) => { d.tables[0].sql_name = "table"; }],
        ["duplicate column", "IR010", (d) => { d.tables[0].columns[1].name = "id"; }],
        ["duplicate table", "IR008", (d) => { d.tables.push(clone(d.tables[0])); }],
        ["duplicate query", "IR017", (d) => { d.queries.push(clone(d.queries[0])); }],
        ["unknown query table", "IR018", (d) => { d.queries[0].table = "missing"; }],
        ["unknown query column", "IR018", (d) => { d.queries[0].where[0].column = "missing"; }],
        ["update without where", "IR018", (d) => { delete d.queries.find((q) => q.name === "user.updateName").where; }],
        ["unkeyed update", "IR020", (d) => {
            d.queries.find((q) => q.name === "user.updateName").where = [{ column: "name", op: "eq", param: "name" }];
        }],
        ["unkeyed delete", "IR020", (d) => {
            d.queries.find((q) => q.name === "user.deleteById").where = [{ column: "name", op: "eq", param: "name" }];
        }],
        ["unkeyed select one", "IR021", (d) => {
            d.queries.find((q) => q.name === "user.findById").where = [{ column: "name", op: "eq", param: "name" }];
        }],
        ["insert missing required column", "IR019", (d) => {
            const query = d.queries.find((q) => q.name === "user.insert");
            query.values = query.values.filter((value) => value.column !== "name");
        }],
        ["duplicate param", "IR019", (d) => { d.queries.find((q) => q.name === "user.list").offset = "limit"; }],
        ["number forbidden", "IR022", (d) => { d.tables[0].columns[0].nullable = 1; }],
        ["number in default", "IR022", (d) => {
            d.tables[0].columns[3].default = { kind: "literal", type: "integer", value: 0 };
        }],
        ["lone surrogate", "IR024", (d) => { d.schema.name = "\ud800"; }],
        ["bad integrity", "IR025", (d) => {
            d.integrity = { algorithm: "sha256", canonical: "esdb-canonical-json-v1", hash: "nope" };
        }],
        ["default type mismatch", "IR016", (d) => {
            d.tables[0].columns[3].default = { kind: "literal", type: "text", value: "x" };
        }],
        ["ES3 reserved method name", "IR031", (d) => { d.queries[0].name = "user.delete"; }],
        ["ES3 reserved param name", "IR031", (d) => { d.queries[0].where[0].param = "new"; }],
        ["autoincrement on non-integer pk", "IR013", (d) => { d.tables[0].columns[1].autoincrement = true; }],
        ["reserved sqlite_ prefix", "IR032", (d) => { d.tables[0].sql_name = "SQLite_shadow"; }],
        ["reserved __esdb_ prefix", "IR032", (d) => { d.tables[0].columns[1].sql_name = "__ESDB_name"; }],
        ["prototype logical name", "IR032", (d) => { d.tables[0].columns[1].name = "constructor"; }],
        ["NOT NULL DEFAULT NULL", "IR016", (d) => {
            d.tables[0].columns[1].default = { kind: "literal", type: "null", value: "" };
        }],
        ["text default NUL", "IR016", (d) => {
            d.tables[0].columns[1].default = { kind: "literal", type: "text", value: "a\u0000b" };
        }],
        ["generator newline injection", "IR030", (d) => { d.generator.source = "x\n*/ injected"; }]
    ];

    for (const [label, code, mutate] of cases) {
        const doc = clone(base);
        mutate(doc);
        expectDiagnostic(ir, doc, code, label);
    }

    {
        const doc = clone(types);
        doc.tables[0].columns[3].default = { kind: "literal", type: "blob", value: "00FF" };
        assert(ir.validateDocument(doc).ok, "nullable typed defaults are valid in IR v1");
    }

    {
        const composite = clone(types);
        composite.queries = [];
        composite.tables[0].columns[1].primary_key = true;
        composite.tables[0].primary_key = ["id", "label"];
        assert(ir.validateDocument(composite).ok, "composite primary key must validate");

        composite.tables[0].columns[0].autoincrement = true;
        expectDiagnostic(ir, composite, "IR013", "autoincrement on composite primary key");
    }

    /* The informative JSON Schema stays aligned with the compiler contract */
    const schemaDoc = JSON.parse(readText(join(ESDB_ROOT, "orm", "ir", "esdb-ir-v1.schema.json")));
    assertEqual(schemaDoc.properties.ir_version.const, ir.IR_VERSION, "JSON Schema ir_version matches the compiler");
    assertEqual(schemaDoc.properties.schema.properties.dialect.const, "sqlite", "JSON Schema dialect matches");
    assertEqual(
        schemaDoc.$id,
        "https://esdb.dev/schemas/esdb-ir-v1.schema.json",
        "JSON Schema $id"
    );

    /* CLI surface */
    const fixturePath = join(FIXTURES_DIR, "user.ir.json");
    const okResult = runCli(["validate", fixturePath]);
    assertEqual(okResult.status, 0, `validate exit: ${okResult.stderr}`);

    const work = freshWorkDir("ir-validation");
    const invalid = clone(base);
    invalid.tables[0].columns[0].type = "bogus";
    const invalidPath = join(work, "invalid.json");
    writeJson(invalidPath, invalid);

    const jsonResult = runCli(["validate", invalidPath, "--json"]);
    assertEqual(jsonResult.status, 1, "invalid document must exit 1");
    const parsed = JSON.parse(jsonResult.stdout);
    assertEqual(parsed.ok, false, "json ok flag");
    assert(parsed.diagnostics.some((diagnostic) => diagnostic.code === "IR011"), "json diagnostics include IR011");

    const textResult = runCli(["validate", invalidPath]);
    assertEqual(textResult.status, 1, "text diagnostics exit 1");
    assert(textResult.stdout.includes("IR011"), "text diagnostics include IR011");

    const hashResult = runCli(["hash", invalidPath]);
    assertEqual(hashResult.status, 1, "hash of an invalid document must fail");
}
