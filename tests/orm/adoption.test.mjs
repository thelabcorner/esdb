import { createRequire } from "node:module";
import { readFileSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import {
    adoptionReportFromKit,
    adoptionReportFromSqlFile,
    baselineArtifactBytes,
    createBaselineArtifact,
    loadBaselineArtifact
} from "../../tools/esdb-schema/src/adoption.mjs";
import { physicalSchemaHash } from "../../tools/esdb-schema/src/physical-schema.mjs";

import {
    assert,
    assertEqual,
    assertThrows,
    ESDB_ROOT,
    FIXTURES_DIR,
    freshWorkDir,
    readJson,
    runCli
} from "./helpers.mjs";

const require = createRequire(import.meta.url);

function createDb(path, sql, userVersion) {
    const { DatabaseSync } = require("node:sqlite");
    const db = new DatabaseSync(path);
    try {
        db.exec(sql);
        db.exec(`PRAGMA user_version=${userVersion}`);
    } finally {
        db.close();
    }
}

function workmarkBaselineAndReport() {
    const workDir = freshWorkDir("adoption-workmark");
    const dbPath = join(workDir, "workmark.sqlite");
    const sqlPath = join(FIXTURES_DIR, "workmark-initial.sql");
    const sql = readFileSync(sqlPath, "utf8");
    createDb(dbPath, sql, 7);

    const before = statSync(dbPath);
    const baselineA = createBaselineArtifact(dbPath);
    const baselineB = createBaselineArtifact(dbPath);
    const afterBaseline = statSync(dbPath);

    assertEqual(baselineA.format, "esdb.adoption-baseline/v1", "baseline format");
    assertEqual(baselineA.normalizer, "sqlite-introspection-v1", "baseline normalizer");
    assertEqual(baselineA.user_version, "7", "baseline user_version");
    assertEqual(
        baselineA.physical_schema_hash,
        "sha256:" + physicalSchemaHash(baselineA.physical_schema),
        "baseline hash must cover its physical snapshot"
    );
    assert(
        baselineArtifactBytes(baselineA).equals(baselineArtifactBytes(baselineB)),
        "baseline artifact bytes must be deterministic"
    );
    assertEqual(afterBaseline.size, before.size, "baseline creation must not change DB size");
    assertEqual(afterBaseline.mtimeMs, before.mtimeMs, "baseline creation must not modify DB");

    const report = adoptionReportFromSqlFile({
        dbPath,
        sqlPath,
        expectedUserVersion: "7",
        baselineArtifact: baselineA
    });
    const afterReport = statSync(dbPath);
    assert(report.match, "matching Workmark DB must be adoptable without mutation");
    assert(report.read_only, "adoption report must declare read-only inspection");
    assert(report.physical_diff.ok, "matching Workmark physical diff must be empty");
    assertEqual(report.actual.user_version, "7", "actual user_version in report");
    assertEqual(report.expected.user_version, "7", "expected user_version in report");
    assert(report.baseline_to_expected.match, "baseline must match the expected Workmark schema");
    assert(report.live_to_baseline.match, "live Workmark DB must match the captured baseline");
    assertEqual(afterReport.size, before.size, "adoption report must not change DB size");
    assertEqual(afterReport.mtimeMs, before.mtimeMs, "adoption report must not modify DB");

    const baselinePath = join(workDir, "baseline.json");
    writeFileSync(baselinePath, baselineArtifactBytes(baselineA));
    assert(statSync(baselinePath).size > 0, "baseline artifact is written separately from DB");
    const loaded = loadBaselineArtifact(baselinePath);
    assert(
        baselineArtifactBytes(loaded).equals(baselineArtifactBytes(baselineA)),
        "validated baseline must round-trip byte-identically"
    );
    assertThrows(
        () => baselineArtifactBytes({
            ...baselineA,
            physical_schema_hash: "sha256:" + "0".repeat(64)
        }),
        "physical hash mismatch",
        "tampered baseline hash must fail closed"
    );
    assertThrows(
        () => baselineArtifactBytes({ ...baselineA, unexpected: "field" }),
        "unknown adoption baseline field",
        "unknown baseline fields must fail closed"
    );
}

function mismatchIsDescriptiveOnly() {
    const workDir = freshWorkDir("adoption-mismatch");
    const dbPath = join(workDir, "workmark-mismatch.sqlite");
    const sqlPath = join(FIXTURES_DIR, "workmark-initial.sql");
    const sql = readFileSync(sqlPath, "utf8").replace(/\r\n/g, "\n").replace(
        "CREATE INDEX idx_applied_operations_applied_at\n  ON applied_operations(applied_at);",
        ""
    );
    createDb(dbPath, sql, 7);

    const before = statSync(dbPath);
    const report = adoptionReportFromSqlFile({
        dbPath,
        sqlPath,
        expectedUserVersion: "7"
    });
    const after = statSync(dbPath);

    assert(!report.match, "physical mismatch must fail adoption match");
    assert(!report.physical_diff.ok, "physical mismatch must be described");
    assert(
        report.physical_diff.mismatches.some(
            (item) => item.kind === "missing_object" &&
                item.object === "index:idx_applied_operations_applied_at"
        ),
        "report must identify the missing Workmark index"
    );
    assertEqual(after.size, before.size, "mismatch detection must not change DB size");
    assertEqual(after.mtimeMs, before.mtimeMs, "mismatch detection must not modify DB");
}

function userVersionMismatchIsSeparate() {
    const workDir = freshWorkDir("adoption-version");
    const dbPath = join(workDir, "workmark.sqlite");
    const sqlPath = join(FIXTURES_DIR, "workmark-initial.sql");
    createDb(dbPath, readFileSync(sqlPath, "utf8"), 6);

    const report = adoptionReportFromSqlFile({
        dbPath,
        sqlPath,
        expectedUserVersion: "7"
    });
    assert(report.physical_diff.ok, "schema may match when user_version does not");
    assert(!report.match, "user_version mismatch must prevent a full adoption match");
    assertEqual(report.user_version_mismatch.expected, "7", "expected version mismatch value");
    assertEqual(report.user_version_mismatch.actual, "6", "actual version mismatch value");
}

function drizzleKitAdoption() {
    const workDir = freshWorkDir("adoption-kit");
    const dbPath = join(workDir, "user.sqlite");
    const example = join(ESDB_ROOT, "examples", "orm", "user");
    const kitDir = join(example, "drizzle-migrations");
    const ir = readJson(join(example, "generated", "user.ir.json"));
    const sql = readFileSync(join(kitDir, "0000_clear_mephisto.sql"), "utf8");
    createDb(dbPath, sql, 1);

    const before = statSync(dbPath);
    const report = adoptionReportFromKit({
        dbPath,
        ir,
        kitDir,
        kitVersion: "0.31.11",
        baseline: 0
    });
    const after = statSync(dbPath);

    assert(report.match, "Drizzle Kit migration and live user DB must match");
    assert(report.ir_compatibility.compatible, "live Kit DB must satisfy the sealed IR contract");
    assertEqual(report.expected.user_version, "1", "Kit target user_version");
    assertEqual(after.size, before.size, "Kit adoption report must not change DB size");
    assertEqual(after.mtimeMs, before.mtimeMs, "Kit adoption report must not modify DB");
}

function cliSurface() {
    const workDir = freshWorkDir("adoption-cli");
    const dbPath = join(workDir, "workmark.sqlite");
    const sqlPath = join(FIXTURES_DIR, "workmark-initial.sql");
    const baselinePath = join(workDir, "baseline.json");
    const reportPath = join(workDir, "report.json");
    createDb(dbPath, readFileSync(sqlPath, "utf8"), 7);

    const baseline = runCli([
        "adoption-baseline",
        "--db", dbPath,
        "--out", baselinePath
    ]);
    assertEqual(baseline.status, 0, "adoption-baseline CLI exit status: " + baseline.stderr);
    const baselineJson = JSON.parse(readFileSync(baselinePath, "utf8"));
    assertEqual(baselineJson.user_version, "7", "CLI baseline user_version");

    const overwrite = runCli([
        "adoption-baseline",
        "--db", dbPath,
        "--out", baselinePath
    ]);
    assertEqual(overwrite.status, 1, "adoption-baseline must refuse to overwrite an existing baseline");

    const report = runCli([
        "adoption-report",
        "--db", dbPath,
        "--expected-sql", sqlPath,
        "--expected-user-version", "7",
        "--baseline-file", baselinePath,
        "--out", reportPath
    ]);
    assertEqual(report.status, 0, "adoption-report CLI exit status: " + report.stderr);
    const reportJson = JSON.parse(readFileSync(reportPath, "utf8"));
    assert(reportJson.match, "CLI adoption report must match canonical Workmark DB");
    assert(reportJson.baseline_to_expected.match, "CLI report baseline must match expected schema");
    assert(reportJson.live_to_baseline.match, "CLI report live DB must match captured baseline");

    const mismatch = runCli([
        "adoption-report",
        "--db", dbPath,
        "--expected-sql", sqlPath,
        "--expected-user-version", "8",
        "--json"
    ]);
    assertEqual(mismatch.status, 1, "CLI user_version mismatch must exit 1");
    const mismatchJson = JSON.parse(mismatch.stdout);
    assert(!mismatchJson.match, "CLI version mismatch report must not match");
    assertEqual(mismatchJson.user_version_mismatch.actual, "7", "CLI mismatch actual version");
    assertEqual(mismatchJson.user_version_mismatch.expected, "8", "CLI mismatch expected version");

    const dbBeforeUnsafeOut = statSync(dbPath);
    const unsafeOut = runCli([
        "adoption-report",
        "--db", dbPath,
        "--expected-sql", sqlPath,
        "--expected-user-version", "7",
        "--out", dbPath
    ]);
    const dbAfterUnsafeOut = statSync(dbPath);
    assertEqual(unsafeOut.status, 2, "adoption-report must reject --out resolving to the live database");
    assert(
        unsafeOut.stderr.includes("must not resolve to the inspected database"),
        "unsafe adoption-report output must explain the safety violation"
    );
    assertEqual(dbAfterUnsafeOut.size, dbBeforeUnsafeOut.size, "unsafe --out rejection must preserve DB size");
    assertEqual(dbAfterUnsafeOut.mtimeMs, dbBeforeUnsafeOut.mtimeMs, "unsafe --out rejection must preserve DB mtime");
}

export async function run() {
    workmarkBaselineAndReport();
    mismatchIsDescriptiveOnly();
    userVersionMismatchIsSeparate();
    drizzleKitAdoption();
    cliSurface();
}
