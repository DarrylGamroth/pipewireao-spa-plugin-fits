#!/usr/bin/env python3
"""Create a non-destructive DCAM-API link for an installed or unpacked runtime."""

from __future__ import annotations

import os
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare-hamamatsu.py LIBDIR OUTPUT_DIR")
    source = pathlib.Path(sys.argv[1]).resolve()
    output = pathlib.Path(sys.argv[2]).resolve()
    candidates = sorted(source.glob("libdcamapi.so*"))
    libraries = [candidate for candidate in candidates if candidate.is_file()]
    if not libraries:
        raise SystemExit(f"libdcamapi.so was not found in {source}")

    # Prefer the unversioned library when the vendor installer has created it.
    library = next(
        (candidate for candidate in libraries if candidate.name == "libdcamapi.so"),
        libraries[-1],
    )
    output.mkdir(parents=True, exist_ok=True)
    target = library.resolve()
    link = output / "libdcamapi.so"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(target)
    marker = ".so."
    if marker in target.name:
        stem, version = target.name.split(marker, 1)
        soname = output / f"{stem}.so.{version.split('.', 1)[0]}"
        if soname.is_symlink() or soname.exists():
            soname.unlink()
        soname.symlink_to(target)
    print(os.fspath(link))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
