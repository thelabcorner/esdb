/*
 * ESDB ORM vertical slice: the "user" authoring schema (Drizzle).
 *
 * This file is the single authoring source. The checked-in IR
 * (generated/user.ir.json) is a build artifact produced by:
 *
 *   node orm/drizzle/src/cli.ts --schema examples/orm/user/schema/user.schema.ts \
 *       --queries examples/orm/user/user.queries.json --schema-name user \
 *       --namespace esdb.orm.user --out <ir.json>
 *
 * Slice contract:
 *   id         INTEGER PRIMARY KEY
 *   name       TEXT NOT NULL
 *   email      TEXT NOT NULL UNIQUE
 *   created_at INTEGER NOT NULL
 */

import { integer, sqliteTable, text } from "drizzle-orm/sqlite-core";

export const user = sqliteTable("user", {
    id: integer("id").primaryKey(),
    name: text("name").notNull(),
    email: text("email").notNull().unique(),
    createdAt: integer("created_at").notNull()
});
