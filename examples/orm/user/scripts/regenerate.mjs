#!/usr/bin/env node
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const here = dirname(fileURLToPath(import.meta.url));
const example = resolve(here, "..");
const esdb = resolve(example, "..", "..", "..");
const frontend = join(esdb, "orm", "drizzle");
const compiler = join(esdb, "tools", "esdb-schema", "bin", "esdb-schema.mjs");
const ir = join(example, "generated", "user.ir.json");
const schema = join(example, "schema", "user.schema.ts");
const queries = join(example, "user.queries.json");
const kitOut = join(example, "drizzle-migrations");

function run(command, args, cwd) {
    const result = spawnSync(command, args, { cwd, encoding: "utf8", stdio: "inherit" });
    if (result.error) throw result.error;
    if (result.status !== 0) process.exit(result.status ?? 1);
}

run(process.execPath, [
    join(frontend, "src", "cli.ts"),
    "--schema", schema,
    "--queries", queries,
    "--schema-name", "user",
    "--namespace", "esdb.orm.user",
    "--out", ir
], esdb);

run(process.execPath, [compiler, "seal", "--in", ir, "--out", ir], esdb);

const kitBin = process.platform === "win32"
    ? join(frontend, "node_modules", ".bin", "drizzle-kit.cmd")
    : join(frontend, "node_modules", ".bin", "drizzle-kit");
if (!existsSync(kitBin)) {
    throw new Error("drizzle-kit is not installed; run npm ci in orm/drizzle first");
}
run(kitBin, ["generate", "--dialect", "sqlite", "--schema", schema, "--out", kitOut], frontend);

run(process.execPath, [
    compiler,
    "package-migrations",
    ir,
    "--kit-dir", kitOut,
    "--out", join(example, "generated"),
    "--kit-version", "0.31.11"
], esdb);

run(process.execPath, [
    compiler,
    "generate",
    ir,
    "--out", join(example, "generated")
], esdb);

console.log("ESDB ORM user artifacts regenerated.");
