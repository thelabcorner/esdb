import { createRequire } from "node:module";
import { readFileSync, statSync } from "node:fs";
import { join } from "node:path";

import {
    canonicalSqlTokens,
    diffIrAgainstPhysical,
    diffKitSqlAgainstIr,
    diffPhysicalSchemas,
    physicalSchemaHash,
    snapshotDatabasePath,
    snapshotSql
} from "../../tools/esdb-schema/src/physical-schema.mjs";

import {
    assert,
    assertEqual,
    ESDB_ROOT,
    FIXTURES_DIR,
    freshWorkDir,
    readJson
} from "./helpers.mjs";

const require = createRequire(import.meta.url);

function changed(base, from, to) {
    const next = base.replace(from, to);
    assert(next !== base, `mutation source must exist: ${from}`);
    return next;
}

function fullWorkmarkRegression() {
    const workmarkSql = readFileSync(join(FIXTURES_DIR, "workmark-initial.sql"), "utf8");
    const workmark = snapshotSql(workmarkSql);
    assertEqual(workmark.tables.length, 25, "full Workmark table count");
    assertEqual(workmark.indexes.length, 34, "full Workmark explicit-index count");
    assertEqual(
        workmark.tables.reduce((count, table) => count + table.foreign_keys.length, 0),
        36,
        "full Workmark foreign-key count"
    );
    assertEqual(
        physicalSchemaHash(workmark),
        "29b0dfec10bea4fc60b53c6806661dc0166ba494c8cf2bade1379a0101d5e004",
        "full Workmark physical-schema hash must remain deterministic"
    );

    const mutations = [
        [
            "DESC/ASC",
            "ON document_incarnations(document_id, last_seen_at DESC);",
            "ON document_incarnations(document_id, last_seen_at ASC);",
            "index:idx_document_incarnations_document_seen"
        ],
        [
            "partial predicate",
            "ON documents(lineage_parent_id) WHERE lineage_parent_id IS NOT NULL;",
            "ON documents(lineage_parent_id) WHERE lineage_parent_id IS NULL;",
            "index:idx_documents_lineage_parent"
        ],
        [
            "default",
            "archived INTEGER NOT NULL DEFAULT 0",
            "archived INTEGER NOT NULL DEFAULT 1",
            "table:projects"
        ],
        [
            "CHECK expression",
            "CHECK(length(id) = 16), name TEXT NOT NULL",
            "CHECK(length(id) = 15), name TEXT NOT NULL",
            "table:projects"
        ],
        [
            "foreign-key target",
            "project_id BLOB REFERENCES projects(id),",
            "project_id BLOB REFERENCES settings(key),",
            "table:documents"
        ],
        [
            "composite primary-key order",
            "PRIMARY KEY(version_id, parent_id),",
            "PRIMARY KEY(parent_id, version_id),",
            "table:version_parents"
        ]
    ];

    for (const [label, from, to, expectedObject] of mutations) {
        const mutated = snapshotSql(changed(workmarkSql, from, to));
        const diff = diffPhysicalSchemas(workmark, mutated);
        assert(!diff.ok, `${label} mutation must be detected`);
        assert(
            diff.mismatches.some((item) => item.object === expectedObject),
            `${label} mutation must identify ${expectedObject}`
        );
    }

    const workDir = freshWorkDir("physical-schema-live");
    const dbPath = join(workDir, "workmark.sqlite");
    const { DatabaseSync } = require("node:sqlite");
    const db = new DatabaseSync(dbPath);
    db.exec(workmarkSql);
    db.close();
    const before = statSync(dbPath);

    const live = diffPhysicalSchemas(workmark, snapshotDatabasePath(dbPath));
    const after = statSync(dbPath);
    assert(live.ok, "read-only live Workmark DB must match the expected physical schema");
    assertEqual(after.size, before.size, "live DB inspection must not change database size");
    assertEqual(after.mtimeMs, before.mtimeMs, "live DB inspection must not modify the DB file");
}

const WORKMARK_STRESS_SQL = `
CREATE TABLE documents(
  id BLOB PRIMARY KEY CHECK(length(id) = 16),
  project_id BLOB,
  lineage_parent_id BLOB REFERENCES documents(id),
  CHECK (lineage_parent_id IS NULL OR lineage_parent_id != id)
);

CREATE TABLE versions(
  id BLOB PRIMARY KEY CHECK(length(id) = 16),
  document_id BLOB NOT NULL REFERENCES documents(id),
  created_on_branch_id BLOB NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(id, document_id),
  FOREIGN KEY(created_on_branch_id, document_id)
    REFERENCES branches(id, document_id)
);

CREATE TABLE branches(
  id BLOB PRIMARY KEY CHECK(length(id) = 16),
  document_id BLOB NOT NULL REFERENCES documents(id),
  head_version_id BLOB,
  created_at INTEGER NOT NULL,
  state INTEGER NOT NULL DEFAULT 0 CHECK(state BETWEEN 0 AND 2),
  UNIQUE(id, document_id),
  FOREIGN KEY(head_version_id, document_id)
    REFERENCES versions(id, document_id)
);

CREATE INDEX idx_documents_lineage_parent
  ON documents(lineage_parent_id) WHERE lineage_parent_id IS NOT NULL;
CREATE INDEX idx_versions_document_created
  ON versions(document_id, created_at DESC);
CREATE INDEX idx_branches_head
  ON branches(head_version_id) WHERE head_version_id IS NOT NULL;
`;

function formattingInvariant() {
    const left = snapshotSql(`
        CREATE TABLE "Thing" (
            "id" INTEGER NOT NULL,
            "value" TEXT DEFAULT 'A',
            PRIMARY KEY ("id"),
            CHECK (length("value") > 0)
        );
        CREATE INDEX "ix_value" ON "Thing" ("value" DESC)
            WHERE "value" IS NOT NULL;
    `);
    const right = snapshotSql(`
        -- formatting and quote-style differences are intentionally irrelevant
        create table [thing](
          [id] integer not null,
          [value] text default 'A',
          primary key([id]),
          check(length([value])>0)
        );
        create index [ix_value] on [thing]([value] desc)
          where [value] is not null;
    `);

    assertEqual(
        physicalSchemaHash(left),
        physicalSchemaHash(right),
        "physical hash must ignore SQL formatting/casing/identifier quote style"
    );
    assert(diffPhysicalSchemas(left, right).ok, "format-only schemas must compare equal");
}

function semanticSensitivity() {
    const base = snapshotSql(`
        CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
        CREATE TABLE child(
          a INTEGER NOT NULL,
          b INTEGER NOT NULL DEFAULT 0,
          CHECK(b > 0),
          FOREIGN KEY(a,b) REFERENCES parent(a,b)
            ON UPDATE CASCADE ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
        );
        CREATE INDEX ix_child ON child(a, b DESC) WHERE b IS NOT NULL;
    `);

    const cases = [
        [
            "CHECK expression",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 0,
              CHECK(b >= 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE CASCADE ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
            );
            CREATE INDEX ix_child ON child(a, b DESC) WHERE b IS NOT NULL;
            `
        ],
        [
            "partial-index predicate",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 0,
              CHECK(b > 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE CASCADE ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
            );
            CREATE INDEX ix_child ON child(a, b DESC) WHERE b > 0;
            `
        ],
        [
            "index DESC ordering",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 0,
              CHECK(b > 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE CASCADE ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
            );
            CREATE INDEX ix_child ON child(a, b ASC) WHERE b IS NOT NULL;
            `
        ],
        [
            "FK action",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 0,
              CHECK(b > 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE NO ACTION ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
            );
            CREATE INDEX ix_child ON child(a, b DESC) WHERE b IS NOT NULL;
            `
        ],
        [
            "FK deferrability",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 0,
              CHECK(b > 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE CASCADE ON DELETE RESTRICT NOT DEFERRABLE
            );
            CREATE INDEX ix_child ON child(a, b DESC) WHERE b IS NOT NULL;
            `
        ],
        [
            "composite PK ordinal",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(b,a));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 0,
              CHECK(b > 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE CASCADE ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
            );
            CREATE INDEX ix_child ON child(a, b DESC) WHERE b IS NOT NULL;
            `
        ],
        [
            "default value",
            `
            CREATE TABLE parent(a INTEGER, b INTEGER, PRIMARY KEY(a,b));
            CREATE TABLE child(
              a INTEGER NOT NULL,
              b INTEGER NOT NULL DEFAULT 1,
              CHECK(b > 0),
              FOREIGN KEY(a,b) REFERENCES parent(a,b)
                ON UPDATE CASCADE ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED
            );
            CREATE INDEX ix_child ON child(a, b DESC) WHERE b IS NOT NULL;
            `
        ]
    ];

    for (const [label, sql] of cases) {
        const changed = snapshotSql(sql);
        assert(
            physicalSchemaHash(changed) !== physicalSchemaHash(base),
            `${label} must affect physicalSchemaHash`
        );
        assert(
            !diffPhysicalSchemas(base, changed).ok,
            `${label} must produce a physical-schema differential`
        );
    }
}

function workmarkClassCoverage() {
    const snapshot = snapshotSql(WORKMARK_STRESS_SQL);
    assertEqual(snapshot.tables.length, 3, "Workmark stress schema table count");
    assertEqual(snapshot.indexes.length, 3, "Workmark stress schema index count");

    const versions = snapshot.tables.find((table) => table.name === "versions");
    const branches = snapshot.tables.find((table) => table.name === "branches");
    assert(versions !== undefined, "versions table must be present");
    assert(branches !== undefined, "branches table must be present");
    assert(versions.foreign_keys.length >= 2, "versions must preserve cyclic/composite FKs");
    assert(branches.foreign_keys.length >= 2, "branches must preserve cyclic/composite FKs");

    const desc = snapshot.indexes.find((index) => index.name === "idx_versions_document_created");
    assert(desc !== undefined, "DESC index must be present");
    assert(desc.columns[1].descending === true, "DESC indexed column must remain descending");

    const partial = snapshot.indexes.find((index) => index.name === "idx_documents_lineage_parent");
    assert(partial !== undefined && partial.partial, "partial index flag must be retained");
    assert(
        partial.definition.includes("kw:where"),
        "partial-index predicate must remain in the conservative definition signature"
    );

    const documentTable = snapshot.tables.find((table) => table.name === "documents");
    assert(
        documentTable.definition.includes("kw:check"),
        "CHECK expressions must remain in the conservative definition signature"
    );
}

function kitVsIrCompatibility() {
    const irPath = join(ESDB_ROOT, "examples", "orm", "user", "generated", "user.ir.json");
    const migrationPath = join(
        ESDB_ROOT,
        "examples",
        "orm",
        "user",
        "drizzle-migrations",
        "0000_clear_mephisto.sql"
    );
    const ir = readJson(irPath);
    const kitSql = readFileSync(migrationPath, "utf8");

    const diff = diffKitSqlAgainstIr(ir, kitSql);
    assert(diff.compatible, "existing Drizzle v1 slice must remain IR-compatible");
    assertEqual(diff.mismatches.length, 0, "existing Drizzle slice mismatch count");
    assert(
        diff.unmodeled.some((item) => item.object === "index:user_email_unique"),
        "named Kit unique index must remain visible as an unmodeled physical fact"
    );

    const weakened = snapshotSql(
        kitSql.replace("CREATE UNIQUE INDEX", "CREATE INDEX")
    );
    const mismatch = diffIrAgainstPhysical(ir, weakened);
    assert(!mismatch.compatible, "removing physical uniqueness must violate the IR contract");
    assert(
        mismatch.mismatches.some((item) => item.path === "$.unique"),
        "uniqueness mismatch must be described at object level"
    );
}

function tokenizerSafety() {
    const left = canonicalSqlTokens(
        "CHECK (x > 0) /* comment */ AND name = 'A''B'"
    );
    const right = canonicalSqlTokens(
        "check(x>0) and name='A''B' -- comment\n"
    );
    assertEqual(
        JSON.stringify(left),
        JSON.stringify(right),
        "conservative SQL tokenizer must ignore formatting/comments/case"
    );
}

export async function run() {
    formattingInvariant();
    semanticSensitivity();
    workmarkClassCoverage();
    fullWorkmarkRegression();
    kitVsIrCompatibility();
    tokenizerSafety();
}
