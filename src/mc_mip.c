/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 midicore contributors
 */
/*
 * mc_mip.c -- band-limited mip levels derived at runtime.
 *
 * A single 256-point cycle carries up to 96 harmonics. Played at MIDI 96 that
 * puts harmonic 8 and above past Nyquist, and they fold back as inharmonic
 * garbage -- measured at 85 % of total energy for a bright instrument.
 *
 * Rather than store extra tables in the bank, this derives them from the one
 * stored cycle using a DFT at init: keep the first N harmonics, drop the rest,
 * resynthesise. The result is band-limited, and costs flash: nothing.
 *
 * Cost is RAM. mc_build_mips() fills a caller-supplied buffer, one 512-byte
 * level per (table, level) pair, in table order, and stops when the buffer is
 * full. A table that did not fit keeps only the levels it got, so the engine
 * degrades instead of failing.
 */
#include "mc_internal.h"
#include <math.h>
#include <string.h>

/* Harmonic ceiling per level. Level 0 is the stored table (96 harmonics).
 * 32 / 8 / 2 cover notes at roughly MIDI 84 / 96 / 108 and above. */
const uint8_t mc_mip_cap[MC_MIP_LEVELS] = { 96, 32, 8, 2 };

/* Naive DFT resynthesis of one 256-point cycle with a harmonic ceiling. */
static void resynth(const int16_t* src, int16_t* dst, int cap) {
  const int N = MC_TABLE_LEN;
  double re[MC_TABLE_LEN / 2], im[MC_TABLE_LEN / 2];
  memset(re, 0, sizeof(re));
  memset(im, 0, sizeof(im));

  for (int k = 1; k <= cap && k < N / 2; k++) {
    double sr = 0.0, si = 0.0;
    for (int n = 0; n < N; n++) {
      const double a = 2.0 * 3.14159265358979 * (double)k * (double)n / (double)N;
      sr += (double)src[n] * cos(a);
      si -= (double)src[n] * sin(a);
    }
    re[k] = sr * 2.0 / N;
    im[k] = si * 2.0 / N;
  }

  double peak = 0.0;
  double tmp[MC_TABLE_LEN];
  for (int n = 0; n < N; n++) {
    double v = 0.0;
    for (int k = 1; k <= cap && k < N / 2; k++) {
      const double a = 2.0 * 3.14159265358979 * (double)k * (double)n / (double)N;
      v += re[k] * cos(a) - im[k] * sin(a);
    }
    tmp[n] = v;
    const double ab = fabs(v);
    if (ab > peak) peak = ab;
  }
  const double g = (peak > 1e-9) ? (32000.0 / peak) : 0.0;
  for (int n = 0; n < N; n++) {
    double v = tmp[n] * g;
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    dst[n] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
  }
}

int mc_build_mips(mc_synth* s, void* buf, size_t bytes) {
  if (!s || !s->hasBank) return 0;
  const int nT = (int)s->bank.numTables;
  if (nT <= 0 || nT > MC_MAX_TABLES) return 0;

  int16_t* slot = (int16_t*)buf;
  const size_t need = (size_t)MC_TABLE_LEN * sizeof(int16_t);
  int derived = 0;

  for (int t = 0; t < nT; t++) {
    s->mip[t][0] = s->bank.tables + (size_t)t * MC_TABLE_LEN;
    s->mipLevels[t] = 1;
    for (int L = 1; L < MC_MIP_LEVELS; L++) {
      if ((size_t)derived * need + need > bytes) break;
      resynth(s->bank.tables + (size_t)t * MC_TABLE_LEN, slot, mc_mip_cap[L]);
      s->mip[t][L] = slot;
      slot += MC_TABLE_LEN;
      derived++;
      s->mipLevels[t] = (uint8_t)(L + 1);
    }
    if ((size_t)derived * need + need > bytes) {
      /* buffer full: remaining tables use the stored cycle only */
      for (int r = t + 1; r < nT; r++) { s->mip[r][0] = s->bank.tables + (size_t)r * MC_TABLE_LEN; s->mipLevels[r] = 1; }
      break;
    }
  }
  s->mipReady = (derived > 0);
  return derived;
}

/* Finest level whose harmonic ceiling still fits under Nyquist. */
const int16_t* mc_pick_table(const mc_synth* s, int table, int64_t inc) {
  if (table < 0 || table >= MC_MAX_TABLES) return s->bank.tables;
  if (!s->mipReady || s->mipLevels[table] <= 1) return s->mip[table][0];

  /* f0 in Hz from the phase increment: inc is Q32 cycles per sample */
  const double f0 = (double)inc * (double)s->sampleRate / 4294967296.0;
  if (f0 <= 1.0) return s->mip[table][0];
  const double allowed = ((double)s->sampleRate * 0.5) / f0;

  for (int L = 1; L < s->mipLevels[table]; L++)
    if ((double)mc_mip_cap[L] <= allowed) return s->mip[table][L];

  return s->mip[table][s->mipLevels[table] - 1];
}
