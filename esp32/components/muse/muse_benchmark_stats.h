/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#pragma once
#include <stdint.h>

/* Bounded, allocation-free logarithmic histogram. Bin i has upper bound 2^i
 * (last bin UINT32_MAX). Percentiles are reported as upper bounds, not exact. */
typedef struct {
    uint32_t n, min, max, hist[32];
    uint64_t sum;
} muse_bench_stat_t;

static inline __attribute__((always_inline)) void muse_bench_add(muse_bench_stat_t *s, uint32_t value)
{
    unsigned bin = 0;
    uint32_t bound = 1;
    while (value > bound && bin < 31) {
        bound <<= 1;
        bin++;
    }
    if (!s->n || value < s->min) s->min = value;
    s->n++;
    s->sum += value;
    if (value > s->max) s->max = value;
    s->hist[bin]++;
}
