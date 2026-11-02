/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Normalized, interruptible speaking choreography. Pure C, fixed storage, no
 * clock/random APIs or allocations; the renderer supplies time and audio level.
 * A rest is a real state, not an invitation to fill every frame with motion. */
enum { MUSE_SPEAK_REST, MUSE_SPEAK_EXPLAIN, MUSE_SPEAK_PRESENT };
typedef struct {
    bool active, heard_voice;
    unsigned action, next, side, leader, points;
    uint32_t seed;
    float time, duration, mode_t, quiet, energy;
    float lift[2], extend[2];
} muse_speaking_t;

static inline float muse_speak_clamp(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static inline float muse_speak_ease(float x)
{
    x = muse_speak_clamp(x, 0, 1);
    return x * x * (3 - 2 * x);
}

static inline float muse_speak_random(muse_speaking_t *s)
{
    s->seed = s->seed * 1664525u + 1013904223u;
    return (s->seed >> 16) / 65535.0f;
}

static inline void muse_speaking_update(muse_speaking_t *s, bool speaking, float dt, float mode_t, float level)
{
    dt = muse_speak_clamp(dt, 0, .2f);
    level = muse_speak_clamp(level, 0, 1);
    if (speaking && (!s->active || mode_t < s->mode_t)) {
        s->active = true;
        s->heard_voice = false;
        s->quiet = 0;
        s->action = MUSE_SPEAK_REST;
        s->next = MUSE_SPEAK_EXPLAIN;
        s->time = 0;
        s->duration = .12f;
        s->leader = muse_speak_random(s) > .5f;
        s->points = 0;
        s->mode_t = mode_t;
    }
    /* mode_t does not advance during preview warm-up. Real state changes
     * start from the current pose even if the first visible frame arrives late. */
    float advance = speaking ? muse_speak_clamp(mode_t - s->mode_t, 0, .2f) : dt;
    s->mode_t = mode_t;
    s->energy += ((speaking ? level : 0) - s->energy) * muse_speak_clamp(dt * 18, 0, 1);
    if (level > .06f && speaking) { s->heard_voice = true; s->quiet = 0; }
    else s->quiet += advance;
    bool pause = speaking && s->heard_voice && s->quiet > .28f;
    if (!speaking || pause) {
        s->action = MUSE_SPEAK_REST;
        s->time = 0;
        s->duration = .18f;
        if (!speaking) s->active = false;
    } else {
        s->time += advance;
        if (s->time >= s->duration) {
            s->time = 0;
            if (s->action == MUSE_SPEAK_REST) {
                s->action = s->next;
                s->side = s->action == MUSE_SPEAK_EXPLAIN ? s->leader : (s->leader ^ (s->points == 0));
                s->duration = s->action == MUSE_SPEAK_EXPLAIN ? 1.55f + .20f * muse_speak_random(s) : 1.12f;
            } else {
                unsigned finished = s->action;
                s->action = MUSE_SPEAK_REST;
                if (finished == MUSE_SPEAK_EXPLAIN) {
                    s->next = MUSE_SPEAK_PRESENT;
                    s->points = 0;
                    s->duration = .75f + .45f * muse_speak_random(s);
                } else if (++s->points < 2) {
                    s->next = MUSE_SPEAK_PRESENT;
                    s->duration = .35f + .25f * muse_speak_random(s);
                } else {
                    s->next = MUSE_SPEAK_EXPLAIN;
                    s->leader ^= 1;
                    s->duration = 1.05f + .65f * muse_speak_random(s);
                }
            }
        }
    }
    float lift[2] = {0}, extend[2] = {0};
    if (speaking && !pause && s->action != MUSE_SPEAK_REST) {
        bool explain = s->action == MUSE_SPEAK_EXPLAIN;
        float prepare = explain ? .32f : .24f, reach = explain ? .42f : .30f;
        float settle = explain ? .42f : .32f;
        float envelope = 1 - muse_speak_ease((s->time - (s->duration - settle)) / settle);
        float amplitude = .88f + .12f * s->energy;
        lift[s->side] = muse_speak_ease(s->time / prepare) * envelope * amplitude;
        extend[s->side] = muse_speak_ease((s->time - prepare * .65f) / reach) * envelope * amplitude;
        if (explain) lift[s->side ^ 1] = lift[s->side] * .16f;
    }
    /* Critically bounded travel, including interrupted preparations and holds.
     * No final gesture is queued when speech ends, and a second hand is optional. */
    float blend = muse_speak_clamp(dt * 14, 0, 1);
    for (int i = 0; i < 2; i++) {
        s->lift[i] += (lift[i] - s->lift[i]) * blend;
        s->extend[i] += (extend[i] - s->extend[i]) * blend;
        if (s->lift[i] < .001f) s->lift[i] = 0;
        if (s->extend[i] < .001f) s->extend[i] = 0;
    }
}
