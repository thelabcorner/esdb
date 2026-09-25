/*
 * esdb-canonical-json-v1 and the semantic-projection hash contract.
 *
 * The contract (orm/ir/esdb-ir-v1.md §6): integrity.hash is the SHA-256 of the
 * canonical *projection* (integrity/generator/annotations removed), not of the
 * sealed file. These tests pin that down explicitly.
 */

import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import {
    assert,
    assertEqual,
    assertThrows,
    clone,
    freshWorkDir,
    loadCompiler,
    loadFixture,
    readText,
    runCli,
    sealFixture,
    sha256
} from "./helpers.mjs";

export async function run() {
    const { canonical } = await loadCompiler();
    const base = loadFixture("user.ir.json");

    /* Canonical form */
    assertEqual(canonical.canonicalize({ b: true, a: true }), '{"a":true,"b":true}', "object keys sort by code point");
    assertEqual(
        canonical.canonicalize({ s: 'a"b\\c\n\u0001' }),
        '{"s":"a\\"b\\\\c\\n\\u0001"}',
        "minimal string escaping"
    );
    assertEqual(
        canonical.canonicalize({ "\u{1F600}": true, "\uFFFF": true }),
        "{\"\uFFFF\":true,\"\u{1F600}\":true}",
        "astral keys sort by code point, not UTF-16 code unit"
    );
    assertThrows(() => canonical.canonicalize({ n: 1 }), "IR022", "JSON numbers are forbidden");
    assertThrows(() => canonical.canonicalize({ list: [1] }), "IR022", "nested JSON numbers are forbidden");
    assertThrows(() => canonical.canonicalize({ s: "\ud800" }), "IR024", "lone surrogates are rejected");
    assertThrows(() => canonical.canonicalize({ s: undefined }), "IR024", "undefined is rejected");

    /* Projection hash contract */
    const sealed = canonical.seal(base);
    const verification = canonical.verifySeal(sealed);
    assert(verification.ok, "sealed document verifies");
    assertEqual(sealed.integrity.hash, verification.expected, "embedded hash equals the computed projection hash");

    const sealedBytes = Buffer.from(canonical.canonicalize(sealed), "utf8");
    assert(
        sha256(sealedBytes) !== sealed.integrity.hash,
        "sealed file SHA-256 must differ from integrity.hash (the projection is the contract)"
    );
    assertEqual(
        sha256(sealedBytes),
        sha256(Buffer.from(canonical.canonicalize(canonical.seal(clone(base))), "utf8")),
        "sealing is byte-deterministic"
    );

    /* Non-semantic fields are excluded */
    const annotated = clone(base);
    annotated.annotations = { ...annotated.annotations, extra: "ignored" };
    annotated.generator = { ...annotated.generator, version: "9.9.9", name: "other" };
    assertEqual(canonical.hashProjection(annotated), canonical.hashProjection(base), "generator/annotations are excluded");

    const reordered = {};
    for (const key of Object.keys(base).reverse()) {
        reordered[key] = base[key];
    }
    assertEqual(canonical.hashProjection(reordered), canonical.hashProjection(base), "document key order does not affect the hash");

    /* Semantic changes change the hash */
    const semanticCases = [
        ["column nullable flag", (d) => { d.tables[0].columns[1].nullable = true; }],
        ["column type", (d) => { d.tables[0].columns[1].type = "real"; }],
        ["column added", (d) => {
            d.tables[0].columns.push({
                name: "note",
                sql_name: "note",
                type: "text",
                nullable: true,
                primary_key: false,
                autoincrement: false,
                unique: false
            });
        }],
        ["column order", (d) => {
            const columns = d.tables[0].columns;
            d.tables[0].columns = [columns[1], columns[0], columns[2], columns[3]];
        }],
        ["query cardinality", (d) => { d.queries[0].cardinality = "many"; }],
        ["query removed", (d) => { d.queries.pop(); }],
        ["unique key", (d) => { d.tables[0].uniques = []; d.tables[0].columns[2].unique = false; }],
        ["default added", (d) => {
            d.tables[0].columns[3].default = { kind: "literal", type: "integer", value: "0" };
        }]
    ];
    for (const [label, mutate] of semanticCases) {
        const doc = clone(base);
        mutate(doc);
        assert(canonical.hashProjection(doc) !== canonical.hashProjection(base), `hash changes for ${label}`);
    }

    /* File-level behavior */
    const work = freshWorkDir("canonical-hash");
    const sealedPath = sealFixture(base, work, "user");
    const text = readText(sealedPath);
    assert(!text.includes("\n"), "sealed file has no trailing newline or insignificant whitespace");
    const parsed = JSON.parse(text);
    assertEqual(canonical.canonicalize(parsed), text, "sealed file bytes are canonical");
    assertEqual(parsed.integrity.hash, canonical.hashProjection(parsed), "sealed file carries the projection hash");

    const secondPath = join(work, "user-again.sealed.json");
    const second = runCli(["seal", "--in", join(work, "user.raw.json"), "--out", secondPath]);
    assertEqual(second.status, 0, "second seal run succeeds");
    assert(readFileSync(sealedPath).equals(readFileSync(secondPath)), "seal is byte-deterministic across runs");

    const check = runCli(["hash", sealedPath, "--check"]);
    assertEqual(check.status, 0, `hash --check passes: ${check.stderr}`);
    const printed = runCli(["hash", sealedPath]);
    assertEqual(printed.stdout.trim(), parsed.integrity.hash, "hash command prints the projection hash");

    const tampered = clone(parsed);
    tampered.schema.namespace = "tampered";
    const tamperedPath = join(work, "tampered.json");
    writeFileSync(tamperedPath, JSON.stringify(tampered));
    const tamperedCheck = runCli(["hash", tamperedPath, "--check"]);
    assertEqual(tamperedCheck.status, 1, "tampered document fails hash --check");
    assert(tamperedCheck.stdout.includes("hash mismatch"), "tampered check reports the mismatch");

    const unsealed = runCli(["hash", join(work, "user.raw.json"), "--check"]);
    assertEqual(unsealed.status, 1, "unsealed document fails hash --check");
}
