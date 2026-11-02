#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
"""Capture instrumented Muse performance, report trials, and compare A/B runs.

No stopwatch, video, or manual transcription. 'collect' owns one serial port,
does not intentionally reset it, flushes raw logs continuously, and writes the
report even on Ctrl-C. 'report' and 'compare' need only the Python standard library.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import sys
import time
from pathlib import Path


def parse_record(line):
    for marker in ("@bench ", "@bench_host "):
        if marker not in line:
            continue
        try:
            result, _ = json.JSONDecoder().raw_decode(line.split(marker, 1)[1])
            if isinstance(result, dict):
                return result
        except (ValueError, TypeError):
            pass
    return None


def distribution(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return {"n": 0}
    return {"n": len(values), "min": values[0], "median": statistics.median(values),
            "p95": values[max(0, math.ceil(len(values) * .95) - 1)], "max": values[-1]}


def merge_metrics(records):
    merged = {}
    for r in records:
        if r.get("type") != "metric":
            continue
        if len(r.get("hist", [])) != 32 or sum(r["hist"]) != r["n"]:
            raise ValueError("Invalid device histogram")
        st = merged.setdefault(r["name"], {"n": 0, "sum": 0, "max": 0, "min": r["min"], "hist": [0] * 32})
        st["n"] += r["n"]
        st["sum"] += r["sum"]
        st["max"] = max(st["max"], r["max"])
        st["min"] = min(st["min"], r["min"])
        st["hist"] = [a + b for a, b in zip(st["hist"], r["hist"])]
    for st in merged.values():
        st["mean"] = st["sum"] / st["n"]
        for q in (50, 95):
            cumulative = 0
            for i, n in enumerate(st["hist"]):
                cumulative += n
                if cumulative >= math.ceil(st["n"] * q / 100):
                    st[f"p{q}_upper_bound"] = min(st["max"], (1 << i) if i < 31 else 0xffffffff)
                    break
    return merged


def summarize(lines, label=""):
    records, warnings, metadata, samples, trials = [], [], [], [], []
    sequences, boots, turns = {}, set(), {}
    pending_case = None
    session_milestones, network = [], []
    for line in lines:
        if re.search(r"Guru Meditation|abort\(\) was called|Brownout detector|Rebooting\.\.\.|input queue full", line):
            warnings.append("Device crash/reset or input-queue failure recorded")
        if "timing (ms after release):" in line:
            session_milestones.append({k: int(v) for k, v in re.findall(r"\b(\w+) \+(-?\d+)", line)})
        heartbeat = re.search(r"hb t=(\d+)s .*?wifi=(\w+) ws=(\w+) raw=(\w+).*?drop\+(\d+)", line)
        if heartbeat:
            network.append(dict(uptime_s=int(heartbeat[1]), wifi=heartbeat[2], connection=heartbeat[3],
                                tunnel=heartbeat[4], tunnel_drops=int(heartbeat[5])))
        r = parse_record(line)
        if r is None:
            if "@bench " in line:
                warnings.append("Malformed/truncated benchmark record")
            continue
        if r.get("type") == "case":
            pending_case = r
            continue
        if r.get("v") != 1 or not isinstance(r.get("boot"), int) or not isinstance(r.get("seq"), int):
            warnings.append("Unsupported/incomplete benchmark record")
            continue
        boot, seq = r["boot"], r["seq"]
        if boot in sequences:
            if seq == sequences[boot]:
                warnings.append("Duplicate record")
                continue
            if seq != sequences[boot] + 1:
                warnings.append(f"Serial sequence gap/reordering at {boot}:{seq}")
        sequences[boot] = seq
        boots.add(boot)
        records.append(r)
        if r["type"] == "meta":
            metadata.append(r)
        elif r["type"] == "sample":
            samples.append(r)
        elif r["type"] == "event":
            name, key = r["event"], (boot, r["turn"])
            if name in ("command_error", "command_busy"):
                warnings.append(name)
            if name in ("reply_begin", "local_begin"):
                turns[key] = {"boot": boot, "turn": r["turn"], "kind": "local" if name == "local_begin" else "reply",
                              "begin_us": r["us"], "release_us": r["value"] or None,
                              "case": pending_case.get("name") if pending_case else None,
                              "warmup": bool(pending_case and pending_case.get("warmup")), "events": []}
                if name == "local_begin":
                    pending_case = None
            if key in turns:
                turns[key]["events"].append(r)
    for turn in turns.values():
        events = sorted(turn.pop("events"), key=lambda r: r["us"])
        by_name = {r["event"]: r for r in events}
        end = next((r for r in reversed(events) if r["event"] in
                    ("reply_end", "reply_cancel", "reply_error", "local_end", "local_cancel", "local_error")), None)
        first = by_name.get("audio_submit")
        nonzero = by_name.get("audio_nonzero_submit")
        turn.update(outcome=end["event"] if end else "incomplete", end_us=end["us"] if end else None,
                    submitted_audio_ms=end["value"] / 16 if end else None,
                    first_submit_us=first["us"] if first else None,
                    nonzero_audio=nonzero is not None)
        turn["release_to_submit_ms"] = ((first["us"] - turn["release_us"]) / 1000
                                        if first and turn["release_us"] else None)
        turn["begin_to_submit_ms"] = (first["us"] - turn["begin_us"]) / 1000 if first else None
        turn["interruption_ms"] = None
        turn["interruption_trigger"] = None
        if end and end["event"].endswith("cancel"):
            trigger = next((r for r in reversed(events) if r["event"] in ("input_down", "local_stop")
                            and r["us"] <= end["us"]), None)
            if trigger:
                turn["interruption_ms"] = (end["us"] - trigger["us"]) / 1000
                turn["interruption_trigger"] = trigger["event"]
                if turn["case"] == "interrupt" and trigger["event"] != "local_stop":
                    warnings.append("Automatic local interruption case was interrupted by user input")
        if turn["outcome"] == "incomplete":
            warnings.append("Capture includes an incomplete trial")
        trials.append(turn)
    if len(boots) > 1:
        warnings.append("Device restarted or capture contains multiple boots")
    if not metadata or not samples:
        warnings.append("Missing metadata or periodic samples; confirm benchmark firmware is enabled")
    drop_base = metadata[0] if metadata else {}
    if any(s.get("event_dropped", 0) > drop_base.get("event_drop_base", 0) or
           s.get("tx_dropped", 0) > drop_base.get("tx_drop_base", 0) for s in samples
           if s["us"] >= drop_base.get("us", 0)):
        warnings.append("Device dropped benchmark events or output")
    counts = {}
    for r in records:
        if r["type"] == "metric":
            counts.setdefault((r["boot"], r["window"]), {})[r["name"]] = r["n"]
    speaking_windows = {(s["boot"], s["window"]) for s in samples
                        if counts.get((s["boot"], s["window"]), {}).get("render_speaking_us", 0)
                        and not counts.get((s["boot"], s["window"]), {}).get("render_other_us", 0)}
    speaking = [s for s in samples if (s["boot"], s["window"]) in speaking_windows]
    cpu = []
    for core in range(2):
        usable = [s for s in speaking if s["cpu_permille"][core] >= 0]
        span = sum(s["duration_us"] for s in usable)
        cpu.append(sum(s["cpu_permille"][core] * s["duration_us"] for s in usable) / span / 10 if span else None)
    measured = [t for t in trials if not t["warmup"] and t["outcome"] != "incomplete"]
    def minimum(field):
        return min((s[field] for s in samples), default=None)
    return {"schema": 1, "label": label, "valid_capture": not warnings, "warnings": sorted(set(warnings)),
            "metadata": metadata, "trials": trials, "samples": len(samples),
            "session_milestones_ms_after_session_release": session_milestones,
            "network_observations": network,
            "tunnel_drops": sum(n["tunnel_drops"] for n in network),
            "response_release_to_submit_ms": distribution(t["release_to_submit_ms"] for t in measured if t["kind"] == "reply"),
            "interruption_ms": distribution(t["interruption_ms"] for t in measured),
            "metrics": merge_metrics(records),
            "speaking_metrics": merge_metrics(r for r in records if r["type"] == "metric" and
                                               (r["boot"], r["window"]) in speaking_windows),
            "speaking_cpu_percent": cpu, "speaking_windows": len(speaking),
            "heap_free_min_observed": minimum("heap_free"), "heap_min_since_boot": minimum("heap_min"),
            "heap_largest_min_observed": minimum("heap_largest"), "psram_free_min_observed": minimum("psram_free"),
            "audio_write_errors": sum(s["audio_errors"] for s in samples),
            "display_errors": sum(s["display_errors"] for s in samples),
            "silence_padding_ms": sum(s["padding_frames"] for s in samples) / 16,
            "late_speaking_frames": sum(s["late"][1] for s in samples),
            "missed_speaking_slots": sum(s["missed_slots"][1] for s in samples),
            "notes": ["Submission timings are software measurements, not physical speaker onset or silence.",
                      "Main replies use silent caption pacing; nonzero_audio distinguishes the local speech sample.",
                      "Histogram percentile values are logarithmic-bin upper bounds.",
                      "CPU is scheduler non-idle time, sampled over windows containing only speaking avatar frames.",
                      "Empty reply reads/silence padding are not proof of an audible underrun."]}


def write_report(out, summary):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    temp = out / "summary.json.tmp"
    temp.write_text(json.dumps(summary, indent=2) + "\n")
    temp.replace(out / "summary.json")
    with (out / "trials.csv").open("w", newline="") as f:
        fields = ["boot", "turn", "kind", "case", "warmup", "outcome", "nonzero_audio", "release_to_submit_ms",
                  "begin_to_submit_ms", "interruption_ms", "interruption_trigger", "submitted_audio_ms", "begin_us", "end_us"]
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(summary["trials"])
    with (out / "metrics.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["scope", "metric", "n", "mean", "min", "p50_upper_bound", "p95_upper_bound", "max"])
        for scope in ("metrics", "speaking_metrics"):
            for name, st in summary[scope].items():
                writer.writerow([scope, name, *[st[k] for k in ("n", "mean", "min", "p50_upper_bound", "p95_upper_bound", "max")]])


def compare(before, after):
    warnings = []
    for label, s in (("before", before), ("after", after)):
        if not s.get("valid_capture"):
            warnings.append(f"{label}: capture has warnings")
    fields = ("board", "frame_us", "audio_rate", "volume", "brightness", "speaker", "workload")
    for key in fields:
        a = {json.dumps(m.get(key), sort_keys=True) for m in before.get("metadata", [])}
        b = {json.dumps(m.get(key), sort_keys=True) for m in after.get("metadata", [])}
        if len(a) != 1 or a != b:
            warnings.append(f"Settings/workload mismatch: {key}")
    def cases(s):
        return sorted((t.get("case") or "", t["outcome"], round(t["submitted_audio_ms"] or 0))
                      for t in s["trials"] if t["kind"] == "local" and not t["warmup"])
    # Interrupted durations depend on serial/dispatch timing; compare their cases, not exact milliseconds.
    for s in (before, after):
        if not s.get("speaking_windows"):
            warnings.append("No complete speaking windows for CPU/display comparison")
    a, b = cases(before), cases(after)
    if [(x, y) for x, y, _ in a] != [(x, y) for x, y, _ in b]:
        warnings.append("Different local test cases/counts/outcomes")
    if [(x, z) for x, y, z in a if y == "local_end"] != [(x, z) for x, y, z in b if y == "local_end"]:
        warnings.append("Different uninterrupted local audio durations")
    changes = {}
    def change(name, a, b):
        if a is None or b is None:
            return
        changes[name] = {"before": a, "after": b, "delta": b - a,
                         "percent": (b - a) / a * 100 if a else None}
    for name in ("audio_write_errors", "display_errors", "heap_free_min_observed", "heap_largest_min_observed"):
        change(name, before.get(name), after.get(name))
    for i in range(2):
        change(f"speaking_cpu_{i}_percent", before["speaking_cpu_percent"][i], after["speaking_cpu_percent"][i])
    for name in sorted(before["metrics"].keys() & after["metrics"].keys()):
        for field in ("mean", "p95_upper_bound", "max"):
            change(name + "." + field, before["metrics"][name][field], after["metrics"][name][field])
    for name in ("response_release_to_submit_ms", "interruption_ms"):
        change(name + ".median", before[name].get("median"), after[name].get("median"))
    return {"schema": 1, "comparable": not warnings, "warnings": sorted(set(warnings)), "changes": changes}


def collect(args):
    import chat  # includes pyserial only when the port is opened
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    plan = []
    if args.local_repeats:
        plan.append({"name": "warmup", "ms": 4000, "warmup": True})
        for _ in range(args.local_repeats):
            plan.extend([{"name": "short", "ms": 500}, {"name": "medium", "ms": 4000},
                         {"name": "long", "ms": 30000}, {"name": "interrupt", "ms": 30000, "stop_after": 5}])
    start, next_action, case_deadline = time.monotonic(), time.monotonic() + 2, None
    active, stopping, metadata_seen, done_at = None, False, False, None
    print(f"Recording to {out.resolve()}. Ctrl-C saves the report.", flush=True)
    try:
        with (out / "raw.log").open("w", buffering=1) as raw, chat.Board(args.port or chat.pick_port()) as board:
            board.drain(.3)
            board.write_line("bench.info")
            try:
                while True:
                    now = time.monotonic()
                    if args.seconds and now - start >= args.seconds:
                        break
                    if not metadata_seen and now - start > 15:
                        raise RuntimeError("No benchmark metadata: flash CONFIG_MUSE_BENCHMARK=y firmware first")
                    if metadata_seen and plan and active is None and now >= next_action:
                        active, stopping = plan.pop(0), False
                        raw.write("@bench_host " + json.dumps({"type": "case", **active}) + "\n")
                        print(f"Local test: {active['name']} ({active['ms']} ms)", flush=True)
                        board.write_line(f"bench.play={active['ms']}")
                        case_deadline = now + active["ms"] / 1000 + 20
                    if active and active.get("stop_at") and not stopping and now >= active["stop_at"]:
                        board.write_line("bench.stop")
                        stopping = True
                    if active and now > case_deadline:
                        raise RuntimeError("Local test timed out; raw log retained")
                    if done_at and not args.seconds and now >= done_at:
                        break
                    line = board.read_line(time.monotonic() + .2)
                    if line is None:
                        continue
                    raw.write(line.rstrip("\r\n") + "\n")
                    r = parse_record(line)
                    if not r:
                        continue
                    if r.get("type") == "meta":
                        metadata_seen = True
                    if r.get("event") == "local_begin" and active and active.get("stop_after"):
                        active["stop_at"] = time.monotonic() + active["stop_after"]
                    if r.get("event") in ("command_error", "command_busy", "local_error"):
                        raise RuntimeError("Device rejected/failed local workload; inspect raw.log")
                    if r.get("event") in ("local_end", "local_cancel") and active:
                        active = None
                        next_action = time.monotonic() + 2
                        if not plan:
                            done_at = next_action + 1
            except KeyboardInterrupt:
                print("Capture stopped; saving report.", flush=True)
    except chat.BoardError as error:
        raise RuntimeError(str(error)) from error
    finally:
        if (out / "raw.log").exists():
            summary = summarize((out / "raw.log").read_text(errors="replace").splitlines(), args.label)
            write_report(out, summary)
            print(f"Report: {out / 'summary.json'}; valid capture: {summary['valid_capture']}", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("collect")
    p.add_argument("--port")
    p.add_argument("--out", required=True)
    p.add_argument("--label", default="")
    p.add_argument("--seconds", type=float, default=0, help="0: end after local suite, or Ctrl-C for interactive capture")
    p.add_argument("--local-repeats", type=int, default=0, help="automatically run short/medium/long/interruption cases")
    p = sub.add_parser("report")
    p.add_argument("log", type=Path)
    p.add_argument("--out", required=True)
    p.add_argument("--label", default="")
    p = sub.add_parser("compare")
    p.add_argument("before", type=Path)
    p.add_argument("after", type=Path)
    p.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "collect":
            if args.seconds < 0 or not 0 <= args.local_repeats <= 20:
                parser.error("seconds must be nonnegative; local-repeats must be 0..20")
            collect(args)
        elif args.command == "report":
            write_report(args.out, summarize(args.log.read_text(errors="replace").splitlines(), args.label))
        else:
            result = compare(json.loads(args.before.read_text()), json.loads(args.after.read_text()))
            args.out.write_text(json.dumps(result, indent=2) + "\n")
            print(f"Comparable: {result['comparable']}; report: {args.out}")
    except (OSError, ValueError, RuntimeError) as e:
        parser.exit(1, str(e) + "\n")


if __name__ == "__main__":
    main()
