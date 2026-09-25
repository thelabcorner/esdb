/*
 * Public surface of the ESDB Drizzle frontend.
 *
 * Drizzle-specific imports live only in adapter.ts.
 */

export { DrizzleAdapterError, collectTables, extractTables, type DrizzleTables } from "./adapter.ts";
export { DeriveError, deriveCrudQueries, mergeDeclaredQueries } from "./derive.ts";
export { buildIr, drizzleOrmPin, FRONTEND_NAME, FRONTEND_VERSION, type BuildIrOptions } from "./build.ts";
export {
    IR_VERSION,
    type IrAssignment,
    type IrCardinality,
    type IrColumn,
    type IrCheck,
    type IrCondition,
    type IrConflictPolicy,
    type IrDocument,
    type IrForeignKey,
    type IrForeignKeyAction,
    type IrGenerator,
    type IrIndex,
    type IrIndexTerm,
    type IrJoin,
    type IrJoinColumnRef,
    type IrJoinCondition,
    type IrLiteral,
    type IrLiteralType,
    type IrOrderTerm,
    type IrQuery,
    type IrQueryKind,
    type IrScalarType,
    type IrTable
} from "./ir-types.ts";
