/*
 * Explicit adoption/baseline tooling for existing SQLite databases.
 *
 * Safety contract:
 *   - existing databases are always opened read-only + query_only;
 *   - no schema SQL is ever executed against the inspected database;
 *   - baseline creation writes only a separate deterministic artifact;
 *   - mismatch reports are descriptive, never corrective.
 */

import { readFileSync } from "node:fs";

import { canonicalize, sha256Hex } from "./canonical.mjs";
import {
    PHYSICAL_NORMALIZER_VERSION,
    PHYSICAL_SCHEMA_FORMAT,
    diffIrAgainstPhysical,
    diffPhysicalSchemas,
    inspectDatabasePath,
    physicalSchemaHash,
    snapshotSql
} from "./physical-schema.mjs";
import {
    normalizeMigrationSql,
    packageKitMigrations
} from "./kit-migrations.mjs";

export const ADOPTION_BASELINE_FORMAT = "esdb.adoption-baseline/v1";
export const ADOPTION_REPORT_FORMAT = "esdb.adoption-report/v1";

function hashLabel(snapshot) {
    return "sha256:" + physicalSchemaHash(snapshot);
}

function ensureSnapshot(snapshot) {
    if (!snapshot || snapshot.format !== PHYSICAL_SCHEMA_FORMAT) {
        throw new Error("adoption tooling requires an ESDB physical schema snapshot");
    }
    if (snapshot.normalizer !== PHYSICAL_NORMALIZER_VERSION) {
        throw new Error(
            "unsupported physical schema normalizer " + String(snapshot.normalizer) +
            "; expected " + PHYSICAL_NORMALIZER_VERSION
        );
    }
    return snapshot;
}

function canonicalUint32Text(value, label) {
    const text = String(value);
    if (!/^(0|[1-9][0-9]*)$/.test(text)) {
        throw new Error(label + " must be a canonical uint32 decimal string");
    }
    const number = Number(text);
    if (!Number.isSafeInteger(number) || number > 0xffffffff) {
        throw new Error(label + " must fit uint32");
    }
    return text;
}

export function baselineArtifactFromState(state) {
    if (!state || typeof state.user_version !== "string") {
        throw new Error("baselineArtifactFromState requires inspected database state");
    }
    const snapshot = ensureSnapshot(state.snapshot);
    return {
        format: ADOPTION_BASELINE_FORMAT,
        normalizer: PHYSICAL_NORMALIZER_VERSION,
        user_version: canonicalUint32Text(state.user_version, "baseline user_version"),
        physical_schema_hash: hashLabel(snapshot),
        physical_schema: snapshot
    };
}

export function createBaselineArtifact(dbPath) {
    return baselineArtifactFromState(inspectDatabasePath(dbPath));
}

export function validateBaselineArtifact(artifact) {
    if (!artifact || artifact.format !== ADOPTION_BASELINE_FORMAT) {
        throw new Error("invalid ESDB adoption baseline format");
    }
    const allowed = new Set([
        "format",
        "normalizer",
        "user_version",
        "physical_schema_hash",
        "physical_schema"
    ]);
    for (const key of Object.keys(artifact)) {
        if (!allowed.has(key)) throw new Error("unknown adoption baseline field " + key);
    }
    if (artifact.normalizer !== PHYSICAL_NORMALIZER_VERSION) {
        throw new Error(
            "unsupported adoption baseline normalizer " + String(artifact.normalizer) +
            "; expected " + PHYSICAL_NORMALIZER_VERSION
        );
    }
    canonicalUint32Text(artifact.user_version, "baseline user_version");
    const snapshot = ensureSnapshot(artifact.physical_schema);
    const expectedHash = hashLabel(snapshot);
    if (artifact.physical_schema_hash !== expectedHash) {
        throw new Error(
            "adoption baseline physical hash mismatch: embedded " +
            String(artifact.physical_schema_hash) + ", computed " + expectedHash
        );
    }
    return artifact;
}

export function parseBaselineArtifact(input) {
    let artifact;
    try {
        artifact = JSON.parse(Buffer.isBuffer(input) ? input.toString("utf8") : String(input));
    } catch (error) {
        throw new Error("cannot parse adoption baseline JSON: " + error.message);
    }
    return validateBaselineArtifact(artifact);
}

export function loadBaselineArtifact(path) {
    return parseBaselineArtifact(readFileSync(path));
}

export function baselineArtifactBytes(artifact) {
    return Buffer.from(canonicalize(validateBaselineArtifact(artifact)), "utf8");
}

export function expectedPhysicalFromSql(sql) {
    const normalized = Array.isArray(sql)
        ? sql.map((part) => normalizeMigrationSql(part))
        : [normalizeMigrationSql(sql)];
    const snapshot = snapshotSql(normalized);
    return {
        snapshot,
        physical_schema_hash: hashLabel(snapshot),
        provenance: {
            kind: "sql",
            statements: String(normalized.length)
        }
    };
}

export function expectedPhysicalFromSqlFile(path) {
    const sql = readFileSync(path);
    const expected = expectedPhysicalFromSql(sql);
    const normalized = normalizeMigrationSql(sql);
    return {
        ...expected,
        provenance: {
            kind: "sql-file",
            normalized_sql_hash: "sha256:" + sha256Hex(normalized)
        }
    };
}

export function expectedPhysicalFromKit(args) {
    const packaged = packageKitMigrations({
        ir: args.ir,
        kitDir: args.kitDir,
        baseline: args.baseline === undefined ? 0 : args.baseline,
        kitVersion: args.kitVersion,
        acknowledgement: args.acknowledgement || null
    });
    const snapshot = snapshotSql(packaged.steps.map((step) => step.sql));
    return {
        snapshot,
        physical_schema_hash: hashLabel(snapshot),
        expected_user_version: String(packaged.targetVersion),
        provenance: {
            kind: "drizzle-kit",
            version: args.kitVersion,
            baseline: String(args.baseline === undefined ? 0 : args.baseline),
            target_version: String(packaged.targetVersion),
            migrations: packaged.manifest.migrations.map((item) => ({
                ordinal: item.ordinal,
                source: item.source,
                sqlHash: item.sqlHash,
                snapshotHash: item.snapshotHash || null
            }))
        }
    };
}

export function buildAdoptionReport(args) {
    const actual = args.actualState || inspectDatabasePath(args.dbPath);
    const expected = ensureSnapshot(args.expectedSnapshot);
    const diff = diffPhysicalSchemas(expected, actual.snapshot);
    const expectedUserVersion = args.expectedUserVersion === undefined ||
        args.expectedUserVersion === null
        ? null
        : String(args.expectedUserVersion);
    const versionMatch = expectedUserVersion === null
        ? null
        : actual.user_version === expectedUserVersion;

    const report = {
        format: ADOPTION_REPORT_FORMAT,
        read_only: true,
        match: diff.ok && versionMatch !== false,
        expected: {
            physical_schema_hash: hashLabel(expected),
            user_version: expectedUserVersion
        },
        actual: {
            physical_schema_hash: hashLabel(actual.snapshot),
            user_version: actual.user_version
        },
        physical_diff: diff
    };

    if (args.expectedProvenance) {
        report.expected.provenance = args.expectedProvenance;
    }
    if (versionMatch === false) {
        report.user_version_mismatch = {
            expected: expectedUserVersion,
            actual: actual.user_version
        };
    }
    if (args.ir) {
        report.ir_compatibility = diffIrAgainstPhysical(args.ir, actual.snapshot);
    }

    if (args.baselineArtifact) {
        const baseline = validateBaselineArtifact(args.baselineArtifact);
        const baselineToExpected = diffPhysicalSchemas(expected, baseline.physical_schema);
        const liveToBaseline = diffPhysicalSchemas(baseline.physical_schema, actual.snapshot);
        const baselineVersionMatch = expectedUserVersion === null
            ? null
            : baseline.user_version === expectedUserVersion;
        const liveBaselineVersionMatch = actual.user_version === baseline.user_version;
        report.baseline = {
            physical_schema_hash: baseline.physical_schema_hash,
            user_version: baseline.user_version
        };
        report.baseline_to_expected = {
            match: baselineToExpected.ok && baselineVersionMatch !== false,
            user_version_match: baselineVersionMatch,
            physical_diff: baselineToExpected
        };
        report.live_to_baseline = {
            match: liveToBaseline.ok && liveBaselineVersionMatch,
            user_version_match: liveBaselineVersionMatch,
            physical_diff: liveToBaseline
        };
    }
    return report;
}

export function adoptionReportFromSql(args) {
    const expected = expectedPhysicalFromSql(args.sql);
    return buildAdoptionReport({
        dbPath: args.dbPath,
        expectedSnapshot: expected.snapshot,
        expectedUserVersion: args.expectedUserVersion,
        expectedProvenance: args.expectedProvenance || expected.provenance,
        ir: args.ir || null,
        baselineArtifact: args.baselineArtifact || null
    });
}

export function adoptionReportFromSqlFile(args) {
    const expected = expectedPhysicalFromSqlFile(args.sqlPath);
    return buildAdoptionReport({
        dbPath: args.dbPath,
        expectedSnapshot: expected.snapshot,
        expectedUserVersion: args.expectedUserVersion,
        expectedProvenance: args.expectedProvenance || expected.provenance,
        ir: args.ir || null,
        baselineArtifact: args.baselineArtifact || null
    });
}

export function adoptionReportFromKit(args) {
    const expected = expectedPhysicalFromKit(args);
    return buildAdoptionReport({
        dbPath: args.dbPath,
        expectedSnapshot: expected.snapshot,
        expectedUserVersion: expected.expected_user_version,
        expectedProvenance: expected.provenance,
        ir: args.ir,
        baselineArtifact: args.baselineArtifact || null
    });
}
