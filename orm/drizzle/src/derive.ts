/*
 * Pure IR derivation (no drizzle imports).
 *
 * Standard CRUD is derived deterministically from extracted IR tables:
 *   <table>.findById      (single-column primary key)
 *   <table>.findBy<Col>   (each single-column unique key)
 *   <table>.list          (ordered by primary key, limit/offset params)
 *   <table>.insert        (all columns without defaults)
 *   <table>.deleteById    (single-column primary key)
 *
 * Updates are never derived: choosing the mutable column set is a product
 * decision, so update queries are declared explicitly (see the example's
 * user.queries.json) and validated by the compiler.
 */

import type { IrQuery, IrTable } from "./ir-types.ts";
import { toPascal } from "./naming.ts";

export class DeriveError extends Error {
    readonly code: string;

    constructor(code: string, message: string) {
        super(`${code} ${message}`);
        this.name = "DeriveError";
        this.code = code;
    }
}

export function deriveCrudQueries(table: IrTable): IrQuery[] {
    const queries: IrQuery[] = [];
    const singlePrimaryKey = table.primary_key.length === 1 ? table.primary_key[0] : null;

    if (singlePrimaryKey !== null) {
        queries.push({
            name: `${table.name}.findById`,
            kind: "select",
            table: table.name,
            where: [{ column: singlePrimaryKey, op: "eq", param: singlePrimaryKey }],
            cardinality: "one"
        });
    }

    for (const key of table.uniques) {
        if (key.length !== 1) {
            continue;
        }
        const column = key[0];
        if (column === singlePrimaryKey) {
            continue;
        }
        queries.push({
            name: `${table.name}.findBy${toPascal(column)}`,
            kind: "select",
            table: table.name,
            where: [{ column, op: "eq", param: column }],
            cardinality: "one"
        });
    }

    if (singlePrimaryKey !== null) {
        queries.push({
            name: `${table.name}.list`,
            kind: "select",
            table: table.name,
            order_by: [{ column: singlePrimaryKey, direction: "asc" }],
            limit: "limit",
            offset: "offset",
            cardinality: "many"
        });
    }

    const insertColumns = table.columns.filter((column) => column.default === undefined);
    if (insertColumns.length > 0) {
        queries.push({
            name: `${table.name}.insert`,
            kind: "insert",
            table: table.name,
            values: insertColumns.map((column) => ({ column: column.name, param: column.name })),
            cardinality: "changes"
        });
    }

    if (singlePrimaryKey !== null) {
        queries.push({
            name: `${table.name}.deleteById`,
            kind: "delete",
            table: table.name,
            where: [{ column: singlePrimaryKey, op: "eq", param: singlePrimaryKey }],
            cardinality: "changes"
        });
    }

    const seen = new Set<string>();
    for (const query of queries) {
        if (seen.has(query.name)) {
            throw new DeriveError("ORM010", `derived query name collision: "${query.name}"`);
        }
        seen.add(query.name);
    }
    return queries;
}

export function mergeDeclaredQueries(derived: IrQuery[], declared: readonly IrQuery[]): IrQuery[] {
    const names = new Set(derived.map((query) => query.name));
    const declaredSorted = [...declared].sort((left, right) =>
        left.name < right.name ? -1 : left.name > right.name ? 1 : 0
    );
    for (const query of declaredSorted) {
        if (typeof query !== "object" || query === null || typeof query.name !== "string") {
            throw new DeriveError("ORM010", "declared query entries must be IR query objects with a name");
        }
        if (names.has(query.name)) {
            throw new DeriveError("ORM010", `declared query "${query.name}" collides with another generated/declared query`);
        }
        names.add(query.name);
    }
    return [...derived, ...declaredSorted].sort((left, right) =>
        left.name < right.name ? -1 : left.name > right.name ? 1 : 0
    );
}
