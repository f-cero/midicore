/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 midicore contributors
 */
/*
 * smf.c -- Standard MIDI File parser.
 *
 * Events are converted to absolute sample frames at parse time, so the
 * sequencer never performs tempo arithmetic on the audio thread. Format 0/1/2,
 * running status, meta and sysex events are all handled; meta/sysex are
 * consumed and discarded except for tempo.
 */
#include "mc_internal.h"
#include <stdlib.h>
#include <string.h>

#define SMF_MAX_EVENTS 300000
#define SMF_MAX_TEMPOS 8192

typedef struct { uint32_t tick; uint32_t usq; } Tempo;

static uint32_t rd32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rdv(const uint8_t** pp, const uint8_t* end) {
  uint32_t v = 0;
  const uint8_t* p = *pp;
  while (p < end) {
    const uint8_t c = *p++;
    v = (v << 7) | (c & 0x7Fu);
    if (!(c & 0x80u)) break;
  }
  *pp = p;
  return v;
}

/* note-offs must precede note-ons at the same frame, or a repeated note
 * kills itself immediately */
static int ev_cmp(const void* x, const void* y) {
  const struct mc_song_event* a = (const struct mc_song_event*)x;
  const struct mc_song_event* b = (const struct mc_song_event*)y;
  if (a->frame != b->frame) return a->frame < b->frame ? -1 : 1;
  const int ao = ((a->type & 0xF0) == 0x80 || ((a->type & 0xF0) == 0x90 && a->b == 0));
  const int bo = ((b->type & 0xF0) == 0x80 || ((b->type & 0xF0) == 0x90 && b->b == 0));
  return bo - ao;
}
static int tempo_cmp(const void* x, const void* y) {
  const Tempo* a = (const Tempo*)x;
  const Tempo* b = (const Tempo*)y;
  return a->tick < b->tick ? -1 : (a->tick > b->tick ? 1 : 0);
}

void mc_song_free(mc_song* g) {
  if (!g) return;
  free(g->ev);
  free(g);
}


/* Pre-pass: walk every track and count real channel events so the event table
 * can be sized exactly. Allocation becomes O(events in the song) instead of a
 * fixed 300k-entry (~2.4 MB) table that no console could ever afford. */
static uint32_t count_events(const uint8_t* p, const uint8_t* end, uint32_t limit)
{
  uint32_t count = 0;
  while (p + 8 <= end && count < limit) {
    if (memcmp(p, "MTrk", 4) != 0) break;
    const uint32_t tlen = rd32(p + 4);
    const uint8_t* q = p + 8;
    const uint8_t* qend = q + tlen;
    if (qend > end) qend = end;
    p = qend;
    uint8_t status = 0;
    while (q < qend && count < limit) {
      rdv(&q, qend);
      if (q >= qend) break;
      uint8_t c = *q;
      if (c & 0x80) { status = c; q++; }
      else if (!status) { q++; continue; }
      if (status == 0xFF) {
        q++;
        if (q >= qend) break;
        const uint32_t ml = rdv(&q, qend);
        if (q + ml > qend) break;
        q += ml;
        continue;
      }
      if (status == 0xF0 || status == 0xF7) {
        const uint32_t ml = rdv(&q, qend);
        if (q + ml > qend) break;
        q += ml;
        continue;
      }
      const uint8_t hi = status & 0xF0;
      const int na = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
      if (q + na > qend) break;
      q += na;
      count++;
    }
  }
  return count;
}

mc_song* mc_song_load(const void* data, size_t bytes, int sampleRate) {
  if (!data || bytes < 14 || sampleRate <= 0) return NULL;
  const uint8_t* b = (const uint8_t*)data;
  const uint8_t* end = b + bytes;
  if (memcmp(b, "MThd", 4) != 0) return NULL;

  const uint32_t hlen = rd32(b + 4);
  if (hlen < 6 || (size_t)(8 + hlen) > bytes) return NULL;
  const uint16_t ntracks = rd16(b + 10);
  const uint16_t div = rd16(b + 12);
  if (div == 0) return NULL;

  int smpte = 0, fps = 25, tpf = 40;
  uint16_t ppqn = div;
  if (div & 0x8000) {
    smpte = 1;
    fps = 256 - (div >> 8);
    tpf = div & 0xFF;
    ppqn = 0;
    if (tpf == 0) return NULL;
  }

  mc_song* g = (mc_song*)calloc(1, sizeof(mc_song));
  Tempo* tp = (Tempo*)calloc(SMF_MAX_TEMPOS, sizeof(Tempo));
  if (!g || !tp) { free(g); free(tp); return NULL; }
  const uint32_t expected = count_events(b + 8 + hlen, end, SMF_MAX_EVENTS);
  const uint32_t evCap = expected + 64;   /* guard: never trust the pre-pass blindly */
  g->ev = (struct mc_song_event*)calloc(evCap, sizeof(struct mc_song_event));
  if (!g->ev) { free(g->ev); free(g); free(tp); return NULL; }

  uint32_t ntempo = 0;
  uint32_t count = 0;
  const uint8_t* p = b + 8 + hlen;

  for (uint16_t t = 0; t < ntracks && p + 8 <= end; t++) {
    if (memcmp(p, "MTrk", 4) != 0) break;
    const uint32_t tlen = rd32(p + 4);
    const uint8_t* q = p + 8;
    const uint8_t* qend = q + tlen;
    if (qend > end) qend = end;
    p = qend;

    uint32_t tick = 0;
    uint8_t status = 0;

    while (q < qend && count < evCap) {
      tick += rdv(&q, qend);
      if (q >= qend) break;
      uint8_t c = *q;
      if (c & 0x80) { status = c; q++; }
      else if (!status) { q++; continue; }

      if (status == 0xFF) {
        const uint8_t mt = *q++;
        const uint32_t ml = rdv(&q, qend);
        if (q + ml > qend) break;
        if (mt == 0x51 && ml >= 3 && ntempo < SMF_MAX_TEMPOS) {
          tp[ntempo].tick = tick;
          tp[ntempo].usq = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
          ntempo++;
        }
        q += ml;
        continue;
      }
      if (status == 0xF0 || status == 0xF7) {
        const uint32_t ml = rdv(&q, qend);
        q += ml;
        continue;
      }

      const uint8_t hi = status & 0xF0;
      const int na = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
      if (q + na > qend) break;
      const uint8_t a = q[0];
      const uint8_t b2 = (na > 1) ? q[1] : 0;
      q += na;

      g->ev[count].frame = tick;   /* ticks for now */
      g->ev[count].type  = status;
      g->ev[count].a     = a;
      g->ev[count].b     = b2;
      count++;
    }
  }

  if (count == 0) { free(g->ev); free(g); free(tp); return NULL; }

  qsort(g->ev, count, sizeof(struct mc_song_event), ev_cmp);

  /* ---- ticks -> frames */
  if (smpte) {
    const double fr = (double)sampleRate / ((double)fps * (double)tpf);
    for (uint32_t i = 0; i < count; i++)
      g->ev[i].frame = (uint32_t)((double)g->ev[i].frame * fr);
  } else {
    if (ntempo == 0) { tp[0].tick = 0; tp[0].usq = 500000; ntempo = 1; }
    qsort(tp, ntempo, sizeof(Tempo), tempo_cmp);
    if (tp[0].tick > 0) {
      for (uint32_t i = ntempo; i > 0; i--) tp[i] = tp[i - 1];
      tp[0].tick = 0;
      tp[0].usq = 500000;
      ntempo++;
    }
    uint32_t seg = 0;
    double acc = 0.0;
    const double perTick = (double)sampleRate / (double)ppqn / 1.0e6;
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t tk = g->ev[i].frame;
      while (seg + 1 < ntempo && tp[seg + 1].tick <= tk) {
        acc += (double)(tp[seg + 1].tick - tp[seg].tick) * (double)tp[seg].usq * perTick;
        seg++;
      }
      const double f = acc + (double)(tk - tp[seg].tick) * (double)tp[seg].usq * perTick;
      g->ev[i].frame = (uint32_t)f;
    }
  }

  g->count  = count;
  g->tracks = (int)ntracks;
  g->frames = g->ev[count - 1].frame;
  g->hasLoop = 0;
  g->loopStart = 0;
  g->loopEnd = g->frames;
  free(tp);
  return g;
}

uint32_t mc_song_frames(const mc_song* g) { return g ? g->frames : 0; }
int mc_song_tracks(const mc_song* g)      { return g ? g->tracks : 0; }
int mc_song_has_loop(const mc_song* g)    { return g ? g->hasLoop : 0; }
