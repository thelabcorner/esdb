/*
 * IR v1 types: the frontend-side mirror of orm/ir/esdb-ir-v1.md.
 *
 * The compiler (tools/esdb-schema/) is normative. This mirror exists so the
 * Drizzle frontend can build a typed IR document before `esdb-schema seal`
 * canonicalizes and hashes it.
 */

export const IR_VERSION = "esdb.ir/v1" as const;

export type IrScalarType = "integer" | "text" | "real" | "blob";
export type IrQueryKind = "select" | "insert" | "upsert" | "update" | "delete";
export type IrCardinality = "one" | "many" | "changes";
export type IrLiteralType = "integer" | "text" | "real" | "blob" | "null";

export interface IrLiteral {
    kind: "literal";
    type: IrLiteralType;
    value: string;
}

export interface IrColumn {
    name: string;
    sql_name: string;
    type: IrScalarType;
    nullable: boolean;
    primary_key: boolean;
    autoincrement: boolean;
    unique: boolean;
    default?: IrLiteral;
}

export type IrForeignKeyAction = "no action" | "restrict" | "cascade" | "set null" | "set default";

export interface IrForeignKey {
    name?: string;
    columns: string[];
    references: {
        table: string;
        columns: string[];
    };
    on_update: IrForeignKeyAction;
    on_delete: IrForeignKeyAction;
}

export interface IrCheck {
    name: string;
    sql: string;
}

export interface IrIndexColumnTerm {
    kind: "column";
    column: string;
    direction: "asc" | "desc";
}

export interface IrIndexExpressionTerm {
    kind: "expression";
    sql: string;
}

export type IrIndexTerm = IrIndexColumnTerm | IrIndexExpressionTerm;

export interface IrIndex {
    name: string;
    unique: boolean;
    terms: IrIndexTerm[];
    where?: string;
}

export interface IrTable {
    name: string;
    sql_name: string;
    columns: IrColumn[];
    primary_key: string[];
    uniques: string[][];
    foreign_keys?: IrForeignKey[];
    checks?: IrCheck[];
    indexes?: IrIndex[];
}

export interface IrCondition {
    table?: string;
    column: string;
    op: "eq";
    param: string;
}

export interface IrAssignment {
    column: string;
    param: string;
}

export interface IrOrderTerm {
    table?: string;
    column: string;
    direction: "asc" | "desc";
}

export interface IrJoinColumnRef {
    table: string;
    column: string;
}

export interface IrJoinCondition {
    left: IrJoinColumnRef;
    right: IrJoinColumnRef;
}

export interface IrJoin {
    kind: "inner" | "left";
    table: string;
    on: IrJoinCondition[];
}

export interface IrConflictPolicy {
    target: string[];
    action: "nothing" | "update";
    set?: IrAssignment[];
}

export interface IrQuery {
    name: string;
    kind: IrQueryKind;
    table: string;
    cardinality: IrCardinality;
    where?: IrCondition[];
    joins?: IrJoin[];
    order_by?: IrOrderTerm[];
    limit?: string;
    offset?: string;
    values?: IrAssignment[];
    set?: IrAssignment[];
    on_conflict?: IrConflictPolicy;
}

export interface IrGenerator {
    name: string;
    version: string;
    source: string;
}

export interface IrDocument {
    ir_version: typeof IR_VERSION;
    schema: {
        name: string;
        dialect: "sqlite";
        namespace?: string;
    };
    tables: IrTable[];
    queries?: IrQuery[];
    generator?: IrGenerator;
    annotations?: Record<string, string>;
}
