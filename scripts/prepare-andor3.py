#!/usr/bin/env python3
"""Create a non-destructive SDK3 runtime link farm for unpacked SDK bundles."""

from __future__ import annotations

import os
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare-andor3.py LIBDIR OUTPUT_DIR")
    source = pathlib.Path(sys.argv[1]).resolve()
    output = pathlib.Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    libraries = sorted(source.glob("lib*.so*"))
    if not libraries:
        raise SystemExit(f"no SDK3 shared libraries found in {source}")
    core: pathlib.Path | None = None
    for library in libraries:
        if not library.is_file():
            continue
        versioned = output / library.name
        if versioned.is_symlink() or versioned.exists():
            versioned.unlink()
        versioned.symlink_to(library)
        stem, separator, _version = library.name.partition(".so.")
        if separator:
            unversioned = output / f"{stem}.so"
            if unversioned.is_symlink() or unversioned.exists():
                unversioned.unlink()
            unversioned.symlink_to(library)
            if stem == "libatcore":
                core = unversioned
        elif library.name == "libatcore.so":
            core = versioned
    if core is None:
        raise SystemExit(f"libatcore.so was not found in {source}")
    print(os.fspath(core))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
