#!/usr/bin/env python3
"""Run the FITS-to-discard graph against an isolated PipeWireAO core."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def interrupted(signum: int, _frame: object) -> None:
    raise SystemExit(128 + signum)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("client", type=Path)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("config_template", type=Path)
    return parser.parse_args()


def stop_process_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
    except ProcessLookupError:
        process.wait(timeout=5)


def wait_for_core(process: subprocess.Popen[bytes], socket: Path, log: Path) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        status = process.poll()
        if status is not None:
            raise RuntimeError(
                f"private PipeWireAO core exited with status {status}:\n"
                + log.read_text(errors="replace")
            )
        if socket.is_socket():
            return
        time.sleep(0.01)
    raise RuntimeError(
        "private PipeWireAO core did not create its socket:\n"
        + log.read_text(errors="replace")
    )


def main() -> None:
    args = parse_args()
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, interrupted)
    with tempfile.TemporaryDirectory(prefix="pipewireao-fits-discard-") as directory:
        root = Path(directory)
        runtime = root / "runtime"
        runtime.mkdir()
        remote = f"pipewireao-fits-discard-{os.getpid()}"
        config = root / "private-core.conf"
        config.write_text(
            args.config_template.read_text().replace("@CORE_NAME@", remote)
        )
        log = root / "daemon.log"
        environment = os.environ.copy()
        environment.update(
            {
                "PIPEWIRE_RUNTIME_DIR": str(runtime),
                "PIPEWIREAO_RUNTIME_DIR": str(runtime),
                "XDG_RUNTIME_DIR": str(runtime),
            }
        )
        with log.open("wb") as output:
            daemon = subprocess.Popen(
                [args.daemon, "-c", config],
                env=environment,
                stdout=output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        try:
            wait_for_core(daemon, runtime / remote, log)
            result = subprocess.run(
                [args.client, remote, root / "image.fits"],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=10,
                check=False,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f"FITS-to-discard client exited with status {result.returncode}:\n"
                    + result.stdout.decode(errors="replace")
                    + "\nprivate core log:\n"
                    + log.read_text(errors="replace")
                )
            if daemon.poll() is not None:
                raise RuntimeError(
                    "private PipeWireAO core exited before cleanup:\n"
                    + log.read_text(errors="replace")
                )
        finally:
            stop_process_group(daemon)
        if daemon.poll() is None:
            raise RuntimeError("private PipeWireAO core survived fixture cleanup")


if __name__ == "__main__":
    main()
