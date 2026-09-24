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
 *   stage         - stages one database path (string argument).
 *   openStaged    - opens the staged path; returns a generation-tagged handle.
 *   close         - closes a handle; stale/invalid handles are rejected.
 *   health        - database health snapshot as a JSON string.
 *   lastError     - last adapter error as a JSON string.
 *   dataVersion   - PRAGMA data_version (change-polling primitive).
 *   handleCount   - number of currently open handles.
 *
 * There is intentionally no arbitrary-SQL method, no callback registration,
 * and no asynchronous native call into JSX in v0.1.
 *
 * Every method has a dummy double argument because no-argument methods bind
 * unreliably on the measured host (Illustrator 30.6.0); callers pass 0.
 */

ESABI_DIRECT_FUNCTION(ping);
ESABI_DIRECT_FUNCTION(abiVersion);
ESABI_DIRECT_FUNCTION(version);
ESABI_DIRECT_FUNCTION(sqliteVersion);
ESABI_DIRECT_FUNCTION(stage);
ESABI_DIRECT_FUNCTION(openStaged);
ESABI_DIRECT_FUNCTION(close);
ESABI_DIRECT_FUNCTION(health);
ESABI_DIRECT_FUNCTION(lastError);
ESABI_DIRECT_FUNCTION(dataVersion);
ESABI_DIRECT_FUNCTION(handleCount);

ESABI_INITIALIZE_FUNCTION;
ESABI_VERSION_FUNCTION;
ESABI_FREE_FUNCTION;
ESABI_TERMINATE_FUNCTION;

#endif /* ESDB_EXTERNALOBJECT_ABI_H */
