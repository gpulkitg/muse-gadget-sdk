# Streaming reply speech

Reply TTS is optional and selected at build time. Enable
`CONFIG_MUSE_TTS_ELEVENLABS`, and set `CONFIG_MUSE_TTS_API_KEY` in the build's
sdkconfig only. The existing voice and model settings default to Matilda and
`eleven_flash_v2_5`. Never commit credentials in device overlays.

The provider runs on a worker with a 16 KiB PSRAM stack. A 32 KiB queue feeds
the session's existing incremental MP3 decoder, resampler, captions, and speaker
buffer. The producer waits when the queue fills; reply duration does not require
a larger allocation. Connection maintenance and cancellation remain on the session
loop. Cancelled workers finish cleanup before another request starts.

To add a provider, implement `muse_tts_provider` from `muse_tts_provider.h` and
select it at the session's `muse_tts_start` call through build configuration.
The provider declares its audio format, writes elementary audio bytes through
the sink, and returns a terminal result. URL, credentials, request schema,
response validation, and transport framing belong in the adapter. Currently only
MP3 is accepted; additional codecs need a decoder path. The sink may partially
queue a write before cancellation, so providers must never retry a request after
a write has started. Poll cancellation around transport calls and use bounded
network waits. Do not access chat state from the worker.

Synthesis retains text independently of captions, up to 32 KiB including the
terminator per message and 128 KiB per turn. Authoritative final text replaces
accumulated deltas. Allocation or limit failures skip synthesis and mark the
reply incomplete, rather than speaking a prefix. Caption storage and Watcher
review limits are unchanged. Typed console turns remain unspoken.

The ElevenLabs adapter retries up to three times before audio enters the queue.
Failures after that point never restart speech. EOF drains compressed audio and
queued PCM. A 30-second progress watchdog detects stalled playback; total active
speech is limited to ten minutes per turn. The regular turn deadline is suspended
while streaming speech is active. Turning the speaker off during speech mutes
the output while the reply and captions keep progressing. Turning it back on
plays the remaining audio; it does not replay the muted portion. A reply that
finishes while muted ends normally. Provider request-size and model limits still
apply independently of firmware memory limits.

Host coverage lives in `test_muse_tts_stream.py`, `test_muse_tts_elevenlabs.py`,
`test_muse_tts_text.py`, `test_muse_tts_decode.py`, and `test_muse_tts_mute.py`. Hardware validation should include audio beyond
33 seconds (eight seconds with compact buffers), final-word playback, cancellation,
connection maintenance, and repeated-turn heap measurements. Full firmware
builds require ESP-IDF v6.0.3 or later as enforced by the repository.

## Device acceptance checklist

Use the physical push-to-talk button with the speaker enabled. Serial typed chat
intentionally does not request TTS. Capture the serial log while testing.

| Test | Action | Pass condition |
| --- | --- | --- |
| Short reply | Ask: “Reply with exactly: The small test is complete.” | One complete spoken reply, no repeat or reset. |
| Long reply and full text | Ask for a single 250–300 word story ending with “The purple lighthouse is still shining.” | Hear the exact ending. `TTS stream started` reports more than 1,023 text bytes; `TTS stream ended` reports more than 524,288 MP3 bytes and `complete`. |
| First-audio latency | Watch the long-reply log. | `reply audio` appears before `TTS stream ended`; playback begins during download. |
| Cancellation | During a long reply, hold push-to-talk and ask for a short different reply. | Old speech stops; only the new reply plays, with no old ending appended. |
| Speaker mute | Switch the speaker off during a long reply, wait 5–10 seconds, then enable it before the reply ends. Also leave it off through a full reply. | Captions keep progressing while muted; re-enabling plays the remaining reply without restarting. No “Reply incomplete” error. Finishing while muted ends normally. |
| Network interruption | Interrupt Wi-Fi during speech, restore it, then make a new request. | No automatic replay of already spoken text, no crash, and the later request works after reconnecting. Buffered audio may drain before the interruption is reported. |
| Repeated turns | Run three long replies and alternate with short replies. | All endings are present, captions continue, and no allocation failure, watchdog, or reboot appears. |

A clean log should not contain `MP3 buffer full`, `TTS playback stalled`, a panic,
or a watchdog reset. `TTS stream interrupted` is expected only in the deliberate
cancellation/network-failure tests. Audible quality and final-word verification
require listening; successful HTTP completion alone does not establish those.

## Device test observations

The first streaming image received 1,465,826 MP3 bytes and played 91.61 seconds
of audio, exceeding the former 512 KiB limit. Push-to-talk interruption followed
by a short reply worked. Interrupting Wi-Fi cancelled the request with a lost-
connection message, and a subsequent request worked after reconnection.

That image incorrectly treated a speaker toggle as cancellation and displayed
“Reply incomplete”. The corrected behavior uses the existing PCM mute path;
the host regression covers a 60-second mute, re-enabling, and EOF while muted.
The corrected speaker-toggle behavior still needs an on-device retest.
