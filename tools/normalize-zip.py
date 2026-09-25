#!/usr/bin/env python3
"""Normalize ZIP entry timestamps without recompressing payloads."""

from __future__ import annotations

import datetime as _dt
import os
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

_LOCAL_SIG = b"PK\x03\x04"
_CENTRAL_SIG = b"PK\x01\x02"
_U16 = struct.Struct("<H")


def _dos_timestamp(epoch: int) -> tuple[int, int]:
    dt = _dt.datetime.fromtimestamp(epoch, tz=_dt.timezone.utc)
    year = min(max(dt.year, 1980), 2107)
    dos_time = (dt.hour << 11) | (dt.minute << 5) | (dt.second // 2)
    dos_date = ((year - 1980) << 9) | (dt.month << 5) | dt.day
    return dos_time, dos_date


def normalize(path: Path, epoch: int) -> None:
    raw = bytearray(path.read_bytes())
    dos_time, dos_date = _dos_timestamp(epoch)

    with zipfile.ZipFile(path, "r") as archive:
        entries = archive.infolist()
        central = archive.start_dir

        for info in entries:
            if raw[central : central + 4] != _CENTRAL_SIG:
                raise RuntimeError(
                    f"invalid central-directory signature at offset {central}"
                )

            name_len = _U16.unpack_from(raw, central + 28)[0]
            extra_len = _U16.unpack_from(raw, central + 30)[0]
            comment_len = _U16.unpack_from(raw, central + 32)[0]

            _U16.pack_into(raw, central + 12, dos_time)
            _U16.pack_into(raw, central + 14, dos_date)

            local = info.header_offset
            if raw[local : local + 4] != _LOCAL_SIG:
                raise RuntimeError(
                    f"invalid local-header signature for {info.filename!r}"
                )
            _U16.pack_into(raw, local + 10, dos_time)
            _U16.pack_into(raw, local + 12, dos_date)

            central += 46 + name_len + extra_len + comment_len

    mode = path.stat().st_mode
    fd, temp_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temp_name, mode)
        os.replace(temp_name, path)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)

    with zipfile.ZipFile(path, "r") as archive:
        bad = archive.testzip()
        if bad is not None:
            raise RuntimeError(f"normalized ZIP failed CRC validation at {bad!r}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {Path(argv[0]).name} <archive.zip>", file=sys.stderr)
        return 2

    archive = Path(argv[1]).resolve()
    if not archive.is_file():
        print(f"ZIP not found: {archive}", file=sys.stderr)
        return 2

    epoch_text = os.environ.get("SOURCE_DATE_EPOCH", "1790208000")
    try:
        epoch = int(epoch_text)
    except ValueError:
        print(f"invalid SOURCE_DATE_EPOCH: {epoch_text!r}", file=sys.stderr)
        return 2

    normalize(archive, epoch)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
