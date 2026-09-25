/*
 * Shared helpers for the ESDB ORM tests.
 *
 * Tests are plain Node ESM modules exporting `run()`; run.mjs aggregates them.
 * Everything runs inside the ESDB tree; scratch output goes to tests/orm/.work
 * (gitignored).
 */

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

export const TESTS_DIR = dirname(fileURLToPath(import.meta.url));
export const ESDB_ROOT = resolve(TESTS_DIR, "..", "..");
export const FIXTURES_DIR = join(TESTS_DIR, "fixtures");
export const WORK_DIR = join(TESTS_DIR, ".work");

export const COMPILER_BIN = join(ESDB_ROOT, "tools", "esdb-schema", "bin", "esdb-schema.mjs");
export const EXAMPLE_DIR = join(ESDB_ROOT, "examples", "orm", "user");
export const EXAMPLE_GENERATED = join(EXAMPLE_DIR, "generated");
export const FRONTEND_CLI = join(ESDB_ROOT, "orm", "drizzle", "src", "cli.ts");
export const FRONTEND_SCHEMA = join(EXAMPLE_DIR, "schema", "user.schema.ts");
export const FRONTEND_QUERIES = join(EXAMPLE_DIR, "user.queries.json");
export const DRIZZLE_MODULES = join(ESDB_ROOT, "orm", "drizzle", "node_modules");

export class SkipError extends Error {
    constructor(reason) {
        super(reason);
        this.name = "SkipError";
    }
}

export function skip(reason) {
    throw new SkipError(reason);
}

export function assert(condition, message) {
    if (!condition) {
        throw new Error(`assertion failed: ${message}`);
    }
}

export function assertEqual(actual, expected, message) {
    if (actual !== expected) {
        throw new Error(`${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
}

export function assertDeepEqual(actual, expected, message) {
    const left = JSON.stringify(actual);
    const right = JSON.stringify(expected);
    if (left !== right) {
        throw new Error(`${message}: expected ${right}, got ${left}`);
    }
}

export function assertThrows(fn, match, message) {
    try {
        fn();
    } catch (error) {
        const text = `${error && error.code !== undefined ? error.code : ""} ${error && error.message ? error.message : String(error)}`;
        if (match !== null && match !== undefined && !text.includes(match)) {
            throw new Error(`${message}: error "${text}" does not include "${match}"`);
        }
        return error;
    }
    throw new Error(`${message}: expected an error`);
}

export function clone(value) {
    return JSON.parse(JSON.stringify(value));
}

export function readJson(path) {
    return JSON.parse(readFileSync(path, "utf8"));
}

export function readText(path) {
    return readFileSync(path, "utf8");
}

export function writeJson(path, value) {
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`);
}

export function loadFixture(name) {
    return readJson(join(FIXTURES_DIR, name));
}

export function sha256(input) {
    return createHash("sha256").update(input).digest("hex");
}

export function runCli(args, options = {}) {
    const result = spawnSync(process.execPath, [COMPILER_BIN, ...args], {
        encoding: "utf8",
        cwd: options.cwd ?? ESDB_ROOT
    });
    return { status: result.status, stdout: result.stdout ?? "", stderr: result.stderr ?? "" };
}

export function runNode(script, args, options = {}) {
    const result = spawnSync(process.execPath, [script, ...args], {
        encoding: "utf8",
        cwd: options.cwd ?? ESDB_ROOT
    });
    return { status: result.status, stdout: result.stdout ?? "", stderr: result.stderr ?? "" };
}

export function freshWorkDir(name) {
    const dir = join(WORK_DIR, name);
    rmSync(dir, { recursive: true, force: true });
    mkdirSync(dir, { recursive: true });
    return dir;
}

export function listFiles(root) {
    const out = [];
    const walk = (dir) => {
        for (const entry of readdirSync(dir, { withFileTypes: true })) {
            const full = join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(full);
            } else {
                out.push(full.slice(root.length + 1).split(sep).join("/"));
            }
        }
    };
    walk(root);
    return out.sort();
}

export function assertTreesEqual(leftRoot, rightRoot, message) {
    const leftFiles = listFiles(leftRoot);
    const rightFiles = listFiles(rightRoot);
    assertDeepEqual(leftFiles, rightFiles, `${message}: file lists differ`);
    for (const relPath of leftFiles) {
        const left = readFileSync(join(leftRoot, relPath));
        const right = readFileSync(join(rightRoot, relPath));
        if (!left.equals(right)) {
            throw new Error(`${message}: ${relPath} differs`);
        }
    }
}

export function sealFixture(fixture, workDir, name) {
    const rawPath = join(workDir, `${name}.raw.json`);
    const sealedPath = join(workDir, `${name}.sealed.json`);
    writeJson(rawPath, fixture);
    const result = runCli(["seal", "--in", rawPath, "--out", sealedPath]);
    assertEqual(result.status, 0, `seal ${name} failed: ${result.stderr}`);
    return sealedPath;
}

export async function loadCompiler() {
    const base = join(ESDB_ROOT, "tools", "esdb-schema", "src");
    const ir = await import(pathToFileURL(join(base, "ir.mjs")).href);
    const canonical = await import(pathToFileURL(join(base, "canonical.mjs")).href);
    const sql = await import(pathToFileURL(join(base, "sql.mjs")).href);
    const naming = await import(pathToFileURL(join(base, "naming.mjs")).href);
    const physical = await import(pathToFileURL(join(base, "physical-schema.mjs")).href);
    return { ir, canonical, sql, naming, physical };
}

export function firstDiagnosticCode(result) {
    return result.diagnostics.length > 0 ? result.diagnostics[0].code : null;
}

export function hasDiagnostic(result, code) {
    return result.diagnostics.some((diagnostic) => diagnostic.code === code);
}

export function expectDiagnostic(irModule, doc, code, label) {
    const result = irModule.validateDocument(doc);
    assert(!result.ok, `${label}: expected validation failure`);
    assert(
        hasDiagnostic(result, code),
        `${label}: expected ${code}, got ${result.diagnostics.map((diagnostic) => `${diagnostic.code}@${diagnostic.path}`).join(", ")}`
    );
    return result;
}
