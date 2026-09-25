/*
 * Frontend assembly: Drizzle tables -> IR v1 document.
 *
 * The document produced here is intentionally *unsealed*: canonicalization,
 * the semantic-projection hash, and sealing belong to the compiler
 * (`esdb-schema seal`), which keeps one canonicalization authority.
 */

import { readFileSync } from "node:fs";

import { extractTables, type DrizzleTables } from "./adapter.ts";
import { DeriveError, deriveCrudQueries, mergeDeclaredQueries } from "./derive.ts";
import { IR_VERSION, type IrDocument, type IrQuery } from "./ir-types.ts";

export const FRONTEND_NAME = "esdb-orm-drizzle";
export const FRONTEND_VERSION = "0.2.0";

export function drizzleOrmPin(): string {
    const packageJson = JSON.parse(readFileSync(new URL("../package.json", import.meta.url), "utf8")) as {
        dependencies?: Record<string, string>;
    };
    return packageJson.dependencies?.["drizzle-orm"] ?? "unknown";
}

export interface BuildIrOptions {
    tables: DrizzleTables;
    declaredQueries?: readonly IrQuery[];
    schemaName?: string;
    namespace?: string;
    source: string;
}

export function buildIr(options: BuildIrOptions): IrDocument {
    const tables = extractTables(options.tables);
    if (tables.length === 0) {
        throw new DeriveError("ORM011", "no Drizzle tables were supplied");
    }
    let schemaName = options.schemaName;
    if (schemaName === undefined) {
        if (tables.length !== 1) {
            throw new DeriveError("ORM011", "schema name is ambiguous for multiple tables; pass --schema-name");
        }
        schemaName = tables[0].name;
    }

    const derived = tables.flatMap((table) => deriveCrudQueries(table));
    const queries = mergeDeclaredQueries(derived, options.declaredQueries ?? []);

    const document: IrDocument = {
        ir_version: IR_VERSION,
        schema: { name: schemaName, dialect: "sqlite" },
        tables,
        queries,
        generator: { name: FRONTEND_NAME, version: FRONTEND_VERSION, source: options.source },
        annotations: { frontend: `drizzle-orm@${drizzleOrmPin()}` }
    };
    if (options.namespace !== undefined) {
        document.schema.namespace = options.namespace;
    }
    return document;
}
