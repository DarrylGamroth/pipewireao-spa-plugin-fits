#!/usr/bin/env python3
# PipeWire
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Qualify the eGrabber source through an isolated PipeWireAO daemon."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


SOURCE_PATTERN = re.compile(
    r'id (\d+), type PipeWire:Interface:Node/3'
    r'(?:(?!\n\tid ).)*?node\.name = "(egrabber_source\.[^"]+)"',
    re.DOTALL,
)
SUMMARY_PATTERN = re.compile(r"frames=(\d+) first=(\d+) last=(\d+)")


class QualificationError(RuntimeError):
    pass


def run_tool(paths, environment, tool, *arguments, timeout=10, check=True):
    result = subprocess.run(
        [str(paths[tool]), *arguments],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    if check and result.returncode != 0:
        raise QualificationError(
            f"{tool} {' '.join(arguments)} failed ({result.returncode}):\n"
            f"{result.stdout}"
        )
    return result


def wait_for(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise QualificationError(f"timed out waiting for {description}")


def list_source(paths, environment):
    listing = run_tool(paths, environment, "cli", "list-objects").stdout
    match = SOURCE_PATTERN.search(listing)
    return None if match is None else (match.group(1), match.group(2))


def connect_latest(paths, environment, source_name, client_name):
    def attempt():
        result = run_tool(
            paths,
            environment,
            "link",
            "-w",
            "-p",
            "{ link.buffer-latest = true }",
            source_name,
            client_name,
            timeout=2,
            check=False,
        )
        return result.returncode == 0

    wait_for(attempt, 5, f"latest-buffer link to {client_name}")


def start_client(paths, environment, directory, source_name, name, frames, hold=0):
    output_path = directory / f"{name}.log"
    output = output_path.open("w+", encoding="utf-8")
    arguments = [str(paths["client"]), source_name, str(frames), name]
    if hold:
        arguments.append(str(hold))
    process = subprocess.Popen(
        arguments,
        env=environment,
        text=True,
        stdout=output,
        stderr=subprocess.STDOUT,
    )
    connect_latest(paths, environment, source_name, name)
    return process, output, output_path


def stop_client(client):
    if client is None:
        return
    process, output, _ = client
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    if not output.closed:
        output.close()


def finish_client(process, output, output_path, timeout=15):
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        process.kill()
        process.wait()
        raise QualificationError(f"client timed out: {output_path}") from error
    output.flush()
    output.seek(0)
    text = output.read()
    output.close()
    if result != 0:
        raise QualificationError(f"client failed ({result}):\n{text}")
    match = SUMMARY_PATTERN.search(text)
    if match is None:
        raise QualificationError(f"client emitted no capture summary:\n{text}")
    frames, first, last = (int(value) for value in match.groups())
    if frames == 0 or last < first:
        raise QualificationError(f"invalid capture summary:\n{text}")
    return text.strip(), (frames, first, last)


def qualify_fanout(paths, environment, directory, source_name):
    first = None
    second = None
    try:
        first = start_client(
            paths,
            environment,
            directory,
            source_name,
            "egrabber-primary",
            80,
            2000,
        )
        wait_for(
            lambda: "holding=" in first[2].read_text(encoding="utf-8"),
            10,
            "primary subscriber lease",
        )
        second = start_client(
            paths, environment, directory, source_name, "egrabber-live-join", 10
        )
        second_text, second_summary = finish_client(*second)
        if first[0].poll() is not None:
            raise QualificationError("primary subscriber ended before live leave")
        first_text, first_summary = finish_client(*first)
        if first_summary[0] <= second_summary[0]:
            raise QualificationError("primary capture did not span live join and leave")
        return first_text, second_text
    finally:
        stop_client(second)
        stop_client(first)


def qualify_active_teardown(paths, environment, directory, node_id, source_name):
    client = None
    try:
        client = start_client(
            paths,
            environment,
            directory,
            source_name,
            "egrabber-retained-lease",
            1,
            2000,
        )

        def client_is_holding():
            client[1].flush()
            return "holding=" in client[2].read_text(encoding="utf-8")

        wait_for(client_is_holding, 10, "client-held buffer lease")
        run_tool(paths, environment, "cli", "destroy", node_id)
        text, _ = finish_client(*client)
        if list_source(paths, environment) is not None:
            raise QualificationError("source remains registered after active teardown")
        return text
    finally:
        stop_client(client)


def build_environment(plugin_build, pipewire_build, runtime_directory):
    environment = os.environ.copy()
    environment.update(
        {
            "PIPEWIREAO_CONFIG_DIR": str(pipewire_build / "src/daemon"),
            "PIPEWIREAO_SPA_PLUGIN_DIR": ":".join(
                (
                    str(plugin_build / "spa/plugins"),
                    str(pipewire_build / "spa/plugins"),
                )
            ),
            "PIPEWIREAO_MODULE_DIR": str(pipewire_build / "src/modules"),
            "PIPEWIREAO_RUNTIME_DIR": str(runtime_directory),
            "PIPEWIREAO_LOG_SYSTEMD": "false",
            "PIPEWIREAO_REMOTE": "pipewire-ao-0",
        }
    )
    library_path = str(pipewire_build / "src/pipewire")
    if environment.get("LD_LIBRARY_PATH"):
        library_path += ":" + environment["LD_LIBRARY_PATH"]
    environment["LD_LIBRARY_PATH"] = library_path
    return environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin_build_directory", type=Path)
    parser.add_argument("pipewire_build_directory", type=Path)
    arguments = parser.parse_args()
    plugin_build = arguments.plugin_build_directory.resolve()
    pipewire_build = arguments.pipewire_build_directory.resolve()
    paths = {
        "daemon": pipewire_build / "src/daemon/pipewire-ao",
        "cli": pipewire_build / "src/tools/pwao-cli",
        "link": pipewire_build / "src/tools/pwao-link",
        "client": plugin_build / "spa/plugins/egrabber/spa-egrabber-host-client",
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        parser.error("missing build artifacts: " + ", ".join(missing))

    temporary = Path(tempfile.mkdtemp(prefix="pwao-egrabber-qualification."))
    runtime = temporary / "run"
    runtime.mkdir()
    environment = build_environment(plugin_build, pipewire_build, runtime)
    daemon_log_path = temporary / "daemon.log"
    daemon_log = daemon_log_path.open("w+", encoding="utf-8")
    daemon = subprocess.Popen(
        [str(paths["daemon"])],
        env=environment,
        text=True,
        stdout=daemon_log,
        stderr=subprocess.STDOUT,
    )
    try:
        socket = runtime / "pipewire-ao-0"
        wait_for(lambda: socket.is_socket(), 5, "PipeWireAO daemon socket")
        run_tool(
            paths,
            environment,
            "cli",
            "create-node",
            "spa-node-factory",
            "{ factory.name=api.egrabber.source object.linger=true }",
        )
        source = wait_for(
            lambda: list_source(paths, environment), 5, "eGrabber source node"
        )
        primary, joining = qualify_fanout(
            paths, environment, temporary, source[1]
        )
        retained = qualify_active_teardown(
            paths, environment, temporary, source[0], source[1]
        )
        if daemon.poll() is not None:
            raise QualificationError("daemon exited during qualification")
        daemon_log.flush()
        daemon_text = daemon_log_path.read_text(encoding="utf-8")
        if "error unset format" in daemon_text or "Device or resource busy" in daemon_text:
            raise QualificationError(f"teardown warning in daemon log:\n{daemon_text}")
        print(primary)
        print(joining)
        print(retained)
        print("eGrabber host qualification passed")
        return 0
    except QualificationError as error:
        print(f"qualification failed: {error}", file=sys.stderr)
        for log_path in sorted(temporary.glob("*.log")):
            contents = log_path.read_text(encoding="utf-8")
            if contents:
                print(f"--- {log_path.name}\n{contents}", file=sys.stderr)
        return 1
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
        daemon_log.close()
        shutil.rmtree(temporary)


if __name__ == "__main__":
    sys.exit(main())
