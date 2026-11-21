/*
 * api.c -- lifecycle, MIDI channel state, and the sequencer.
 */
#include "mc_internal.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- lifecycle */
mc_synth* mc_create(int sampleRate, int maxVoices) {
  if (sampleRate < 8000 || sampleRate > 48000) return NULL;
  if (maxVoices < 1 || maxVoices > MC_MAX_VOICES) return NULL;

  mc_synth* s = (mc_synth*)calloc(1, sizeof(mc_synth));
  if (!s) return NULL;
  s->sampleRate = sampleRate;
  s->maxVoices  = maxVoices;
  s->masterGain = 2867;                 /* Q12, 0.70: headroom for 16 voices */
  for (int c = 0; c < MC_MAX_CHANNELS; c++) {
    s->drumCh[c] = (c == 9) ? 1 : 0;    /* GM default: channel 10 is drums */
    s->bend[c] = 65536;                 /* Q16, no bend */
    s->vol[c]  = 100;
    s->expr[c] = 127;
  }
  mc_tables_init(sampleRate);
  s->initialized = 1;
  return s;
}

void mc_destroy(mc_synth* s) {
  if (!s) return;
  if (s->song) mc_song_free((mc_song*)s->song);
  free(s);
}

int mc_load_bank(mc_synth* s, const void* bank, size_t bytes) {
  if (!s) return MC_ERR_ARG;
  mc_bank b;
  const int r = mc_bank_open(&b, bank, bytes);
  if (r != MC_OK) {
    const char* m = "bank error";
    switch (r) {
      case MC_ERR_MAGIC: m = "bad magic"; break;
      case MC_ERR_VERSION: m = "bad version"; break;
      case MC_ERR_TRUNCATED: m = "truncated"; break;
      case MC_ERR_CRC: m = "crc mismatch"; break;
      case MC_ERR_BAD_TABLE: m = "bad table index"; break;
      default: break;
    }
    strncpy(s->err, m, sizeof(s->err) - 1);
    return r;
  }
  s->bank = b;
  s->hasBank = 1;
  s->err[0] = 0;
  return MC_OK;
}

/* ------------------------------------------------------------- MIDI state */
void mc_program_change(mc_synth* s, int ch, int program) {
  if (!s || ch < 0 || ch > 15) return;
  s->prog[ch] = (uint8_t)(program & 127);
  /* A program change on the drum channel means the file is using channel 10
   * melodically. Game rips do this constantly; a General MIDI file never sends
   * one to channel 10. mc_set_drum_channel() overrides this either way. */
  if (s->drumCh[ch]) s->drumCh[ch] = 0;
}

void mc_set_drum_channel(mc_synth* s, int ch) {
  if (!s) return;
  for (int c = 0; c < MC_MAX_CHANNELS; c++) s->drumCh[c] = 0;
  if (ch >= 0 && ch < MC_MAX_CHANNELS) s->drumCh[ch] = 1;
}

void mc_pitch_bend(mc_synth* s, int ch, int value) {
  if (!s || ch < 0 || ch > 15) return;
  if (value < -8192) value = -8192;
  if (value > 8191) value = 8191;
  /* +/- 2 semitones, linearised: ratio = 1 + value/8192 * 0.1192 */
  const int32_t nb = 65536 + (int32_t)((int64_t)value * 7810 / 8192);
  const int32_t ob = s->bend[ch];
  if (ob == nb) return;
  s->bend[ch] = nb;
  if (ob <= 0) return;
  for (int i = 0; i < s->maxVoices; i++) {
    mc_voice* v = &s->voices[i];
    if (!v->active || v->ch != ch) continue;
    for (int k = 0; k < v->opCount; k++)
      v->op[k].inc = (uint32_t)(((int64_t)v->op[k].inc * nb) / ob);
  }
}

void mc_control_change(mc_synth* s, int ch, int cc, int value) {
  if (!s || ch < 0 || ch > 15) return;
  if (value < 0) value = 0; if (value > 127) value = 127;
  switch (cc) {
    case 7:  s->vol[ch] = (uint8_t)value; break;
    case 11: s->expr[ch] = (uint8_t)value; break;
    case 10: s->pan[ch] = (int8_t)(value - 64); break;
    case 64: s->sustainPedal = (uint8_t)(value >= 64); break;
    case 120: case 123: mc_all_notes_off(s, 0); break;
    default: break;
  }
}

void mc_channel_pressure(mc_synth* s, int ch, int value) {
  (void)s; (void)ch; (void)value;   /* accepted, not applied */
}

void mc_set_master_gain(mc_synth* s, int gain_q8) {
  if (!s) return;
  if (gain_q8 < 0) gain_q8 = 0;
  if (gain_q8 > 512) gain_q8 = 512;
  s->masterGain = gain_q8 * 16;     /* Q8 input -> Q12 internal */
}

int mc_active_voices(const mc_synth* s) {
  if (!s) return 0;
  int n = 0;
  for (int i = 0; i < s->maxVoices; i++) if (s->voices[i].active) n++;
  return n;
}

void mc_get_stats(const mc_synth* s, uint32_t* voicedSamples,
                  uint32_t* renderCalls, uint16_t* peak) {
  if (!s) return;
  if (voicedSamples) *voicedSamples = s->voicedSamples;
  if (renderCalls)   *renderCalls   = s->renderCalls;
  if (peak)          *peak          = s->peak;
}

const char* mc_last_error(const mc_synth* s) { return s ? s->err : "no synth"; }

/* --------------------------------------------------------------- player */
void mc_player_start(mc_synth* s, const mc_song* g) {
  if (!s) return;
  if (s->song && s->song != g) mc_song_free((mc_song*)s->song);
  s->song = g;
  s->pos = 0;
  s->nextEvent = 0;
  mc_all_notes_off(s, 1);
}

void mc_player_stop(mc_synth* s) {
  if (!s) return;
  mc_all_notes_off(s, 1);
  s->song = NULL;
  s->pos = 0;
  s->nextEvent = 0;
}

void mc_player_set_loop(mc_synth* s, int loop) { if (s) s->loop = loop ? 1 : 0; }

static void dispatch(mc_synth* s, const struct mc_song_event* e) {
  const int ch = e->type & 0x0F;
  switch (e->type & 0xF0) {
    case 0x80: mc_note_off(s, ch, e->a); break;
    case 0x90: if (e->b) mc_note_on(s, ch, e->a, e->b);
               else mc_note_off(s, ch, e->a); break;
    case 0xB0: mc_control_change(s, ch, e->a, e->b); break;
    case 0xC0: mc_program_change(s, ch, e->a); break;
    case 0xE0: mc_pitch_bend(s, ch, ((int)e->b << 7 | e->a) - 8192); break;
    default: break;
  }
}

void mc_player_render(mc_synth* s, int16_t* out, int frames, int* done) {
  if (done) *done = 0;
  if (!s || !out || frames <= 0) return;

  int offset = 0;
  while (offset < frames) {
    int n = frames - offset;
    if (n > 64) n = 64;

    if (s->song) {
      const uint32_t horizon = s->pos + (uint32_t)n;
      while (s->nextEvent < s->song->count &&
             s->song->ev[s->nextEvent].frame < horizon) {
        dispatch(s, &s->song->ev[s->nextEvent]);
        s->nextEvent++;
      }
      s->pos = horizon;
      if (s->nextEvent >= s->song->count) {
        /* all events consumed: wait for the tail to ring out */
        if (s->pos > s->song->frames + (uint32_t)(s->sampleRate * 2)) {
          if (s->loop) { s->pos = 0; s->nextEvent = 0; mc_all_notes_off(s, 1); }
          else if (done) { *done = 1; }
        }
      }
    }
    mc_render(s, out + offset * 2, n);
    offset += n;
  }
}

uint32_t mc_player_position(const mc_synth* s) { return s ? s->pos : 0; }
