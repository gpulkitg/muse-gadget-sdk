/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_MUSE_BENCHMARK
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#ifdef __cplusplus
extern "C" {
#endif
void muse_benchmark_start(void);
void muse_benchmark_input(bool down);
void muse_benchmark_begin(bool local);
void muse_benchmark_end(const char *event);
void muse_benchmark_event(const char *event, int64_t value);
void muse_benchmark_ui_pause(void);
void muse_benchmark_frame(int mode, int64_t at, uint32_t render_us, uint32_t total_us, uint32_t period_us);
void muse_benchmark_dirty(uint32_t pixels);
void muse_benchmark_audio(int64_t at, uint32_t elapsed_us, unsigned frames, bool ok, bool nonzero);
void muse_benchmark_padding(unsigned frames);
void muse_benchmark_buffer(unsigned frames, unsigned capacity);
void muse_benchmark_display_begin(int64_t queued, unsigned pixels);
void muse_benchmark_display_done(int64_t at);
void muse_benchmark_display_error(void);
bool muse_benchmark_command(const char *line);
unsigned muse_benchmark_take_local(void);
bool muse_benchmark_local_stop(void);
#ifdef __cplusplus
}
#endif
#endif
