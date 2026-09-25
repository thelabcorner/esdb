/*
 * Deterministic generation: byte-for-byte stability, golden-tree currency,
 * --check drift detection, and sealing gates.
 */

import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import {
    EXAMPLE_GENERATED,
    assert,
    assertDeepEqual,
    assertEqual,
    clone,
    freshWorkDir,
    listFiles,
    readJson,
    readText,
    runCli,
    writeJson
} from "./helpers.mjs";

export async function run() {
    const goldenIrPath = join(EXAMPLE_GENERATED, "user.ir.json");
    const goldenIr = readJson(goldenIrPath);
    const work = freshWorkDir("codegen");
    const firstDir = join(work, "a");
    const secondDir = join(work, "b");

    const first = runCli(["build", goldenIrPath, "--out", firstDir]);
    assertEqual(first.status, 0, `first build: ${first.stderr}`);
    const second = runCli(["build", goldenIrPath, "--out", secondDir]);
    assertEqual(second.status, 0, `second build: ${second.stderr}`);

    assertDeepEqual(listFiles(firstDir), listFiles(secondDir), "build file sets are identical");
    for (const relPath of listFiles(firstDir)) {
        assert(
            readFileSync(join(firstDir, relPath)).equals(readFileSync(join(secondDir, relPath))),
            `${relPath} is byte-identical across runs`
        );
    }

    const goldenFiles = listFiles(EXAMPLE_GENERATED);
    const generatedBindings = listFiles(firstDir);
    const goldenBindings = goldenFiles.filter(
        (relPath) => relPath !== "user.ir.json" && !relPath.startsWith("migrations/")
    );
    assertDeepEqual(
        goldenBindings,
        generatedBindings,
        "golden binding tree is exactly the compiler-generated model artifacts"
    );
    assert(
        goldenFiles.includes("migrations/manifest.json") &&
        goldenFiles.includes("migrations/user_migrations.hpp"),
        "production migration package coexists with generated model artifacts"
    );
    for (const relPath of generatedBindings) {
        assert(
            readFileSync(join(firstDir, relPath)).equals(readFileSync(join(EXAMPLE_GENERATED, relPath))),
            `golden ${relPath} is current`
        );
    }

    const checkOk = runCli(["build", goldenIrPath, "--out", EXAMPLE_GENERATED, "--check"]);
    assertEqual(checkOk.status, 0, `--check passes on the golden tree: ${checkOk.stderr}`);
    assert(checkOk.stdout.includes("ok"), "--check reports ok");

    const tamperedDir = join(work, "tampered");
    assertEqual(runCli(["build", goldenIrPath, "--out", tamperedDir]).status, 0, "tamper baseline build");
    writeFileSync(join(tamperedDir, "ts", "user.ts"), "tampered\n");
    const checkBad = runCli(["build", goldenIrPath, "--out", tamperedDir, "--check"]);
    assertEqual(checkBad.status, 1, "--check detects drift");
    assert(checkBad.stdout.includes("stale ts/user.ts"), "--check names the stale file");

    const unsealedDoc = clone(goldenIr);
    delete unsealedDoc.integrity;
    const unsealedPath = join(work, "unsealed.json");
    writeJson(unsealedPath, unsealedDoc);
    const unsealed = runCli(["build", unsealedPath, "--out", join(work, "unsealed")]);
    assertEqual(unsealed.status, 1, "unsealed IR is refused");
    assert(unsealed.stderr.includes("not sealed"), `unsealed message: ${unsealed.stderr}`);

    const mismatchDoc = clone(goldenIr);
    mismatchDoc.schema.namespace = "tampered";
    const mismatchPath = join(work, "mismatch.json");
    writeJson(mismatchPath, mismatchDoc);
    const mismatch = runCli(["build", mismatchPath, "--out", join(work, "mismatch")]);
    assertEqual(mismatch.status, 1, "tampered IR is refused");
    assert(mismatch.stderr.includes("hash mismatch"), `hash mismatch message: ${mismatch.stderr}`);

    const generateDir = join(work, "generate-only");
    assertEqual(runCli(["generate", goldenIrPath, "--out", generateDir]).status, 0, "generate succeeds");
    assertDeepEqual(
        listFiles(generateDir),
        [
            "bridge/user_orm_bridge.h",
            "bridge/user_orm_bridge.json",
            "bridge/user_orm_bridge_stub.c",
            "cpp/user_repository.hpp",
            "es3/user_repository.jsx",
            "ts/user.ts"
        ],
        "generate produces the binding artifacts only"
    );

    const retiredMigrate = runCli(["migrate", goldenIrPath, "--out", join(work, "retired-migrate")]);
    assertEqual(retiredMigrate.status, 2, "legacy IR-authored migrate command is retired");
    assert(
        retiredMigrate.stderr.includes("package-migrations"),
        "retired migrate directs callers to the pinned Drizzle Kit packaging path"
    );

    for (const relPath of ["cpp/user_repository.hpp", "es3/user_repository.jsx", "ts/user.ts"]) {
        assert(
            readText(join(firstDir, relPath)).includes(`sha256:${goldenIr.integrity.hash}`),
            `${relPath} embeds the IR hash`
        );
    }
    const bridgeManifest = readJson(join(firstDir, "bridge", "user_orm_bridge.json"));
    assertEqual(bridgeManifest.ir_hash, goldenIr.integrity.hash, "bridge manifest embeds the IR hash");
    const updatePlan = bridgeManifest.operations.find((operation) => operation.name === "userUpdateName");
    assertDeepEqual(
        updatePlan.api_params.map((param) => param.name),
        ["id", "name"],
        "bridge manifest preserves ergonomic UPDATE API order"
    );
    assertDeepEqual(
        updatePlan.bind_params.map((param) => param.name),
        ["name", "id"],
        "bridge manifest records SQLite UPDATE bind order independently"
    );
    assert(
        updatePlan.api_params.every((param) => typeof param.nullable === "boolean"),
        "bridge manifest nullability is machine-readable boolean metadata"
    );

    const goldenText = readText(goldenIrPath);
    assert(!goldenText.includes("\n"), "golden IR is canonical single-line bytes");
    assertEqual(runCli(["hash", goldenIrPath, "--check"]).status, 0, "golden hash --check passes");
}
