#ifndef ESDB_EXTERNALOBJECT_ABI_H
#define ESDB_EXTERNALOBJECT_ABI_H

#include <esabi/esabi.h>

/*
 * ESDB's ExternalObject adapter uses ESABI as the sole definition of the host
 * binary interface. This header declares only ESDB's direct methods; ESABI
 * owns value layout, tags, errors, lifecycle entry points, packing, and
 * calling convention.
 *
 * Method surface (deliberately thin; see docs/ABI.md "ExternalObject adapter"):
 *
 *   ping          - numeric smoke test; returns 42.
 *   abiVersion    - ESDB ABI revision as an integer.
 *   version       - ESDB product version string.
 *   sqliteVersion - linked SQLite version string.
 *   stage         - legacy raw-string path staging.
 *   stageHex      - UTF-8 path bytes as ASCII hex; preferred JSX path lane.
 *   openStaged    - opens the staged path; returns a generation-tagged handle.
 *   close         - closes a handle; stale/invalid handles are rejected.
 *   health        - database health snapshot as a JSON string.
 *   lastError     - last adapter error as a JSON string.
 *   dataVersion   - PRAGMA data_version (change-polling primitive).
 *   querySql      - bounded typed single-statement SQL escape hatch. SQL text
 *                   and typed parameter packet are separate; values are bound.
 *   handleCount   - number of currently open handles.
 *   transaction*  - synchronous transaction ownership for JSX batch updates.
 *   objectStore*  - durable SQLite-backed ObjectStore operations.
 *   store*        - process-memory Store operations shared through ESDBCore.
 *                   Exact 64-bit values/revisions cross as decimal strings.
 *
 * querySql is the explicit advanced SQL escape hatch; generated ORM calls do
 * not use it. There is no callback registration and no asynchronous native call
 * into JSX. Store change delivery is pull-only.
 *
 * Every method has a dummy double argument because no-argument methods bind
 * unreliably on the measured host (Illustrator 30.6.0); callers pass 0.
 */

ESABI_DIRECT_FUNCTION(ping);
ESABI_DIRECT_FUNCTION(abiVersion);
ESABI_DIRECT_FUNCTION(version);
ESABI_DIRECT_FUNCTION(sqliteVersion);
ESABI_DIRECT_FUNCTION(stage);
ESABI_DIRECT_FUNCTION(stageHex);
ESABI_DIRECT_FUNCTION(openStaged);
ESABI_DIRECT_FUNCTION(close);
ESABI_DIRECT_FUNCTION(health);
ESABI_DIRECT_FUNCTION(lastError);
ESABI_DIRECT_FUNCTION(dataVersion);
ESABI_DIRECT_FUNCTION(querySql);
ESABI_DIRECT_FUNCTION(handleCount);
ESABI_DIRECT_FUNCTION(transactionBegin);
ESABI_DIRECT_FUNCTION(transactionCommit);
ESABI_DIRECT_FUNCTION(transactionRollback);
ESABI_DIRECT_FUNCTION(transactionActive);
ESABI_DIRECT_FUNCTION(objectStoreEnsure);
ESABI_DIRECT_FUNCTION(objectStorePutNumber);
ESABI_DIRECT_FUNCTION(objectStorePutText);
ESABI_DIRECT_FUNCTION(objectStoreGet);
ESABI_DIRECT_FUNCTION(objectStoreDelete);
ESABI_DIRECT_FUNCTION(objectStoreExists);
ESABI_DIRECT_FUNCTION(objectStoreCount);
ESABI_DIRECT_FUNCTION(objectStoreScan);
ESABI_DIRECT_FUNCTION(objectStoreRevision);
ESABI_DIRECT_FUNCTION(objectStoreChanges);
ESABI_DIRECT_FUNCTION(objectStorePrune);
ESABI_DIRECT_FUNCTION(storeDestroy);
ESABI_DIRECT_FUNCTION(storePutNumber);
ESABI_DIRECT_FUNCTION(storePutText);
ESABI_DIRECT_FUNCTION(storePatch);
ESABI_DIRECT_FUNCTION(storeGet);
ESABI_DIRECT_FUNCTION(storeDelete);
ESABI_DIRECT_FUNCTION(storeExists);
ESABI_DIRECT_FUNCTION(storeCount);
ESABI_DIRECT_FUNCTION(storeScan);
ESABI_DIRECT_FUNCTION(storeClear);
ESABI_DIRECT_FUNCTION(storeRevision);
ESABI_DIRECT_FUNCTION(storeRetainedFloor);
ESABI_DIRECT_FUNCTION(storeChanges);

ESABI_INITIALIZE_FUNCTION;
ESABI_VERSION_FUNCTION;
ESABI_FREE_FUNCTION;
ESABI_TERMINATE_FUNCTION;

#endif /* ESDB_EXTERNALOBJECT_ABI_H */
