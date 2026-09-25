/*
 * C++/C syntax checks for the generated bindings and the example consumer.
 *
 * The generated headers use only C++11 constructs; the check runs with
 * -std=c++14 because the installed MSVC standard library (clang's default STL
 * on this host) requires C++14 headers. No linking or execution happens here:
 * running the C++ lane requires a built ESDB library, which this test does not
 * assume.
 */

import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";

import {
    ESDB_ROOT,
    EXAMPLE_DIR,
    EXAMPLE_GENERATED,
    assertEqual,
    freshWorkDir,
    loadFixture,
    runCli,
    sealFixture,
    skip
} from "./helpers.mjs";

function findTool(name) {
    const candidates = [];
    if (process.env.CLANG_PATH && existsSync(process.env.CLANG_PATH)) {
        candidates.push(process.env.CLANG_PATH);
    }
    candidates.push(`C:\\Program Files\\LLVM\\bin\\${name}.exe`);
    candidates.push(name);
    for (const candidate of candidates) {
        const result = spawnSync(candidate, ["--version"], { encoding: "utf8" });
        if (result.status === 0) {
            return candidate;
        }
    }
    return null;
}

function compile(compiler, args, label) {
    const result = spawnSync(compiler, args, { encoding: "utf8" });
    assertEqual(result.status, 0, `${label}: ${result.stderr || result.stdout}`);
    assertEqual(result.stderr.trim(), "", `${label}: no diagnostics`);
}

export async function run() {
    const clangxx = findTool("clang++");
    if (clangxx === null) {
        skip("clang++ not found (set CLANG_PATH or install LLVM)");
    }
    const clang = findTool("clang");

    const includes = ["-I", join(ESDB_ROOT, "include"), "-I", join(ESDB_ROOT, "third_party", "sqlite")];

    compile(
        clangxx,
        ["-fsyntax-only", "-std=c++14", "-Wall", "-Wextra", ...includes, join(EXAMPLE_GENERATED, "cpp", "user_repository.hpp")],
        "generated user repository"
    );

    compile(
        clangxx,
        ["-fsyntax-only", "-std=c++14", "-Wall", "-Wextra", ...includes, join(EXAMPLE_DIR, "main.cpp")],
        "example main.cpp"
    );

    compile(
        clangxx,
        [
            "-fsyntax-only",
            "-std=c++14",
            "-Wall",
            "-Wextra",
            ...includes,
            "-I",
            join(ESDB_ROOT, "..", "esabi", "include"),
            join(EXAMPLE_DIR, "bridge", "user_orm_bridge.cpp")
        ],
        "concrete user ESABI bridge"
    );

    compile(
        clangxx,
        [
            "-fsyntax-only",
            "-std=c++14",
            "-Wall",
            "-Wextra",
            ...includes,
            join(EXAMPLE_GENERATED, "migrations", "user_migrations.hpp")
        ],
        "generated user migration wrapper"
    );

    const work = freshWorkDir("cpp-syntax");
    const typesSealed = sealFixture(loadFixture("types.ir.json"), work, "types");
    const typesDir = join(work, "gen");
    assertEqual(runCli(["build", typesSealed, "--out", typesDir]).status, 0, "types fixture build");
    compile(
        clangxx,
        ["-fsyntax-only", "-std=c++14", "-Wall", "-Wextra", ...includes, join(typesDir, "cpp", "metric_repository.hpp")],
        "generated metric repository"
    );

    if (clang === null) {
        return;
    }
    compile(
        clang,
        [
            "-fsyntax-only",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-I",
            join(ESDB_ROOT, "..", "esabi", "include"),
            "-I",
            join(EXAMPLE_GENERATED, "bridge"),
            join(EXAMPLE_GENERATED, "bridge", "user_orm_bridge_stub.c")
        ],
        "generated bridge stub"
    );
}
