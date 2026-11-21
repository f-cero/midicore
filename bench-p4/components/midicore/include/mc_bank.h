/*
 * mc_bank.h -- the SYN2 instrument bank format.
 *
 * A bank is a flat, little-endian blob that can be memory-mapped straight from
 * flash. Nothing in it is a pointer, so it needs no relocation and no fixups:
 * a bank built on a PC plays on the target unchanged.
 *
 * Everything is fixed width. Do not add a field without bumping MC_BANK_VERSION.
 *
 *   offset  size  field
 *   0       4     magic "SYN2"
 *   4       2     version           = 1
 *   6       2     headerBytes       = 32
 *   8       2     numTables         single-cycle wavetables, 256 int16 each
 *   10      2     numPatches        instrument programs
 *   12      4     tableOff          byte offset of the wavetable block
 *   16      4     patchOff          byte offset of the patch block
 *   20      4     totalBytes        whole file size (sanity check)
 *   24      4     crc32             CRC-32 of bytes [32, totalBytes)
 *   28      4     reserved          = 0
 *
 * Wavetable block: numTables * 256 * int16, little endian, one full cycle,
 * index 0 = phase 0. Values are allowed to reach +/-32767.
 *
 * Patch block: numPatches * mc_patch (64 bytes), see below.
 */
#ifndef MC_BANK_H
#define MC_BANK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_BANK_MAGIC      "SYN2"
#define MC_BANK_VERSION    1
#define MC_TABLE_LEN       256
#define MC_MAX_OPS         4
#define MC_PATCH_BYTES     64
#define MC_HDR_BYTES       32
#define MC_NUM_PROGRAMS    128     /* melodic, GM order */
#define MC_NUM_DRUMS       128     /* drum notes 35..81 used */

/* ---------------------------------------------------------------- algorithms
 * MC_ALG_ADD    all operators sum into the output
 * MC_ALG_FM2    op1 phase-modulates op0;  op0 audible (op2/op3 additive)
 * MC_ALG_STACK  op3 -> op2 -> op1 -> op0; only op0 audible
 * MC_ALG_PAIR   (op1 -> op0) + (op3 -> op2), both carriers audible
 */
enum {
  MC_ALG_ADD   = 0,
  MC_ALG_FM2   = 1,
  MC_ALG_STACK = 2,
  MC_ALG_PAIR  = 3
};

/* ------------------------------------------------------------------- flags */
enum {
  MC_F_DRUM      = 1 << 0,  /* patch is a drum key, ignores program change  */
  MC_F_ONESHOT   = 1 << 1,  /* envelope never sustains; key-up still releases */
  MC_F_NOINTERP  = 1 << 2,  /* nearest-neighbour table read (harsher, cheaper) */
  MC_F_LOOP      = 1 << 3   /* reserved: sustained = loop, implied by ADSR    */
};

/* Filter modes: a 2-pole state-variable filter. MC_FILT_OFF costs nothing. */
enum { MC_FILT_OFF = 0, MC_FILT_LP = 1, MC_FILT_HP = 2, MC_FILT_BP = 3 };

/* LFO routing */
enum { MC_LFO_OFF = 0, MC_LFO_VIB = 1, MC_LFO_TREM = 2, MC_LFO_BOTH = 3 };

/* --------------------------------------------------------------- operator
 * Ten bytes. `mul` maps to a frequency multiple: 0 = 0.5x, 1 = 1x, ... 15 = 15x.
 * `detune` is a signed value in 1/16 of a semitone (-8..+7).
 * `out` is the operator's contribution to the mix; 0 means "modulator only".
 * Envelope rates are 0..127, where 127 is fastest (see mc_rate.h).
 */
typedef struct {
  uint8_t table;     /* index into the bank's wavetable block        */
  uint8_t mul;       /* frequency multiple, 0 => 0.5x                */
  uint8_t level;     /* 0..127, drives FM index / additive amplitude */
  uint8_t keyScale;  /* 0..127, level attenuation per octave         */
  int8_t  detune;    /* -8..+7 sixteenths of a semitone              */
  uint8_t attack;    /* 0..127                                       */
  uint8_t decay;     /* 0..127                                       */
  uint8_t sustain;   /* 0..127 sustain level                         */
  uint8_t release;   /* 0..127                                       */
  uint8_t out;       /* 0..127 output level, 0 = modulator only      */
} mc_op;

/* ------------------------------------------------------------------ patch
 * 64 bytes exactly. `pan` is centred at 0, range -64..+63.
 */
typedef struct {
  uint8_t  opCount;      /* 1..4                                        */
  uint8_t  algorithm;    /* MC_ALG_*                                    */
  uint8_t  flags;        /* MC_F_*                                      */
  uint8_t  velDepth;     /* 0..127, how much velocity scales level      */
  uint8_t  keyTrack;     /* 0..127, level attenuation per octave        */
  uint8_t  filterMode;   /* MC_FILT_*                                   */
  uint8_t  filterRes;    /* 0..127                                      */
  uint8_t  filterCut;    /* 0..127, base cutoff on a log scale          */
  uint8_t  filterEnvAmt; /* 0..127, envelope sweep in cutoff units      */
  uint8_t  filterKeyTrk; /* 0..127, cutoff tracking per octave          */
  uint8_t  lfoMode;      /* MC_LFO_*                                    */
  uint8_t  lfoRate;      /* 0..127                                      */
  uint8_t  lfoDelay;     /* 0..255, 4 ms units                          */
  uint8_t  lfoVib;       /* 0..127, vibrato depth in 1/32 semitone      */
  uint8_t  lfoTrem;      /* 0..127, tremolo depth                       */
  uint8_t  gain;         /* 0..127, patch output level                  */
  int8_t   pan;          /* -64..+63                                    */
  int8_t   pitchEnvAmt;  /* -24..+24 semitones at note-on               */
  uint8_t  pitchEnvRate; /* 0..127, how fast it returns to pitch        */
  uint8_t  pad0;
  uint32_t pad1;
  mc_op    op[MC_MAX_OPS];
} mc_patch;

/* ----------------------------------------------------------------- header */
typedef struct {
  char     magic[4];
  uint16_t version;
  uint16_t headerBytes;
  uint16_t numTables;
  uint16_t numPatches;
  uint32_t tableOff;
  uint32_t patchOff;
  uint32_t totalBytes;
  uint32_t crc32;
  uint32_t reserved;
} mc_bank_hdr;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(mc_op)    == 10, "mc_op must be 10 bytes");
_Static_assert(sizeof(mc_patch) == MC_PATCH_BYTES, "mc_patch must be 64 bytes");
_Static_assert(sizeof(mc_bank_hdr) == MC_HDR_BYTES, "header must be 32 bytes");
#endif

/* Parsed, validated view of a bank. Points into the caller's buffer. */
typedef struct {
  const mc_bank_hdr* hdr;
  const int16_t*     tables;   /* numTables * 256 */
  const mc_patch*    patches;  /* numPatches */
  uint16_t           numTables;
  uint16_t           numPatches;
} mc_bank;

/* Returns 0 on success, negative MC_ERR_* on failure. Does not copy data. */
int mc_bank_open(mc_bank* out, const void* data, size_t bytes);

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). */
uint32_t mc_crc32(const void* data, size_t len);

/* Look up the patch for a GM program (0..127) or drum note (0..127).
 * Returns NULL when the bank has no such patch. */
const mc_patch* mc_bank_patch(const mc_bank* b, int program);
const mc_patch* mc_bank_drum(const mc_bank* b, int note);

#ifdef __cplusplus
}
#endif
#endif /* MC_BANK_H */
