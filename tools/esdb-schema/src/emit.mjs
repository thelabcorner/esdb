/*
 * esdb-schema: deterministic file emission.
 *
 * Generated artifacts are written as exact bytes (LF only, no newline
 * conversion, no BOM). `checkFiles` is the drift gate used by tests and CI.
 */

import { existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, relative, resolve, sep } from "node:path";

function toBytes(content) {
    return Buffer.isBuffer(content) ? content : Buffer.from(content, "utf8");
}

export function writeFiles(outDir, files) {
    const written = [];
    for (const file of files) {
        const target = resolve(outDir, file.relPath);
        const root = resolve(outDir);
        if (target !== root && !target.startsWith(root + sep)) {
            throw new Error(`refusing to write outside the output directory: ${file.relPath}`);
        }
        mkdirSync(dirname(target), { recursive: true });
        writeFileSync(target, toBytes(file.content));
        written.push(file.relPath);
    }
    return written;
}

function assertContained(outDir, relPath) {
    const root = resolve(outDir);
    const target = resolve(root, relPath);
    if (target !== root && !target.startsWith(root + sep)) {
        throw new Error(`refusing to inspect outside the output directory: ${relPath}`);
    }
    return { root, target };
}

function walkFiles(root, dir, out) {
    if (!existsSync(dir) || !statSync(dir).isDirectory()) return;
    const entries = readdirSync(dir, { withFileTypes: true });
    for (const entry of entries) {
        const full = resolve(dir, entry.name);
        if (entry.isDirectory()) walkFiles(root, full, out);
        else if (entry.isFile()) out.push(relative(root, full).split(sep).join("/"));
    }
}

export function checkFiles(outDir, files, options = {}) {
    const differences = [];
    const expectedPaths = new Set();
    const touchedRoots = new Set();

    for (const file of files) {
        const { target } = assertContained(outDir, file.relPath);
        const normalized = file.relPath.split("\\").join("/");
        expectedPaths.add(normalized);
        touchedRoots.add(normalized.includes("/") ? normalized.slice(0, normalized.indexOf("/")) : normalized);
        if (!existsSync(target)) {
            differences.push({ relPath: file.relPath, reason: "missing" });
            continue;
        }
        const expected = toBytes(file.content);
        const actual = readFileSync(target);
        if (!expected.equals(actual)) {
            differences.push({ relPath: file.relPath, reason: "content differs" });
        }
    }

    if (options.strict === true) {
        const root = resolve(outDir);
        for (const touchedRoot of touchedRoots) {
            const { target } = assertContained(outDir, touchedRoot);
            const actualPaths = [];
            walkFiles(root, target, actualPaths);
            for (const actualPath of actualPaths) {
                if (!expectedPaths.has(actualPath)) {
                    differences.push({ relPath: actualPath, reason: "unexpected generated file" });
                }
            }
        }
    }

    return { ok: differences.length === 0, differences };
}
