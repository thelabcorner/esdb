/*
 * SQL identifier validation, injection rejection, bind-only SQL emission, and
 * migration packaging.
 */

import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

import {
    EXAMPLE_GENERATED,
    assert,
    assertEqual,
    assertThrows,
    clone,
    freshWorkDir,
    loadCompiler,
    loadFixture,
    readJson,
    readText,
    runCli,
    writeJson
} from "./helpers.mjs";

export async function run() {
    const { ir, sql, canonical } = await loadCompiler();
    const base = loadFixture("user.ir.json");
    const goldenIr = readJson(join(EXAMPLE_GENERATED, "user.ir.json"));
    const goldenHash = goldenIr.integrity.hash;

    /* Identifier validation and injection rejection */
    const injectionCases = [
        ["statement terminator", "name; DROP TABLE user; --"],
        ["comment marker", "name--"],
        ["double quote", 'na"me'],
        ["backtick", "na`me"],
        ["bracket", "na[me"],
        ["whitespace", "na me"],
        ["empty", ""],
        ["leading digit", "1name"],
        ["over length", `a${"b".repeat(64)}`],
        ["non-ascii", "名前"],
        ["newline", "na\nme"],
        ["null byte", "na\u0000me"]
    ];
    for (const [label, name] of injectionCases) {
        const doc = clone(base);
        doc.tables[0].columns[1].sql_name = name;
        const result = ir.validateDocument(doc);
        assert(!result.ok, `identifier "${label}" must be rejected`);
        assert(
            result.diagnostics.some((diagnostic) => diagnostic.code === "IR009"),
            `identifier "${label}" must fail IR009, got ${result.diagnostics.map((d) => d.code).join(",")}`
        );
    }

    for (const keyword of ["select", "table", "index", "where", "user"]) {
        const doc = clone(base);
        doc.tables[0].sql_name = keyword;
        const result = ir.validateDocument(doc);
        if (keyword === "user") {
            assert(result.ok, "the slice table name must not be a reserved word");
        } else {
            assert(
                result.diagnostics.some((diagnostic) => diagnostic.code === "IR028"),
                `keyword "${keyword}" must fail IR028`
            );
        }
    }

    assert(sql.isReservedWord("SELECT"), "reserved word check is case-insensitive");
    assert(!sql.isReservedWord("user"), "user is not reserved");
    assertEqual(sql.quoteIdentifier("user"), '"user"', "identifiers are double-quoted");

    /* Keyed mutation enforcement */
    for (const [label, mutate] of [
        ["update", (d) => {
            d.queries.find((query) => query.name === "user.updateName").where =
                [{ column: "name", op: "eq", param: "name" }];
        }],
        ["delete", (d) => {
            d.queries.find((query) => query.name === "user.deleteById").where =
                [{ column: "name", op: "eq", param: "name" }];
        }]
    ]) {
        const doc = clone(base);
        mutate(doc);
        const result = ir.validateDocument(doc);
        assert(!result.ok, `${label} must be rejected`);
        assert(result.diagnostics.some((diagnostic) => diagnostic.code === "IR020"), `${label} must fail IR020`);
    }

    /* Generated DDL */
    const pkg = sql.buildMigrationPackage(goldenIr, {
        compilerName: "esdb-schema",
        compilerVersion: "0.2.0",
        irHash: goldenHash
    });
    const initFile = pkg.files.find((file) => file.relPath.endsWith(".sql"));
    const oracleSql = initFile.content;
    assert(oracleSql.includes('CREATE TABLE "user" ('), "bootstrap oracle quotes the table identifier");
    assert(oracleSql.includes('"id" INTEGER NOT NULL'), "id column DDL");
    assert(oracleSql.includes('"name" TEXT NOT NULL'), "name column DDL");
    assert(oracleSql.includes('"email" TEXT NOT NULL'), "email is NOT NULL per the slice contract");
    assert(oracleSql.includes('"created_at" INTEGER NOT NULL'), "physical created_at column DDL");
    assert(oracleSql.includes('PRIMARY KEY ("id")'), "primary key constraint");
    assert(oracleSql.includes('UNIQUE ("email")'), "unique constraint");
    assert(!oracleSql.includes("AUTOINCREMENT"), "no AUTOINCREMENT for the slice");

    /* Manifest hashes the exact migration bytes */
    const manifestFile = pkg.files.find((file) => file.relPath.endsWith("manifest.json"));
    const manifest = JSON.parse(manifestFile.content);
    assertEqual(manifest.ir_hash, goldenHash, "manifest records the IR hash");
    assertEqual(manifest.migrations[0].sha256, canonical.sha256Hex(initFile.content), "manifest hashes the SQL file");
    assertEqual(manifestFile.content, canonical.canonicalize(manifest), "manifest is canonical JSON");
    /* buildMigrationPackage is the IR-derived bootstrap/oracle surface only.
     * Production migration bytes/manifest come from package-migrations + Drizzle Kit. */
    const productionManifest = JSON.parse(readFileSync(join(EXAMPLE_GENERATED, "migrations", "manifest.json"), "utf8"));
    assertEqual(productionManifest.formatVersion, "2", "production manifest is v2");
    assertEqual(productionManifest.kit.version, "0.31.11", "production manifest pins Drizzle Kit");
    assertEqual(productionManifest.irHash, `sha256:${goldenHash}`, "production manifest binds to the sealed IR");

    /* Query SQL is bind-only */
    for (const query of goldenIr.queries) {
        const table = goldenIr.tables.find((candidate) => candidate.name === query.table);
        const { sql: statement, params } = sql.emitQuerySql(table, query);
        const placeholders = (statement.match(/\?/g) || []).length;
        assertEqual(placeholders, params.length, `${query.name}: one placeholder per parameter`);
        assert(!statement.includes("'"), `${query.name}: no string literals`);
        assert(!statement.includes(";"), `${query.name}: no statement separators`);
        assert(!statement.includes("--"), `${query.name}: no comment markers`);
        assert(
            /^[A-Za-z0-9_",().=?\s]+$/.test(statement),
            `${query.name}: SQL contains only identifiers, keywords, and ? placeholders`
        );
        for (const param of params) {
            assert(!statement.includes(`:${param.name}`), `${query.name}: no named parameter :${param.name}`);
            assert(!statement.includes(`@${param.name}`), `${query.name}: no named parameter @${param.name}`);
        }
    }

    const findById = goldenIr.queries.find((query) => query.name === "user.findById");
    assertEqual(
        sql.emitQuerySql(goldenIr.tables[0], findById).sql,
        'SELECT "id","name","email","created_at" FROM "user" WHERE "id" = ?',
        "findById SQL shape"
    );

    /* Relational compile-time query breadth: joins + deterministic upsert. */
    const relational = clone(base);
    relational.tables.push({
        name: "profile",
        sql_name: "profile",
        columns: [
            { name: "id", sql_name: "id", type: "integer", nullable: false, primary_key: true, autoincrement: false, unique: false },
            { name: "userId", sql_name: "user_id", type: "integer", nullable: false, primary_key: false, autoincrement: false, unique: true },
            { name: "label", sql_name: "label", type: "text", nullable: false, primary_key: false, autoincrement: false, unique: false }
        ],
        primary_key: ["id"],
        uniques: [["userId"]],
        foreign_keys: [{
            name: "fk_profile_user",
            columns: ["userId"],
            references: { table: "user", columns: ["id"] },
            on_update: "no action",
            on_delete: "cascade"
        }]
    });
    relational.queries.push({
        name: "user.withProfile",
        kind: "select",
        table: "user",
        cardinality: "many",
        joins: [{
            kind: "left",
            table: "profile",
            on: [{ left: { table: "user", column: "id" }, right: { table: "profile", column: "userId" } }]
        }],
        where: [{ column: "id", op: "eq", param: "id" }],
        order_by: [{ table: "profile", column: "id", direction: "desc" }]
    });
    relational.queries.push({
        name: "profile.upsert",
        kind: "upsert",
        table: "profile",
        cardinality: "changes",
        values: [
            { column: "id", param: "id" },
            { column: "userId", param: "userId" },
            { column: "label", param: "label" }
        ],
        on_conflict: {
            target: ["userId"],
            action: "update",
            set: [{ column: "label", param: "nextLabel" }]
        }
    });
    relational.tables.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    relational.queries.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    const relationalValidation = ir.validateDocument(relational);
    assert(relationalValidation.ok, `relational IR must validate: ${JSON.stringify(relationalValidation.diagnostics)}`);
    const joinQuery = relational.queries.find((query) => query.name === "user.withProfile");
    const userTable = relational.tables.find((table) => table.name === "user");
    const profileTable = relational.tables.find((table) => table.name === "profile");
    const joinSql = sql.emitQuerySql(userTable, joinQuery, relational);
    assert(
        joinSql.sql.includes('LEFT JOIN "profile" ON "user"."id" = "profile"."user_id"'),
        "join SQL uses structural table/column references"
    );
    assert(joinSql.sql.includes('ORDER BY "profile"."id" DESC'), "joined-table ordering is qualified");
    const upsertQuery = relational.queries.find((query) => query.name === "profile.upsert");
    const upsertSql = sql.emitQuerySql(profileTable, upsertQuery, relational);
    assertEqual(
        upsertSql.sql,
        'INSERT INTO "profile" ("id","user_id","label") VALUES (?,?,?) ON CONFLICT ("user_id") DO UPDATE SET "label" = ?',
        "upsert SQL is deterministic and bind-only"
    );
    assertEqual(upsertSql.params.length, 4, "upsert bind order covers insert values then conflict update values");

    /* Literal emission */
    assertEqual(sql.literalToSql({ kind: "literal", type: "text", value: "it's" }), "'it''s'", "text literals double quotes");
    assertEqual(sql.literalToSql({ kind: "literal", type: "blob", value: "00FF" }), "X'00FF'", "blob literals are X'..'");
    assertEqual(sql.literalToSql({ kind: "literal", type: "integer", value: "7" }), "7", "integer literals");
    assertEqual(sql.literalToSql({ kind: "literal", type: "null", value: "" }), "NULL", "null literals");

    /* Invalid IR never reaches SQL emission through the CLI */
    const work = freshWorkDir("sql");
    const invalid = clone(base);
    invalid.tables[0].columns[1].sql_name = "name; DROP TABLE user; --";
    const invalidPath = join(work, "invalid.json");
    writeJson(invalidPath, invalid);
    const outDir = join(work, "out");
    const bootstrap = runCli(["bootstrap-migration", invalidPath, "--out", outDir]);
    assertEqual(bootstrap.status, 1, "bootstrap oracle refuses invalid IR");
    assert(!existsSync(outDir), "bootstrap oracle writes nothing for invalid IR");
}
