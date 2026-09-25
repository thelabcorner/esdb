/*
 * Drizzle frontend: pinned dependencies, public-metadata extraction, derived
 * CRUD, declared-query merging, unsupported-construct diagnostics, and the
 * golden IR being the frontend's own output.
 *
 * Skips cleanly when node_modules has not been installed (see
 * orm/drizzle/package.json and examples/orm/user/package.json).
 */

import { existsSync, readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

import {
    DRIZZLE_MODULES,
    ESDB_ROOT,
    EXAMPLE_DIR,
    EXAMPLE_GENERATED,
    FRONTEND_CLI,
    FRONTEND_QUERIES,
    FRONTEND_SCHEMA,
    assert,
    assertDeepEqual,
    assertEqual,
    assertThrows,
    freshWorkDir,
    readJson,
    readText,
    runCli,
    runNode,
    skip
} from "./helpers.mjs";

export async function run() {
    if (!existsSync(DRIZZLE_MODULES)) {
        skip("orm/drizzle/node_modules is not installed; run `npm install` in orm/drizzle");
    }
    if (!existsSync(join(EXAMPLE_DIR, "node_modules"))) {
        skip("examples/orm/user/node_modules is not installed; run `npm install` in examples/orm/user");
    }

    /* Pinned versions */
    const frontendPackage = readJson(join(ESDB_ROOT, "orm", "drizzle", "package.json"));
    assertEqual(frontendPackage.dependencies["drizzle-orm"], "0.45.3", "drizzle-orm is pinned exactly");
    assertEqual(frontendPackage.devDependencies["drizzle-kit"], "0.31.11", "drizzle-kit is pinned exactly");

    /* End-to-end extraction reproduces the checked-in golden IR byte-for-byte */
    const work = freshWorkDir("drizzle-frontend");
    const rawPath = join(work, "user.ir.json");
    const extract = runNode(FRONTEND_CLI, [
        "--schema",
        FRONTEND_SCHEMA,
        "--queries",
        FRONTEND_QUERIES,
        "--schema-name",
        "user",
        "--namespace",
        "esdb.orm.user",
        "--out",
        rawPath
    ]);
    assertEqual(extract.status, 0, `frontend extraction: ${extract.stderr}`);
    assert(extract.stdout.includes("drizzle-orm pin: 0.45.3"), "extraction reports the pin");

    const sealedPath = join(work, "user.sealed.json");
    assertEqual(runCli(["seal", "--in", rawPath, "--out", sealedPath]).status, 0, "seal frontend output");
    assert(
        readFileSync(sealedPath).equals(readFileSync(join(EXAMPLE_GENERATED, "user.ir.json"))),
        "golden IR is exactly the sealed frontend output (not hand-authored)"
    );

    const golden = readJson(join(EXAMPLE_GENERATED, "user.ir.json"));
    assertEqual(golden.tables.length, 1, "one table");
    assertDeepEqual(
        golden.tables[0].columns.map((column) => [column.name, column.type, column.nullable, column.primary_key, column.unique]),
        [
            ["id", "integer", false, true, false],
            ["name", "text", false, false, false],
            ["email", "text", false, false, true],
            ["createdAt", "integer", false, false, false]
        ],
        "user columns match the slice contract (email is NOT NULL UNIQUE)"
    );
    assertDeepEqual(golden.tables[0].primary_key, ["id"], "primary key");
    assertDeepEqual(golden.tables[0].uniques, [["email"]], "unique key");
    assertDeepEqual(
        golden.queries.map((query) => query.name),
        ["user.deleteById", "user.findByEmail", "user.findById", "user.insert", "user.list", "user.updateName"],
        "derived CRUD plus the declared updateName query"
    );
    assertEqual(golden.generator.source, "examples/orm/user/schema/user.schema.ts", "generator source label");
    assertEqual(golden.annotations.frontend, "drizzle-orm@0.45.3", "frontend annotation");

    /* Adapter units against the real schema module */
    const adapter = await import(pathToFileURL(join(ESDB_ROOT, "orm", "drizzle", "src", "adapter.ts")).href);
    const derive = await import(pathToFileURL(join(ESDB_ROOT, "orm", "drizzle", "src", "derive.ts")).href);
    const build = await import(pathToFileURL(join(ESDB_ROOT, "orm", "drizzle", "src", "build.ts")).href);

    assertEqual(build.drizzleOrmPin(), "0.45.3", "runtime pin matches package.json");

    const schemaModule = await import(pathToFileURL(FRONTEND_SCHEMA).href);
    const tables = adapter.collectTables(schemaModule);
    assertDeepEqual(Object.keys(tables), ["user"], "schema exports exactly one Drizzle table");
    const extracted = adapter.extractTables(tables);
    assertEqual(extracted[0].name, "user", "adapter preserves the logical table/export name");
    assertEqual(extracted[0].sql_name, "user", "adapter preserves the configured SQLite table name");
    assertEqual(extracted[0].columns[2].unique, true, "column unique flag extracted");
    assertEqual(extracted[0].columns[3].name, "createdAt", "Drizzle property key is the logical column name");
    assertEqual(extracted[0].columns[3].sql_name, "created_at", "configured SQLite column name is the physical name");

    /* Unsupported constructs fail loudly (ORM codes) */
    const core = await import(pathToFileURL(join(DRIZZLE_MODULES, "drizzle-orm", "sqlite-core", "index.js")).href);
    const drizzleRoot = await import(pathToFileURL(join(DRIZZLE_MODULES, "drizzle-orm", "index.js")).href);

    const timestamp = core.sqliteTable("stamped", {
        at: core.integer("at", { mode: "timestamp" }).notNull().primaryKey()
    });
    assertThrows(() => adapter.extractTables({ stamped: timestamp }), "ORM001", "timestamp mode rejected");

    const booleanMode = core.sqliteTable("flags", {
        id: core.integer("id").primaryKey(),
        on: core.integer("on", { mode: "boolean" }).notNull()
    });
    assertThrows(() => adapter.extractTables({ flags: booleanMode }), "ORM001", "boolean mode rejected");

    const withDefaultFn = core.sqliteTable("runtime_default", {
        id: core.integer("id").primaryKey(),
        label: core.text("label").$defaultFn(() => "x")
    });
    assertThrows(() => adapter.extractTables({ runtime_default: withDefaultFn }), "ORM002", "runtime defaults rejected");

    const sqlDefault = core.sqliteTable("sql_default", {
        id: core.integer("id").primaryKey(),
        n: core.integer("n").default(drizzleRoot.sql`42`)
    });
    assertThrows(() => adapter.extractTables({ sql_default: sqlDefault }), "ORM003", "SQL expression defaults rejected");

    const orphanParent = core.sqliteTable("orphan_parent", {
        id: core.integer("id").primaryKey()
    });
    const orphanChild = core.sqliteTable("orphan_child", {
        id: core.integer("id").primaryKey(),
        parentId: core.integer("parent_id").references(() => orphanParent.id)
    });
    assertThrows(
        () => adapter.extractTables({ orphan_child: orphanChild }),
        "ORM004",
        "foreign-key targets must be exported in the same schema"
    );

    const noPk = core.sqliteTable("no_pk", { a: core.text("a").notNull() });
    assertThrows(() => adapter.extractTables({ no_pk: noPk }), "ORM006", "missing primary key rejected");

    /* Supported shapes */
    const composite = core.sqliteTable(
        "composite",
        { a: core.integer("a").notNull(), b: core.text("b").notNull() },
        (table) => [core.primaryKey({ columns: [table.a, table.b] })]
    );
    const compositeTables = adapter.extractTables({ composite });
    assertDeepEqual(compositeTables[0].primary_key, ["a", "b"], "composite primary key extracted");
    assert(compositeTables[0].columns.every((column) => column.primary_key), "composite pk flags set on member columns");

    const auto = core.sqliteTable("auto", { id: core.integer("id").primaryKey({ autoIncrement: true }) });
    assertEqual(adapter.extractTables({ auto })[0].columns[0].autoincrement, true, "autoincrement extracted");

    const defaults = core.sqliteTable("defaults", {
        id: core.integer("id").primaryKey(),
        n: core.integer("n").notNull().default(7),
        s: core.text("s").default("hi"),
        b: core.blob("b").default(Buffer.from([0, 255]))
    });
    const defaultColumns = adapter.extractTables({ defaults })[0].columns;
    assertDeepEqual(defaultColumns[1].default, { kind: "literal", type: "integer", value: "7" }, "integer default");
    assertDeepEqual(defaultColumns[2].default, { kind: "literal", type: "text", value: "hi" }, "text default");
    assertDeepEqual(defaultColumns[3].default, { kind: "literal", type: "blob", value: "00FF" }, "blob default");

    const uniqueIndex = core.sqliteTable(
        "unique_indexed",
        { id: core.integer("id").primaryKey(), code: core.text("code").notNull() },
        (table) => [core.uniqueIndex("unique_indexed_code").on(table.code)]
    );
    assertDeepEqual(adapter.extractTables({ unique_indexed: uniqueIndex })[0].uniques, [["code"]], "unique index becomes a unique key");

    const relationalParent = core.sqliteTable("rel_parent", {
        id: core.integer("id").primaryKey(),
        code: core.text("code").notNull().unique()
    });
    const relationalChild = core.sqliteTable(
        "rel_child",
        {
            id: core.integer("id").primaryKey(),
            parentId: core.integer("parent_id").notNull().references(() => relationalParent.id, {
                onDelete: "cascade",
                onUpdate: "restrict"
            }),
            seq: core.integer("seq").notNull(),
            label: core.text("label").notNull()
        },
        (table) => [
            core.index("idx_rel_child_parent_seq").on(table.parentId, drizzleRoot.desc(table.seq)),
            core.index("idx_rel_child_partial").on(table.parentId).where(drizzleRoot.sql`${table.seq} > 0`),
            core.check("ck_rel_child_seq", drizzleRoot.sql`${table.seq} >= 0`)
        ]
    );
    const relationalTables = adapter.extractTables({
        child: relationalChild,
        parent: relationalParent
    });
    const relationalChildIr = relationalTables.find((table) => table.name === "child");
    assertEqual(relationalTables.length, 2, "multi-table Drizzle schema extracted");
    assertEqual(relationalChildIr.foreign_keys.length, 1, "foreign key extracted");
    assertEqual(relationalChildIr.foreign_keys[0].references.table, "parent", "foreign-key target uses logical table name");
    assertEqual(relationalChildIr.foreign_keys[0].on_delete, "cascade", "foreign-key ON DELETE extracted");
    assertEqual(relationalChildIr.foreign_keys[0].on_update, "restrict", "foreign-key ON UPDATE extracted");
    assertEqual(relationalChildIr.checks[0].name, "ck_rel_child_seq", "named CHECK extracted");
    assertEqual(relationalChildIr.indexes.length, 2, "normal and partial indexes extracted");
    assertEqual(
        relationalChildIr.indexes.find((index) => index.name === "idx_rel_child_parent_seq").terms[1].direction,
        "desc",
        "DESC index term extracted"
    );
    assert(
        relationalChildIr.indexes.find((index) => index.name === "idx_rel_child_partial").where.includes("> 0"),
        "partial index predicate extracted through public SQLite dialect"
    );

    /* Derived queries and declared-query collisions */
    const derived = derive.deriveCrudQueries(extracted[0]);
    assertDeepEqual(
        derived.map((query) => query.name),
        ["user.findById", "user.findByEmail", "user.list", "user.insert", "user.deleteById"],
        "derived CRUD order"
    );
    assertThrows(
        () => derive.mergeDeclaredQueries(derived, [{ name: "user.findById" }]),
        "ORM010",
        "declared/derived collisions are rejected"
    );

    /* Drizzle imports stay isolated in adapter.ts; drizzle-kit is never imported */
    const srcDir = join(ESDB_ROOT, "orm", "drizzle", "src");
    for (const name of readdirSync(srcDir)) {
        const text = readText(join(srcDir, name));
        assert(!/from\s+"drizzle-kit"/.test(text), `${name} must not import drizzle-kit`);
        assert(!text.includes('require("drizzle-kit")'), `${name} must not require drizzle-kit`);
        if (name !== "adapter.ts") {
            assert(!text.includes('from "drizzle-orm'), `${name} must not import drizzle-orm directly`);
        }
    }
}
