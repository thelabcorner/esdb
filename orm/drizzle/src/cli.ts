#!/usr/bin/env node
/*
 * Drizzle -> IR v1 extraction CLI.
 *
 *   node src/cli.ts --schema <schema.ts> --out <ir.json> [options]
 *
 * Options:
 *   --queries <queries.json>  declared named queries (IR query objects)
 *   --schema-name <name>      IR schema name (default: single table name)
 *   --namespace <ns>          informational IR namespace
 *   --source <label>          generator.source label (default: relative path)
 *
 * The output is unsealed; run `esdb-schema seal` to canonicalize and hash it.
 * This CLI imports drizzle-orm only through adapter.ts.
 */

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, relative, resolve, sep } from "node:path";
import { pathToFileURL } from "node:url";

import { collectTables } from "./adapter.ts";
import { buildIr, drizzleOrmPin, FRONTEND_VERSION } from "./build.ts";
import type { IrQuery } from "./ir-types.ts";

interface Options {
    schema: string | null;
    out: string | null;
    queries: string | null;
    schemaName: string | null;
    namespace: string | null;
    source: string | null;
    help: boolean;
}

function usage(): string {
    return [
        `esdb-orm-drizzle ${FRONTEND_VERSION} (frontend -> esdb.ir/v1)`,
        "",
        "usage: node src/cli.ts --schema <schema.ts> --out <ir.json> [options]",
        "",
        "options:",
        "  --queries <queries.json>  declared named queries (array, or { queries: [...] })",
        "  --schema-name <name>      IR schema name (default: the single table name)",
        "  --namespace <ns>          informational IR namespace",
        "  --source <label>          generator.source label (default: relative schema path)",
        "  --help                    print this text"
    ].join("\n");
}

function parseArgs(argv: string[]): Options {
    const options: Options = {
        schema: null,
        out: null,
        queries: null,
        schemaName: null,
        namespace: null,
        source: null,
        help: false
    };
    for (let i = 0; i < argv.length; i += 1) {
        const arg = argv[i];
        if (arg === "--help" || arg === "-h") {
            options.help = true;
        } else if (arg === "--schema" || arg === "--out" || arg === "--queries" || arg === "--schema-name" || arg === "--namespace" || arg === "--source") {
            const value = argv[i + 1];
            if (value === undefined || value.startsWith("--")) {
                throw new Error(`${arg} requires a value`);
            }
            const key = arg.slice(2).replace(/-([a-z])/g, (_match, letter: string) => letter.toUpperCase()) as
                | "schema"
                | "out"
                | "queries"
                | "schemaName"
                | "namespace"
                | "source";
            options[key] = value;
            i += 1;
        } else {
            throw new Error(`unknown argument "${arg}"`);
        }
    }
    return options;
}

function projectRelativeSource(schemaPath: string): string {
    let cursor = dirname(schemaPath);
    while (true) {
        if (
            existsSync(resolve(cursor, "CMakeLists.txt")) &&
            existsSync(resolve(cursor, "include", "esdb", "esdb.h"))
        ) {
            return relative(cursor, schemaPath).split(sep).join("/");
        }
        const parent = dirname(cursor);
        if (parent === cursor) {
            return resolve(schemaPath).split(sep).join("/");
        }
        cursor = parent;
    }
}

function loadDeclaredQueries(path: string): IrQuery[] {
    const parsed = JSON.parse(readFileSync(path, "utf8")) as unknown;
    if (Array.isArray(parsed)) {
        return parsed as IrQuery[];
    }
    if (parsed !== null && typeof parsed === "object" && Array.isArray((parsed as { queries?: unknown }).queries)) {
        return (parsed as { queries: IrQuery[] }).queries;
    }
    throw new Error(`${path}: declared queries must be an array or an object with a queries array`);
}

async function main(): Promise<number> {
    const options = parseArgs(process.argv.slice(2));
    if (options.help) {
        console.log(usage());
        return 0;
    }
    if (options.schema === null || options.out === null) {
        console.error("--schema and --out are required");
        return 2;
    }

    const schemaPath = resolve(options.schema);
    const schemaModule = (await import(pathToFileURL(schemaPath).href)) as Record<string, unknown>;
    const tables = collectTables(schemaModule);
    const declaredQueries = options.queries === null ? [] : loadDeclaredQueries(resolve(options.queries));
    const sourceLabel = options.source ?? projectRelativeSource(schemaPath);

    const document = buildIr({
        tables,
        declaredQueries,
        schemaName: options.schemaName ?? undefined,
        namespace: options.namespace ?? undefined,
        source: sourceLabel
    });

    const outPath = resolve(options.out);
    mkdirSync(dirname(outPath) || ".", { recursive: true });
    writeFileSync(outPath, `${JSON.stringify(document, null, 2)}\n`);

    console.log(`drizzle-orm pin: ${drizzleOrmPin()}`);
    console.log(`extracted ${document.tables.length} table(s), ${document.queries?.length ?? 0} query(ies) -> ${outPath}`);
    console.log("unsealed output; next: esdb-schema seal --in <ir.json> --out <ir.json>");
    return 0;
}

main().then(
    (code) => {
        process.exitCode = code;
    },
    (error: unknown) => {
        console.error(error instanceof Error ? `${error.name}: ${error.message}` : String(error));
        process.exitCode = 1;
    }
);
