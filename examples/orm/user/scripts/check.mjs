#!/usr/bin/env node
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const here = dirname(fileURLToPath(import.meta.url));
const example = resolve(here, "..");
const esdb = resolve(example, "..", "..", "..");
const frontend = join(esdb, "orm", "drizzle");
const compiler = join(esdb, "tools", "esdb-schema", "bin", "esdb-schema.mjs");
const goldenIr = join(example, "generated", "user.ir.json");
const schema = join(example, "schema", "user.schema.ts");
const queries = join(example, "user.queries.json");
const temp = mkdtempSync(join(tmpdir(), "esdb-orm-user-"));
const candidateIr = join(temp, "user.ir.json");

function run(command, args, cwd) {
    const result = spawnSync(command, args, { cwd, encoding: "utf8", stdio: "inherit" });
    if (result.error) throw result.error;
    if (result.status !== 0) process.exit(result.status ?? 1);
}

try {
    run(process.execPath, [
        join(frontend, "src", "cli.ts"),
        "--schema", schema,
        "--queries", queries,
        "--schema-name", "user",
        "--namespace", "esdb.orm.user",
        "--out", candidateIr
    ], esdb);
    run(process.execPath, [compiler, "seal", "--in", candidateIr, "--out", candidateIr], esdb);

    if (!readFileSync(candidateIr).equals(readFileSync(goldenIr))) {
        throw new Error("generated/user.ir.json is stale relative to the Drizzle authoring schema");
    }

    run(process.execPath, [compiler, "hash", goldenIr, "--check"], esdb);
    run(process.execPath, [
        compiler, "generate", goldenIr,
        "--out", join(example, "generated"), "--check"
    ], esdb);
    run(process.execPath, [
        compiler, "package-migrations", goldenIr,
        "--kit-dir", join(example, "drizzle-migrations"),
        "--out", join(example, "generated"),
        "--kit-version", "0.31.11",
        "--check"
    ], esdb);
    console.log("ESDB ORM user drift checks PASS.");
} finally {
    rmSync(temp, { recursive: true, force: true });
}
