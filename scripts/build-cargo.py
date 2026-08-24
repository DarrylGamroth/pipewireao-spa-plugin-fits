#!/usr/bin/env python3
"""Build one Cargo package and copy its cdylib to a Meson output path."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cargo", required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--target-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--profile", choices=("debug", "release"), required=True)
    parser.add_argument("--spa-include", type=pathlib.Path, required=True)
    parser.add_argument("--pkg-config-path")
    args = parser.parse_args()

    command = [
        args.cargo,
        "build",
        "--locked",
        "--manifest-path",
        str(args.manifest),
        "--package",
        "calculon-spa-plugins",
        "--target-dir",
        str(args.target_dir),
    ]
    if args.profile == "release":
        command.append("--release")
    environment = os.environ.copy()
    environment["PIPEWIREAO_SPA_INCLUDE_DIR"] = str(args.spa_include)
    if args.pkg_config_path:
        environment["PKG_CONFIG_PATH"] = args.pkg_config_path
    subprocess.run(command, check=True, env=environment)

    library = args.target_dir / args.profile / "libcalculon_spa_plugins.so"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(library, args.output)


if __name__ == "__main__":
    main()
