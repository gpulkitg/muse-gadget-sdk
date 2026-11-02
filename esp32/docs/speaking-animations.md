# Speaking gestures

The bundled avatar uses a local, procedural controller in
`components/muse/muse_speaking.h`. It adds no network requests, speech buffering,
or dependency on response wording. The character renders at the board's existing
frame period. Custom avatar renderers remain responsible for their own movement.

The vocabulary is deliberately general:

- **Explain:** raise one paw and extend it outward; the other gives a small
  supporting movement. Preparation, extension, hold and settling take about
  1.55–1.75 seconds when uninterrupted.
- **Present successive points:** a compact extension on one side, a rest, then
  an optional movement on the other side. Each movement lasts about 1.12 seconds.
  Nothing requires the reply to continue until both sides have moved.
- **Rest:** lower the paws and keep subtle breathing/blinking. Rest durations
  vary between movements, and leading hands alternate across gestures/turns.

These are continuous poses rather than fixed clips. On leaving speaking, the
renderer blends the current hand positions into the next state's pose over
0.28 seconds. Mouth animation stops with speaking; the hands may finish relaxing
afterward. A long response produces a sequence of individual gestures and rests,
not one animation stretched to the response length. Large frame-time gaps are
bounded so they cannot advance through a queue of overdue gestures.

Speech energy scales articulation and gently changes hand amplitude. Once the
controller has heard voice energy, a quiet period longer than 0.28 seconds lets
the hands settle; movement resumes after a short rest. Entirely silent caption
playback still gets general hand gestures, since the selected `main` baseline
paces text replies with silence rather than TTS. Its mouth stays closed.
Visible mouth shapes approximate articulation; they are not phoneme lip sync.

Feet stay planted, body lean follows the active hand, and paws remain inside
the 64-pixel drawing area. The shading bounds include extended hands, so larger
gestures do not disappear at the old body rectangle's edges.

## Verification

`test_speaking_animation.py` exercises cutoffs at 179 different points, long
responses, real pauses, resumed speech, silent captions and preview warm-up.
It also runs the production renderer through repeated state changes under
AddressSanitizer and UndefinedBehaviorSanitizer.

The existing GIF tool now renders speaking, long speaking, a short reply,
an audio pause and an interruption into listening:

```sh
python3 -m unittest discover -s esp32/tests -p test_speaking_animation.py
python3 esp32/tools/muse/make_gifs.py --default /tmp/muse-speaking-previews
```

GIF generation needs Pillow. It uses the actual renderer, without audio output.
When benchmark instrumentation is enabled, `gesture_explain`, `gesture_present`
and `gesture_rest` events record rendered transitions and the leading hand.
See `animation-benchmark.md` for performance measurement and its limits.
