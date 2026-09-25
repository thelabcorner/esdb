/*
 * Drizzle Kit migration packaging: lint, deterministic normalization, version
 * pinning, destructive acknowledgements, generated wrapper, and drift checks.
 */

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

import {
    ESDB_ROOT,
    EXAMPLE_DIR,
    EXAMPLE_GENERATED,
    assert,
    assertEqual,
    assertThrows,
    freshWorkDir,
    readJson,
    runCli
} from "./helpers.mjs";

export async function run() {
    const kit = await import(pathToFileURL(join(ESDB_ROOT, "tools", "esdb-schema", "src", "kit-migrations.mjs")).href);
    const lint = await import(pathToFileURL(join(ESDB_ROOT, "tools", "esdb-schema", "src", "migration-lint.mjs")).href);
    const ir = readJson(join(EXAMPLE_GENERATED, "user.ir.json"));

    assertEqual(kit.SUPPORTED_DRIZZLE_KIT_VERSION, "0.31.11", "Drizzle Kit production pin");
    assertEqual(kit.normalizeMigrationSql("SELECT 1;\r\n\r\n"), "SELECT 1;\n", "SQL normalization is LF + one trailing newline");
    assertThrows(
        () => kit.normalizeMigrationSql(Buffer.from([0xc3, 0x28])),
        "MIG008",
        "invalid UTF-8 migration input is rejected"
    );
    assertThrows(
        () => kit.normalizeMigrationSql(Buffer.from([0xef, 0xbb, 0xbf, 0x53, 0x45, 0x4c, 0x45, 0x43, 0x54])),
        "MIG008",
        "UTF-8 BOM migration input is rejected"
    );
    assertThrows(
        () => kit.normalizeMigrationSql("SELECT '\0';"),
        "MIG008",
        "NUL migration input is rejected before C-string embedding"
    );

    for (const sql of [
        "BEGIN; CREATE TABLE x(id INTEGER);",
        "END;",
        "PRAGMA user_version=1;",
        "PRAGMA journal_mode=WAL;",
        "PRAGMA locking_mode=EXCLUSIVE;",
        "PRAGMA schema_version=1;",
        "ATTACH DATABASE 'x' AS x;",
        "DETACH DATABASE x;",
        "VACUUM;",
        "CREATE TEMP TABLE x(id INTEGER);",
        "SELECT * FROM sqlite_master;"
    ]) {
        assert(!lint.lintMigrationSql(sql).ok, `migration policy rejects: ${sql}`);
    }
    assert(lint.lintMigrationSql("CREATE TABLE x(v TEXT DEFAULT 'BEGIN;');").ok, "keywords inside SQL strings are ignored");
    for (const malformed of [
        "SELECT 'unterminated",
        "CREATE TABLE \"unterminated (id INTEGER);",
        "/* unterminated"
    ]) {
        assertThrows(
            () => lint.lintMigrationSql(malformed),
            "MIG007",
            `malformed SQL is rejected by migration lint: ${malformed}`
        );
    }

    const work = freshWorkDir("migration-packaging");
    const kitDir = join(work, "kit");
    mkdirSync(join(kitDir, "meta"), { recursive: true });
    writeFileSync(join(kitDir, "0000_init.sql"), "CREATE TABLE \"user\" (\"id\" INTEGER PRIMARY KEY);\r\n");
    writeFileSync(join(kitDir, "meta", "0000_snapshot.json"), "{\"id\":\"snapshot\"}\n");

    const packaged = kit.packageKitMigrations({
        ir,
        kitDir,
        baseline: 0,
        kitVersion: "0.31.11"
    });
    assertEqual(packaged.manifest.formatVersion, "2", "manifest v2");
    assertEqual(packaged.manifest.baseline, "0", "baseline serialized canonically");
    assertEqual(packaged.manifest.targetVersion, "1", "target user_version");
    assertEqual(packaged.manifest.kit.tool, "drizzle-kit-cli", "manifest names CLI authority");
    assertEqual(packaged.manifest.kit.version, "0.31.11", "manifest records pinned Kit version");
    assertEqual(packaged.manifest.irHash, `sha256:${ir.integrity.hash}`, "manifest binds sealed IR");
    assertEqual(
        packaged.manifest.physicalSchema.normalizerVersion,
        "sqlite-introspection-v1",
        "manifest pins physical-schema normalizer"
    );
    assertEqual(packaged.manifest.physicalSchema.basis, "full-kit-chain", "baseline-zero physical hash basis");
    assert(
        /^sha256:[0-9a-f]{64}$/.test(packaged.manifest.physicalSchema.hash),
        "baseline-zero package records realized physical schema hash"
    );
    assertEqual(
        packaged.physicalSchema.hash,
        packaged.manifest.physicalSchema.hash,
        "returned physical identity matches manifest"
    );
    assert(packaged.files.some((file) => file.relPath === "migrations/user_migrations.hpp"), "C++ migration wrapper emitted");
    const wrapper = packaged.files.find((file) => file.relPath === "migrations/user_migrations.hpp").content;
    assert(wrapper.includes("esdb_migrate"), "wrapper delegates transaction/version semantics to ESDB Runtime");
    assert(wrapper.includes("physical_schema_normalizer()"), "wrapper exports physical normalizer identity");
    assert(wrapper.includes("has_expected_physical_schema() { return true; }"), "baseline-zero wrapper exposes resolved physical identity");
    assert(wrapper.includes("expected_physical_schema_hash()"), "wrapper exports expected physical hash");
    assert(wrapper.includes("target_version() { return 1u; }"), "wrapper pins target version");

    assertThrows(
        () => kit.packageKitMigrations({ ir, kitDir, baseline: 0, kitVersion: "0.31.10" }),
        "pinned to 0.31.11",
        "non-pinned Kit versions are rejected"
    );

    const destructiveDir = join(work, "destructive");
    mkdirSync(destructiveDir, { recursive: true });
    writeFileSync(join(destructiveDir, "0001_drop.sql"), "DROP TABLE \"user\";\n");
    assertThrows(
        () => kit.packageKitMigrations({ ir, kitDir: destructiveDir, baseline: 1, kitVersion: "0.31.11" }),
        "MIG006",
        "destructive migration requires explicit acknowledgement"
    );
    const acknowledged = kit.packageKitMigrations({
        ir,
        kitDir: destructiveDir,
        baseline: 1,
        kitVersion: "0.31.11",
        acknowledgement: { acknowledgeDestructive: true, sources: ["0001_drop.sql"] }
    });
    assertEqual(acknowledged.manifest.migrations[0].destructive, true, "destructive flag recorded");
    assertEqual(acknowledged.manifest.migrations[0].destructiveAck, true, "destructive acknowledgement recorded");
    assertEqual(
        acknowledged.manifest.physicalSchema.basis,
        "external-baseline-required",
        "nonzero baseline does not pretend later Kit SQL defines the whole physical schema"
    );
    assertEqual(acknowledged.manifest.physicalSchema.hash, null, "adopted baseline physical hash remains unresolved");
    const acknowledgedWrapper = acknowledged.files.find(
        (file) => file.relPath === "migrations/user_migrations.hpp"
    ).content;
    assert(
        acknowledgedWrapper.includes("has_expected_physical_schema() { return false; }"),
        "adopted-baseline wrapper exposes unresolved physical identity"
    );
    assertThrows(
        () => kit.packageKitMigrations({
            ir,
            kitDir: destructiveDir,
            baseline: 1,
            kitVersion: "0.31.11",
            acknowledgement: { acknowledgeDestructive: true, source: "0001_drop.sql" }
        }),
        "unknown destructive acknowledgement field",
        "acknowledgement sidecars fail closed on misspelled fields"
    );
    assertThrows(
        () => kit.packageKitMigrations({
            ir,
            kitDir: destructiveDir,
            baseline: 1,
            kitVersion: "0.31.11",
            acknowledgement: { acknowledgeDestructive: true, sources: ["../0001_drop.sql"] }
        }),
        "canonical relative POSIX paths",
        "acknowledgement paths cannot escape the Kit root"
    );
    assertThrows(
        () => kit.packageKitMigrations({
            ir,
            kitDir: destructiveDir,
            baseline: 1,
            kitVersion: "0.31.11",
            acknowledgement: {
                acknowledgeDestructive: true,
                sources: ["0001_drop.sql", "0001_drop.sql"]
            }
        }),
        "duplicated",
        "duplicate acknowledgement sources are rejected"
    );

    const check = runCli([
        "package-migrations",
        join(EXAMPLE_GENERATED, "user.ir.json"),
        "--kit-dir", join(EXAMPLE_DIR, "drizzle-migrations"),
        "--out", EXAMPLE_GENERATED,
        "--kit-version", "0.31.11",
        "--check"
    ]);
    assertEqual(check.status, 0, `checked-in Kit package is current: ${check.stderr}`);
}
