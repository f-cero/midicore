/*
 * seq.c -- one sequencer, two sources, zero per-song allocation.
 *
 * FILE mode walks a Standard MIDI File in place with one cursor per track and
 * an incremental tempo map: memory is O(tracks), not O(events). Works directly
 * out of cart ROM, flash or any const blob.
 *
 * STREAM mode takes events pushed over a link, already time-stamped, and
 * schedules them into a fixed ring.
 *
 * Both feed mc_seq_render(), which splits each block at event boundaries so
 * timing is sample-accurate rather than block-quantised.
 */
#include "mc_internal.h"
#include <stdlib.h>
#include <string.h>

#define MC_SEQ_MODE_FILE   0
#define MC_SEQ_MODE_STREAM 1

static uint32_t rd32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t rdv_cur(const uint8_t** pp, const uint8_t* end) {
  uint32_t d = 0;
  const uint8_t* p = *pp;
  while (p < end) {
    const uint8_t c = *p++;
    d = (d << 7) | (uint32_t)(c & 0x7F);
    if (!(c & 0x80)) break;
    if (d > (1u << 25)) break;             /* malformed: stop runaway */
  }
  *pp = p;
  return d;
}

static uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1));
  }
  return c;
}

/* ------------------------------------------------------------ file mode */
static int tr_step(mc_seq* q, int t) {
  mc_seq_tr* T = &q->tr[t];
  if (T->done) return 0;
  while (T->p < T->end) {
    const uint32_t d = rdv_cur(&T->p, T->end);
    if (T->p >= T->end) break;
    T->tick += d;
    const uint8_t c = *T->p;
    if (c & 0x80) { T->status = c; T->p++; }
    else if (!T->status) { T->p++; continue; }

    if (T->status == 0xFF) {
      const uint8_t mt = *T->p++;
      const uint32_t ml = rdv_cur(&T->p, T->end);
      if (T->p + ml > T->end) break;
      if (mt == 0x51 && ml >= 3) {
        T->usq   = ((uint32_t)T->p[0] << 16) | ((uint32_t)T->p[1] << 8) | T->p[2];
        T->type  = 0xFF; T->a = 0; T->b = 0;
        T->ptick = T->tick; T->valid = 1; T->p += ml;
        return 1;
      }
      T->p += ml;
      continue;
    }
    if (T->status == 0xF0 || T->status == 0xF7) {
      const uint32_t ml = rdv_cur(&T->p, T->end);
      if (T->p + ml > T->end) break;
      T->p += ml;
      continue;
    }
    const uint8_t hi = T->status & 0xF0;
    const int na = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    if (T->p + na > T->end) break;
    T->type  = T->status;
    T->a     = T->p[0];
    T->b     = (na > 1) ? T->p[1] : 0;
    T->ptick = T->tick;
    T->valid = 1;
    T->p += na;
    return 1;
  }
  T->done = 1;
  T->valid = 0;
  return 0;
}

static uint32_t tick_to_frame(mc_seq* q, uint32_t tick) {
  if (q->smpte) {
    const int64_t den = (int64_t)q->fps * (int64_t)q->tpf;
    return (uint32_t)((int64_t)tick * q->sampleRate / (den ? den : 1));
  }
  if (tick > q->lastTick) {
    const uint64_t fptQ16 = ((uint64_t)q->usq * (uint64_t)q->sampleRate << 16) /
                            ((uint64_t)q->ppqn * 1000000ull);
    q->accQ16 += (int64_t)(tick - q->lastTick) * (int64_t)fptQ16;
    q->lastTick = tick;
  }
  return (uint32_t)(q->accQ16 >> 16);
}

static int seq_next_file(mc_seq* q, uint32_t* frame, uint8_t* type,
                         uint8_t* a, uint8_t* b) {
  for (;;) {
    int best = -1;
    for (int t = 0; t < q->ntracks; t++) {
      mc_seq_tr* T = &q->tr[t];
      if (!T->valid && !T->done) tr_step(q, t);
      if (T->valid && (best < 0 || T->ptick < q->tr[best].ptick)) best = t;
    }
    if (best < 0) return 0;
    mc_seq_tr* T = &q->tr[best];
    const uint32_t f = tick_to_frame(q, T->ptick);
    if (T->type == 0xFF) {
      if (T->usq) q->usq = T->usq;
      T->valid = 0;
      continue;
    }
    *frame = f; *type = T->type; *a = T->a; *b = T->b;
    T->valid = 0;
    return 1;
  }
}

mc_seq* mc_seq_open_file(const void* smf, size_t bytes, int sampleRate) {
  if (!smf || bytes < 14 || sampleRate < 8000 || sampleRate > 48000) return NULL;
  const uint8_t* b = (const uint8_t*)smf;
  if (memcmp(b, "MThd", 4) != 0) return NULL;

  const uint32_t hlen = rd32(b + 4);
  if (hlen < 6 || (size_t)8 + hlen > bytes) return NULL;
  const uint16_t fmt = (uint16_t)((b[8] << 8) | b[9]);
  const uint16_t ntr = (uint16_t)((b[10] << 8) | b[11]);
  const uint16_t div = (uint16_t)((b[12] << 8) | b[13]);
  if (div == 0 || fmt == 2 || ntr == 0 || ntr > MC_SEQ_MAXTRACKS) return NULL;

  mc_seq* q = (mc_seq*)calloc(1, sizeof(mc_seq));
  if (!q) return NULL;
  q->mode = MC_SEQ_MODE_FILE;
  q->sampleRate = sampleRate;
  q->base = b; q->bytes = bytes;
  q->ntracks = ntr;
  q->usq = 500000;
  if (div & 0x8000) {
    q->smpte = 1;
    q->fps = 256 - (div >> 8);
    q->tpf = div & 0xFF;
    if (!q->fps || !q->tpf) { free(q); return NULL; }
  } else {
    q->ppqn = div;
  }

  const uint8_t* p = b + 8 + hlen;
  const uint8_t* end = b + bytes;
  int made = 0;
  for (uint16_t t = 0; t < ntr; t++) {
    if (p + 8 > end || memcmp(p, "MTrk", 4) != 0) break;
    const uint32_t tlen = rd32(p + 4);
    const uint8_t* qs = p + 8;
    const uint8_t* qe = qs + tlen;
    if (qe > end) qe = end;
    q->tr[made].p = qs; q->tr[made].end = qe;
    q->tr[made].usq = 0;
    made++;
    p = qe;
  }
  if (!made) { free(q); return NULL; }
  q->ntracks = made;
  return q;
}

/* ---------------------------------------------------------- stream mode */
mc_seq* mc_seq_open_stream(int sampleRate, int ppqn) {
  if (sampleRate < 8000 || sampleRate > 48000) return NULL;
  if (ppqn <= 0) ppqn = 480;
  mc_seq* q = (mc_seq*)calloc(1, sizeof(mc_seq));
  if (!q) return NULL;
  q->mode = MC_SEQ_MODE_STREAM;
  q->sampleRate = sampleRate;
  q->ppqn = ppqn;
  return q;
}

static uint32_t ms_to_frames(const mc_seq* q, uint32_t ms) {
  return (uint32_t)(((uint64_t)ms * (uint64_t)q->sampleRate) / 1000u);
}

int mc_seq_push(mc_seq* q, const uint8_t* ev, int count, uint16_t dur_ms) {
  if (!q || q->mode != MC_SEQ_MODE_STREAM) return MC_ERR_ARG;
  if (count < 0 || (count && !ev)) return MC_ERR_ARG;
  if (q->count + count > MC_SEQ_RING) return -1;

  uint32_t at = q->schedAt ? q->schedAt : q->now;
  uint32_t acc = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t* e = ev + i * 4;
    acc += ms_to_frames(q, e[0]);
    mc_seq_ev* r = &q->ring[q->tail];
    r->at = at + acc;
    r->type = e[1]; r->a = e[2]; r->b = e[3];
    q->tail = (q->tail + 1) % MC_SEQ_RING;
    q->count++;
  }
  q->schedAt = at + ms_to_frames(q, dur_ms);
  return MC_OK;
}

int mc_seq_push_frame(mc_seq* q, const uint8_t* f, size_t bytes) {
  if (!q || !f || bytes < 6) return MC_ERR_ARG;
  if (f[0] != 0xA5) return MC_ERR_MAGIC;
  const uint16_t len = (uint16_t)((f[2] << 8) | f[3]);
  if (bytes < (size_t)len + 6) return MC_ERR_TRUNCATED;
  if (crc8(f, (size_t)len + 4) != f[len + 4]) return MC_ERR_CRC;
  const uint8_t* pl = f + 4;
  switch (f[1]) {
    case 0x01: {                        /* CHUNK: seq,dur,n, then 4B events */
      if (len < 6) return MC_ERR_TRUNCATED;
      const uint16_t dur = (uint16_t)((pl[2] << 8) | pl[3]);
      const uint16_t n   = (uint16_t)((pl[4] << 8) | pl[5]);
      if (len < 6u + (size_t)n * 4u) return MC_ERR_TRUNCATED;
      return mc_seq_push(q, pl + 6, (int)n, dur);
    }
    case 0x02: {                        /* IMMEDIATE: play at once */
      if (len < 4) return MC_ERR_TRUNCATED;
      const uint16_t n = (uint16_t)(len / 4);
      for (uint16_t i = 0; i < n; i++) {
        if (q->count >= MC_SEQ_RING) return -1;
        mc_seq_ev* r = &q->ring[q->tail];
        r->at = q->now;
        r->type = pl[i * 4 + 1]; r->a = pl[i * 4 + 2]; r->b = pl[i * 4 + 3];
        q->tail = (q->tail + 1) % MC_SEQ_RING;
        q->count++;
      }
      return MC_OK;
    }
    case 0x03:                          /* FLUSH */
      mc_seq_flush(q);
      return MC_OK;
    default:
      return MC_OK;                     /* snapshot/telemetry: ignored here */
  }
}

static int seq_next_stream(mc_seq* q, uint32_t* frame, uint8_t* type,
                           uint8_t* a, uint8_t* b) {
  if (q->count <= 0) return 0;
  mc_seq_ev* r = &q->ring[q->head];
  if (r->at > q->now) return 0;
  *frame = r->at; *type = r->type; *a = r->a; *b = r->b;
  q->head = (q->head + 1) % MC_SEQ_RING;
  q->count--;
  return 1;
}

int mc_seq_pending(const mc_seq* q) {
  if (!q || q->mode != MC_SEQ_MODE_STREAM || q->count <= 0) return 0;
  const int last = (q->head + q->count - 1) % MC_SEQ_RING;
  const int32_t d = (int32_t)(q->ring[last].at - q->now);
  return d > 0 ? (int)(((int64_t)d * 1000) / q->sampleRate) : 0;
}

void mc_seq_flush(mc_seq* q) {
  if (!q) return;
  q->head = q->tail = q->count = 0;
  q->schedAt = 0;
}

void mc_seq_free(mc_seq* q) { free(q); }

uint32_t mc_seq_position(const mc_seq* q) { return q ? q->now : 0; }

/* -------------------------------------------------------------- dispatch */
static void apply_event(mc_synth* s, uint8_t st, uint8_t a, uint8_t b) {
  const int ch = st & 0x0F;
  switch (st & 0xF0) {
    case 0x90: if (b) mc_note_on(s, ch, a, b); else mc_note_off(s, ch, a); break;
    case 0x80: mc_note_off(s, ch, a); break;
    case 0xB0: mc_control_change(s, ch, a, b); break;
    case 0xC0: mc_program_change(s, ch, a); break;
    case 0xD0: mc_channel_pressure(s, ch, a); break;
    case 0xE0: mc_pitch_bend(s, ch, ((int)b << 7 | a) - 8192); break;
    default: break;
  }
}

void mc_seq_render(mc_synth* s, mc_seq* q, int16_t* out, int frames, int* done) {
  if (done) *done = 0;
  if (!s || !q || !out || frames <= 0) return;

  while (frames > 0) {
    if (!q->pendValid) {
      const int have = (q->mode == MC_SEQ_MODE_FILE)
        ? seq_next_file(q, &q->pendFrame, &q->pendType, &q->pendA, &q->pendB)
        : seq_next_stream(q, &q->pendFrame, &q->pendType, &q->pendA, &q->pendB);
      if (!have) {
        if (q->mode == MC_SEQ_MODE_STREAM && q->count > 0) {
          /* event queued but not due yet: this is silence, not the end */
          mc_render(s, out, frames); q->now += (uint32_t)frames; break;
        }
        if (done) *done = 1;
        break;
      }
      q->pendValid = 1;
    }
    int off = (int)(q->pendFrame - q->now);
    if (off < 0) off = 0;
    if (off > frames) off = frames;
    if (off == 0) {
      apply_event(s, q->pendType, q->pendA, q->pendB);
      q->pendValid = 0;
      continue;
    }
    const int n = off > MC_BLOCK_MAX ? MC_BLOCK_MAX : off;
    mc_render(s, out, n);
    out += n * 2;
    frames -= n;
    q->now += (uint32_t)n;
  }
}
