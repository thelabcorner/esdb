/*
 * Relational ORM vertical slice: multi-table schema structure, compile-time
 * joins, deterministic upsert, generated language surfaces, and the physical
 * Kit/IR-style semantic differential.
 */

import { join } from "node:path";

import {
    assert,
    assertDeepEqual,
    assertEqual,
    freshWorkDir,
    loadCompiler,
    readJson,
    readText,
    runCli,
    sealFixture
} from "./helpers.mjs";

function column(name, sqlName, type, options = {}) {
    return {
        name,
        sql_name: sqlName,
        type,
        nullable: options.nullable === true,
        primary_key: options.primaryKey === true,
        autoincrement: false,
        unique: options.unique === true
    };
}

function relationalFixture() {
    return {
        ir_version: "esdb.ir/v1",
        schema: { name: "relational", dialect: "sqlite" },
        tables: [
            {
                name: "project",
                sql_name: "project",
                columns: [
                    column("id", "id", "integer", { primaryKey: true }),
                    column("name", "name", "text", { unique: true })
                ],
                primary_key: ["id"],
                uniques: [["name"]]
            },
            {
                name: "task",
                sql_name: "task",
                columns: [
                    column("id", "id", "integer", { primaryKey: true }),
                    column("projectId", "project_id", "integer"),
                    column("externalKey", "external_key", "text", { unique: true }),
                    column("title", "title", "text"),
                    column("rank", "rank", "integer")
                ],
                primary_key: ["id"],
                uniques: [["externalKey"]],
                foreign_keys: [
                    {
                        name: "fk_task_project",
                        columns: ["projectId"],
                        references: { table: "project", columns: ["id"] },
                        on_update: "restrict",
                        on_delete: "cascade"
                    }
                ],
                checks: [
                    { name: "ck_task_rank", sql: "\"rank\" >= 0" }
                ],
                indexes: [
                    {
                        name: "idx_task_project_rank",
                        unique: false,
                        terms: [
                            { kind: "column", column: "projectId", direction: "asc" },
                            { kind: "column", column: "rank", direction: "desc" }
                        ]
                    },
                    {
                        name: "idx_task_rank_positive",
                        unique: false,
                        terms: [
                            { kind: "column", column: "rank", direction: "asc" }
                        ],
                        where: "\"rank\" > 0"
                    }
                ]
            }
        ],
        queries: [
            {
                name: "task.byProjectName",
                kind: "select",
                table: "task",
                cardinality: "many",
                joins: [
                    {
                        kind: "inner",
                        table: "project",
                        on: [
                            {
                                left: { table: "task", column: "projectId" },
                                right: { table: "project", column: "id" }
                            }
                        ]
                    }
                ],
                where: [
                    { table: "project", column: "name", op: "eq", param: "projectName" }
                ],
                order_by: [
                    { table: "task", column: "rank", direction: "desc" }
                ]
            },
            {
                name: "task.upsert",
                kind: "upsert",
                table: "task",
                cardinality: "changes",
                values: [
                    { column: "id", param: "id" },
                    { column: "projectId", param: "projectId" },
                    { column: "externalKey", param: "externalKey" },
                    { column: "title", param: "title" },
                    { column: "rank", param: "rank" }
                ],
                on_conflict: {
                    target: ["externalKey"],
                    action: "update",
                    set: [
                        { column: "title", param: "nextTitle" },
                        { column: "rank", param: "nextRank" }
                    ]
                }
            }
        ]
    };
}

export async function run() {
    const { ir, sql, physical } = await loadCompiler();
    const fixture = relationalFixture();

    const validation = ir.validateDocument(fixture);
    assert(validation.ok, `relational fixture validates: ${JSON.stringify(validation.diagnostics)}`);

    const task = fixture.tables.find((table) => table.name === "task");
    const joinQuery = fixture.queries.find((query) => query.name === "task.byProjectName");
    const upsertQuery = fixture.queries.find((query) => query.name === "task.upsert");

    const joinStatement = sql.emitQuerySql(task, joinQuery, fixture);
    assertEqual(
        joinStatement.sql,
        'SELECT "task"."id","task"."project_id","task"."external_key","task"."title","task"."rank" FROM "task" INNER JOIN "project" ON "task"."project_id" = "project"."id" WHERE "project"."name" = ? ORDER BY "task"."rank" DESC',
        "join SQL is fixed, fully qualified, and projects only the base row"
    );
    assertDeepEqual(
        joinStatement.params.map((param) => [param.name, param.table, param.column, param.type]),
        [["projectName", "project", "name", "text"]],
        "joined-table predicate parameter retains its owning table and TEXT type"
    );

    const upsert = sql.emitQuerySql(task, upsertQuery, fixture);
    assertEqual(
        upsert.sql,
        'INSERT INTO "task" ("id","project_id","external_key","title","rank") VALUES (?,?,?,?,?) ON CONFLICT ("external_key") DO UPDATE SET "title" = ?, "rank" = ?',
        "upsert SQL is deterministic and bind-only"
    );
    assertDeepEqual(
        upsert.params.map((param) => param.name),
        ["id", "projectId", "externalKey", "title", "rank", "nextTitle", "nextRank"],
        "upsert bind order is values followed by conflict-update assignments"
    );
    assertDeepEqual(
        ir.deriveApiParams(task, upsertQuery, fixture).map((param) => param.name),
        upsert.params.map((param) => param.name),
        "upsert API and bind order are identical for this statement shape"
    );

    const schemaSql = sql.emitSchemaSql(fixture);
    assert(schemaSql.some((statement) => statement.includes("FOREIGN KEY")), "IR oracle emits the foreign key");
    assert(schemaSql.some((statement) => statement.includes("CHECK")), "IR oracle emits the check");
    assert(schemaSql.some((statement) => statement.startsWith("CREATE INDEX \"idx_task_project_rank\"")), "IR oracle emits explicit index");
    const expectedPhysical = physical.snapshotIr(fixture);
    const compatible = physical.diffIrAgainstPhysical(fixture, expectedPhysical);
    assert(compatible.compatible, `relational physical self-differential is compatible: ${JSON.stringify(compatible.mismatches)}`);
    assertEqual(
        compatible.unmodeled.filter((item) => item.kind === "foreign_keys" || item.kind === "checks" || item.kind === "index").length,
        0,
        "modeled relational facts are no longer reported as unmodeled"
    );

    const fkMutation = physical.snapshotSql(
        schemaSql.map((statement) =>
            statement.includes("FOREIGN KEY")
                ? statement.replace("ON DELETE CASCADE", "ON DELETE RESTRICT")
                : statement
        )
    );
    assert(
        !physical.diffIrAgainstPhysical(fixture, fkMutation).compatible,
        "foreign-key action mismatch is detected"
    );

    const checkMutation = physical.snapshotSql(
        schemaSql.map((statement) =>
            statement.includes("CHECK")
                ? statement.replace('\"rank\" >= 0', '\"rank\" >= 1')
                : statement
        )
    );
    assert(
        !physical.diffIrAgainstPhysical(fixture, checkMutation).compatible,
        "CHECK mismatch is detected"
    );

    const indexMutation = physical.snapshotSql(
        schemaSql.map((statement) =>
            statement.includes('CREATE INDEX "idx_task_project_rank"')
                ? statement.replace('"rank" DESC', '"rank" ASC')
                : statement
        )
    );
    assert(
        !physical.diffIrAgainstPhysical(fixture, indexMutation).compatible,
        "index ordering mismatch is detected"
    );

    const work = freshWorkDir("relational");
    const sealedPath = sealFixture(fixture, work, "relational");
    const outDir = join(work, "generated");
    const built = runCli(["build", sealedPath, "--out", outDir]);
    assertEqual(built.status, 0, `relational build: ${built.stderr}`);

    const cpp = readText(join(outDir, "cpp", "relational_repository.hpp"));
    assert(cpp.includes("class ProjectRepository"), "C++ emits project repository");
    assert(cpp.includes("class TaskRepository"), "C++ emits task repository");
    assert(cpp.includes("INNER JOIN \\\"project\\\""), "C++ embeds fixed join SQL");
    assert(cpp.includes("ON CONFLICT"), "C++ embeds fixed upsert SQL");

    const ts = readText(join(outDir, "ts", "relational.ts"));
    assert(ts.includes('table: "project"'), "TS query metadata retains joined parameter table");
    assert(ts.includes('type: "text"'), "TS query metadata retains joined parameter type");

    const es3 = readText(join(outDir, "es3", "relational_repository.jsx"));
    assert(es3.includes('"taskByProjectName"'), "ES3 emits table-qualified relational operation");
    assert(es3.includes('columns: ["id","projectId","externalKey","title","rank"]'), "ES3 embeds query-specific base-row shape");
    assert(es3.includes("function decodeRow(meta, fields, esdbOptions)"), "ES3 decodes rows from query-specific metadata");

    const bridge = readJson(join(outDir, "bridge", "relational_orm_bridge.json"));
    const joinOp = bridge.operations.find((operation) =>
        operation.kind === "select" &&
        operation.cardinality === "many" &&
        operation.params.length === 1 &&
        operation.params[0].name === "projectName"
    );
    assert(joinOp, "bridge manifest contains relational select operation");
    assertEqual(joinOp.params[0].type, "text", "bridge marshals joined predicate as text");

    const firstFiles = [
        cpp,
        ts,
        es3,
        readText(join(outDir, "bridge", "relational_orm_bridge.json"))
    ];
    const secondDir = join(work, "generated-again");
    assertEqual(runCli(["build", sealedPath, "--out", secondDir]).status, 0, "second relational build");
    const secondFiles = [
        readText(join(secondDir, "cpp", "relational_repository.hpp")),
        readText(join(secondDir, "ts", "relational.ts")),
        readText(join(secondDir, "es3", "relational_repository.jsx")),
        readText(join(secondDir, "bridge", "relational_orm_bridge.json"))
    ];
    assertDeepEqual(secondFiles, firstFiles, "relational generation is byte-deterministic");
}
