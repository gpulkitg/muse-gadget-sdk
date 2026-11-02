# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
"""Measurement correctness and capture integrity; no board or network needed."""
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("muse_benchmark_tool", ROOT / "tools/muse/benchmark.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class Capture:
    def __init__(self):
        self.seq = 0
        self.lines = []

    def add(self, kind, **data):
        self.seq += 1
        self.lines.append("I (100) other: @bench " + json.dumps(
            dict(v=1, boot=42, seq=self.seq, type=kind, **data)))
        return self

    def event(self, name, us, value=0, turn=1):
        return self.add("event", event=name, us=us, value=value, turn=turn)

    def metric(self, name, n=2, value=8):
        hist = [0] * 32
        hist[max(0, (value - 1).bit_length())] = n
        return self.add("metric", window=1, name=name, n=n, sum=n * value, min=value, max=value, hist=hist)

    def ready(self):
        self.add("meta", board="test", frame_us=40000, audio_rate=16000, volume=70,
                 brightness=100, speaker=True, workload="main_silent_captions_and_local_mp3")
        self.add("sample", us=1000000, window=1, duration_us=1000000, mode_at_end=4,
                 cpu_permille=[200, 400], heap_free=30000, heap_min=25000, heap_largest=24000,
                 psram_free=4000000, event_dropped=0, tx_dropped=0, late=[0, 1], missed_slots=[0, 0],
                 dirty_px=[0, 400], display_px=500, display_errors=0, audio_errors=0,
                 audio_frames=16000, padding_frames=320, buffer_capacity=32000)
        self.metric("render_speaking_us")
        return self


class Measurements(unittest.TestCase):
    def test_actual_production_histogram_handles_zero_and_large_values(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "stats.c").write_text('''
#include <assert.h>
#include <stdint.h>
#include "muse_benchmark_stats.h"
int main(void) {
    muse_bench_stat_t s = {0};
    muse_bench_add(&s, 0); muse_bench_add(&s, 1); muse_bench_add(&s, 3);
    muse_bench_add(&s, UINT32_MAX); muse_bench_add(&s, UINT32_MAX);
    assert(s.n == 5 && s.min == 0 && s.max == UINT32_MAX);
    assert(s.sum == (uint64_t)UINT32_MAX * 2 + 4);
    assert(s.hist[0] == 2 && s.hist[2] == 1 && s.hist[31] == 2);
    return 0;
}
''')
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "components/muse"),
                            str(p / "stats.c"), "-o", str(p / "stats")], check=True, capture_output=True)
            subprocess.run([str(p / "stats")], check=True)

    def test_release_latency_and_silent_reply_are_not_audible_speech(self):
        c = Capture().ready().event("reply_begin", 200000, value=100000)
        c.event("audio_submit", 600000, value=1).event("reply_end", 1600000, value=16000)
        s = bench.summarize(c.lines)
        self.assertTrue(s["valid_capture"])
        self.assertEqual(s["response_release_to_submit_ms"]["median"], 500)
        self.assertFalse(s["trials"][0]["nonzero_audio"])
        self.assertEqual(s["trials"][0]["submitted_audio_ms"], 1000)

    def test_interrupt_uses_last_trigger_and_preserves_nonzero_audio(self):
        c = Capture().ready().event("local_begin", 10)
        c.event("audio_nonzero_submit", 50).event("input_down", 100)
        c.event("local_stop", 200).event("local_cancel", 21200, value=320)
        s = bench.summarize(c.lines)
        self.assertEqual(s["interruption_ms"]["median"], 21)
        self.assertTrue(s["trials"][0]["nonzero_audio"])

    def test_merge_uses_counts_not_average_of_window_averages(self):
        c = Capture().ready()
        c.metric("render_speaking_us", n=1, value=32)
        st = bench.summarize(c.lines)["metrics"]["render_speaking_us"]
        self.assertEqual(st["mean"], 16)
        self.assertEqual(st["min"], 8)
        self.assertEqual(st["p95_upper_bound"], 32)

    def test_mixed_mode_window_excluded_from_speaking_cpu(self):
        c = Capture().ready().metric("render_other_us")
        s = bench.summarize(c.lines)
        self.assertEqual(s["speaking_windows"], 0)
        self.assertEqual(s["speaking_cpu_percent"], [None, None])

    def test_speaking_cpu_and_padding_units(self):
        s = bench.summarize(Capture().ready().lines)
        self.assertEqual(s["speaking_cpu_percent"], [20, 40])
        self.assertEqual(s["silence_padding_ms"], 20)

    def test_missing_serial_record_invalidates_capture(self):
        c = Capture().ready()
        c.lines.pop(1)
        self.assertFalse(bench.summarize(c.lines)["valid_capture"])

    def test_dropped_device_events_invalidates_capture(self):
        c = Capture().ready()
        r = bench.parse_record(c.lines[1])
        r["event_dropped"] = 1
        c.lines[1] = "@bench " + json.dumps(r)
        self.assertFalse(bench.summarize(c.lines)["valid_capture"])

    def test_truncated_record_and_missing_metadata_invalid(self):
        self.assertFalse(bench.summarize(['@bench {"v":1'])['valid_capture'])

    def test_warmup_excluded_from_trial_latency_summary(self):
        c = Capture().ready()
        c.lines.append('@bench_host {"type":"case","name":"warmup","warmup":true}')
        c.event("local_begin", 1).event("audio_submit", 2, value=1).event("local_end", 100, value=16)
        s = bench.summarize(c.lines)
        self.assertTrue(s['trials'][0]['warmup'])
        self.assertEqual(s['response_release_to_submit_ms']['n'], 0)

    def test_incomplete_trial_retained_and_capture_flagged(self):
        c = Capture().ready().event("reply_begin", 1)
        s = bench.summarize(c.lines)
        self.assertEqual(s['trials'][0]['outcome'], 'incomplete')
        self.assertFalse(s['valid_capture'])

    def test_report_writes_csv_and_json_and_replays_same_raw_input(self):
        s = bench.summarize(Capture().ready().lines, "before")
        with tempfile.TemporaryDirectory() as d:
            bench.write_report(d, s)
            self.assertEqual(json.loads((Path(d)/'summary.json').read_text()), s)
            self.assertIn('p95_upper_bound', (Path(d)/'metrics.csv').read_text())

    def test_comparison_checks_settings_and_workload(self):
        a = bench.summarize(Capture().ready().lines)
        b = json.loads(json.dumps(a))
        self.assertTrue(bench.compare(a,b)['comparable'])
        b['metadata'][0]['brightness'] = 50
        self.assertFalse(bench.compare(a,b)['comparable'])

    def test_bad_histogram_is_rejected(self):
        c = Capture().ready()
        r = bench.parse_record(c.lines[-1])
        r['hist'] = [0] * 32
        c.lines[-1] = '@bench ' + json.dumps(r)
        with self.assertRaises(ValueError):
            bench.summarize(c.lines)

    def test_session_milestones_and_tunnel_drops_have_distinct_names(self):
        c = Capture().ready()
        c.lines.extend(['I (100) muse_chat: timing (ms after release): sent +10 ack +40 text +600',
                        'I (1000) link.heartbeat: hb t=1s setup=done wifi=up ws=up raw=up ble=off int=30K/20K tun drop+2'])
        s = bench.summarize(c.lines)
        self.assertEqual(s['session_milestones_ms_after_session_release'][0]['text'],600)
        self.assertEqual(s['tunnel_drops'],2)
        self.assertEqual(s['audio_write_errors'],0)

    def test_panic_in_raw_log_invalidates_capture(self):
        c = Capture().ready()
        c.lines.append("Guru Meditation Error: Core 0 panic'ed")
        self.assertFalse(bench.summarize(c.lines)['valid_capture'])

    def test_manual_stop_is_not_mistaken_for_scheduled_interruption(self):
        c = Capture().ready()
        c.lines.append('@bench_host {"type":"case","name":"interrupt"}')
        c.event('local_begin', 10).event('input_down', 100).event('local_cancel', 200)
        s = bench.summarize(c.lines)
        self.assertEqual(s['trials'][0]['interruption_trigger'], 'input_down')
        self.assertFalse(s['valid_capture'])


if __name__ == '__main__':
    unittest.main()
