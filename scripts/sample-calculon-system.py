#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Collect perf call graphs for the warmed reference Calculon system."""

import argparse
import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


SUPPORT_PATH = Path(__file__).with_name("profile-calculon-system.py")
SUPPORT_SPEC = importlib.util.spec_from_file_location("calculon_profile_support", SUPPORT_PATH)
SUPPORT = importlib.util.module_from_spec(SUPPORT_SPEC)
SUPPORT_SPEC.loader.exec_module(SUPPORT)


def run_sample(args, binary, alpao, implementation, dso, cpu, latency_csv, perf_data):
    ready_r, ready_w = os.pipe()
    start_r, start_w = os.pipe()
    done_r, done_w = os.pipe()
    finish_r, finish_w = os.pipe()
    environment = os.environ.copy()
    SUPPORT.with_hugetlb_tunable(environment, SUPPORT.PAGE_POLICIES[args.page_policy])
    environment.update(
        {
            "PW_BENCHMARK_MEMORY": args.memory_policy,
            "PW_BENCHMARK_WARMUP": str(args.warmup),
            "PW_BENCHMARK_LATENCY_CSV": str(latency_csv),
            "PW_BENCHMARK_PROFILE_READY_FD": str(ready_w),
            "PW_BENCHMARK_PROFILE_START_FD": str(start_r),
            "PW_BENCHMARK_PROFILE_DONE_FD": str(done_w),
            "PW_BENCHMARK_PROFILE_FINISH_FD": str(finish_r),
        }
    )
    command = []
    if args.rt_priority > 0:
        command.extend(("chrt", "-f", str(args.rt_priority)))
    command.extend(
        (
            "taskset",
            "-c",
            str(cpu),
            str(binary),
            str(dso),
            str(alpao),
            "mock",
            str(args.samples),
            str(args.actuators),
            "",
            str(args.width),
            str(args.height),
            str(args.region_width),
            str(args.region_height),
        )
    )
    child = subprocess.Popen(
        command,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        pass_fds=(ready_w, start_r, done_w, finish_r),
    )
    for fd in (ready_w, start_r, done_w, finish_r):
        os.close(fd)
    perf = None
    try:
        tid = struct.unpack("i", SUPPORT.wait_read(ready_r, struct.calcsize("i")))[0]
        if os.sched_getaffinity(tid) != {cpu}:
            raise RuntimeError(f"affinity mismatch for TID {tid}")
        if args.rt_priority > 0 and (
            os.sched_getscheduler(tid) != os.SCHED_FIFO
            or os.sched_getparam(tid).sched_priority != args.rt_priority
        ):
            raise RuntimeError(f"real-time scheduler mismatch for TID {tid}")
        control_r, control_w = os.pipe()
        ack_r, ack_w = os.pipe()
        perf_command = [
            "perf",
            "record",
            "--output",
            str(perf_data),
            "--delay=-1",
            f"--control=fd:{control_r},{ack_w}",
            "--event",
            args.event,
            "--freq",
            str(args.frequency),
            "--call-graph",
            "dwarf,8192",
            "--tid",
            str(tid),
        ]
        perf = subprocess.Popen(
            perf_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            pass_fds=(control_r, ack_w),
        )
        os.close(control_r)
        os.close(ack_w)
        os.write(control_w, b"enable\n")
        if not SUPPORT.valid_perf_ack(SUPPORT.wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge enable")
        os.write(start_w, b"\x01")
        SUPPORT.wait_read(done_r, 1, timeout=max(120, args.samples // 50))
        os.write(control_w, b"disable\n")
        if not SUPPORT.valid_perf_ack(SUPPORT.wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge disable")
        os.write(finish_w, b"\x01")
        child_stdout, child_stderr = child.communicate(timeout=60)
        perf_stdout, perf_stderr = perf.communicate(timeout=60)
        if child.returncode != 0:
            raise RuntimeError(f"benchmark failed: {child_stderr}")
        if perf.returncode != 0:
            raise RuntimeError(f"perf record failed: {perf_stderr}")
    finally:
        for fd in (ready_r, start_w, done_r, finish_w):
            try:
                os.close(fd)
            except OSError:
                pass
        for name in ("control_w", "ack_r"):
            fd = locals().get(name)
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        if child.poll() is None:
            child.kill()
            child.wait()
        if perf is not None and perf.poll() is None:
            perf.kill()
            perf.wait()
    SUPPORT.validate_latency_csv(latency_csv, args.samples)
    SUPPORT.parse_benchmark_output(child_stdout, args.samples)
    return command, perf_command, child_stdout, child_stderr, perf_stdout, perf_stderr


def perf_report(perf_data, children):
    command = [
        "perf",
        "report",
        "--input",
        str(perf_data),
        "--stdio",
        "--percent-limit",
        "0.1",
        "--sort",
        "comm,dso,symbol",
        "--children" if children else "--no-children",
    ]
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=120,
    )
    if result.returncode != 0:
        raise RuntimeError(f"perf report failed: {result.stdout}")
    demangle = subprocess.run(
        ["c++filt"],
        input=result.stdout,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=120,
    )
    if demangle.returncode != 0:
        raise RuntimeError(f"c++filt failed: {demangle.stdout}")
    return command, demangle.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("alpao_dso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--implementation", action="append", default=[], required=True)
    parser.add_argument("--samples", type=int, default=2_000)
    parser.add_argument("--warmup", type=int, default=256)
    parser.add_argument("--frequency", type=int, default=4_999)
    parser.add_argument("--event", default="cycles:u")
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--rt-priority", type=int, default=88)
    parser.add_argument("--page-policy", choices=SUPPORT.PAGE_POLICIES, default="base")
    parser.add_argument(
        "--memory-policy", choices=("default", "prefault", "locked"), default="default"
    )
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--region-width", type=int, default=8)
    parser.add_argument("--region-height", type=int, default=8)
    parser.add_argument("--actuators", type=int, default=468)
    args = parser.parse_args()
    if args.samples < 1_000 or args.warmup < 1 or args.frequency < 1:
        raise SystemExit("at least 1000 samples and positive warmup/frequency are required")
    event_name = re.sub(r"[^A-Za-z0-9_.-]+", "-", args.event).strip("-")
    if not event_name:
        raise SystemExit("event must contain at least one filename-safe character")
    binary = args.binary.resolve(strict=True)
    alpao = args.alpao_dso.resolve(strict=True)
    implementations = SUPPORT.parse_implementations(args.implementation)
    output = args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[1]
    cpu, allowed = SUPPORT.select_cpu(args.cpu)
    SUPPORT.capture_environment(
        output, source_root, binary, alpao, implementations, cpu, allowed
    )
    with (output / "environment.txt").open("a", encoding="utf-8") as stream:
        stream.write(
            f"\n## sampler\n{Path(__file__).resolve()}\n"
            f"sha256={SUPPORT.sha256_file(Path(__file__).resolve())}\n"
        )
    SUPPORT.snapshot_system(output / "system-before.txt")
    started = datetime.datetime.now(datetime.timezone.utc)
    commands = []
    with tempfile.TemporaryDirectory(prefix="calculon-cycle-sample-") as temporary:
        temporary = Path(temporary)
        for implementation, dso in implementations.items():
            print(f"sampling implementation={implementation}", flush=True)
            stem = (
                f"{event_name}-{implementation}-{args.page_policy}-"
                f"{args.memory_policy}"
            )
            latency_csv = output / f"latency-{stem}.csv"
            perf_data = temporary / f"{stem}.data"
            values = run_sample(
                args, binary, alpao, implementation, dso, cpu, latency_csv, perf_data
            )
            command, perf_command, child_out, child_err, perf_out, perf_err = values
            report_command, report = perf_report(perf_data, children=False)
            children_command, children_report = perf_report(perf_data, children=True)
            (output / f"{stem}-report.txt").write_text(report, encoding="utf-8")
            (output / f"{stem}-children-report.txt").write_text(
                children_report, encoding="utf-8"
            )
            buildids = SUPPORT.run_text(["perf", "buildid-list", "-i", str(perf_data)])
            (output / f"{stem}-buildids.txt").write_text(buildids, encoding="utf-8")
            commands.append(
                f"$ {' '.join(command)}\n$ {' '.join(perf_command)}\n"
                f"$ {' '.join(report_command)}\n$ {' '.join(children_command)}\n"
                f"benchmark-stdout:\n{child_out}benchmark-stderr:\n{child_err}"
                f"perf-stdout:\n{perf_out}perf-stderr:\n{perf_err}\n"
            )
    ended = datetime.datetime.now(datetime.timezone.utc)
    (output / "commands.log").write_text("\n".join(commands), encoding="utf-8")
    SUPPORT.snapshot_system(output / "system-after.txt")
    metadata = {
        "started_utc": started.isoformat(),
        "ended_utc": ended.isoformat(),
        "duration_seconds": (ended - started).total_seconds(),
        "samples": args.samples,
        "warmup": args.warmup,
        "cpu": cpu,
        "rt_priority": args.rt_priority,
        "page_policy": args.page_policy,
        "memory_policy": args.memory_policy,
        "implementations": {name: str(path) for name, path in implementations.items()},
        "event": args.event,
        "frequency_hz": args.frequency,
        "call_graph": "dwarf,8192",
        "raw_perf_data_retained": False,
        "measurement_scope": "profiled main TID between warmed full-system gates",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    SUPPORT.write_manifest(output)


if __name__ == "__main__":
    main()
