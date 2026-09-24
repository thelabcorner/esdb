# Vendored SQLite

ESDB vendors the SQLite amalgamation so a fresh source checkout is buildable without a network fetch or a system SQLite dependency.

The authoritative release pin is [`cmake/sqlite-pin.json`](../../cmake/sqlite-pin.json). CMake verifies the SHA-256 of each vendored source file at configure time before compiling it.

Current pin:

- SQLite 3.53.4
- upstream amalgamation product 3530400
- source archive: https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
- archive SHA3-256: `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`

The amalgamation is distributed by the SQLite project as public-domain software. See SQLite's upstream copyright/public-domain notice for details.

To verify the current vendored tree or repair it from the pinned upstream archive:

```text
cmake -P tools/fetch-sqlite.cmake
```

To force a verified re-download:

```text
cmake -DESDB_SQLITE_FORCE=ON -P tools/fetch-sqlite.cmake
```

On Windows, `tools/fetch-sqlite.ps1` is a convenience wrapper around the same cross-platform CMake script. It does not carry a second copy of the release pin.
