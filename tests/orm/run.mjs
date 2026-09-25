/*
 * ESDB ORM test runner.
 *
 *   node tests/orm/run.mjs            (from the esdb/ directory)
 *   node ../../tests/orm/run.mjs      (from tools/esdb-schema/)
 *
 * Runs every *.test.mjs module in this directory. Each module exports
 * `run()`; a thrown SkipError marks the module skipped with a reason.
 */

import { mkdirSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { SkipError, WORK_DIR } from "./helpers.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const files = readdirSync(here)
    .filter((name) => name.endsWith(".test.mjs"))
    .sort();

mkdirSync(WORK_DIR, { recursive: true });

let passed = 0;
let failed = 0;
let skipped = 0;
const started = Date.now();

for (const file of files) {
    const label = file.replace(/\.test\.mjs$/, "");
    try {
        const module = await import(pathToFileURL(join(here, file)).href);
        if (typeof module.run !== "function") {
            throw new Error("test module must export run()");
        }
        await module.run();
        passed += 1;
        console.log(`PASS ${label}`);
    } catch (error) {
        if (error instanceof SkipError) {
            skipped += 1;
            console.log(`SKIP ${label}: ${error.message}`);
        } else {
            failed += 1;
            console.error(`FAIL ${label}`);
            console.error(error && error.stack ? error.stack : String(error));
        }
    }
}

console.log(`${passed} passed, ${failed} failed, ${skipped} skipped (${Date.now() - started} ms)`);
process.exitCode = failed === 0 ? 0 : 1;
