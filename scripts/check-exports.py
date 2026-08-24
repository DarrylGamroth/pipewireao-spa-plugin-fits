#!/usr/bin/env python3
"""Require the Calculon SPA cdylib to expose only its SPA entry point."""

from __future__ import annotations

import argparse
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("library")
    args = parser.parse_args()

    result = subprocess.run(
        [args.nm, "-D", "--defined-only", args.library],
        check=True,
        capture_output=True,
        text=True,
    )
    exports = {
        line.split()[-1]
        for line in result.stdout.splitlines()
        if line.split()
    }
    expected = {"spa_handle_factory_enum"}
    if exports != expected:
        raise SystemExit(
            f"unexpected dynamic exports: expected {sorted(expected)}, got {sorted(exports)}"
        )


if __name__ == "__main__":
    main()
