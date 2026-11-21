/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 midicore contributors
 */
/*
 * mc_tables.c -- rate and waveform tables, built once per sample rate.
 *
 * Floating point appears here and nowhere else: powf/expf run a handful of
 * times during init and never inside the render loop.
 */
#include "mc_internal.h"
#include <math.h>
#include <string.h>

int32_t g_attInc[128];
int32_t g_decMul[128];
int16_t g_sine64[64];
static int g_builtFor = 0;

/*
 * Rate curve. index 127 = 2 ms, each step down doubles the time roughly every
 * 9 indices, index 0 = ~36 s. Linear in the log domain so a linear MIDI
 * controller sweep feels even.
 */
static double rate_seconds(int r) {
  return 0.002 * pow(2.0, (127.0 - (double)r) / 9.0);
}

void mc_tables_init(int sampleRate) {
  if (g_builtFor == sampleRate) return;
  g_builtFor = sampleRate;

  for (int r = 0; r < 128; r++) {
    const double secs = rate_seconds(r);
    const double samples = secs * (double)sampleRate;

    /* attack: linear ramp over the whole time, at least 1 LSB per sample */
    double inc = 65536.0 / (samples < 1.0 ? 1.0 : samples);
    if (inc < 1.0) inc = 1.0;
    if (inc > 65536.0) inc = 65536.0;
    g_attInc[r] = (int32_t)(inc + 0.5);

    /* decay/release: one-pole exponential, Q16 coefficient.
     * env -= (env * mul) >> 16 reaches 1/e after `samples`. */
    double mul = 65536.0 * (1.0 - exp(-1.0 / (samples < 1.0 ? 1.0 : samples)));
    if (mul < 1.0) mul = 1.0;
    if (mul > 65000.0) mul = 65000.0;
    g_decMul[r] = (int32_t)(mul + 0.5);
  }

  /* 64-entry sine, Q15. Used for the LFO only. */
  for (int i = 0; i < 64; i++) {
    const double v = sin(2.0 * 3.14159265358979 * (double)i / 64.0);
    g_sine64[i] = (int16_t)(v * 32767.0);
  }
}
