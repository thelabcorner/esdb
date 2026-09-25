#!/usr/bin/env node

import { existsSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");

function parseArgs(argv) {
    const out = { buildDir: join(root, "build-orm-bench"), repeats: 9, enforce: false, jsonOnly: false };
    for (let i = 0; i < argv.length; i += 1) {
        const arg = argv[i];
        if (arg === "--build-dir") out.buildDir = resolve(argv[++i]);
        else if (arg === "--repeats") out.repeats = Number(argv[++i]);
        else if (arg === "--enforce") out.enforce = true;
        else if (arg === "--json-only") out.jsonOnly = true;
        else throw new Error(`unknown argument: ${arg}`);
    }
    if (!Number.isInteger(out.repeats) || out.repeats < 5) {
        throw new Error("--repeats must be an integer >= 5");
    }
    return out;
}

function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    const mid = Math.floor(sorted.length / 2);
    return sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
}

function spread(values) {
    const med = median(values);
    if (med === 0) return 0;
    return (Math.max(...values) - Math.min(...values)) / med;
}

function exePath(buildDir, name) {
    const candidates = process.platform === "win32"
        ? [join(buildDir, "Release", `${name}.exe`), join(buildDir, `${name}.exe`)]
        : [join(buildDir, name), join(buildDir, "Release", name)];
    return candidates.find(existsSync) ?? candidates[0];
}

function balancedRunOrder(repeats) {
    const order = [];
    while (order.length < repeats * 2) {
        order.push("generated", "handwritten", "handwritten", "generated");
    }
    return order.slice(0, repeats * 2);
}

function runScheduled(executables, repeats) {
    const rows = { generated: [], handwritten: [] };
    const order = [];
    for (const implementation of balancedRunOrder(repeats)) {
        const path = executables[implementation];
        if (!existsSync(path)) throw new Error(`benchmark executable not found: ${path}`);
        const result = spawnSync(path, [], { cwd: root, encoding: "utf8", windowsHide: true });
        if (result.status !== 0) {
            throw new Error(`${path} failed (${result.status}): ${result.stderr || result.stdout}`);
        }
        const lines = result.stdout.trim().split(/\r?\n/).filter(Boolean);
        rows[implementation].push(JSON.parse(lines[lines.length - 1]));
        order.push(implementation);
    }
    return { rows, order };
}

function summarize(rows) {
    const numericKeys = Object.keys(rows[0]).filter((key) => typeof rows[0][key] === "number");
    const summary = {
        implementation: rows[0].implementation,
        repeats: rows.length,
        statement_reuse: rows.every((row) => row.statement_reuse === true),
        metrics: {}
    };
    for (const key of numericKeys) {
        const values = rows.map((row) => row[key]);
        summary.metrics[key] = {
            median: median(values),
            min: Math.min(...values),
            max: Math.max(...values),
            relative_spread: spread(values)
        };
    }
    const scanSamples = rows.flatMap((row) => row.scan_samples_ns ?? []);
    summary.scan_internal = {
        samples: scanSamples.length,
        median: median(scanSamples),
        min: Math.min(...scanSamples),
        max: Math.max(...scanSamples),
        relative_spread: spread(scanSamples)
    };
    return summary;
}

function fileBytes(path) {
    return existsSync(path) ? statSync(path).size : null;
}

function objectPath(buildDir, target, stem) {
    const candidates = process.platform === "win32"
        ? [
            join(buildDir, `${target}.dir`, "Release", `${stem}.obj`),
            join(buildDir, `${target}.dir`, "RelWithDebInfo", `${stem}.obj`)
        ]
        : [
            join(buildDir, "CMakeFiles", `${target}.dir`, "bench", `${stem}.cpp.o`),
            join(buildDir, `${target}.dir`, `${stem}.cpp.o`)
        ];
    return candidates.find(existsSync) ?? candidates[0];
}

const options = parseArgs(process.argv.slice(2));
const generatedExe = exePath(options.buildDir, "esdb_orm_bench_generated");
const handwrittenExe = exePath(options.buildDir, "esdb_orm_bench_handwritten");

const scheduled = runScheduled({
    generated: generatedExe,
    handwritten: handwrittenExe
}, options.repeats);
const handwritten = summarize(scheduled.rows.handwritten);
const generated = summarize(scheduled.rows.generated);

const hm = handwritten.metrics;
const gm = generated.metrics;
const decodedRowsMatch =
    gm.decoded_row_checksum.median === hm.decoded_row_checksum.median;
const ratios = {
    startup_latency: gm.startup_ns.median / hm.startup_ns.median,
    prepare_latency: gm.prepare_ns.median / hm.prepare_ns.median,
    point_read_latency: gm.point_read_ns.median / hm.point_read_ns.median,
    point_write_latency: gm.point_write_ns.median / hm.point_write_ns.median,
    scan_10k_latency: gm.scan_10k_ns.median / hm.scan_10k_ns.median,
    bulk_insert_throughput: gm.bulk_insert_rows_per_second.median / hm.bulk_insert_rows_per_second.median,
    prepare_per_statement_latency: gm.prepare_per_statement_ns.median / hm.prepare_per_statement_ns.median,
    prepare_each_point_read_latency: gm.prepare_each_point_read_ns.median / hm.prepare_each_point_read_ns.median,
    generated_persistent_vs_prepare_each:
        gm.prepare_each_point_read_ns.median / gm.point_read_ns.median,
    handwritten_persistent_vs_prepare_each:
        hm.prepare_each_point_read_ns.median / hm.point_read_ns.median
};

const generatedObject = objectPath(
    options.buildDir, "esdb_orm_bench_generated", "orm_bench_generated");
const handwrittenObject = objectPath(
    options.buildDir, "esdb_orm_bench_handwritten", "orm_bench_handwritten");
const sizes = {
    generated_executable_bytes: fileBytes(generatedExe),
    handwritten_executable_bytes: fileBytes(handwrittenExe),
    generated_object_bytes: fileBytes(generatedObject),
    handwritten_object_bytes: fileBytes(handwrittenObject),
    generated_repository_source_bytes: fileBytes(join(root, "examples", "orm", "user", "generated", "cpp", "user_repository.hpp")),
    handwritten_repository_source_bytes: fileBytes(join(root, "bench", "orm_bench_handwritten.cpp"))
};
sizes.executable_ratio = sizes.generated_executable_bytes / sizes.handwritten_executable_bytes;
sizes.object_ratio =
    sizes.generated_object_bytes !== null && sizes.handwritten_object_bytes !== null
        ? sizes.generated_object_bytes / sizes.handwritten_object_bytes
        : null;
sizes.source_ratio = sizes.generated_repository_source_bytes / sizes.handwritten_repository_source_bytes;

const budgets = {
    point_read_latency_max: 1.15,
    point_write_latency_max: 1.15,
    scan_10k_latency_max: 1.10
};
const stabilityLimits = {
    scan_10k_repeats_min: 9,
    scan_10k_relative_spread_max: 0.20
};
const scanProcessMedianSpread = Math.max(
    gm.scan_10k_ns.relative_spread,
    hm.scan_10k_ns.relative_spread
);
const stabilityStatus = {
    scan_10k:
        options.repeats >= stabilityLimits.scan_10k_repeats_min &&
        scanProcessMedianSpread <= stabilityLimits.scan_10k_relative_spread_max
};
const budgetStatus = {
    point_read: ratios.point_read_latency <= budgets.point_read_latency_max,
    point_write: ratios.point_write_latency <= budgets.point_write_latency_max,
    scan_10k: stabilityStatus.scan_10k && ratios.scan_10k_latency <= budgets.scan_10k_latency_max,
    statement_reuse: generated.statement_reuse && handwritten.statement_reuse,
    decoded_rows_match: decodedRowsMatch
};

const report = {
    format: "esdb.orm-benchmark/v2",
    execution_order: scheduled.order,
    generated,
    handwritten,
    ratios,
    sizes,
    budgets,
    stability_limits: stabilityLimits,
    stability_measurements: {
        scan_10k_process_median_relative_spread: scanProcessMedianSpread
    },
    stability_status: stabilityStatus,
    decoded_rows_match: decodedRowsMatch,
    budget_status: budgetStatus,
    all_provisional_budgets_pass: Object.values(budgetStatus).every(Boolean)
};

if (!options.jsonOnly) {
    const ns = (value) => `${value.toFixed(1)} ns`;
    const ms = (value) => `${(value / 1e6).toFixed(3)} ms`;
    const rowsPerSecond = (value) => `${value.toFixed(0)} rows/s`;
    const ratio = (value) => `${value.toFixed(4)}x`;
    const percent = (value) => `${(value * 100).toFixed(1)}%`;
    const detail = (generatedValue, handwrittenValue, format) =>
        `generated ${format(generatedValue.median)} [min ${format(generatedValue.min)}, max ${format(generatedValue.max)}, spread ${percent(generatedValue.relative_spread)}] / handwritten ${format(handwrittenValue.median)} [min ${format(handwrittenValue.min)}, max ${format(handwrittenValue.max)}, spread ${percent(handwrittenValue.relative_spread)}]`;
    console.log("ESDB ORM benchmark");
    console.log(`  execution order: ${scheduled.order.join(" -> ")}`);
    console.log(`  startup:        ${detail(gm.startup_ns, hm.startup_ns, ns)} = ${ratio(ratios.startup_latency)}`);
    console.log(`  prepare:        ${detail(gm.prepare_ns, hm.prepare_ns, ns)} = ${ratio(ratios.prepare_latency)}`);
    console.log(`  point read:     ${detail(gm.point_read_ns, hm.point_read_ns, ns)} = ${ratio(ratios.point_read_latency)}`);
    console.log(`  point write:    ${detail(gm.point_write_ns, hm.point_write_ns, ns)} = ${ratio(ratios.point_write_latency)}`);
    console.log(`  scan 10k:       ${detail(gm.scan_10k_ns, hm.scan_10k_ns, ms)} = ${ratio(ratios.scan_10k_latency)}`);
    console.log(`  scan internal:  generated ${generated.scan_internal.samples} samples [min ${ms(generated.scan_internal.min)}, max ${ms(generated.scan_internal.max)}, spread ${percent(generated.scan_internal.relative_spread)}] / handwritten ${handwritten.scan_internal.samples} samples [min ${ms(handwritten.scan_internal.min)}, max ${ms(handwritten.scan_internal.max)}, spread ${percent(handwritten.scan_internal.relative_spread)}]`);
    console.log(`  bulk insert:    ${detail(gm.bulk_insert_rows_per_second, hm.bulk_insert_rows_per_second, rowsPerSecond)} = ${ratio(ratios.bulk_insert_throughput)}`);
    console.log(`  prepare/stmt:   ${detail(gm.prepare_per_statement_ns, hm.prepare_per_statement_ns, ns)} = ${ratio(ratios.prepare_per_statement_latency)}`);
    console.log(`  prepare-each:   ${detail(gm.prepare_each_point_read_ns, hm.prepare_each_point_read_ns, ns)} = ${ratio(ratios.prepare_each_point_read_latency)}`);
    console.log(`  decoded rows:   ${decodedRowsMatch ? "MATCH" : "MISMATCH"} (checksum generated ${gm.decoded_row_checksum.median}, handwritten ${hm.decoded_row_checksum.median})`);
    console.log(`  persistent reuse speedup: generated ${ratio(ratios.generated_persistent_vs_prepare_each)} / handwritten ${ratio(ratios.handwritten_persistent_vs_prepare_each)}`);
    console.log(`  statement memory: generated ${gm.statement_memory_bytes.median} B / handwritten ${hm.statement_memory_bytes.median} B`);
    console.log(`  peak outstanding SQLite allocs (read/write/scan/bulk): generated ${gm.read_peak_allocation_count.median}/${gm.write_peak_allocation_count.median}/${gm.scan_peak_allocation_count.median}/${gm.bulk_peak_allocation_count.median} / handwritten ${hm.read_peak_allocation_count.median}/${hm.write_peak_allocation_count.median}/${hm.scan_peak_allocation_count.median}/${hm.bulk_peak_allocation_count.median}`);
    console.log(`  executable size: generated ${sizes.generated_executable_bytes} B / handwritten ${sizes.handwritten_executable_bytes} B = ${ratio(sizes.executable_ratio)}`);
    if (sizes.object_ratio !== null) {
        console.log(`  object size: generated ${sizes.generated_object_bytes} B / handwritten ${sizes.handwritten_object_bytes} B = ${ratio(sizes.object_ratio)}`);
    }
    console.log(`  source size: generated ${sizes.generated_repository_source_bytes} B / handwritten ${sizes.handwritten_repository_source_bytes} B = ${ratio(sizes.source_ratio)}`);
    console.log(`  scan stability: ${stabilityStatus.scan_10k ? "STABLE" : "UNSTABLE"} (repeats ${options.repeats}/${stabilityLimits.scan_10k_repeats_min} min, max process-median spread ${percent(scanProcessMedianSpread)}/${percent(stabilityLimits.scan_10k_relative_spread_max)} max)`);
    console.log(`  provisional latency budgets: ${report.all_provisional_budgets_pass ? "PASS" : "FAIL"}`);
}
console.log(JSON.stringify(report));

if (options.enforce && !report.all_provisional_budgets_pass) process.exitCode = 1;
