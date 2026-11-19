/* mc_internal.h -- private engine state. Not installed. */
#ifndef MC_INTERNAL_H
#define MC_INTERNAL_H
#include "midicore.h"

#define MC_BLOCK_MAX 256
#define MC_ENV_ONE   65536      /* Q16, envelope full scale */

/* One operator's per-voice state. 24 bytes. */
typedef struct {
  uint32_t phase;
  uint32_t inc;       /* base phase increment, Q32 of one cycle */
  uint32_t vibScale;  /* inc * lfoVib >> 15, for vibrato            */
  int32_t  env;       /* 0..65536 */
  int32_t  sus;       /* sustain level, Q16 */
  uint8_t  stage;     /* 0 attack, 1 decay, 2 sustain, 3 release   */
  uint8_t  relIdx;    /* release rate index                        */
} mc_opv;

typedef struct {
  mc_opv   op[MC_MAX_OPS];
  const mc_patch* patch;
  uint32_t age;
  uint32_t lfoPhase;
  uint32_t lfoInc;
  uint16_t lfoDelay;   /* samples remaining before vibrato starts   */
  uint16_t vibActive;
  int32_t  pitchEnv;   /* Q16 semitones, decays to zero             */
  int32_t  pitchEnvStep;
  int32_t  f1, f2;     /* filter state                              */
  uint8_t  active;
  uint8_t  released;
  uint8_t  ch;
  uint8_t  note;
  uint8_t  vel;
  uint8_t  opCount;
  uint8_t  pad[2];
} mc_voice;

struct mc_synth {
  int      sampleRate;
  int      maxVoices;
  uint8_t  initialized;

  mc_bank  bank;
  int      hasBank;

  mc_voice voices[MC_MAX_VOICES];
  int32_t  mixL[MC_BLOCK_MAX];
  int32_t  mixR[MC_BLOCK_MAX];

  uint8_t  prog[MC_MAX_CHANNELS];
  uint8_t  vol[MC_MAX_CHANNELS];
  uint8_t  expr[MC_MAX_CHANNELS];
  int8_t   pan[MC_MAX_CHANNELS];
  int32_t  bend[MC_MAX_CHANNELS];   /* Q16 ratio, 65536 = no bend */
  uint8_t  sustainPedal;
  uint8_t  held[MC_MAX_CHANNELS][128];

  int32_t  masterGain;              /* Q8 */

  /* sequencer */
  const mc_song* song;
  uint32_t pos;
  uint32_t nextEvent;
  int      loop;

  /* stats */
  uint32_t voicedSamples;
  uint32_t renderCalls;
  uint16_t peak;
  char     err[48];
};

/* Envelope rate tables, built once per sample rate (src/mc_tables.c). */
extern int32_t g_attInc[128];
extern int32_t g_decMul[128];
extern int16_t g_sine64[64];
void mc_tables_init(int sampleRate);

/* song internals shared with player.c */
struct mc_song_event {
  uint32_t frame;
  uint8_t  type;
  uint8_t  a;
  uint8_t  b;
};
struct mc_song {
  struct mc_song_event* ev;
  uint32_t count;
  uint32_t frames;
  uint32_t loopStart, loopEnd;
  int      tracks;
  int      hasLoop;
};

#endif
