/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#include "muse_benchmark.h"
#include "muse_benchmark_stats.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "muse_board.h"
#include "muse_console.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"

enum { RENDER_OTHER, UI_OTHER, INTERVAL_OTHER, RENDER_SPEAKING, UI_SPEAKING,
       INTERVAL_SPEAKING, AUDIO_WRITE, DISPLAY, DISPLAY_QUEUE, BUFFER, N_STATS };
static const char *const NAMES[] = { "render_other_us", "ui_other_us", "interval_other_us",
    "render_speaking_us", "ui_speaking_us", "interval_speaking_us", "audio_write_us",
    "display_band_us", "display_queue_us", "reply_buffer_frames" };
typedef struct {
    muse_bench_stat_t stat[N_STATS];
    uint32_t late[2], missed[2], dirty_px[2], display_px, display_errors;
    uint32_t audio_errors, audio_frames, padding_frames, buffer_capacity;
} window_t;
typedef struct { int64_t us, value; const char *name; uint32_t turn; } event_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static window_t s_window;
static event_t s_events[32];
static unsigned s_head, s_count, s_event_dropped;
static uint32_t s_turn, s_boot, s_seq, s_tx_dropped;
static bool s_active, s_first, s_nonzero, s_info = true;
static int64_t s_release, s_frame_at, s_display_at;
static int s_frame_mode = -1;
static unsigned s_local_ms;
static bool s_stop_local;
static uint32_t s_turn_frames;
static char s_line[1200];  /* logger task is the only formatter/writer */

static void event_locked(const char *name, int64_t value, int64_t now)
{
    if (s_count == 32) { s_event_dropped++; return; }
    s_events[(s_head + s_count++) % 32] = (event_t){ now, value, name, s_turn };
}

void muse_benchmark_event(const char *name, int64_t value)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    event_locked(name, value, now);
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_input(bool down)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (!down) s_release = now;
    event_locked(down ? "input_down" : "input_up", 0, now);
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_begin(bool local)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_turn++;
    s_active = true;
    s_first = s_nonzero = false;
    s_turn_frames = 0;
    event_locked(local ? "local_begin" : "reply_begin", local ? 0 : s_release, now);
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_end(const char *name)
{
    portENTER_CRITICAL(&s_lock);
    event_locked(name, s_turn_frames, esp_timer_get_time());
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_ui_pause(void)
{
    portENTER_CRITICAL(&s_lock);
    s_frame_at = 0;
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_frame(int mode, int64_t at, uint32_t render, uint32_t total, uint32_t period)
{
    int speaking = mode == MUSE_MODE_SPEAKING, index = speaking ? RENDER_SPEAKING : RENDER_OTHER;
    portENTER_CRITICAL(&s_lock);
    muse_bench_add(&s_window.stat[index], render);
    muse_bench_add(&s_window.stat[index + 1], total);
    if (s_frame_at && mode == s_frame_mode) {
        uint32_t interval = at - s_frame_at;
        muse_bench_add(&s_window.stat[index + 2], interval);
        if (interval > period + period / 2) s_window.late[speaking]++;
        if (interval >= period * 2) s_window.missed[speaking] += interval / period - 1;
    }
    s_frame_mode = mode;
    s_frame_at = at;
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_dirty(uint32_t pixels)
{
    int speaking = muse_state_mode(NULL) == MUSE_MODE_SPEAKING;
    portENTER_CRITICAL(&s_lock);
    s_window.dirty_px[speaking] += pixels;
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_audio(int64_t at, uint32_t elapsed, unsigned frames, bool ok, bool nonzero)
{
    portENTER_CRITICAL(&s_lock);
    muse_bench_add(&s_window.stat[AUDIO_WRITE], elapsed);
    s_window.audio_frames += ok ? frames : 0;
    if (s_active && ok) s_turn_frames += frames;
    s_window.audio_errors += !ok;
    if (s_active && !s_first) {
        event_locked("audio_submit", ok, at);
        if (ok) event_locked("audio_first_write_done", 0, at + elapsed);
        s_first = true;
    }
    if (s_active && ok && nonzero && !s_nonzero) {
        event_locked("audio_nonzero_submit", 0, at);
        s_nonzero = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_padding(unsigned frames)
{
    portENTER_CRITICAL(&s_lock);
    s_window.padding_frames += frames;
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_buffer(unsigned frames, unsigned capacity)
{
    portENTER_CRITICAL(&s_lock);
    muse_bench_add(&s_window.stat[BUFFER], frames);
    s_window.buffer_capacity = capacity;
    portEXIT_CRITICAL(&s_lock);
}

void muse_benchmark_display_begin(int64_t queued, unsigned pixels)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_display_at = now;
    muse_bench_add(&s_window.stat[DISPLAY_QUEUE], now - queued);
    s_window.display_px += pixels;
    portEXIT_CRITICAL(&s_lock);
}

void IRAM_ATTR muse_benchmark_display_done(int64_t at)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (s_display_at && at >= s_display_at) {
        muse_bench_add(&s_window.stat[DISPLAY], at - s_display_at);
        s_display_at = 0;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void muse_benchmark_display_error(void)
{
    portENTER_CRITICAL(&s_lock);
    s_window.display_errors++;
    portEXIT_CRITICAL(&s_lock);
}

bool muse_benchmark_command(const char *line)
{
    if (!strcmp(line, "bench.stop")) {
        portENTER_CRITICAL(&s_lock);
        s_stop_local = true;
        event_locked("local_stop", 0, esp_timer_get_time());
        portEXIT_CRITICAL(&s_lock);
        return true;
    }
    if (!strcmp(line, "bench.info")) {
        portENTER_CRITICAL(&s_lock); s_info = true; portEXIT_CRITICAL(&s_lock);
        return true;
    }
    if (strncmp(line, "bench.play=", 11)) return false;
    char *end;
    unsigned long ms = strtoul(line + 11, &end, 10);
    if (!line[11] || *end || ms < 200 || ms > 60000) {
        muse_benchmark_event("command_error", 1);
        return true;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_local_ms || s_active) event_locked("command_busy", 0, esp_timer_get_time());
    else { s_local_ms = ms; s_stop_local = false; event_locked("local_requested", ms, esp_timer_get_time()); }
    portEXIT_CRITICAL(&s_lock);
    return true;
}

unsigned muse_benchmark_take_local(void)
{
    portENTER_CRITICAL(&s_lock);
    unsigned ms = s_local_ms;
    s_local_ms = 0;
    portEXIT_CRITICAL(&s_lock);
    return ms;
}

bool muse_benchmark_local_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    bool stop = s_stop_local;
    portEXIT_CRITICAL(&s_lock);
    return stop;
}

static void emit(const char *format, ...)
{
    int n = snprintf(s_line, sizeof(s_line), "@bench {\"v\":1,\"boot\":%"PRIu32",\"seq\":%"PRIu32",", s_boot, ++s_seq);
    va_list args;
    va_start(args, format);
    int added = vsnprintf(s_line + n, sizeof(s_line) - n, format, args);
    va_end(args);
    if (added < 0 || (size_t)(n + added + 3) > sizeof(s_line)) { s_tx_dropped++; return; }
    n += added;
    memcpy(s_line + n, "}\n", 3);
    if (!muse_console_write(s_line, n + 2)) s_tx_dropped++;
}

static void logger(void *arg)
{
    (void)arg;
    configRUN_TIME_COUNTER_TYPE idle[portNUM_PROCESSORS] = {0};
    int64_t previous = esp_timer_get_time();
    bool cpu_ready = false;
    uint32_t window = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time(), elapsed = now - previous;
        previous = now;
        window++;
        portENTER_CRITICAL(&s_lock);
        bool info = s_info;
        s_info = false;
        window_t snapshot = s_window;
        memset(&s_window, 0, sizeof(s_window));
        unsigned dropped = s_event_dropped;
        portEXIT_CRITICAL(&s_lock);
        if (info) {
            const esp_app_desc_t *app = esp_app_get_description();
            char sha[65];
            for (int i = 0; i < 32; i++) snprintf(sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);
            emit("\"type\":\"meta\",\"us\":%lld,\"board\":\"%s\",\"elf_sha256\":\"%s\",\"frame_us\":%d,\"audio_rate\":16000,\"volume\":%d,\"brightness\":%d,\"speaker\":%s,\"event_drop_base\":%u,\"tx_drop_base\":%"PRIu32",\"workload\":\"main_silent_captions_and_local_mp3\"",
                 (long long)now, muse_board->name, sha, muse_board->frame_ms * 1000,
                 muse_settings_volume(), muse_settings_brightness(), muse_settings_speaker_on() ? "true" : "false", dropped, s_tx_dropped);
        }
        for (;;) {
            portENTER_CRITICAL(&s_lock);
            bool have = s_count != 0;
            event_t ev = have ? s_events[s_head] : (event_t){0};
            if (have) { s_head = (s_head + 1) % 32; s_count--; }
            portEXIT_CRITICAL(&s_lock);
            if (!have) break;
            emit("\"type\":\"event\",\"us\":%lld,\"turn\":%"PRIu32",\"event\":\"%s\",\"value\":%lld",
                 (long long)ev.us, ev.turn, ev.name, (long long)ev.value);
        }
        int cpu[2] = {-1, -1};
        for (int i = 0; i < portNUM_PROCESSORS; i++) {
            TaskStatus_t task;
            vTaskGetInfo(xTaskGetIdleTaskHandleForCore(i), &task, pdFALSE, eInvalid);
            configRUN_TIME_COUNTER_TYPE delta = task.ulRunTimeCounter - idle[i];
            idle[i] = task.ulRunTimeCounter;
            if (cpu_ready && elapsed > 0) {
                int64_t busy = 1000 - (int64_t)((uint64_t)delta * 1000 / elapsed);
                cpu[i] = busy < 0 ? 0 : busy > 1000 ? 1000 : busy;
            }
        }
        cpu_ready = true;
        emit("\"type\":\"sample\",\"us\":%lld,\"window\":%"PRIu32",\"duration_us\":%lld,\"mode_at_end\":%d,\"cpu_permille\":[%d,%d],\"heap_free\":%u,\"heap_min\":%u,\"heap_largest\":%u,\"psram_free\":%u,\"event_dropped\":%u,\"tx_dropped\":%"PRIu32",\"late\":[%u,%u],\"missed_slots\":[%u,%u],\"dirty_px\":[%u,%u],\"display_px\":%u,\"display_errors\":%u,\"audio_errors\":%u,\"audio_frames\":%u,\"padding_frames\":%u,\"buffer_capacity\":%u",
             (long long)now, window, (long long)elapsed, muse_state_mode(NULL), cpu[0], cpu[1],
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             dropped, s_tx_dropped, snapshot.late[0], snapshot.late[1], snapshot.missed[0], snapshot.missed[1],
             snapshot.dirty_px[0], snapshot.dirty_px[1], snapshot.display_px, snapshot.display_errors,
             snapshot.audio_errors, snapshot.audio_frames, snapshot.padding_frames, snapshot.buffer_capacity);
        for (int i = 0; i < N_STATS; i++) {
            muse_bench_stat_t *st = &snapshot.stat[i];
            if (!st->n) continue;
            char hist[360];
            int used = 0;
            for (int j = 0; j < 32; j++) used += snprintf(hist + used, sizeof(hist) - used, "%s%"PRIu32, j ? "," : "", st->hist[j]);
            emit("\"type\":\"metric\",\"window\":%"PRIu32",\"name\":\"%s\",\"n\":%"PRIu32",\"sum\":%"PRIu64",\"min\":%"PRIu32",\"max\":%"PRIu32",\"hist\":[%s]",
                 window, NAMES[i], st->n, st->sum, st->min, st->max, hist);
        }
    }
}

void muse_benchmark_start(void)
{
    s_boot = esp_random();
    if (xTaskCreatePinnedToCoreWithCaps(logger, "muse_bench", 6144, NULL, 1, NULL, 0, MUSE_BIG_CAPS) != pdPASS) {
        /* Deliberately fail benchmark startup rather than silently collect an incomplete run. */
        abort();
    }
}
