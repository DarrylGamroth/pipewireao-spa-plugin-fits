#!/usr/bin/env python3
"""Extract the FliSdk C/C++ development payload without running its installer."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("installer", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    installer = args.installer.resolve(strict=True)
    destination = args.destination.resolve()
    source_digest = sha256(installer)
    marker = destination / ".source-sha256"
    required = (
        destination / "include" / "FliSdk_C_V2.h",
        destination / "lib" / "release" / "libFliSdk.so",
        destination / "3rdParty",
    )
    if marker.is_file() and marker.read_text(encoding="ascii").strip() == source_digest:
        if all(path.exists() for path in required):
            return 0
    if destination.exists():
        raise SystemExit(
            f"refusing to replace incomplete FliSdk extraction: {destination}"
        )

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="flisdk-extract-", dir=destination.parent
    ) as temporary:
        temporary_path = Path(temporary)
        bundle = temporary_path / "bundle"
        sdk = temporary_path / "sdk"
        subprocess.run(
            [
                str(installer),
                "--noexec",
                "--keep",
                "--noprogress",
                "--nochown",
                "--target",
                str(bundle),
            ],
            check=True,
        )
        unpacker = bundle / "7zz"
        unpacker.chmod(unpacker.stat().st_mode | 0o100)
        archive = bundle / "packages" / "sdk" / "c_cpp" / "data.7z"
        subprocess.run(
            [
                str(unpacker),
                "x",
                "-y",
                f"-o{sdk}",
                str(archive),
                "include/*",
                "lib/release/*",
                "3rdParty/*",
                "GrabbersConfigs/*",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        for path in (
            sdk / "include" / "FliSdk_C_V2.h",
            sdk / "lib" / "release" / "libFliSdk.so",
            sdk / "3rdParty",
        ):
            if not path.exists():
                raise SystemExit(f"FliSdk payload is missing {path.relative_to(sdk)}")
        (sdk / ".source-sha256").write_text(source_digest + "\n", encoding="ascii")
        os.rename(sdk, destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
