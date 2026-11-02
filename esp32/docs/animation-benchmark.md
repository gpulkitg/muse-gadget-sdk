# Avatar animation performance benchmark

The before image is main commit `1b894f551e64416e2844452ec6056ec56d6a10f1`
with measurement instrumentation and the original avatar. The after image adds
speaking gestures to that same instrumented source/configuration. `main` remains
unchanged. Preserve each image, sdkconfig, ELF hash, source revision/diff and raw
capture. The earlier uninstrumented image remains a separate reference.

## Build

Use ESP-IDF v6.0.1 and the correct board overlay. Follow `../AGENTS.md` for SDK
token handling and board identification. Enable `CONFIG_MUSE_BENCHMARK=y` in the
private build sdkconfig. This selects FreeRTOS runtime statistics; verify
`CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y` in both images. Normal builds
leave benchmarking disabled. Do not enable screenshots or video for this test.

## Automatic measurements

From the repository root, using a Python environment containing pyserial:

```sh
python esp32/tools/muse/benchmark.py collect --port PORT \
  --out /tmp/muse-before --label before --local-repeats 3
```

The collector does not intentionally reset the board. It owns the serial port,
requests metadata, and flushes `raw.log` continuously. Close other serial clients
first. The automatic suite plays the existing bundled MP3: one 4-second warm-up,
then three repetitions of 0.5-second, 4-second, 30-second, and interrupted playback.
The interrupted case is stopped through `bench.stop`, without opening a microphone
turn or sending a message to Muse. Its stop time is approximately five seconds
after the begin event is received; the actual device event times are measured.
PCM repeats for requests longer than the clip, so the loop boundary is part of
the identical workload, not evidence of a regression. Local decode preparation
is outside the measured speaking period. The speaker must be enabled.

For natural interactions, leave the collector running and use the gadget normally:

```sh
python esp32/tools/muse/benchmark.py collect --port PORT \
  --out /tmp/muse-before-interactive --label before-interactive --seconds 600
```

There is no manual timing or transcription. Ctrl-C also saves the report. Commands
`>bench.play=200..60000`, `>bench.stop`, and `>bench.info` exist only in instrumented
images. Playback commands are intended for an idle voice-note device; a request
can wait until the current operation finishes. Don't mix an automatic local suite
with user voice turns. Local stops never cancel ordinary user replies.

Outputs are `summary.json`, `trials.csv`, `metrics.csv`, and `raw.log`. Keep these
outside version control and copy temporary runs to permanent storage. Rebuild a
report, or compare matching before/after workloads:

```sh
python esp32/tools/muse/benchmark.py report /tmp/muse-before/raw.log \
  --out /tmp/muse-before-reported --label before
python esp32/tools/muse/benchmark.py compare \
  /tmp/muse-before/summary.json /tmp/muse-after/summary.json \
  --out /tmp/muse-comparison.json
```

## Interpretation and controls

- `main` currently uses silent PCM to pace reply captions, not TTS. Natural-turn
  latency is therefore **release to audio submission**, not audible speech onset.
  `nonzero_audio` reports whether any nonzero unmuted PCM was submitted. The local
  MP3 workload provides real speaker output without a network or model dependency.
- Input/reply timestamps use the device's microsecond clock. Input-to-cancellation
  is software handling time, not measured physical speaker silence. Successful
  PCM samples give submitted duration; interrupted tails can still be in hardware.
- Render time measures `muse_pixel_render`; UI time adds dirty-region computation
  and invalidation, excluding later LVGL work. Frame intervals concern active avatar
  updates; sleeping/menu/image periods reset interval tracking. A late update is
  over 1.5 times the configured period; missed slots count whole skipped periods.
  These counters do not measure the panel's physical refresh rate.
- On boards using `muse_lcd_bands`, display metrics cover dequeue-to-final-DMA
  callback for each **band**, including copying and any TE wait. Queue delay and
  pixels transferred are separate. Dirty pixel counts are requested invalidation
  area, not unique screen pixels or actual bus traffic. Other display backends
  have no band metrics; absence must not be interpreted as zero cost.
- Histograms use 32 logarithmic bins. Means/minima/maxima are exact integer
  aggregates; p50/p95 are bin upper bounds capped at the observed maximum.
- Scheduler non-idle CPU estimates use per-core idle-task runtime deltas and the
  ESP timer. They include interrupt/scheduler accounting effects, not task-level
  attribution. Only windows with speaking frames and no other avatar frames are
  included in the speaking CPU/display summary. USB keeps this board awake;
  these are not battery-power measurements. Keep captures under one hour because
  the selected runtime clock wraps at about 71 minutes.
- Audio-write duration includes expected blocking for playback pacing. Failed
  writes are counted. Reply buffer occupancy is sampled **after** each voice-note
  read. Silence padding/empty reads are not necessarily underruns or audible
  glitches. No claim of acoustic quality, physical output timing, or power is
  made without suitable external capture equipment.
- Hooks use fixed storage and short critical sections. One low-priority task
  emits aggregated records about once per second; it uses a PSRAM stack where
  available. The USB diagnostic TX ring is 2048 bytes so complete JSON records
  fit in one atomic driver write (normal builds keep the driver's default).
  No logging or allocation occurs in frame/audio/DMA hooks. Profiling
  itself adds overhead; keep it identical in both images. Failed writes, queue
  drops, serial sequence gaps, incomplete trials, and restarts flag the capture.
- Match board, volume, brightness, speaker state, frame period, runtime clock,
  workload repetitions and power connection. Record actual response durations.
  Compare networked latency across repeated trials, not a single response.
  A p95 from a small trial count is descriptive, not a reliable tail estimate.

Gesture lifecycle events can use the same event queue once gestures are added.
They are intentionally absent from this instrumentation-only baseline.
