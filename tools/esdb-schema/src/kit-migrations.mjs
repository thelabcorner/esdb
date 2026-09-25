/*
 * Deterministic packaging of Drizzle Kit CLI output for ESDB Runtime.
 *
 * This module never imports drizzle-kit. Kit remains the DDL/diff authority;
 * ESDB only validates and packages the ordinary SQL files Kit produced.
 */

import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { basename, extname, join, relative, resolve, sep } from "node:path";
import { TextDecoder } from "node:util";

import { canonicalize, sha256Hex } from "./canonical.mjs";
import { assertMigrationSqlSafe } from "./migration-lint.mjs";
import {
    PHYSICAL_NORMALIZER_VERSION,
    physicalSchemaHash,
    snapshotSql
} from "./physical-schema.mjs";

export const MIGRATION_MANIFEST_VERSION = "2";
export const MIGRATION_NORMALIZER_VERSION = "lf-v1";
export const SUPPORTED_DRIZZLE_KIT_VERSION = "0.31.11";

const UTF8_DECODER = new TextDecoder("utf-8", { fatal: true });

function migrationInputError(message) {
    const error = new Error(`MIG008 ${message}`);
    error.code = "MIG008";
    return error;
}

function relativePosix(root, path) {
    return relative(root, path).split(sep).join("/");
}

function lexicalCompare(left, right) {
    return left < right ? -1 : left > right ? 1 : 0;
}

function walkFiles(root) {
    const out = [];
    const walk = (dir) => {
        const entries = readdirSync(dir, { withFileTypes: true })
            .slice()
            .sort((a, b) => lexicalCompare(a.name, b.name));
        for (const entry of entries) {
            const full = join(dir, entry.name);
            if (entry.isDirectory()) walk(full);
            else if (entry.isFile()) out.push(full);
        }
    };
    walk(root);
    return out;
}

export function normalizeMigrationSql(input) {
    let text;
    if (Buffer.isBuffer(input)) {
        if (input.length >= 3 && input[0] === 0xef && input[1] === 0xbb && input[2] === 0xbf) {
            throw migrationInputError("migration SQL must not contain a UTF-8 BOM");
        }
        try {
            text = UTF8_DECODER.decode(input);
        } catch {
            throw migrationInputError("migration SQL is not valid UTF-8");
        }
    } else {
        text = String(input);
        if (text.charCodeAt(0) === 0xfeff) {
            throw migrationInputError("migration SQL must not contain a UTF-8 BOM");
        }
    }
    if (text.indexOf("\0") !== -1) {
        throw migrationInputError("migration SQL must not contain U+0000");
    }
    return text.replace(/\r\n?/g, "\n").replace(/\n*$/, "") + "\n";
}

function migrationSlug(source) {
    const stem = basename(source, extname(source)).replace(/^[0-9]+[_-]*/, "");
    const slug = stem
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/[^A-Za-z0-9]+/g, "_")
        .replace(/^_+|_+$/g, "")
        .toLowerCase();
    return slug || "migration";
}

function sourcePrefix(source) {
    const match = /^([0-9]+)/.exec(basename(source));
    return match ? match[1] : null;
}

function snapshotFor(sqlSource, snapshots) {
    const prefix = sourcePrefix(sqlSource);
    if (prefix === null) return null;
    const expected = prefix + "_snapshot.json";
    return snapshots.find((path) => basename(path) === expected) || null;
}

function validateDestructiveAcknowledgement(acknowledgement) {
    if (acknowledgement === null || acknowledgement === undefined) return null;
    if (typeof acknowledgement !== "object" || Array.isArray(acknowledgement) ||
        acknowledgement.acknowledgeDestructive !== true) {
        throw new Error("destructive acknowledgement must contain acknowledgeDestructive: true");
    }
    for (const key of Object.keys(acknowledgement)) {
        if (key !== "acknowledgeDestructive" && key !== "sources") {
            throw new Error(`unknown destructive acknowledgement field "${key}"`);
        }
    }
    if (!("sources" in acknowledgement)) return acknowledgement;
    if (!Array.isArray(acknowledgement.sources)) {
        throw new Error("destructive acknowledgement sources must be an array when present");
    }
    const seen = new Set();
    for (const source of acknowledgement.sources) {
        if (typeof source !== "string" || source.length === 0 || source.indexOf("\\") !== -1 ||
            source.startsWith("/") || source.split("/").includes("..")) {
            throw new Error("destructive acknowledgement sources must be non-empty canonical relative POSIX paths");
        }
        if (seen.has(source)) {
            throw new Error(`destructive acknowledgement source "${source}" is duplicated`);
        }
        seen.add(source);
    }
    return acknowledgement;
}

function isDestructiveAcknowledged(source, acknowledgement) {
    if (acknowledgement === null) return false;
    if (!Array.isArray(acknowledgement.sources)) return true;
    return acknowledgement.sources.includes(source);
}

function cppStringLiteral(text) {
    const bytes = Buffer.from(String(text), "utf8");
    if (bytes.length === 0) return "\"\"";
    return Array.from(bytes, (byte) => {
        const hex = byte.toString(16).toUpperCase().padStart(2, "0");
        return "\"\\x" + hex + "\"";
    }).join("");
}

function cppNamespace(schemaName) {
    const safe = String(schemaName)
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/[^A-Za-z0-9]+/g, "_")
        .replace(/^_+|_+$/g, "")
        .toLowerCase();
    return "esdb_generated_" + safe + "_migrations";
}

export function generateMigrationWrapper(ir, steps, baseline, targetVersion, physicalSchema) {
    const schema = ir.schema.name;
    const ns = cppNamespace(schema);
    const guard = "ESDB_GENERATED_" +
        schema.replace(/[^A-Za-z0-9]+/g, "_").toUpperCase() +
        "_MIGRATIONS_HPP_INCLUDED";
    const lines = [
        "// Generated by esdb-schema. DO NOT EDIT.",
        "// IR: " + ir.ir_version + " sha256:" + ir.integrity.hash,
        "// Drizzle Kit SQL is the DDL authority; ESDB owns migration execution/versioning.",
        "#ifndef " + guard,
        "#define " + guard,
        "",
        "#include <esdb/esdb.h>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace " + ns + " {",
        "",
        "inline const char *expected_ir_hash() { return " + cppStringLiteral(ir.integrity.hash) + "; }",
        "inline const char *physical_schema_normalizer() { return " + cppStringLiteral(physicalSchema.normalizerVersion) + "; }",
        "inline bool has_expected_physical_schema() { return " + (physicalSchema.hash !== null ? "true" : "false") + "; }",
        "inline const char *expected_physical_schema_hash() { return " + cppStringLiteral(physicalSchema.hash === null ? "" : physicalSchema.hash.replace(/^sha256:/, "")) + "; }",
        "inline std::uint32_t baseline_version() { return " + baseline + "u; }",
        "inline std::uint32_t target_version() { return " + targetVersion + "u; }",
        ""
    ];

    steps.forEach((step, index) => {
        const n = index + 1;
        lines.push("static const char kMigrationSql" + n + "[] = " + cppStringLiteral(step.sql) + ";");
        lines.push("inline esdb_status apply_migration_" + n + "(");
        lines.push("    esdb_database *database,");
        lines.push("    std::uint32_t from_version,");
        lines.push("    std::uint32_t to_version,");
        lines.push("    void *user_data,");
        lines.push("    esdb_error *error) {");
        lines.push("    (void)from_version;");
        lines.push("    (void)to_version;");
        lines.push("    (void)user_data;");
        lines.push("    return esdb_exec(database, kMigrationSql" + n + ", error);");
        lines.push("}");
        lines.push("");
    });

    lines.push("inline esdb_status migrate(esdb_database *database, esdb_error *error) {");
    if (steps.length === 0) {
        lines.push("    (void)database;");
        lines.push("    (void)error;");
        lines.push("    return ESDB_OK;");
    } else {
        lines.push("    esdb_migration migrations[] = {");
        steps.forEach((step, index) => {
            const comma = index + 1 < steps.length ? "," : "";
            lines.push(
                "        { " + step.fromVersion + "u, " + step.toVersion +
                "u, &apply_migration_" + (index + 1) + ", NULL }" + comma
            );
        });
        lines.push("    };");
        lines.push(
            "    return esdb_migrate(database, " + targetVersion +
            "u, migrations, static_cast<std::uint32_t>(sizeof(migrations) / sizeof(migrations[0])), error);"
        );
    }
    lines.push("}");
    lines.push("");
    lines.push("}  // namespace " + ns);
    lines.push("");
    lines.push("#endif  // " + guard);
    lines.push("");
    return lines.join("\n");
}

export function packageKitMigrations(args) {
    const ir = args.ir;
    const root = resolve(args.kitDir);
    const baseline = args.baseline === undefined ? 0 : args.baseline;
    const kitVersion = args.kitVersion;
    const acknowledgement = validateDestructiveAcknowledgement(args.acknowledgement || null);

    if (!existsSync(root) || !statSync(root).isDirectory()) {
        throw new Error("Kit output directory does not exist: " + args.kitDir);
    }
    if (!ir || !ir.integrity || typeof ir.integrity.hash !== "string") {
        throw new Error("packageKitMigrations requires a sealed IR");
    }
    if (!Number.isSafeInteger(baseline) || baseline < 0 || baseline > 0xffffffff) {
        throw new Error("baseline must be a uint32");
    }
    if (typeof kitVersion !== "string" || kitVersion.length === 0) {
        throw new Error("kitVersion is required");
    }
    if (kitVersion !== SUPPORTED_DRIZZLE_KIT_VERSION) {
        throw new Error(
            "unsupported Drizzle Kit version " + kitVersion +
            "; ESDB ORM v1 is pinned to " + SUPPORTED_DRIZZLE_KIT_VERSION
        );
    }

    const allFiles = walkFiles(root);
    const sqlFiles = allFiles
        .filter((path) => extname(path).toLowerCase() === ".sql")
        .sort((a, b) => lexicalCompare(relativePosix(root, a), relativePosix(root, b)));
    if (sqlFiles.length === 0) throw new Error("Drizzle Kit output contains no .sql migrations");
    const snapshots = allFiles.filter((path) => /_snapshot\.json$/i.test(basename(path)));
    if (baseline + sqlFiles.length > 0xffffffff) {
        throw new Error("migration target version exceeds uint32");
    }

    const steps = [];
    const files = [];
    sqlFiles.forEach((fullPath, index) => {
        const source = relativePosix(root, fullPath);
        const sql = normalizeMigrationSql(readFileSync(fullPath));
        const lint = assertMigrationSqlSafe(sql);
        const destructiveAck = lint.destructive
            ? isDestructiveAcknowledged(source, acknowledgement)
            : false;
        if (lint.destructive && !destructiveAck) {
            const error = new Error(
                'MIG006 destructive migration "' + source + '" requires an acknowledgement sidecar'
            );
            error.code = "MIG006";
            throw error;
        }

        const fromVersion = baseline + index;
        const toVersion = fromVersion + 1;
        const slug = migrationSlug(source);
        const file = "migrations/v" + String(toVersion).padStart(4, "0") + "__" + slug + ".sql";
        const snapshot = snapshotFor(source, snapshots);
        const snapshotHash = snapshot ? "sha256:" + sha256Hex(readFileSync(snapshot)) : null;
        const step = {
            ordinal: String(index + 1),
            fromVersion,
            toVersion,
            source,
            file,
            sql,
            sqlHash: "sha256:" + sha256Hex(sql),
            snapshotHash,
            destructive: lint.destructive,
            destructiveAck
        };
        steps.push(step);
        files.push({ relPath: file, content: sql });
    });

    const targetVersion = baseline + steps.length;
    const physicalSchema = baseline === 0
        ? {
            normalizerVersion: PHYSICAL_NORMALIZER_VERSION,
            hash: "sha256:" + physicalSchemaHash(snapshotSql(steps.map((step) => step.sql))),
            basis: "full-kit-chain"
        }
        : {
            normalizerVersion: PHYSICAL_NORMALIZER_VERSION,
            hash: null,
            basis: "external-baseline-required"
        };
    const manifest = {
        formatVersion: MIGRATION_MANIFEST_VERSION,
        irHash: "sha256:" + ir.integrity.hash,
        baseline: String(baseline),
        targetVersion: String(targetVersion),
        normalizerVersion: MIGRATION_NORMALIZER_VERSION,
        physicalSchema,
        kit: {
            tool: "drizzle-kit-cli",
            version: kitVersion,
            ordering: "lexicographic-relative-path"
        },
        migrations: steps.map((step) => {
            const item = {
                ordinal: step.ordinal,
                fromVersion: String(step.fromVersion),
                toVersion: String(step.toVersion),
                source: step.source,
                file: step.file,
                sqlHash: step.sqlHash,
                destructive: step.destructive,
                destructiveAck: step.destructiveAck
            };
            if (step.snapshotHash !== null) item.snapshotHash = step.snapshotHash;
            return item;
        })
    };

    files.push({ relPath: "migrations/manifest.json", content: canonicalize(manifest) });
    files.push({
        relPath: "migrations/" + ir.schema.name + "_migrations.hpp",
        content: generateMigrationWrapper(ir, steps, baseline, targetVersion, physicalSchema)
    });

    return { files, manifest, steps, targetVersion, physicalSchema };
}

export function loadDestructiveAcknowledgement(path) {
    if (!path) return null;
    return validateDestructiveAcknowledgement(JSON.parse(readFileSync(path, "utf8")));
}
