#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
# SPDX-License-Identifier: MIT

"""Profile the warmed reference Calculon-to-ALPAO path and its memory policy."""

import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import re
import select
import statistics
import struct
import subprocess
import time


EVENT_GROUPS = {
    "core": "{cycles:u,instructions:u,branches:u,branch-misses:u}",
    "l1d": "{cycles:u,instructions:u,L1-dcache-loads:u,L1-dcache-load-misses:u}",
    "l2": (
        "{cycles:u,instructions:u,l2_request_g1.all_no_prefetch:u,"
        "l2_cache_req_stat.ic_dc_miss_in_l2:u}"
    ),
    "dtlb": "{cycles:u,instructions:u,l1_dtlb_misses:u,l2_dtlb_misses:u}",
    "fills": (
        "{cycles:u,instructions:u,ls_any_fills_from_sys.int_cache:u,"
        "ls_any_fills_from_sys.mem_io_local:u}"
    ),
    "faults": "{page-faults,minor-faults,major-faults,context-switches,cpu-migrations}",
}

PAGE_POLICIES = {
    "base": "0",
    "thp": "1",
    "hugetlb": "2",
}

LATENCY_NAMES = {
    "pixel+controller+normalizer SPA callbacks": "algorithm",
    "scheduled submit+ALPAO sink+ASDK": "sink",
    "raw detector frame to ALPAO completion": "total",
}

LATENCY_RE = re.compile(
    r"^(?P<name>.*): n=(?P<n>\d+) mean=(?P<mean>[0-9.]+) ns "
    r"p50=(?P<p50>\d+) ns p90=(?P<p90>\d+) ns p99=(?P<p99>\d+) ns "
    r"(?:p99\.9=(?P<p999>\d+) ns )?max=(?P<max>\d+) ns$"
)
FIRST_RE = re.compile(
    r"^first-cycle: algorithms=(?P<algorithm>\d+) ns sink=(?P<sink>\d+) ns "
    r"minor-faults=(?P<minor>\d+) major-faults=(?P<major>\d+)$"
)

SUMMARY_FIELDS = (
    "repetition",
    "implementation",
    "page_policy",
    "memory_policy",
    "group",
    "samples",
    "warmup",
    "cpu",
    "rt_priority",
    "measurement_seconds",
    "first_algorithm_ns",
    "first_sink_ns",
    "first_minor_faults",
    "first_major_faults",
    "algorithm_mean_ns",
    "algorithm_p50_ns",
    "algorithm_p90_ns",
    "algorithm_p99_ns",
    "algorithm_p99_9_ns",
    "algorithm_max_ns",
    "sink_mean_ns",
    "sink_p50_ns",
    "sink_p90_ns",
    "sink_p99_ns",
    "sink_p99_9_ns",
    "sink_max_ns",
    "total_mean_ns",
    "total_p50_ns",
    "total_p90_ns",
    "total_p99_ns",
    "total_p99_9_ns",
    "total_max_ns",
    "rss_kb",
    "anon_huge_kb",
    "private_hugetlb_kb",
    "shared_hugetlb_kb",
    "locked_kb",
    "vm_lck_kb",
    "cycles_per_frame",
    "instructions_per_frame",
    "ipc",
    "branches_per_frame",
    "branch_miss_rate",
    "l1d_loads_per_frame",
    "l1d_misses_per_frame",
    "l1d_miss_rate",
    "l2_requests_per_frame",
    "l2_misses_per_frame",
    "l2_miss_rate",
    "l1_dtlb_misses_per_frame",
    "l2_dtlb_misses_per_frame",
    "local_ccx_cache_fills_per_frame",
    "local_dram_io_fills_per_frame",
    "page_faults",
    "minor_faults",
    "major_faults",
    "context_switches",
    "cpu_migrations",
    "minimum_counter_coverage_percent",
    "latency_csv",
    "raw_perf_stat",
)


def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError as error:
        return f"unavailable: {error}"


def run_text(command, cwd=None, timeout=60):
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"
    return f"exit={result.returncode}\n{result.stdout.strip()}"


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cpu_topology(cpu):
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
    return read_text(root / "physical_package_id"), read_text(root / "core_id")


def select_cpu(requested):
    allowed = sorted(os.sched_getaffinity(0))
    if requested is not None:
        if requested not in allowed:
            raise ValueError(f"CPU {requested} is not in allowed affinity {allowed}")
        return requested, allowed
    physical = []
    seen = set()
    for cpu in allowed:
        topology = cpu_topology(cpu)
        if topology not in seen:
            physical.append(cpu)
            seen.add(topology)
    if not physical:
        raise ValueError("no physical CPU is available")
    return physical[-1], allowed


def snapshot_system(path):
    sections = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "loadavg": read_text("/proc/loadavg"),
        "uptime": read_text("/proc/uptime"),
        "stat": read_text("/proc/stat"),
        "interrupts": read_text("/proc/interrupts"),
        "softirqs": read_text("/proc/softirqs"),
        "meminfo": read_text("/proc/meminfo"),
    }
    with path.open("w", encoding="utf-8") as stream:
        for name, value in sections.items():
            stream.write(f"## {name}\n{value}\n\n")


def capture_environment(output, source_root, binary, alpao, implementations, cpu, allowed):
    runner = Path(__file__).resolve()
    calculon_root = (source_root.parent / "calculon-algorithms").resolve()
    entries = {
        "uname": run_text(["uname", "-a"]),
        "lscpu": run_text(["lscpu"]),
        "perf-version": run_text(["perf", "--version"]),
        "rustc-version": run_text(["rustc", "--version", "--verbose"]),
        "cargo-version": run_text(["cargo", "--version", "--verbose"]),
        "perf-event-paranoid": read_text("/proc/sys/kernel/perf_event_paranoid"),
        "kernel-command-line": read_text("/proc/cmdline"),
        "process-limits": read_text("/proc/self/limits"),
        "transparent-hugepage-enabled": read_text(
            "/sys/kernel/mm/transparent_hugepage/enabled"
        ),
        "transparent-hugepage-defrag": read_text(
            "/sys/kernel/mm/transparent_hugepage/defrag"
        ),
        "hugepage-meminfo": "\n".join(
            line for line in read_text("/proc/meminfo").splitlines() if "Huge" in line
        ),
        "hugetlbfs-mount": run_text(["findmnt", "-T", "/dev/hugepages"]),
        "glibc-tunables": run_text(
            ["/lib64/ld-linux-x86-64.so.2", "--list-tunables"]
        ),
        "plugin-git-head": run_text(["git", "rev-parse", "HEAD"], cwd=source_root),
        "plugin-git-status": run_text(["git", "status", "--short"], cwd=source_root),
        "calculon-git-head": run_text(["git", "rev-parse", "HEAD"], cwd=calculon_root),
        "calculon-git-status": run_text(
            ["git", "status", "--short"], cwd=calculon_root
        ),
        "selected-cpu": f"cpu={cpu} topology={cpu_topology(cpu)}\nallowed={allowed}",
        "benchmark-binary": f"{binary}\nsha256={sha256_file(binary)}",
        "alpao-dso": f"{alpao}\nsha256={sha256_file(alpao)}",
        "profiler-runner": f"{runner}\nsha256={sha256_file(runner)}",
    }
    for name, dso in implementations.items():
        entries[f"implementation-{name}"] = f"{dso}\nsha256={sha256_file(dso)}"
    frequency = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for name in (
        "scaling_driver",
        "scaling_governor",
        "energy_performance_preference",
        "scaling_min_freq",
        "scaling_max_freq",
    ):
        entries[f"cpu-{cpu}-{name}"] = read_text(frequency / name)
    with (output / "environment.txt").open("w", encoding="utf-8") as stream:
        stream.write(
            f"captured-utc={datetime.datetime.now(datetime.timezone.utc).isoformat()}\n"
        )
        for name, value in entries.items():
            stream.write(f"\n## {name}\n{value}\n")
    (output / "perf-list.txt").write_text(
        run_text(["perf", "list", "--details"]), encoding="utf-8"
    )
    for name, root in (("plugin", source_root), ("calculon", calculon_root)):
        diff = subprocess.run(
            ["git", "diff", "--binary", "HEAD"],
            cwd=root,
            stdout=subprocess.PIPE,
            check=True,
        ).stdout
        (output / f"{name}-source.patch").write_bytes(diff)


def wait_read(fd, size, timeout=60):
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise TimeoutError(f"timed out reading fd {fd}")
        chunk = os.read(fd, size - len(result))
        if not chunk:
            raise EOFError(f"unexpected EOF on fd {fd}")
        result.extend(chunk)
    return bytes(result)


def wait_line(fd, timeout=60):
    result = bytearray()
    while not result.endswith(b"\n"):
        result.extend(wait_read(fd, 1, timeout))
    return bytes(result)


def valid_perf_ack(value):
    return value.lstrip(b"\0") == b"ack\n"


def parse_kb_fields(text):
    result = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        match = re.match(r"\s*(\d+)\s+kB$", value)
        if match:
            result[name] = int(match.group(1))
    return result


def parse_benchmark_output(output, samples):
    result = {}
    latency_count = 0
    for line in output.splitlines():
        first = FIRST_RE.match(line)
        if first:
            result.update(
                {
                    "first_algorithm_ns": first.group("algorithm"),
                    "first_sink_ns": first.group("sink"),
                    "first_minor_faults": first.group("minor"),
                    "first_major_faults": first.group("major"),
                }
            )
            continue
        latency = LATENCY_RE.match(line)
        if latency and latency.group("name") in LATENCY_NAMES:
            prefix = LATENCY_NAMES[latency.group("name")]
            if int(latency.group("n")) != samples:
                raise ValueError(f"unexpected sample count in {line!r}")
            result[f"{prefix}_mean_ns"] = latency.group("mean")
            for source, target in (
                ("p50", "p50_ns"),
                ("p90", "p90_ns"),
                ("p99", "p99_ns"),
                ("p999", "p99_9_ns"),
                ("max", "max_ns"),
            ):
                result[f"{prefix}_{target}"] = latency.group(source) or ""
            latency_count += 1
    if latency_count != len(LATENCY_NAMES) or "first_algorithm_ns" not in result:
        raise ValueError(f"unexpected benchmark output: {output!r}")
    return result


def validate_latency_csv(path, samples):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != samples:
        raise ValueError(f"{path} contains {len(rows)} samples, expected {samples}")
    for expected, row in enumerate(rows):
        if int(row["iteration"]) != expected:
            raise ValueError(f"out-of-order latency sample in {path}: {row}")
        if int(row["total_ns"]) != int(row["algorithm_ns"]) + int(row["sink_ns"]):
            raise ValueError(f"invalid latency identity in {path}: {row}")


def parse_perf_stat(path):
    counters = {}
    coverage = []
    with path.open(encoding="utf-8") as stream:
        for fields in csv.reader(stream, delimiter=";"):
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0].startswith("<not"):
                raise ValueError(f"counter unavailable in {path}: {fields}")
            if len(fields) < 5:
                raise ValueError(f"invalid perf stat row in {path}: {fields}")
            event = fields[2].removesuffix(":u")
            counters[event] = int(float(fields[0]))
            coverage.append(float(fields[4]))
    if not counters:
        raise ValueError(f"no counters in {path}")
    minimum = min(coverage)
    if minimum < 99.9:
        raise ValueError(f"multiplexed counter group in {path}: {minimum}%")
    return counters, minimum


def ratio(numerator, denominator):
    return float(numerator) / float(denominator) if denominator else 0.0


def add_derived_counters(row, counters, samples):
    cycles = counters.get("cycles")
    instructions = counters.get("instructions")
    if cycles is not None:
        row["cycles_per_frame"] = cycles / samples
    if instructions is not None:
        row["instructions_per_frame"] = instructions / samples
    if cycles is not None and instructions is not None:
        row["ipc"] = ratio(instructions, cycles)
    mappings = {
        "branches": "branches_per_frame",
        "L1-dcache-loads": "l1d_loads_per_frame",
        "L1-dcache-load-misses": "l1d_misses_per_frame",
        "l2_request_g1.all_no_prefetch": "l2_requests_per_frame",
        "l2_cache_req_stat.ic_dc_miss_in_l2": "l2_misses_per_frame",
        "l1_dtlb_misses": "l1_dtlb_misses_per_frame",
        "l2_dtlb_misses": "l2_dtlb_misses_per_frame",
        "ls_any_fills_from_sys.int_cache": "local_ccx_cache_fills_per_frame",
        "ls_any_fills_from_sys.mem_io_local": "local_dram_io_fills_per_frame",
    }
    for event, field in mappings.items():
        if event in counters:
            row[field] = counters[event] / samples
    if "branches" in counters and "branch-misses" in counters:
        row["branch_miss_rate"] = ratio(counters["branch-misses"], counters["branches"])
    if "L1-dcache-loads" in counters and "L1-dcache-load-misses" in counters:
        row["l1d_miss_rate"] = ratio(
            counters["L1-dcache-load-misses"], counters["L1-dcache-loads"]
        )
    if (
        "l2_request_g1.all_no_prefetch" in counters
        and "l2_cache_req_stat.ic_dc_miss_in_l2" in counters
    ):
        row["l2_miss_rate"] = ratio(
            counters["l2_cache_req_stat.ic_dc_miss_in_l2"],
            counters["l2_request_g1.all_no_prefetch"],
        )
    for event, field in (
        ("page-faults", "page_faults"),
        ("minor-faults", "minor_faults"),
        ("major-faults", "major_faults"),
        ("context-switches", "context_switches"),
        ("cpu-migrations", "cpu_migrations"),
    ):
        if event in counters:
            row[field] = counters[event]


def with_hugetlb_tunable(environment, value):
    parts = [
        part
        for part in environment.get("GLIBC_TUNABLES", "").split(":")
        if part and not part.startswith("glibc.malloc.hugetlb=")
    ]
    parts.append(f"glibc.malloc.hugetlb={value}")
    environment["GLIBC_TUNABLES"] = ":".join(parts)


def run_profile(args, output, binary, alpao, implementation, dso, page_policy,
                memory_policy, group, repetition, cpu, capture_smaps):
    ready_r, ready_w = os.pipe()
    start_r, start_w = os.pipe()
    done_r, done_w = os.pipe()
    finish_r, finish_w = os.pipe()
    environment = os.environ.copy()
    with_hugetlb_tunable(environment, PAGE_POLICIES[page_policy])
    stem = (
        f"{implementation}-{page_policy}-{memory_policy}-{group}-rep{repetition}"
    )
    latency_csv = output / f"latency-{stem}.csv"
    raw = output / f"stat-{stem}.csv"
    environment.update(
        {
            "PW_BENCHMARK_MEMORY": memory_policy,
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
    perf_command = None
    try:
        tid = struct.unpack("i", wait_read(ready_r, struct.calcsize("i")))[0]
        observed_affinity = os.sched_getaffinity(tid)
        observed_policy = os.sched_getscheduler(tid)
        observed_priority = os.sched_getparam(tid).sched_priority
        if observed_affinity != {cpu}:
            raise RuntimeError(f"affinity mismatch for TID {tid}: {observed_affinity}")
        if args.rt_priority > 0 and (
            observed_policy != os.SCHED_FIFO or observed_priority != args.rt_priority
        ):
            raise RuntimeError(
                f"scheduler mismatch: policy={observed_policy} priority={observed_priority}"
            )
        rollup_text = read_text(f"/proc/{child.pid}/smaps_rollup")
        status_text = read_text(f"/proc/{child.pid}/status")
        if capture_smaps:
            (output / f"smaps-{implementation}-{page_policy}-{memory_policy}-rep{repetition}.txt").write_text(
                read_text(f"/proc/{child.pid}/smaps") + "\n", encoding="utf-8"
            )
        rollup = parse_kb_fields(rollup_text)
        status = parse_kb_fields(status_text)
        control_r, control_w = os.pipe()
        ack_r, ack_w = os.pipe()
        perf_command = [
            "perf",
            "stat",
            "--no-big-num",
            "-x",
            ";",
            "--output",
            str(raw),
            "--delay=-1",
            f"--control=fd:{control_r},{ack_w}",
            "-e",
            EVENT_GROUPS[group],
            "-t",
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
        if not valid_perf_ack(wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge enable")
        measured_started = time.monotonic()
        os.write(start_w, b"\x01")
        wait_read(done_r, 1, timeout=max(120, args.samples // 50))
        measurement_seconds = time.monotonic() - measured_started
        os.write(control_w, b"disable\n")
        if not valid_perf_ack(wait_line(ack_r)):
            raise RuntimeError("perf did not acknowledge disable")
        os.write(finish_w, b"\x01")
        child_stdout, child_stderr = child.communicate(timeout=60)
        perf_stdout, perf_stderr = perf.communicate(timeout=60)
        if child.returncode != 0:
            raise RuntimeError(f"benchmark failed: {child_stderr}")
        if perf.returncode != 0:
            raise RuntimeError(f"perf stat failed: {perf_stderr}")
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
    validate_latency_csv(latency_csv, args.samples)
    values = parse_benchmark_output(child_stdout, args.samples)
    counters, coverage = parse_perf_stat(raw)
    row = {field: "" for field in SUMMARY_FIELDS}
    row.update(
        {
            "repetition": repetition,
            "implementation": implementation,
            "page_policy": page_policy,
            "memory_policy": memory_policy,
            "group": group,
            "samples": args.samples,
            "warmup": args.warmup,
            "cpu": cpu,
            "rt_priority": args.rt_priority,
            "measurement_seconds": measurement_seconds,
            "rss_kb": rollup.get("Rss", ""),
            "anon_huge_kb": rollup.get("AnonHugePages", ""),
            "private_hugetlb_kb": rollup.get("Private_Hugetlb", ""),
            "shared_hugetlb_kb": rollup.get("Shared_Hugetlb", ""),
            "locked_kb": rollup.get("Locked", ""),
            "vm_lck_kb": status.get("VmLck", ""),
            "minimum_counter_coverage_percent": coverage,
            "latency_csv": latency_csv.name,
            "raw_perf_stat": raw.name,
        }
    )
    row.update(values)
    add_derived_counters(row, counters, args.samples)
    if int(row.get("cpu_migrations", 0) or 0) != 0:
        raise RuntimeError(f"profiled thread migrated: {row}")
    command_log = (
        f"$ GLIBC_TUNABLES={environment['GLIBC_TUNABLES']} "
        f"PW_BENCHMARK_MEMORY={memory_policy} {' '.join(command)}\n"
        f"$ {' '.join(perf_command)}\n"
        f"benchmark-stdout:\n{child_stdout}benchmark-stderr:\n{child_stderr}"
        f"perf-stdout:\n{perf_stdout}perf-stderr:\n{perf_stderr}\n"
    )
    return row, command_log


def write_aggregates(rows, output):
    keys = ("implementation", "page_policy", "memory_policy", "group")
    excluded = set(keys) | {
        "repetition",
        "samples",
        "warmup",
        "cpu",
        "rt_priority",
        "latency_csv",
        "raw_perf_stat",
    }
    metrics = [field for field in SUMMARY_FIELDS if field not in excluded]
    groups = {}
    for row in rows:
        key = tuple(str(row[name]) for name in keys)
        groups.setdefault(key, []).append(row)
    fields = list(keys) + ["repetitions"]
    for metric in metrics:
        fields.extend((f"{metric}_min", f"{metric}_median", f"{metric}_max"))
    with (output / "aggregate.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        for key, members in sorted(groups.items()):
            result = dict(zip(keys, key))
            result["repetitions"] = len(members)
            for metric in metrics:
                values = [
                    float(member[metric]) for member in members if member.get(metric) != ""
                ]
                if values:
                    result[f"{metric}_min"] = min(values)
                    result[f"{metric}_median"] = statistics.median(values)
                    result[f"{metric}_max"] = max(values)
            writer.writerow(result)


def write_summary(rows, output):
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, SUMMARY_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_manifest(output):
    with (output / "SHA256SUMS").open("w", encoding="utf-8") as stream:
        for path in sorted(output.iterdir()):
            if path.name != "SHA256SUMS" and path.is_file():
                stream.write(f"{sha256_file(path)}  {path.name}\n")


def parse_implementations(values):
    result = {}
    for value in values:
        try:
            name, path = value.split("=", 1)
        except ValueError as error:
            raise argparse.ArgumentTypeError("implementation must be NAME=PATH") from error
        if not name or name in result:
            raise argparse.ArgumentTypeError(f"invalid implementation name: {name!r}")
        result[name] = Path(path).resolve(strict=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("alpao_dso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--implementation", action="append", default=[], required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--samples", type=int, default=2_000)
    parser.add_argument("--warmup", type=int, default=256)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--rt-priority", type=int, default=88)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--region-width", type=int, default=8)
    parser.add_argument("--region-height", type=int, default=8)
    parser.add_argument("--actuators", type=int, default=468)
    parser.add_argument("--page-policy", choices=PAGE_POLICIES, action="append", default=[])
    parser.add_argument(
        "--memory-policy",
        choices=("default", "prefault", "locked"),
        action="append",
        default=[],
    )
    parser.add_argument("--group", choices=EVENT_GROUPS, action="append", default=[])
    parser.add_argument("--seed", type=int, default=20260825)
    args = parser.parse_args()
    if args.repetitions <= 0 or args.samples < 1_000 or args.warmup < 1:
        raise SystemExit("positive repetitions, at least 1000 samples, and warmup are required")
    binary = args.binary.resolve(strict=True)
    alpao = args.alpao_dso.resolve(strict=True)
    implementations = parse_implementations(args.implementation)
    output = args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit(f"refusing to overwrite output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[1]
    cpu, allowed = select_cpu(args.cpu)
    page_policies = args.page_policy or list(PAGE_POLICIES)
    memory_policies = args.memory_policy or ["default", "prefault", "locked"]
    groups = args.group or list(EVENT_GROUPS)
    capture_environment(
        output, source_root, binary, alpao, implementations, cpu, allowed
    )
    snapshot_system(output / "system-before.txt")
    cases = []
    for repetition in range(1, args.repetitions + 1):
        repeated = [
            (repetition, implementation, page, memory, group)
            for implementation in implementations
            for page in page_policies
            for memory in memory_policies
            for group in groups
        ]
        random.Random(args.seed + repetition).shuffle(repeated)
        cases.extend(repeated)
    rows = []
    logs = []
    captured_smaps = set()
    started = datetime.datetime.now(datetime.timezone.utc)
    for index, (repetition, implementation, page, memory, group) in enumerate(cases, 1):
        stem = f"{implementation}-{page}-{memory}-{group}-rep{repetition}"
        print(f"[{index}/{len(cases)}] {stem}", flush=True)
        smaps_key = (repetition, implementation, page, memory)
        row, log = run_profile(
            args,
            output,
            binary,
            alpao,
            implementation,
            implementations[implementation],
            page,
            memory,
            group,
            repetition,
            cpu,
            smaps_key not in captured_smaps,
        )
        captured_smaps.add(smaps_key)
        rows.append(row)
        logs.append(log)
        (output / "commands.log").write_text("\n".join(logs), encoding="utf-8")
        write_summary(rows, output)
        time.sleep(0.05)
    ended = datetime.datetime.now(datetime.timezone.utc)
    write_summary(rows, output)
    write_aggregates(rows, output)
    snapshot_system(output / "system-after.txt")
    metadata = {
        "started_utc": started.isoformat(),
        "ended_utc": ended.isoformat(),
        "duration_seconds": (ended - started).total_seconds(),
        "random_seed": args.seed,
        "repetitions": args.repetitions,
        "samples": args.samples,
        "warmup": args.warmup,
        "cpu": cpu,
        "rt_priority": args.rt_priority,
        "workload": {
            "width": args.width,
            "height": args.height,
            "region_width": args.region_width,
            "region_height": args.region_height,
            "actuators": args.actuators,
        },
        "implementations": {name: str(path) for name, path in implementations.items()},
        "page_policies": {name: PAGE_POLICIES[name] for name in page_policies},
        "memory_policies": memory_policies,
        "event_groups": {name: EVENT_GROUPS[name] for name in groups},
        "measurement_scope": "profiled main TID between warmed full-system gates",
        "latency_model": "closed-loop service time",
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    write_manifest(output)


if __name__ == "__main__":
    main()
