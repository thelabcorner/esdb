#ifndef ESDB_GENERATED_USER_ORM_BRIDGE_H
#define ESDB_GENERATED_USER_ORM_BRIDGE_H

#include <esabi/esabi.h>

/*
 * Generated project-specific ESABI bridge contract for schema "user".
 * IR: esdb.ir/v1 sha256:f619d9734d3872bd3b788d50799a02a82ac83d026cb824ae408ba3588c86c5ee
 * Source: examples/orm/user/schema/user.schema.ts
 *
 * This is NOT the ESDB ExternalObject adapter. It is the named-operation
 * contract a project bridge DLL implements; the generated ES3 facade calls
 * exactly these operations and never SQL.
 *
 * Handshake and lifecycle:
 *   ormPing(0)        -> 42 (load check)
 *   ormVersion(0)     -> "f619d9734d3872bd3b788d50799a02a82ac83d026cb824ae408ba3588c86c5ee" (drift check; raw hash string)
 *   ormOpen(0,pathHex)-> U1:H:<handle> | U1:E:...
 *   ormClose(handle)  -> U1:C:1 | U1:E:...
 *   ormLastError(0)   -> U1:O | U1:E:...
 *
 * Named operations (handle first; every parameter is a U1 lane string):
 *   userDeleteById(handle, id(i))
 *   userFindByEmail(handle, email(t))
 *   userFindById(handle, id(i))
 *   userInsert(handle, id(i), name(t), email(t), createdAt(i))
 *   userList(handle, limit(i), offset(i))
 *   userUpdateName(handle, id(i), name(t))
 *
 * U1 wire v1 (ASCII only):
 *   ok        "U1:O"
 *   none      "U1:N"
 *   error     "U1:E:<esabi_error_decimal>:<messageHex>"
 *   handle    "U1:H:<handle_decimal>"
 *   changes   "U1:C:<changes_decimal>"
 *   row       "U1:R:<fieldCount>"  then per field "|<tag>|<valueHex>"
 *   rows      "U1:L:<rowCount>"    then per row "|<fieldCount>" + fields
 *   tags: n null | i int64 decimal | e int64 decimal (exact mode)
 *         t UTF-8 | r real decimal | b blob
 *   valueHex: uppercase hex of the payload bytes (empty for null)
 *
 * Parameter lanes (ES3 -> native), one ESABI string per parameter:
 *   "i:<canonical decimal>" | "t:<hex>" | "r:<decimal>" | "b:<hex>" | "n"
 * Implementations MUST reject any parameter that does not match its lane
 * grammar; no parameter text may ever reach SQL.
 */

ESABI_DIRECT_FUNCTION(ormPing);
ESABI_DIRECT_FUNCTION(ormVersion);
ESABI_DIRECT_FUNCTION(ormOpen);
ESABI_DIRECT_FUNCTION(ormClose);
ESABI_DIRECT_FUNCTION(ormLastError);
ESABI_DIRECT_FUNCTION(userDeleteById);  /* userDeleteById_ds */
ESABI_DIRECT_FUNCTION(userFindByEmail);  /* userFindByEmail_ds */
ESABI_DIRECT_FUNCTION(userFindById);  /* userFindById_ds */
ESABI_DIRECT_FUNCTION(userInsert);  /* userInsert_dssss */
ESABI_DIRECT_FUNCTION(userList);  /* userList_dss */
ESABI_DIRECT_FUNCTION(userUpdateName);  /* userUpdateName_dss */

ESABI_INITIALIZE_FUNCTION;
ESABI_VERSION_FUNCTION;
ESABI_FREE_FUNCTION;
ESABI_TERMINATE_FUNCTION;

#endif /* ESDB_GENERATED_USER_ORM_BRIDGE_H */
