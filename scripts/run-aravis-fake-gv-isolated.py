#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Run one Aravis fake-GV test in a loopback-only network namespace."""

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Start an Aravis fake GigE Vision camera in a private network "
            "namespace and run a test command beside it. The namespace has "
            "no physical network interfaces."
        )
    )
    parser.add_argument(
        "--inside",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--serial",
        default="GV01",
        help="fake-camera serial number (default: %(default)s)",
    )
    parser.add_argument("fake_camera", type=Path)
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="test command, preceded by --",
    )
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("a test command is required after --")
    return args


def enter_namespace(args: argparse.Namespace) -> None:
    unshare = shutil.which("unshare")
    if unshare is None:
        raise RuntimeError("unshare is required")
    argv = [
        unshare,
        "--user",
        "--map-root-user",
        "--net",
        "--fork",
        "--kill-child=SIGKILL",
        sys.executable,
        str(Path(__file__).resolve()),
        "--inside",
        "--serial",
        args.serial,
        str(args.fake_camera.resolve()),
        "--",
        *args.command,
    ]
    os.execv(unshare, argv)


def stop_process(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_inside(args: argparse.Namespace) -> int:
    ip = shutil.which("ip")
    if ip is None:
        raise RuntimeError("ip from iproute2 is required")
    if not args.fake_camera.is_file() or not os.access(
        args.fake_camera, os.X_OK
    ):
        raise RuntimeError(f"fake camera is not executable: {args.fake_camera}")

    subprocess.run([ip, "link", "set", "lo", "up"], check=True)
    camera = subprocess.Popen(
        [
            str(args.fake_camera),
            "--interface",
            "127.0.0.1",
            "--serial",
            args.serial,
        ]
    )
    try:
        # The simulator has no readiness endpoint. Bound this wait and verify
        # that startup did not fail before invoking the consumer.
        time.sleep(0.25)
        if camera.poll() is not None:
            raise RuntimeError(
                f"fake camera exited during startup with status {camera.returncode}"
            )
        return subprocess.run(args.command, check=False).returncode
    finally:
        stop_process(camera)


def main() -> int:
    args = parse_args()
    try:
        if not args.inside:
            enter_namespace(args)
        return run_inside(args)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
