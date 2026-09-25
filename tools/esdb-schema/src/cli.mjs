/*
 * esdb-schema CLI.
 *
 * Commands:
 *   validate <ir.json> [--json]          validate an IR document
 *   seal --in <ir.json> --out <ir.json>  canonicalize + embed the IR hash
 *   hash <ir.json> [--check]             print (or verify) the IR hash
 *   physical-hash --db <database> [--json] inspect a live DB read-only
 *   physical-diff <ir.json> --kit-dir <dir> --kit-version <version> [--db <database>] [--json]
 *   adoption-baseline --db <database> --out <file>
 *   adoption-report [<ir.json>] --db <database> (--expected-sql <file> | --kit-dir <dir> --kit-version <version>) [--expected-user-version N] [--out <file>] [--json]
 *   package-migrations <ir.json> --kit-dir <dir> --out <dir> --kit-version <version> [--baseline N] [--ack file] [--check]
 *   bootstrap-migration <ir.json> --out <dir> [--check]  IR-derived oracle only
 *   generate <ir.json> --out <dir> [--check]
 *   build <ir.json> --out <dir> [--check]  alias of generate
 *
 * Generation and migration packaging require a sealed IR whose embedded hash
 * matches the canonical projection; `seal` is the only command that writes an
 * unsealed document's hash.
 */

import { existsSync, mkdirSync, readFileSync, realpathSync, statSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";

import { COMPILER_NAME, COMPILER_VERSION, IR_VERSION, IrValidationError, validateDocument } from "./ir.mjs";
import { canonicalize, hashProjection, seal, verifySeal } from "./canonical.mjs";
import { buildMigrationPackage } from "./sql.mjs";
import { loadDestructiveAcknowledgement, packageKitMigrations } from "./kit-migrations.mjs";
import { generateCpp } from "./codegen-cpp.mjs";
import { generateEs3 } from "./codegen-es3.mjs";
import { generateTs } from "./codegen-ts.mjs";
import { generateBridge } from "./codegen-bridge.mjs";
import { checkFiles, writeFiles } from "./emit.mjs";
import {
    PHYSICAL_NORMALIZER_VERSION,
    PHYSICAL_VERIFICATION_FORMAT,
    diffIrAgainstPhysical,
    diffPhysicalSchemas,
    inspectDatabasePath,
    physicalSchemaHash,
    snapshotSql
} from "./physical-schema.mjs";
import {
    adoptionReportFromKit,
    adoptionReportFromSqlFile,
    baselineArtifactBytes,
    createBaselineArtifact,
    loadBaselineArtifact
} from "./adoption.mjs";

export class CliError extends Error {
    constructor(exitCode, message) {
        super(message);
        this.name = "CliError";
        this.exitCode = exitCode;
    }
}

function usage() {
    return [
        `${COMPILER_NAME} ${COMPILER_VERSION} (${IR_VERSION})`,
        "",
        "usage:",
        "  esdb-schema validate <ir.json> [--json]",
        "  esdb-schema seal --in <ir.json> --out <ir.json>",
        "  esdb-schema hash <ir.json> [--check]",
        "  esdb-schema physical-hash --db <database> [--json]",
        "  esdb-schema physical-diff <ir.json> --kit-dir <dir> --kit-version <version> [--db <database>] [--baseline N] [--ack file] [--json]",
        "  esdb-schema adoption-baseline --db <database> --out <file>",
        "  esdb-schema adoption-report [<ir.json>] --db <database> (--expected-sql <file> | --kit-dir <dir> --kit-version <version>) [--expected-user-version N] [--out <file>] [--json]",
        "  esdb-schema package-migrations <ir.json> --kit-dir <dir> --out <dir> --kit-version <version> [--baseline N] [--ack file] [--check]",
        "  esdb-schema bootstrap-migration <ir.json> --out <dir> [--check]",
        "  esdb-schema generate <ir.json> --out <dir> [--check]",
        "  esdb-schema build <ir.json> --out <dir> [--check]",
        "",
        "options:",
        "  --in <path>    input IR (seal)",
        "  --out <path>   output file (seal) or generated-artifact directory",
        "  --kit-dir <d>  committed Drizzle Kit CLI output directory (package-migrations)",
        "  --kit-version  Drizzle Kit version used to author the SQL",
        "  --baseline <n> ESDB user_version before the first packaged step (default 0)",
        "  --ack <file>   destructive-migration acknowledgement sidecar",
        "  --db <path>    live SQLite database; inspection is read-only",
        "  --expected-sql <file> canonical expected schema SQL; executed only in scratch SQLite",
        "  --expected-user-version <n> expected PRAGMA user_version for SQL adoption reports",
        "  --baseline-file <file> validated adoption baseline to compare with target and live DB",
        "  --json         machine-readable output",
        "  --check        compare generated output against the directory without writing",
        "  --version      print the compiler version",
        "  --help         print this text"
    ].join("\n");
}

function parseArgs(argv) {
    const options = {
        command: null,
        positional: [],
        in: null,
        out: null,
        kitDir: null,
        kitVersion: null,
        baseline: "0",
        ack: null,
        db: null,
        expectedSql: null,
        expectedUserVersion: null,
        baselineFile: null,
        json: false,
        check: false,
        help: false,
        version: false
    };
    for (let i = 0; i < argv.length; i += 1) {
        const arg = argv[i];
        if (
            arg === "--in" ||
            arg === "--out" ||
            arg === "--kit-dir" ||
            arg === "--kit-version" ||
            arg === "--baseline" ||
            arg === "--ack" ||
            arg === "--db" ||
            arg === "--expected-sql" ||
            arg === "--expected-user-version" ||
            arg === "--baseline-file"
        ) {
            const value = argv[i + 1];
            if (value === undefined || value.startsWith("--")) {
                throw new CliError(2, `${arg} requires a value`);
            }
            const key = {
                "--in": "in",
                "--out": "out",
                "--kit-dir": "kitDir",
                "--kit-version": "kitVersion",
                "--baseline": "baseline",
                "--ack": "ack",
                "--db": "db",
                "--expected-sql": "expectedSql",
                "--expected-user-version": "expectedUserVersion",
                "--baseline-file": "baselineFile"
            }[arg];
            options[key] = value;
            i += 1;
        } else if (arg === "--json") {
            options.json = true;
        } else if (arg === "--check") {
            options.check = true;
        } else if (arg === "--help" || arg === "-h") {
            options.help = true;
        } else if (arg === "--version") {
            options.version = true;
        } else if (arg.startsWith("--")) {
            throw new CliError(2, `unknown option "${arg}"`);
        } else if (options.command === null) {
            options.command = arg;
        } else {
            options.positional.push(arg);
        }
    }
    return options;
}

function loadIr(path) {
    let text;
    try {
        text = readFileSync(path, "utf8");
    } catch (error) {
        throw new CliError(2, `cannot read ${path}: ${error.message}`);
    }
    try {
        return JSON.parse(text);
    } catch (error) {
        throw new CliError(2, `cannot parse JSON from ${path}: ${error.message}`);
    }
}

function validateOrThrow(ir) {
    const result = validateDocument(ir);
    if (!result.ok) {
        throw new IrValidationError(result.diagnostics);
    }
    return result;
}

function requireSealed(ir) {
    if (!ir.integrity) {
        throw new CliError(1, "IR is not sealed; run `esdb-schema seal --in <ir.json> --out <ir.json>` first");
    }
    const verification = verifySeal(ir);
    if (!verification.ok) {
        throw new CliError(
            1,
            `IR hash mismatch: embedded ${String(verification.actual)}, computed ${verification.expected}`
        );
    }
    return verification.expected;
}

function contextFor(ir, irHash) {
    return {
        compilerName: COMPILER_NAME,
        compilerVersion: COMPILER_VERSION,
        irHash,
        source: ir.generator && typeof ir.generator.source === "string" ? ir.generator.source : ""
    };
}

function parseBaseline(options) {
    if (!/^(0|[1-9][0-9]*)$/.test(options.baseline)) {
        throw new CliError(2, "--baseline must be a canonical non-negative integer");
    }
    const baseline = Number(options.baseline);
    if (!Number.isSafeInteger(baseline) || baseline > 0xffffffff) {
        throw new CliError(2, "--baseline must fit uint32");
    }
    return baseline;
}

function parseOptionalUserVersion(value, optionName) {
    if (value === null || value === undefined) return null;
    if (!/^(0|[1-9][0-9]*)$/.test(value)) {
        throw new CliError(2, optionName + " must be a canonical non-negative integer");
    }
    const parsed = Number(value);
    if (!Number.isSafeInteger(parsed) || parsed > 0xffffffff) {
        throw new CliError(2, optionName + " must fit uint32");
    }
    return String(parsed);
}

function normalizedFsIdentity(path) {
    const absolute = resolve(path);
    let canonical = absolute;
    if (existsSync(absolute)) {
        try {
            canonical = realpathSync.native ? realpathSync.native(absolute) : realpathSync(absolute);
        } catch {
            canonical = absolute;
        }
    }
    return process.platform === "win32" ? canonical.toLowerCase() : canonical;
}

function sameExistingFile(left, right) {
    if (normalizedFsIdentity(left) === normalizedFsIdentity(right)) return true;
    if (!existsSync(left) || !existsSync(right)) return false;
    try {
        const a = statSync(left);
        const b = statSync(right);
        return a.dev === b.dev && a.ino !== 0 && a.ino === b.ino;
    } catch {
        return false;
    }
}

function collectArtifacts(ir, ctx, mode) {
    const artifacts = [];
    if (mode === "bootstrap-migration") {
        /* Explicit oracle/bootstrap path only. Production migration SQL comes
         * from package-migrations and pinned Drizzle Kit CLI output. */
        artifacts.push(...buildMigrationPackage(ir, ctx).files);
    }
    if (mode === "generate" || mode === "build") {
        artifacts.push(...generateCpp(ir, ctx));
        artifacts.push(...generateEs3(ir, ctx));
        artifacts.push(...generateTs(ir, ctx));
        artifacts.push(...generateBridge(ir, ctx));
    }
    return artifacts;
}

function commandValidate(options, stdout) {
    const target = options.in || options.positional[0];
    if (!target) {
        throw new CliError(2, "validate requires an IR path");
    }
    const ir = loadIr(target);
    const result = validateDocument(ir);
    if (options.json) {
        stdout(`${JSON.stringify({ ok: result.ok, diagnostics: result.diagnostics })}`);
    } else if (result.ok) {
        stdout(`ok ${target}`);
    } else {
        for (const diagnostic of result.diagnostics) {
            stdout(`${diagnostic.code} ${diagnostic.path} ${diagnostic.message}`);
        }
    }
    return result.ok ? 0 : 1;
}

function commandSeal(options, stdout) {
    const inPath = options.in || options.positional[0];
    const outPath = options.out || options.positional[1];
    if (!inPath || !outPath) {
        throw new CliError(2, "seal requires --in <ir.json> and --out <ir.json>");
    }
    const ir = loadIr(inPath);
    validateOrThrow(ir);
    const sealed = seal(ir);
    const bytes = Buffer.from(canonicalize(sealed), "utf8");
    mkdirSync(dirname(outPath) || ".", { recursive: true });
    writeFileSync(outPath, bytes);
    stdout(`sealed ${outPath} sha256:${sealed.integrity.hash}`);
    return 0;
}

function commandHash(options, stdout) {
    const target = options.positional[0] || options.in;
    if (!target) {
        throw new CliError(2, "hash requires an IR path");
    }
    const ir = loadIr(target);
    validateOrThrow(ir);
    const computed = hashProjection(ir);
    if (options.check) {
        const actual = ir.integrity && typeof ir.integrity.hash === "string" ? ir.integrity.hash : null;
        if (actual !== computed) {
            stdout(`hash mismatch: embedded ${String(actual)}, computed ${computed}`);
            return 1;
        }
        stdout(`ok sha256:${computed}`);
        return 0;
    }
    stdout(computed);
    return 0;
}

function commandPhysicalHash(options, stdout) {
    const databasePath = options.db || options.positional[0];
    if (!databasePath) {
        throw new CliError(2, "physical-hash requires --db <database>");
    }
    const inspected = inspectDatabasePath(databasePath);
    const hash = physicalSchemaHash(inspected.snapshot);
    if (options.json) {
        stdout(canonicalize({
            format: PHYSICAL_VERIFICATION_FORMAT,
            normalizer: PHYSICAL_NORMALIZER_VERSION,
            user_version: inspected.user_version,
            physical_schema_hash: `sha256:${hash}`
        }));
    } else {
        stdout(`sha256:${hash}`);
    }
    return 0;
}

function commandPhysicalDiff(options, stdout) {
    const target = options.positional[0] || options.in;
    if (!target || !options.kitDir || !options.kitVersion) {
        throw new CliError(
            2,
            "physical-diff requires an IR path, --kit-dir <dir>, and --kit-version <version>"
        );
    }

    const baseline = parseBaseline(options);
    const ir = loadIr(target);
    validateOrThrow(ir);
    const irHash = requireSealed(ir);
    const acknowledgement = options.ack ? loadDestructiveAcknowledgement(options.ack) : null;
    const packaged = packageKitMigrations({
        ir,
        kitDir: options.kitDir,
        baseline,
        kitVersion: options.kitVersion,
        acknowledgement
    });

    const expected = snapshotSql(packaged.steps.map((step) => step.sql));
    const expectedHash = physicalSchemaHash(expected);
    const irDiff = diffIrAgainstPhysical(ir, expected);

    let live = null;
    let liveOk = true;
    if (options.db) {
        const inspected = inspectDatabasePath(options.db);
        const schemaDiff = diffPhysicalSchemas(expected, inspected.snapshot);
        const versionMatch = inspected.user_version === String(packaged.targetVersion);
        liveOk = schemaDiff.ok && versionMatch;
        live = {
            user_version: inspected.user_version,
            expected_user_version: String(packaged.targetVersion),
            version_match: versionMatch,
            physical_schema_hash: `sha256:${physicalSchemaHash(inspected.snapshot)}`,
            schema: schemaDiff
        };
    }

    const result = {
        format: PHYSICAL_VERIFICATION_FORMAT,
        normalizer: PHYSICAL_NORMALIZER_VERSION,
        ir_hash: `sha256:${irHash}`,
        expected: {
            source: "drizzle-kit-cli",
            kit_version: options.kitVersion,
            target_user_version: String(packaged.targetVersion),
            physical_schema_hash: `sha256:${expectedHash}`
        },
        kit_vs_ir: irDiff,
        live
    };

    if (options.json) {
        stdout(canonicalize(result));
    } else {
        stdout(`expected physical sha256:${expectedHash} user_version=${packaged.targetVersion}`);
        stdout(
            `kit-vs-ir compatible=${irDiff.compatible ? "yes" : "no"} ` +
            `mismatches=${irDiff.mismatches.length} unmodeled=${irDiff.unmodeled.length}`
        );
        if (live !== null) {
            stdout(
                `live version=${live.user_version}/${live.expected_user_version} ` +
                `schema=${live.schema.ok ? "match" : "mismatch"} ` +
                `mismatches=${live.schema.mismatches.length}`
            );
        }
    }

    return irDiff.compatible && liveOk ? 0 : 1;
}

function commandAdoptionBaseline(options, stdout) {
    if (!options.db || !options.out) {
        throw new CliError(2, "adoption-baseline requires --db <database> and --out <file>");
    }
    const artifact = createBaselineArtifact(options.db);
    const bytes = baselineArtifactBytes(artifact);
    mkdirSync(dirname(options.out) || ".", { recursive: true });
    try {
        writeFileSync(options.out, bytes, { flag: "wx" });
    } catch (error) {
        if (error && error.code === "EEXIST") {
            throw new CliError(
                1,
                "adoption baseline already exists; refusing to overwrite " + options.out
            );
        }
        throw error;
    }
    stdout(
        "wrote adoption baseline " + options.out + " " +
        artifact.physical_schema_hash + " user_version=" + artifact.user_version
    );
    return 0;
}

function commandAdoptionReport(options, stdout) {
    if (!options.db) {
        throw new CliError(2, "adoption-report requires --db <database>");
    }
    const usesSql = options.expectedSql !== null;
    const usesKit = options.kitDir !== null || options.kitVersion !== null;
    if (usesSql === usesKit) {
        throw new CliError(
            2,
            "adoption-report requires exactly one expected source: --expected-sql <file> OR --kit-dir <dir> --kit-version <version>"
        );
    }

    const target = options.positional[0] || options.in;
    const baselineArtifact = options.baselineFile
        ? loadBaselineArtifact(options.baselineFile)
        : null;
    let ir = null;
    if (target) {
        ir = loadIr(target);
        validateOrThrow(ir);
        requireSealed(ir);
    }

    let report;
    if (usesSql) {
        report = adoptionReportFromSqlFile({
            dbPath: options.db,
            sqlPath: options.expectedSql,
            expectedUserVersion: parseOptionalUserVersion(
                options.expectedUserVersion,
                "--expected-user-version"
            ),
            ir,
            baselineArtifact
        });
    } else {
        if (!target || !options.kitDir || !options.kitVersion) {
            throw new CliError(
                2,
                "Kit adoption-report requires an IR path, --kit-dir <dir>, and --kit-version <version>"
            );
        }
        if (options.expectedUserVersion !== null) {
            throw new CliError(
                2,
                "--expected-user-version is derived from packaged Kit migrations; do not pass it in Kit mode"
            );
        }
        const acknowledgement = options.ack ? loadDestructiveAcknowledgement(options.ack) : null;
        report = adoptionReportFromKit({
            dbPath: options.db,
            ir,
            kitDir: options.kitDir,
            kitVersion: options.kitVersion,
            baseline: parseBaseline(options),
            acknowledgement,
            baselineArtifact
        });
    }

    const encoded = canonicalize(report);
    if (options.out) {
        if (sameExistingFile(options.db, options.out)) {
            throw new CliError(
                2,
                "adoption-report --out must not resolve to the inspected database"
            );
        }
        mkdirSync(dirname(options.out) || ".", { recursive: true });
        writeFileSync(options.out, Buffer.from(encoded, "utf8"));
    }
    if (options.json || !options.out) {
        stdout(encoded);
    } else {
        stdout(
            "adoption " + (report.match ? "match" : "mismatch") +
            " expected=" + report.expected.physical_schema_hash +
            " actual=" + report.actual.physical_schema_hash +
            " mismatches=" + report.physical_diff.mismatches.length
        );
    }

    const irOk = !report.ir_compatibility || report.ir_compatibility.compatible;
    return report.match && irOk ? 0 : 1;
}

function commandBuild(options, stdout, mode) {
    const target = options.positional[0] || options.in;
    const outDir = options.out;
    if (!target || !outDir) {
        throw new CliError(2, `${mode} requires an IR path and --out <dir>`);
    }
    const ir = loadIr(target);
    validateOrThrow(ir);
    const irHash = requireSealed(ir);
    const ctx = contextFor(ir, irHash);
    const files = collectArtifacts(ir, ctx, mode);
    if (options.check) {
        const result = checkFiles(outDir, files, {
            strict: mode === "generate" || mode === "build"
        });
        if (!result.ok) {
            for (const difference of result.differences) {
                stdout(`stale ${difference.relPath}: ${difference.reason}`);
            }
            return 1;
        }
        stdout(`ok ${files.length} file(s) match ${outDir}`);
        return 0;
    }
    writeFiles(outDir, files);
    stdout(`wrote ${files.length} file(s) to ${outDir}`);
    return 0;
}

function commandPackageMigrations(options, stdout) {
    const target = options.positional[0] || options.in;
    const outDir = options.out;
    if (!target || !outDir || !options.kitDir || !options.kitVersion) {
        throw new CliError(
            2,
            "package-migrations requires an IR path, --kit-dir <dir>, --kit-version <version>, and --out <dir>"
        );
    }
    const baseline = parseBaseline(options);

    const ir = loadIr(target);
    validateOrThrow(ir);
    requireSealed(ir);
    const acknowledgement = options.ack ? loadDestructiveAcknowledgement(options.ack) : null;
    const packaged = packageKitMigrations({
        ir,
        kitDir: options.kitDir,
        baseline,
        kitVersion: options.kitVersion,
        acknowledgement
    });

    if (options.check) {
        const result = checkFiles(outDir, packaged.files, { strict: true });
        if (!result.ok) {
            for (const difference of result.differences) {
                stdout(`stale ${difference.relPath}: ${difference.reason}`);
            }
            return 1;
        }
        stdout(`ok ${packaged.files.length} migration artifact(s) match ${outDir}`);
        return 0;
    }
    writeFiles(outDir, packaged.files);
    stdout(
        `wrote ${packaged.files.length} migration artifact(s) to ${outDir}; target user_version ${packaged.targetVersion}`
    );
    return 0;
}

export function run(argv, io) {
    const stdout = (io && io.stdout) || ((text) => process.stdout.write(`${text}\n`));
    const options = parseArgs(argv);
    if (options.help) {
        stdout(usage());
        return 0;
    }
    if (options.version) {
        stdout(`${COMPILER_VERSION}`);
        return 0;
    }
    switch (options.command) {
        case "validate":
            return commandValidate(options, stdout);
        case "seal":
            return commandSeal(options, stdout);
        case "hash":
            return commandHash(options, stdout);
        case "physical-hash":
            return commandPhysicalHash(options, stdout);
        case "physical-diff":
            return commandPhysicalDiff(options, stdout);
        case "adoption-baseline":
            return commandAdoptionBaseline(options, stdout);
        case "adoption-report":
            return commandAdoptionReport(options, stdout);
        case "package-migrations":
            return commandPackageMigrations(options, stdout);
        case "migrate":
            throw new CliError(
                2,
                "migrate no longer authors SQL from IR; use package-migrations with pinned Drizzle Kit CLI output"
            );
        case "bootstrap-migration":
        case "generate":
        case "build":
            return commandBuild(options, stdout, options.command);
        default:
            stdout(usage());
            return options.command === null ? 0 : 2;
    }
}
