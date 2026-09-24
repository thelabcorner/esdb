# Compression qualification plan

Compression is a backend experiment, not a prerequisite for ESDB semantics.

## Reference

The semantic oracle is ESDB over ordinary SQLite 3.53.4.

Plain SQLite currently advertises:

- WAL: supported;
- multi-process use: supported;
- stock SQLite tooling: supported;
- online backup: supported;
- compression: unsupported.

Any compressed backend must advertise its own capability matrix rather than inherit these flags.

## Candidate shape

The preferred research direction is transparent page/storage compression below logical SQL/schema semantics.

Zstd is the first codec candidate because the goal is to reduce local database footprint without converting application schemas into compressed blobs.

ChunkDB row/chunk experiments remain research evidence. They are not the ESDB base format.

## Correctness gates

A backend cannot enter performance comparison until it passes:

1. identical logical SQL/Store behavior against plain SQLite;
2. transaction commit/rollback/savepoint parity;
3. integrity checking;
4. migration rollback;
5. backup/restore;
6. abrupt writer termination and recovery;
7. WAL/checkpoint behavior if WAL is advertised;
8. multi-process readers/writers if multi-process is advertised;
9. corruption/fault-injection cases;
10. deterministic codec/version metadata and upgrade policy.

A backend that cannot preserve a capability must report it as unsupported.

## Compatibility gate

Compression may make the physical database unreadable by stock sqlite3.

If so, ESDB must provide an explicit export/inspection path before products that require stock-tool observability can select that backend.

Workmark should initially remain on plain SQLite unless measured data shows its metadata/index database contains enough compressible material to justify a compressed backend. Its large content-addressed payloads are a separate storage concern.

## Performance experiment

Only after correctness:

- representative page sizes;
- cache-size sweep;
- WAL/checkpoint configuration;
- cold/warm reads;
- write transaction throughput;
- database size;
- backup/export cost;
- process startup/open latency;
- CPU time and peak memory.

The decision criterion is a Pareto comparison against plain SQLite, not compression ratio alone.

## Initial candidate research

Potential candidates include a codec-capable ZIPVFS-style architecture and open compressed-VFS implementations used as experimental controls.

No backend is considered selected until the repository contains reproducible crash/concurrency evidence for it.
