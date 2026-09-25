import vm from "node:vm";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

import {
    DRIZZLE_MODULES,
    ESDB_ROOT,
    assert,
    assertDeepEqual,
    assertEqual,
    assertTreesEqual,
    clone,
    expectDiagnostic,
    freshWorkDir,
    loadCompiler,
    readJson,
    readText,
    runCli,
    sealFixture,
    skip
} from "./helpers.mjs";

const EXACT_INT64 = "9007199254740993";

async function loadDrizzle() {
    if (!existsSync(DRIZZLE_MODULES)) {
        skip("orm/drizzle/node_modules is not installed; run `npm install` in orm/drizzle");
    }
    const core = await import(pathToFileURL(join(DRIZZLE_MODULES, "drizzle-orm", "sqlite-core", "index.js")).href);
    const root = await import(pathToFileURL(join(DRIZZLE_MODULES, "drizzle-orm", "index.js")).href);
    const adapter = await import(pathToFileURL(join(ESDB_ROOT, "orm", "drizzle", "src", "adapter.ts")).href);
    const build = await import(pathToFileURL(join(ESDB_ROOT, "orm", "drizzle", "src", "build.ts")).href);
    return { adapter, build, core, root };
}

function makeTables(core, root) {
    const { blob, check, foreignKey, index, integer, sqliteTable, text, unique } = core;
    const { desc, sql } = root;

    const projects = sqliteTable("projects", {
        id: blob("id").primaryKey(),
        name: text("name").notNull(),
        createdAt: integer("created_at").notNull(),
        archived: integer("archived").notNull().default(0)
    }, (table) => [
        index("idx_projects_archived").on(table.archived)
    ]);

    const documents = sqliteTable("documents", {
        id: blob("id").primaryKey(),
        projectId: blob("project_id"),
        displayName: text("display_name").notNull(),
        createdAt: integer("created_at").notNull(),
        lastSeenAt: integer("last_seen_at")
    }, (table) => [
        foreignKey({
            name: "documents_project_id_fk",
            columns: [table.projectId],
            foreignColumns: [projects.id]
        }).onUpdate("no action").onDelete("set null"),
        index("idx_documents_project").on(table.projectId),
        index("idx_documents_last_seen").on(table.lastSeenAt).where(sql`${table.lastSeenAt} IS NOT NULL`),
        check("ck_documents_last_seen", sql`${table.lastSeenAt} IS NULL OR ${table.lastSeenAt} >= ${table.createdAt}`)
    ]);

    const versions = sqliteTable("versions", {
        id: blob("id").primaryKey(),
        documentId: blob("document_id").notNull(),
        rootManifestObjectId: blob("root_manifest_object_id"),
        createdAt: integer("created_at").notNull(),
        reason: integer("reason").notNull(),
        level: integer("level").notNull(),
        label: text("label"),
        description: text("description"),
        integrityState: integer("integrity_state").notNull().default(0)
    }, (table) => [
        foreignKey({
            name: "versions_document_id_fk",
            columns: [table.documentId],
            foreignColumns: [documents.id]
        }).onUpdate("restrict").onDelete("cascade"),
        unique("versions_id_document_unique").on(table.id, table.documentId),
        index("idx_versions_document_created").on(table.documentId, desc(table.createdAt)),
        check("ck_versions_level", sql`${table.level} >= 0`)
    ]);

    const branches = sqliteTable("branches", {
        id: blob("id").primaryKey(),
        documentId: blob("document_id").notNull(),
        name: text("name").notNull(),
        headVersionId: blob("head_version_id"),
        createdFromVersionId: blob("created_from_version_id"),
        createdAt: integer("created_at").notNull(),
        state: integer("state").notNull().default(0),
        deletedAt: integer("deleted_at"),
        autoNamed: integer("auto_named").notNull().default(1)
    }, (table) => [
        foreignKey({
            name: "branches_document_id_fk",
            columns: [table.documentId],
            foreignColumns: [documents.id]
        }).onUpdate("restrict").onDelete("cascade"),
        foreignKey({
            name: "branches_head_version_fk",
            columns: [table.headVersionId, table.documentId],
            foreignColumns: [versions.id, versions.documentId]
        }).onUpdate("cascade").onDelete("restrict"),
        index("idx_branches_head").on(table.headVersionId).where(sql`${table.headVersionId} IS NOT NULL`)
    ]);

    return { branches, documents, projects, versions };
}

function declaredQueries() {
    return [
        {
            name: "versions.byProjectName",
            kind: "select",
            table: "versions",
            cardinality: "many",
            joins: [
                {
                    kind: "inner",
                    table: "documents",
                    on: [{
                        left: { table: "versions", column: "documentId" },
                        right: { table: "documents", column: "id" }
                    }]
                },
                {
                    kind: "inner",
                    table: "projects",
                    on: [{
                        left: { table: "documents", column: "projectId" },
                        right: { table: "projects", column: "id" }
                    }]
                }
            ],
            where: [{ table: "projects", column: "name", op: "eq", param: "projectName" }],
            order_by: [{ table: "versions", column: "createdAt", direction: "desc" }]
        },
        {
            name: "versions.upsert",
            kind: "upsert",
            table: "versions",
            cardinality: "changes",
            values: [
                { column: "id", param: "id" },
                { column: "documentId", param: "documentId" },
                { column: "rootManifestObjectId", param: "rootManifestObjectId" },
                { column: "createdAt", param: "createdAt" },
                { column: "reason", param: "reason" },
                { column: "level", param: "level" },
                { column: "label", param: "label" },
                { column: "description", param: "description" }
            ],
            on_conflict: {
                target: ["id", "documentId"],
                action: "update",
                set: [
                    { column: "rootManifestObjectId", param: "nextRootManifestObjectId" },
                    { column: "level", param: "nextLevel" },
                    { column: "label", param: "nextLabel" }
                ]
            }
        }
    ];
}

function loadFacade(code, bridge) {
    const sandbox = {
        ExternalObject: function () {
            return bridge;
        },
        Error,
        String,
        Number,
        Math,
        isFinite,
        parseInt
    };
    vm.runInNewContext(code, sandbox);
    return sandbox.ESDB_ORM_WORKMARK;
}

export async function run() {
    const { adapter, build, core, root } = await loadDrizzle();
    const tables = makeTables(core, root);
    const extracted = adapter.extractTables(tables);
    const document = build.buildIr({
        tables,
        declaredQueries: declaredQueries(),
        schemaName: "workmark",
        namespace: "esdb.orm.workmark",
        source: "tests/orm/workmark-relational.fixture"
    });

    assertDeepEqual(
        extracted.map((table) => table.name),
        ["branches", "documents", "projects", "versions"],
        "Workmark-derived tables are extracted in deterministic logical order"
    );
    assertDeepEqual(
        document.tables.map((table) => table.name),
        ["branches", "documents", "projects", "versions"],
        "the assembled IR preserves deterministic table order"
    );

    const branches = document.tables.find((table) => table.name === "branches");
    const documents = document.tables.find((table) => table.name === "documents");
    const versions = document.tables.find((table) => table.name === "versions");
    const headForeignKey = branches.foreign_keys.find((foreignKey) => foreignKey.name === "branches_head_version_fk");
    assertDeepEqual(headForeignKey.columns, ["headVersionId", "documentId"], "composite Workmark FK local order is preserved");
    assertDeepEqual(headForeignKey.references, { table: "versions", columns: ["id", "documentId"] }, "composite Workmark FK target is preserved");
    assertEqual(headForeignKey.on_update, "cascade", "composite FK update action is extracted");
    assertEqual(headForeignKey.on_delete, "restrict", "composite FK delete action is extracted");
    assertDeepEqual(
        versions.uniques.find((key) => key.length === 2),
        ["id", "documentId"],
        "Workmark composite uniqueness is extracted"
    );
    assertEqual(
        versions.indexes.find((index) => index.name === "idx_versions_document_created").terms[1].direction,
        "desc",
        "Workmark explicit DESC index is extracted"
    );
    assert(
        branches.indexes.find((index) => index.name === "idx_branches_head").where.includes("IS NOT NULL"),
        "Workmark partial index predicate is extracted"
    );
    assert(
        documents.checks.find((check) => check.name === "ck_documents_last_seen").sql.includes("last_seen_at"),
        "Workmark nullable CHECK expression is extracted"
    );
    assertEqual(versions.columns.find((column) => column.name === "createdAt").nullable, false, "exact int64 column is NOT NULL");
    assertEqual(documents.columns.find((column) => column.name === "lastSeenAt").nullable, true, "Workmark nullable int64 column is nullable");
    assertEqual(versions.columns.find((column) => column.name === "rootManifestObjectId").nullable, true, "Workmark nullable BLOB column is nullable");

    const { ir, physical, sql } = await loadCompiler();
    const validation = ir.validateDocument(document);
    assert(validation.ok, `Workmark-derived IR validates: ${JSON.stringify(validation.diagnostics)}`);

    const missingInsert = clone(document);
    const documentsInsert = missingInsert.queries.find((query) => query.name === "documents.insert");
    documentsInsert.values = documentsInsert.values.filter((value) => value.column !== "displayName");
    expectDiagnostic(ir, missingInsert, "IR019", "insert requires NOT NULL columns without defaults");

    const missingUpsertValue = clone(document);
    const versionsUpsert = missingUpsertValue.queries.find((query) => query.name === "versions.upsert");
    versionsUpsert.values = versionsUpsert.values.filter((value) => value.column !== "createdAt");
    expectDiagnostic(ir, missingUpsertValue, "IR019", "upsert requires NOT NULL columns without defaults");

    const nullableOmitted = clone(document);
    const nullableUpsert = nullableOmitted.queries.find((query) => query.name === "versions.upsert");
    nullableUpsert.values = nullableUpsert.values.filter((value) => !["rootManifestObjectId", "label", "description"].includes(value.column));
    assert(ir.validateDocument(nullableOmitted).ok, "nullable upsert values may be omitted");

    const missingWhere = clone(document);
    delete missingWhere.queries.find((query) => query.name === "documents.deleteById").where;
    expectDiagnostic(ir, missingWhere, "IR020", "delete requires a keyed where clause");

    const unkeyedOne = clone(document);
    const branchFind = unkeyedOne.queries.find((query) => query.name === "branches.findById");
    branchFind.where[0].column = "name";
    expectDiagnostic(ir, unkeyedOne, "IR021", "cardinality-one select requires a key or unique where clause");

    const missingConflict = clone(document);
    delete missingConflict.queries.find((query) => query.name === "versions.upsert").on_conflict;
    expectDiagnostic(ir, missingConflict, "IR040", "upsert requires an explicit conflict policy");

    const sameTableCollision = clone(document);
    const collidingQuery = sameTableCollision.queries.find((query) => query.name === "branches.findById");
    const originalQuery = clone(collidingQuery);
    collidingQuery.name = "branches.find_by_id";
    sameTableCollision.queries.splice(sameTableCollision.queries.indexOf(collidingQuery) + 1, 0, originalQuery);
    expectDiagnostic(ir, sameTableCollision, "IR032", "method-name collisions remain rejected within one table");

    const work = freshWorkDir("workmark-relational");
    const sealedPath = sealFixture(document, work, "workmark");
    const sealed = readJson(sealedPath);
    const versionsTable = sealed.tables.find((table) => table.name === "versions");
    const joinQuery = sealed.queries.find((query) => query.name === "versions.byProjectName");
    const upsertQuery = sealed.queries.find((query) => query.name === "versions.upsert");
    const joinStatement = sql.emitQuerySql(versionsTable, joinQuery, sealed);
    assertEqual(
        joinStatement.sql,
        'SELECT "versions"."id","versions"."document_id","versions"."root_manifest_object_id","versions"."created_at","versions"."reason","versions"."level","versions"."label","versions"."description","versions"."integrity_state" FROM "versions" INNER JOIN "documents" ON "versions"."document_id" = "documents"."id" INNER JOIN "projects" ON "documents"."project_id" = "projects"."id" WHERE "projects"."name" = ? ORDER BY "versions"."created_at" DESC',
        "Workmark join SQL is explicit, fully qualified, and base-row-only"
    );
    assertDeepEqual(
        joinStatement.params.map((param) => [param.name, param.table, param.column, param.type]),
        [["projectName", "projects", "name", "text"]],
        "joined predicate keeps its owning table and type"
    );
    const upsertStatement = sql.emitQuerySql(versionsTable, upsertQuery, sealed);
    assertEqual(
        upsertStatement.sql,
        'INSERT INTO "versions" ("id","document_id","root_manifest_object_id","created_at","reason","level","label","description") VALUES (?,?,?,?,?,?,?,?) ON CONFLICT ("id","document_id") DO UPDATE SET "root_manifest_object_id" = ?, "level" = ?, "label" = ?',
        "Workmark upsert SQL has a deterministic composite conflict target"
    );
    const expectedUpsertParams = [
        "id",
        "documentId",
        "rootManifestObjectId",
        "createdAt",
        "reason",
        "level",
        "label",
        "description",
        "nextRootManifestObjectId",
        "nextLevel",
        "nextLabel"
    ];
    assertDeepEqual(upsertStatement.params.map((param) => param.name), expectedUpsertParams, "upsert bind order is values then conflict updates");
    assertDeepEqual(
        ir.deriveApiParams(versionsTable, upsertQuery, sealed).map((param) => param.name),
        ir.deriveBindParams(versionsTable, upsertQuery, sealed).map((param) => param.name),
        "upsert API and bind order remain identical"
    );
    assertEqual(
        ir.deriveBindParams(versionsTable, upsertQuery, sealed).find((param) => param.name === "createdAt").type,
        "integer",
        "exact int64 parameter retains the IR integer type"
    );

    const schemaSql = sql.emitSchemaSql(sealed);
    const expectedPhysical = physical.snapshotIr(sealed);
    const compatible = physical.diffIrAgainstPhysical(sealed, expectedPhysical);
    assert(compatible.compatible, `Workmark physical self-differential is compatible: ${JSON.stringify(compatible.mismatches)}`);
    assertEqual(
        compatible.unmodeled.filter((item) => ["foreign_keys", "checks", "index"].includes(item.kind)).length,
        0,
        "Workmark FKs, CHECKs, and indexes are modeled physical facts"
    );

    const outDir = join(work, "generated");
    assertEqual(runCli(["build", sealedPath, "--out", outDir]).status, 0, "Workmark-derived build succeeds");
    assertEqual(runCli(["generate", sealedPath, "--out", outDir, "--check"]).status, 0, "Workmark-derived generated tree passes --check");
    const secondDir = join(work, "generated-again");
    assertEqual(runCli(["build", sealedPath, "--out", secondDir]).status, 0, "second Workmark-derived build succeeds");
    assertTreesEqual(outDir, secondDir, "Workmark-derived C++/TS/ES3 generation is byte-deterministic");

    const cpp = readText(join(outDir, "cpp", "workmark_repository.hpp"));
    assert(cpp.includes("class BranchesRepository"), "C++ emits branches repository");
    assert(cpp.includes("class DocumentsRepository"), "C++ emits documents repository");
    assert(cpp.includes("class ProjectsRepository"), "C++ emits projects repository");
    assert(cpp.includes("class VersionsRepository"), "C++ emits versions repository");
    assert(cpp.includes("std::int64_t created_at;"), "C++ preserves exact int64 row fields");
    assert(cpp.includes("bool root_manifest_object_id_is_null;"), "C++ preserves nullable BLOB state");
    assert(cpp.includes("sqlite3_prepare_v3"), "C++ uses SQLite prepared statements");
    assert(cpp.includes("SQLITE_PREPARE_PERSISTENT"), "C++ prepares persistent statements");
    assert(cpp.includes("sqlite3_bind_int64"), "C++ binds exact integers through the int64 API");
    assert(cpp.includes("ON CONFLICT"), "C++ embeds the fixed Workmark upsert SQL");
    assert(!cpp.includes("std::optional"), "C++ output remains C++11-compatible without std::optional");

    const tsPath = join(outDir, "ts", "workmark.ts");
    const tsText = readText(tsPath);
    assert(/export interface DocumentsRowExact \{[\s\S]*?createdAt: string;[\s\S]*?lastSeenAt: string \| null;/.test(tsText), "TS exact int64 row preserves nullable INTEGER state");
    assert(tsText.includes("rootManifestObjectId: string | null;"), "TS exact row preserves nullable BLOB state");
    const tsModule = await import(pathToFileURL(tsPath).href);
    assertEqual(tsModule.toInt64Param(EXACT_INT64), EXACT_INT64, "TS accepts exact decimal int64 parameters");
    assertEqual(tsModule.fromInt64Param(EXACT_INT64, true), EXACT_INT64, "TS returns exact decimal int64 rows in exact mode");
    const tsUpsert = tsModule.WORKMARK_QUERIES.find((query) => query.name === "versions.upsert");
    assertDeepEqual(tsUpsert.apiParams.map((param) => param.name), expectedUpsertParams, "TS records ergonomic upsert API order");
    assertDeepEqual(tsUpsert.bindParams.map((param) => param.name), expectedUpsertParams, "TS records SQLite upsert bind order");
    assertEqual(tsUpsert.apiParams.find((param) => param.name === "rootManifestObjectId").nullable, true, "TS records nullable upsert metadata");

    const bridge = readJson(join(outDir, "bridge", "workmark_orm_bridge.json"));
    const bridgeNames = bridge.operations.map((operation) => operation.name);
    assertEqual(new Set(bridgeNames).size, bridgeNames.length, "multi-table bridge operation names are unique");
    assert(bridgeNames.includes("workmarkBranchesFindById"), "multi-table bridge names include the table prefix");
    assert(bridgeNames.includes("workmarkDocumentsFindById"), "multi-table bridge names disambiguate repository operations");
    const bridgeUpsert = bridge.operations.find((operation) => operation.name === "workmarkVersionsUpsert");
    assertDeepEqual(bridgeUpsert.api_params.map((param) => param.name), expectedUpsertParams, "bridge records upsert API order");
    assertDeepEqual(bridgeUpsert.bind_params.map((param) => param.name), expectedUpsertParams, "bridge records upsert bind order");
    assertEqual(bridgeUpsert.params.find((param) => param.name === "createdAt").nullable, false, "bridge marks exact int64 parameter non-null");
    assertEqual(bridgeUpsert.params.find((param) => param.name === "rootManifestObjectId").nullable, true, "bridge marks nullable upsert parameter");

    const es3 = readText(join(outDir, "es3", "workmark_repository.jsx"));
    assert(es3.includes('"versionsByProjectName"'), "ES3 emits the table-qualified Workmark join operation");
    assert(es3.includes('"versionsUpsert"'), "ES3 emits the table-qualified Workmark upsert operation");
    assert(es3.includes("nullable: true"), "ES3 records nullable upsert parameters");
    assert(!/\bSELECT\b/i.test(es3), "ES3 output contains no SQL text");

    const calls = [];
    const fakeBridge = {
        ormVersion: () => bridge.ir_hash,
        ormPing: () => 42,
        ormOpen: () => "U1:H:7",
        ormClose: () => "U1:C:1",
        ormLastError: () => "U1:O",
        unload: () => {},
        workmarkVersionsByProjectName: (...args) => {
            calls.push(["workmarkVersionsByProjectName", ...args]);
            return "U1:L:0";
        },
        workmarkVersionsUpsert: (...args) => {
            calls.push(["workmarkVersionsUpsert", ...args]);
            return "U1:C:1";
        }
    };
    const api = loadFacade(es3, fakeBridge);
    api.load("lib:fake");
    const handle = api.open("C:/tmp/workmark.sqlite");
    const repository = api.repository(handle);
    assertDeepEqual(repository.versionsByProjectName("Project"), [], "ES3 join operation returns an empty page");
    assertEqual(
        repository.versionsUpsert({
            id: "0102",
            documentId: "AABB",
            rootManifestObjectId: null,
            createdAt: EXACT_INT64,
            reason: 1,
            level: 2,
            label: null,
            description: null,
            nextRootManifestObjectId: null,
            nextLevel: 3,
            nextLabel: "next"
        }),
        1,
        "ES3 upsert returns changes"
    );
    assertDeepEqual(
        calls.find((call) => call[0] === "workmarkVersionsByProjectName"),
        ["workmarkVersionsByProjectName", 7, "t:50726F6A656374"],
        "ES3 join parameter uses the canonical text lane"
    );
    assertDeepEqual(
        calls.find((call) => call[0] === "workmarkVersionsUpsert"),
        [
            "workmarkVersionsUpsert",
            7,
            "b:0102",
            "b:AABB",
            "n",
            "i:9007199254740993",
            "i:1",
            "i:2",
            "n",
            "n",
            "n",
            "i:3",
            "t:6E657874"
        ],
        "ES3 upsert preserves exact int64, NULL, and API argument order"
    );
    api.close(handle);
    api.unload();
}
