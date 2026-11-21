/*
 * synth.c -- the midicore voice engine.
 *
 * One voice = up to four operators. Each operator is a 256-point single-cycle
 * wavetable read with linear interpolation, an ADSR envelope in Q16, and a
 * phase increment computed once at note-on. Operators are combined by one of
 * four algorithms (additive, 2-op FM, 4-op stack, twin pair).
 *
 * Everything is integer. The per-sample inner loop touches no memory beyond
 * the voice struct, the wavetable and the two mix accumulators, so a voice
 * render is branch-predictable and register-friendly.
 */
#include "mc_internal.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ util */
static inline int32_t clamp16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return v;
}

/* FM phase offset in Q32. (m*level)>>5 can reach ~130k, and shifting that by
 * 16 exceeds int32, so the clamp happens before the cast: level 127 is capped
 * at ~2 cycles of modulation depth instead of wrapping to an arbitrary value. */
static inline uint32_t fm_off(int32_t m, int level) {
  int64_t v = ((int64_t)m * (int64_t)level) >> 5;
  v <<= 16;
  if (v >  0x7FFFFFFFLL) v =  0x7FFFFFFFLL;
  if (v < -0x7FFFFFFFLL) v = -0x7FFFFFFFLL;
  return (uint32_t)(int32_t)v;
}

static inline int32_t tab_read(const int16_t* t, uint32_t phase) {
  const uint32_t idx  = phase >> 24;
  const int32_t  a    = t[idx];
  const int32_t  b    = t[(idx + 1) & (MC_TABLE_LEN - 1)];
  const int32_t  fr   = (int32_t)((phase >> 12) & 0xFFF);
  return a + (((b - a) * fr) >> 12);
}

/* MIDI note -> phase increment, Q32 of one cycle per sample. */
static inline uint32_t note_inc(int note, int sampleRate) {
  /* 2^(semis/12) in Q30 for the twelve residues. Computed in stages so no
   * intermediate exceeds 64 bits: 440*2^30 fits, then shift by octaves. */
  static const int32_t kSemis[12] = {
    1073741824, 1137768624, 1205531264, 1277327216, 1353392128,
    1433991936, 1519390848, 1609850368, 1705670656, 1807225856,
    1914836992, 2028844032
  };
  int semis = note - 69;
  int oct = semis / 12;
  int rem = semis % 12;
  if (rem < 0) { rem += 12; oct -= 1; }

  /* Q10 Hz keeps 10 fractional bits. Truncating to whole Hz puts a 30.9 Hz
   * note at 30 Hz -- 49 cents flat, and audible on a bass line. */
  int64_t v = ((int64_t)440 * (int64_t)kSemis[rem]) >> 20;   /* Q10 Hz */
  if (oct > 0) v <<= oct; else v >>= -oct;
  if (v <= 0) return 1;
  /* Q10 Hz -> Q32 phase: v / 1024 * 2^32 / sampleRate */
  return (uint32_t)((v << 22) / (int64_t)sampleRate);
}

/* ------------------------------------------------------------------ voice */
static mc_voice* alloc_voice_pool(mc_synth* s, int pool, int prio) {
  int lo = 0, hi = s->maxVoices;
  if (s->sfxCount > 0) {
    if (pool == 1) { lo = s->sfxStart; hi = s->sfxStart + s->sfxCount; }
    else           { lo = 0;           hi = s->musicVoices; }
  }
  if (hi > s->maxVoices) hi = s->maxVoices;
  if (lo >= hi) { lo = 0; hi = s->maxVoices; }

  mc_voice* idle = NULL;
  mc_voice* rel  = NULL;
  for (int i = lo; i < hi; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active) { if (!idle || v->age < idle->age) idle = v; }
    else if (v->released) { if (!rel || v->age < rel->age) rel = v; }
  }
  if (idle) return idle;
  if (rel)  return rel;

  mc_voice* o = NULL;
  for (int i = lo; i < hi; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active) continue;
    if (!o) { o = v; continue; }
    switch (s->stealPolicy) {
      case MC_STEAL_QUIETEST:
        if (v->velGain < o->velGain) { o = v; }
        break;
      case MC_STEAL_LOWPRI:
        if (v->prio < o->prio || (v->prio == o->prio && v->age < o->age)) { o = v; }
        break;
      default:
        if (v->age < o->age) { o = v; }
        break;
    }
  }
  (void)prio;
  return o ? o : &s->voices[lo];
}

/* Recompute the per-operator increments, e.g. after a pitch bend. */
static void recompute_incs(mc_synth* s, mc_voice* v) {
  const mc_patch* p = v->patch;
  for (int i = 0; i < v->opCount; i++) {
    const mc_op* o = &p->op[i];
    int mulx2 = (o->mul == 0) ? 1 : (int)o->mul * 2;   /* 0.5x .. 15x */
    int note = v->note + (int)((o->detune * 100) / 1600);  /* 1/16 semitone */
    int64_t inc = note_inc(note, s->sampleRate);
    inc = (inc * mulx2) >> 1;
    inc = (inc * (int64_t)s->bend[v->ch]) >> 16;
    v->op[i].inc = (uint32_t)inc;
    v->op[i].vibScale = (uint32_t)(((int64_t)v->op[i].inc * p->lfoVib) >> 15);
    v->op[i].tbl = mc_pick_table(s, o->table, (int64_t)v->op[i].inc);
  }
  if (p->pitchEnvAmt) {
    /* a short one-shot pitch sweep at note-on, depth in semitones */
    v->pitchEnv = (int32_t)p->pitchEnvAmt * 65536;
    const int32_t t = g_attInc[p->pitchEnvRate] > 0 ? g_attInc[p->pitchEnvRate] : 1;
    v->pitchEnvStep = (int32_t)(((int64_t)65536 * 65536 / 2) / (t * 64 + 1));
    if (v->pitchEnvStep < 1) v->pitchEnvStep = 1;
  } else {
    v->pitchEnv = 0;
    v->pitchEnvStep = 0;
  }
}

/* ---------------------------------------------------------------- note in */
static void note_on_impl(mc_synth* s, int ch, int note, int vel, int pool, int prio) {
  if (!s || !s->hasBank) return;
  if (ch < 0 || ch > 15 || note < 0 || note > 127) return;
  if (vel <= 0) { mc_note_off(s, ch, note); return; }
  if (vel > 127) vel = 127;

  int program = s->prog[ch];
  const mc_patch* p = s->drumCh[ch] ? mc_bank_drum(&s->bank, note)
                                    : mc_bank_patch(&s->bank, program);
  if (!p) return;

  mc_voice* v = alloc_voice_pool(s, pool, prio);
  if (!v) return;
  memset(v, 0, sizeof(*v));
  v->active   = 1;
  v->pool     = (uint8_t)pool;
  v->prio     = (uint8_t)prio;
  v->handle   = (int16_t)(++s->sfxNextHandle);
  v->ch       = (uint8_t)ch;
  v->note     = (uint8_t)note;
  v->vel      = (uint8_t)vel;
  v->patch    = p;
  v->age      = ++s->voicedSamples;   /* monotonic, doubles as an id */
  v->opCount  = p->opCount ? p->opCount : 1;
  if (v->opCount > MC_MAX_OPS) v->opCount = MC_MAX_OPS;

  /* velocity curve: quadratic in velocity, scaled by the patch's depth.
   * depth 0 means the patch ignores velocity entirely (organs, pads). */
  const int32_t vcurve = (int32_t)(((int64_t)vel * vel * 65536) / (127 * 127));
  const int32_t depth  = (int32_t)p->velDepth * 65536 / 127;
  v->velGain = 65536 - (int32_t)(((int64_t)(65536 - vcurve) * depth) >> 16);
  if (v->velGain < 0) v->velGain = 0;
  v->panEff = (int8_t)(p->pan + s->pan[ch]);
  if (v->panEff < -64) v->panEff = -64;
  if (v->panEff >  63) v->panEff =  63;

  for (int i = 0; i < v->opCount; i++) {
    const mc_op* o = &p->op[i];
    mc_opv* ov = &v->op[i];
    int32_t sus = (int32_t)o->sustain * 65536 / 127;
    ov->sus = sus;
    ov->env = 0;
    ov->stage = 0;
    ov->relIdx = o->release;
    ov->inc = 0;
  }
  recompute_incs(s, v);
  v->lfoInc = (uint32_t)((int64_t)(1 << 28) * (int64_t)(2 + p->lfoRate) / (int64_t)s->sampleRate);
  v->lfoDelay = (uint16_t)((uint32_t)p->lfoDelay * 4u * (uint32_t)s->sampleRate / 1000u);
  v->vibActive = (p->lfoMode & MC_LFO_VIB) ? 1 : 0;
}

void mc_note_off(mc_synth* s, int ch, int note) {
  if (!s) return;
  (void)ch;
  for (int i = 0; i < s->maxVoices; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active || v->released || v->note != note) continue;
    if (v->pool == 1) continue;          /* one-shots die on their own */
    v->released = 1;
    for (int k = 0; k < v->opCount; k++) v->op[k].stage = 3;
  }
}

void mc_all_notes_off(mc_synth* s, int hard) {
  if (!s) return;
  for (int i = 0; i < s->maxVoices; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active) continue;
    if (hard) { v->active = 0; continue; }
    v->released = 1;
    for (int k = 0; k < v->opCount; k++) v->op[k].stage = 3;
  }
}

/* -------------------------------------------------------------- rendering */
/* Pitch-envelope offset, in Q32 phase units per sample of the carrier. */
static inline uint32_t pitch_env_off(mc_synth* s, mc_voice* v) {
  if (v->pitchEnv <= 0) return 0;
  /* semitones Q16 -> ratio approximation: 1 + semis*ln2/12 */
  int32_t semiQ16 = v->pitchEnv;
  int64_t off = ((int64_t)v->op[0].inc * semiQ16) / 65536;
  off = (off * 5943) / 100000;      /* ln(2)/12 */
  return (uint32_t)(int32_t)off;
}

/*
 * Advance one operator by one sample. Returns its enveloped output in Q15.
 * `phaseOff` is an extra phase offset (FM), `lfoS` is the current LFO value.
 */
static inline int32_t op_step(mc_synth* s, int k, int32_t lfoS, uint32_t phaseOff,
                              mc_voice* v, int* dead) {
  mc_opv* o  = &v->op[k];
  const mc_op* po = &v->patch->op[k];

  uint32_t inc = o->inc;
  if (lfoS && v->vibActive)
    inc += (uint32_t)(int32_t)(((int64_t)o->vibScale * lfoS) >> 15);

  const int16_t* t = o->tbl ? o->tbl : (s->bank.tables + (size_t)po->table * MC_TABLE_LEN);
  int32_t smp = tab_read(t, o->phase + phaseOff);
  o->phase += inc;

  int32_t e = o->env;
  switch (o->stage) {
    case 0:
      e += g_attInc[po->attack];
      if (e >= MC_ENV_ONE) { e = MC_ENV_ONE; o->stage = 1; }
      break;
    case 1: {
      e -= ((e >> 8) * g_decMul[po->decay] >> 8) + 1;
      if (e <= o->sus) { e = o->sus; o->stage = 2; }
      break;
    }
    case 2:
      break;
    default: {
      const int32_t rm = g_decMul[v->op[k].relIdx];
      e -= ((e >> 8) * rm >> 8) + 1;
      if (e <= 0) { e = 0; *dead = 1; }
      break;
    }
  }
  o->env = e;
  return (smp * (e >> 1)) >> 15;
}

static void render_voice(mc_synth* s, mc_voice* v, int frames) {
  const mc_patch* p = v->patch;
  int32_t* mixL = s->mixL;
  int32_t* mixR = s->mixR;

  /* constant pan for the whole note: patch pan plus channel pan */
  const int32_t panPad = (int32_t)v->panEff + 64;          /* 0..127 */
  const int32_t gainL  = ((128 - panPad) * p->gain) >> 7;  /* 0..127 */
  const int32_t gainR  = (panPad * p->gain) >> 7;

  for (int i = 0; i < frames; i++) {
    v->lfoPhase += v->lfoInc;
    int32_t lfoS = 0;
    if (v->vibActive) {
      if (v->lfoDelay) v->lfoDelay--;
      else lfoS = g_sine64[(v->lfoPhase >> 26) & 63];
    }
    if (v->pitchEnv > 0) {
      v->pitchEnv -= v->pitchEnvStep;
      if (v->pitchEnv < 0) v->pitchEnv = 0;
    }

    const uint32_t peOff = pitch_env_off(s, v);
    int dead = 0;
    int32_t out = 0;

    switch (p->algorithm) {
      case MC_ALG_FM2: {
        const int32_t m1 = op_step(s, 1, lfoS, 0, v, &dead);
        const int32_t md = ((m1 * p->op[1].level) >> 5);
        const int32_t o0 = op_step(s, 0, lfoS,
                                   fm_off(m1, p->op[1].level) + peOff, v, &dead);
        out = (o0 * p->op[0].out) >> 7;
        if (v->opCount > 2) {
          const int32_t o2 = op_step(s, 2, lfoS, peOff, v, &dead);
          out += (o2 * p->op[2].out) >> 7;
        }
        break;
      }
      case MC_ALG_STACK: {
        const int32_t m3 = op_step(s, 3, lfoS, 0, v, &dead);
        const int32_t m2 = op_step(s, 2, lfoS,
                                   fm_off(m3, p->op[3].level), v, &dead);
        const int32_t m1 = op_step(s, 1, lfoS,
                                   fm_off(m2, p->op[2].level), v, &dead);
        const int32_t o0 = op_step(s, 0, lfoS,
                                   fm_off(m1, p->op[1].level) + peOff, v, &dead);
        out = (o0 * p->op[0].out) >> 7;
        break;
      }
      case MC_ALG_PAIR: {
        const int32_t m1 = op_step(s, 1, lfoS, 0, v, &dead);
        const int32_t o0 = op_step(s, 0, lfoS,
                                   fm_off(m1, p->op[1].level) + peOff, v, &dead);
        const int32_t m3 = (v->opCount > 3) ? op_step(s, 3, lfoS, 0, v, &dead) : 0;
        const int32_t o2 = (v->opCount > 2)
                             ? op_step(s, 2, lfoS,
                                       fm_off(m3, p->op[3].level) + peOff, v, &dead)
                             : 0;
        out = ((o0 * p->op[0].out) >> 7) + ((o2 * p->op[2].out) >> 7);
        break;
      }
      default: {  /* MC_ALG_ADD */
        for (int k = 0; k < v->opCount; k++) {
          const int32_t o = op_step(s, k, lfoS, peOff, v, &dead);
          out += (o * p->op[k].out) >> 7;
        }
        break;
      }
    }

    /* tremolo */
    if (p->lfoMode & MC_LFO_TREM) {
      const int32_t depth = p->lfoTrem;
      out -= (int32_t)(((int64_t)out * (g_sine64[(v->lfoPhase >> 26) & 63] + 32768)
                        * depth) >> 30);
    }

    out = (int32_t)(((int64_t)out * v->velGain) >> 16);
    const int32_t l = (out * gainL) >> 8;
    const int32_t r = (out * gainR) >> 8;
    mixL[i] += l;
    mixR[i] += r;

    if (dead) {
      /* a voice is gone only when every operator has fully released */
      int any = 0;
      for (int k = 0; k < v->opCount; k++)
        if (v->op[k].env > 0) { any = 1; break; }
      if (!any) { v->active = 0; break; }
    }
  }
}

void mc_render(mc_synth* s, int16_t* out, int frames) {
  if (!s || !out) return;
  if (frames <= 0) return;

  int done = 0;
  while (done < frames) {
    int n = frames - done;
    if (n > MC_BLOCK_MAX) n = MC_BLOCK_MAX;

    memset(s->mixL, 0, sizeof(int32_t) * (size_t)n);
    memset(s->mixR, 0, sizeof(int32_t) * (size_t)n);

    for (int i = 0; i < s->maxVoices; i++) {
      mc_voice* v = &s->voices[i];
      if (v->active) render_voice(s, v, n);
    }

    for (int i = 0; i < n; i++) {
      int32_t l = (s->mixL[i] * s->masterGain) >> 12;
      int32_t r = (s->mixR[i] * s->masterGain) >> 12;
      l = clamp16(l); r = clamp16(r);
      int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
      if (al > s->peak) s->peak = (uint16_t)al;
      if (ar > s->peak) s->peak = (uint16_t)ar;
      out[(done + i) * 2 + 0] = (int16_t)l;
      out[(done + i) * 2 + 1] = (int16_t)r;
    }
    done += n;
  }
  s->renderCalls++;
}

/* ------------------------------------------------------------------ bank */
uint32_t mc_crc32(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}

int mc_bank_open(mc_bank* out, const void* data, size_t bytes) {
  if (!out || !data) return MC_ERR_ARG;
  if (bytes < MC_HDR_BYTES) return MC_ERR_TRUNCATED;
  const mc_bank_hdr* h = (const mc_bank_hdr*)data;
  if (memcmp(h->magic, MC_BANK_MAGIC, 4) != 0) return MC_ERR_MAGIC;
  if (h->version != MC_BANK_VERSION) return MC_ERR_VERSION;
  if (h->totalBytes != bytes) return MC_ERR_TRUNCATED;
  if (h->numTables == 0 || h->numPatches == 0) return MC_ERR_BAD_TABLE;

  const size_t tblEnd = h->tableOff + (size_t)h->numTables * MC_TABLE_LEN * 2;
  const size_t patEnd = h->patchOff + (size_t)h->numPatches * MC_PATCH_BYTES;
  if (tblEnd > bytes || patEnd > bytes) return MC_ERR_TRUNCATED;
  if (h->tableOff < MC_HDR_BYTES || h->patchOff < MC_HDR_BYTES) return MC_ERR_BAD_TABLE;
  if (h->crc32 && mc_crc32((const uint8_t*)data + MC_HDR_BYTES,
                           bytes - MC_HDR_BYTES) != h->crc32) return MC_ERR_CRC;

  /* every operator's table index must be in range, or the engine will read
   * past the table block at note-on. Cheap check, done once. */
  const mc_patch* ps = (const mc_patch*)((const uint8_t*)data + h->patchOff);
  for (unsigned i = 0; i < h->numPatches; i++) {
    if (ps[i].opCount < 1 || ps[i].opCount > MC_MAX_OPS) return MC_ERR_BAD_TABLE;
    for (int k = 0; k < ps[i].opCount; k++)
      if (ps[i].op[k].table >= h->numTables) return MC_ERR_BAD_TABLE;
  }

  out->hdr       = h;
  out->tables    = (const int16_t*)((const uint8_t*)data + h->tableOff);
  out->patches   = ps;
  out->numTables = h->numTables;
  out->numPatches= h->numPatches;
  return MC_OK;
}

const mc_patch* mc_bank_patch(const mc_bank* b, int program) {
  if (!b || !b->patches) return NULL;
  if (program < 0 || program >= MC_NUM_PROGRAMS) return NULL;
  if ((int)b->numPatches <= program) return NULL;
  return &b->patches[program];
}

const mc_patch* mc_bank_drum(const mc_bank* b, int note) {
  if (!b || !b->patches) return NULL;
  const int idx = MC_NUM_PROGRAMS + note;
  if (idx >= (int)b->numPatches) return NULL;
  return &b->patches[idx];
}

/* --------------------------------------------------------- voice pools */
void mc_note_on(mc_synth* s, int ch, int note, int vel) {
  note_on_impl(s, ch, note, vel, 0, 255);
}

int mc_set_voice_pools(mc_synth* s, int musicVoices, int sfxVoices) {
  if (!s) return MC_ERR_ARG;
  if (musicVoices < 0 || sfxVoices < 0) return MC_ERR_ARG;
  if (musicVoices + sfxVoices > s->maxVoices) return MC_ERR_ARG;
  if (musicVoices == 0 && sfxVoices == 0) {
    s->musicVoices = s->maxVoices; s->sfxStart = 0; s->sfxCount = 0;
    return MC_OK;
  }
  s->musicVoices = musicVoices;
  s->sfxStart    = musicVoices;
  s->sfxCount    = sfxVoices;
  for (int i = 0; i < s->maxVoices; i++) {
    const int inMusic = (i < musicVoices);
    const int inSfx   = (i >= s->sfxStart && i < s->sfxStart + sfxVoices);
    if (!inMusic && !inSfx) s->voices[i].active = 0;
  }
  return MC_OK;
}

void mc_set_steal_policy(mc_synth* s, mc_steal_t p) {
  if (s) s->stealPolicy = (int)p;
}

int mc_sfx_play(mc_synth* s, int ch, int program, int note, int vel, int prio) {
  if (!s || !s->hasBank || ch < 0 || ch > 15) return -1;
  if (vel <= 0 || note < 0 || note > 127) return -1;
  if (program < 0) program = 0;
  const uint8_t oldProg = s->prog[ch];
  const uint8_t oldDrum = s->drumCh[ch];
  s->prog[ch]   = (uint8_t)(program & 127);
  s->drumCh[ch] = 0;                    /* an effect names its own sound */
  note_on_impl(s, ch, note, vel, 1, prio);
  s->prog[ch]   = oldProg;
  s->drumCh[ch] = oldDrum;

  mc_voice* best = NULL;
  for (int i = 0; i < s->maxVoices; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active || v->pool != 1 || v->note != note) continue;
    if (!best || v->age > best->age) best = v;
  }
  return best ? (int)best->handle : -1;
}

void mc_sfx_stop(mc_synth* s, int handle) {
  if (!s) return;
  for (int i = 0; i < s->maxVoices; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active || v->released || v->handle != (int16_t)handle) continue;
    if (v->pool != 1) continue;
    v->released = 1;
    for (int k = 0; k < v->opCount; k++) v->op[k].stage = 3;
  }
}

int mc_sfx_active(const mc_synth* s) {
  if (!s) return 0;
  int n = 0;
  for (int i = 0; i < s->maxVoices; i++)
    if (s->voices[i].active && s->voices[i].pool == 1) n++;
  return n;
}
