/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 midicore contributors
 */
/*
 * midicore.h -- public API for the midicore synthesis engine and MIDI player.
 *
 * Design constraints, in priority order:
 *   1. No allocation on the audio path. Everything is sized at mc_create().
 *   2. No floating point. All DSP is fixed point, so it behaves identically on
 *      a soft float target and costs no FPU context save.
 *   3. No OS dependency. The core never touches HAL, SD, I2S or stdio.
 *
 * Typical embed:
 *
 *   mc_synth* s = mc_create(32000, 16);
 *   mc_load_bank(s, bank_bytes, bank_size);      // bank stays mapped
 *   mc_song*  g = mc_song_load(midi_bytes, midi_len, 32000);
 *   mc_player_start(s, g);
 *   for (;;) {
 *     mc_player_render(s, buf, frames, &done);   // buf = int16 stereo
 *     i2s_write(buf, frames);
 *   }
 */
#ifndef MIDICORE_H
#define MIDICORE_H

#include <stdint.h>
#include <stddef.h>
#include "mc_bank.h"

#ifdef __cplusplus
extern "C" {

/* ------------------------------------------------- voice pools / one-shots
 * Split the voice array so sound effects can never steal a music note and
 * music can never steal an effect. Both pools are carved from maxVoices.
 * Pools are optional: until mc_set_voice_pools() is called there is one pool.
 */
typedef enum {
  MC_STEAL_OLDEST = 0,   /* default: first-in wins the axe   */
  MC_STEAL_QUIETEST,     /* quietest voice is reused         */
  MC_STEAL_LOWPRI        /* lowest prio, then oldest         */
} mc_steal_t;

int  mc_set_voice_pools(mc_synth* s, int musicVoices, int sfxVoices);
void mc_set_steal_policy(mc_synth* s, mc_steal_t p);
/* Fire-and-forget one-shot in the SFX pool. Returns a handle, or -1.
 * `prio` 0..255: higher survives stealing longer. */
int  mc_sfx_play(mc_synth* s, int ch, int program, int note, int vel, int prio);
void mc_sfx_stop(mc_synth* s, int handle);
int  mc_sfx_active(const mc_synth* s);

/* ------------------------------------------------------------ sequencers
 * Two sources, one renderer. Neither allocates a per-song event table.
 *   FILE  : parse straight out of a blob (cart ROM, mmap, embedded array).
 *           Per-track cursors + incremental tempo, O(tracks) state.
 *   STREAM: events arrive as wire frames from another MCU.
 */
typedef struct mc_seq mc_seq;

mc_seq* mc_seq_open_file(const void* smf, size_t bytes, int sampleRate);
mc_seq* mc_seq_open_stream(int sampleRate, int ppqn);
void    mc_seq_free(mc_seq* q);

/* Push a chunk: `ev` is count * (dt:u8 ms, status, a, b). Returns MC_OK,
 * -1 if the ring is full (apply backpressure), MC_ERR_ARG on bad input. */
int  mc_seq_push(mc_seq* q, const uint8_t* ev, int count, uint16_t dur_ms);
/* Push one wire frame: A5 | type | len:u16 | payload | crc8. */
int  mc_seq_push_frame(mc_seq* q, const uint8_t* frame, size_t bytes);
int  mc_seq_pending(const mc_seq* q);            /* ms of events queued   */
void mc_seq_flush(mc_seq* q);                    /* drop queue, resync    */
/* Renders from the sequencer, applying events at sample-accurate offsets. */
void mc_seq_render(mc_synth* s, mc_seq* q, int16_t* out, int frames, int* done);
uint32_t mc_seq_position(const mc_seq* q);

#endif

#define MC_MAX_CHANNELS 16
#define MC_MAX_VOICES 64   /* hard ceiling at build time */

/* ------------------------------------------------------------------ errors */
enum {
  MC_OK             =  0,
  MC_ERR_ARG        = -1,
  MC_ERR_MAGIC      = -2,
  MC_ERR_VERSION    = -3,
  MC_ERR_TRUNCATED  = -4,
  MC_ERR_CRC        = -5,
  MC_ERR_NOMEM      = -6,
  MC_ERR_BAD_TABLE  = -7,
  MC_ERR_BAD_MIDI   = -8,
  MC_ERR_TOO_MANY_VOICES = -9
};

typedef struct mc_synth mc_synth;

/* --------------------------------------------------------------- lifecycle
 * sampleRate  8000..48000; 22050 or 32000 are the sweet spots for a game.
 * maxVoices   1..MC_MAX_VOICES. Voices are preallocated, never grown.
 */
mc_synth* mc_create(int sampleRate, int maxVoices);
void      mc_destroy(mc_synth* s);

/* Bank must outlive the synth. Pass pointer + byte length; nothing is copied.
 * On success the synth owns a validated view; on failure it keeps its old one.
 */
int mc_load_bank(mc_synth* s, const void* bank, size_t bytes);

/* ------------------------------------------------------------- MIDI input */
void mc_note_on(mc_synth* s, int ch, int note, int vel);
void mc_note_off(mc_synth* s, int note_off_ch, int note);
void mc_program_change(mc_synth* s, int ch, int program);
void mc_control_change(mc_synth* s, int ch, int cc, int value);
void mc_pitch_bend(mc_synth* s, int ch, int value);   /* -8192..+8191 */
void mc_channel_pressure(mc_synth* s, int ch, int value);
void mc_all_notes_off(mc_synth* s, int hard);
void mc_set_master_gain(mc_synth* s, int gain_q8);

/* Derive band-limited copies of every stored wavetable into `buf`, so high notes
 * do not alias. Fills as many levels as fit and returns the number derived; 0
 * leaves the engine working exactly as before. Budget about 512 bytes per level
 * per table (101 tables x 3 levels = ~150 KB). Put it in PSRAM. */
int mc_build_mips(mc_synth* s, void* buf, size_t bytes);
/* Which channel is percussion; -1 disables drums entirely. Channel 10 is the
 * default, but a program change received on it switches that channel to
 * melodic (game rips rely on this). */
void mc_set_drum_channel(mc_synth* s, int ch);    /* 0..256, 256 = 1.0 */

/* ----------------------------------------------------------------- render
 * Writes `frames` interleaved stereo int16. Always fills the whole buffer.
 */
void mc_render(mc_synth* s, int16_t* out, int frames);

/* Diagnostics. Both are cheap and safe to call from a status task. */
int mc_active_voices(const mc_synth* s);
void mc_get_stats(const mc_synth* s, uint32_t* voicedSamples,
                  uint32_t* renderCalls, uint16_t* peak);

/* ------------------------------------------------------------ MIDI files
 * An mc_song is a parse of a Standard MIDI File with every event already
 * converted to an absolute sample frame. The parser owns its memory.
 */
typedef struct mc_song mc_song;

mc_song* mc_song_load(const void* data, size_t bytes, int sampleRate);
void     mc_song_free(mc_song* g);
uint32_t mc_song_frames(const mc_song* g);
int      mc_song_tracks(const mc_song* g);
int      mc_song_has_loop(const mc_song* g);

/* ----------------------------------------------------------------- player
 * A sequencer that feeds a synth. One song at a time per synth.
 */
void mc_player_start(mc_synth* s, const mc_song* g);
void mc_player_stop(mc_synth* s);
void mc_player_set_loop(mc_synth* s, int loop);

/* Renders `frames` of audio AND advances the sequencer. `done` (optional) is
 * set to 1 when a non-looping song has finished. Safe to call with g == NULL.
 */
void mc_player_render(mc_synth* s, int16_t* out, int frames, int* done);
uint32_t mc_player_position(const mc_synth* s);   /* frames since start */

/* True when the most recent bank validation or song parse failed. */
const char* mc_last_error(const mc_synth* s);

#ifdef __cplusplus
}

/* ------------------------------------------------- voice pools / one-shots
 * Split the voice array so sound effects can never steal a music note and
 * music can never steal an effect. Both pools are carved from maxVoices.
 * Pools are optional: until mc_set_voice_pools() is called there is one pool.
 */
typedef enum {
  MC_STEAL_OLDEST = 0,   /* default: first-in wins the axe   */
  MC_STEAL_QUIETEST,     /* quietest voice is reused         */
  MC_STEAL_LOWPRI        /* lowest prio, then oldest         */
} mc_steal_t;

int  mc_set_voice_pools(mc_synth* s, int musicVoices, int sfxVoices);
void mc_set_steal_policy(mc_synth* s, mc_steal_t p);
/* Fire-and-forget one-shot in the SFX pool. Returns a handle, or -1.
 * `prio` 0..255: higher survives stealing longer. */
int  mc_sfx_play(mc_synth* s, int ch, int program, int note, int vel, int prio);
void mc_sfx_stop(mc_synth* s, int handle);
int  mc_sfx_active(const mc_synth* s);

/* ------------------------------------------------------------ sequencers
 * Two sources, one renderer. Neither allocates a per-song event table.
 *   FILE  : parse straight out of a blob (cart ROM, mmap, embedded array).
 *           Per-track cursors + incremental tempo, O(tracks) state.
 *   STREAM: events arrive as wire frames from another MCU.
 */
typedef struct mc_seq mc_seq;

mc_seq* mc_seq_open_file(const void* smf, size_t bytes, int sampleRate);
mc_seq* mc_seq_open_stream(int sampleRate, int ppqn);
void    mc_seq_free(mc_seq* q);

/* Push a chunk: `ev` is count * (dt:u8 ms, status, a, b). Returns MC_OK,
 * -1 if the ring is full (apply backpressure), MC_ERR_ARG on bad input. */
int  mc_seq_push(mc_seq* q, const uint8_t* ev, int count, uint16_t dur_ms);
/* Push one wire frame: A5 | type | len:u16 | payload | crc8. */
int  mc_seq_push_frame(mc_seq* q, const uint8_t* frame, size_t bytes);
int  mc_seq_pending(const mc_seq* q);            /* ms of events queued   */
void mc_seq_flush(mc_seq* q);                    /* drop queue, resync    */
/* Renders from the sequencer, applying events at sample-accurate offsets. */
void mc_seq_render(mc_synth* s, mc_seq* q, int16_t* out, int frames, int* done);
uint32_t mc_seq_position(const mc_seq* q);

#endif

/* ------------------------------------------------- voice pools / one-shots
 * Split the voice array so sound effects can never steal a music note and
 * music can never steal an effect. Both pools are carved from maxVoices.
 * Pools are optional: until mc_set_voice_pools() is called there is one pool.
 */
typedef enum {
  MC_STEAL_OLDEST = 0,   /* default: first-in wins the axe   */
  MC_STEAL_QUIETEST,     /* quietest voice is reused         */
  MC_STEAL_LOWPRI        /* lowest prio, then oldest         */
} mc_steal_t;

int  mc_set_voice_pools(mc_synth* s, int musicVoices, int sfxVoices);
void mc_set_steal_policy(mc_synth* s, mc_steal_t p);
/* Fire-and-forget one-shot in the SFX pool. Returns a handle, or -1.
 * `prio` 0..255: higher survives stealing longer. */
int  mc_sfx_play(mc_synth* s, int ch, int program, int note, int vel, int prio);
void mc_sfx_stop(mc_synth* s, int handle);
int  mc_sfx_active(const mc_synth* s);

/* ------------------------------------------------------------ sequencers
 * Two sources, one renderer. Neither allocates a per-song event table.
 *   FILE  : parse straight out of a blob (cart ROM, mmap, embedded array).
 *           Per-track cursors + incremental tempo, O(tracks) state.
 *   STREAM: events arrive as wire frames from another MCU.
 */
typedef struct mc_seq mc_seq;

mc_seq* mc_seq_open_file(const void* smf, size_t bytes, int sampleRate);
mc_seq* mc_seq_open_stream(int sampleRate, int ppqn);
void    mc_seq_free(mc_seq* q);

/* Push a chunk: `ev` is count * (dt:u8 ms, status, a, b). Returns MC_OK,
 * -1 if the ring is full (apply backpressure), MC_ERR_ARG on bad input. */
int  mc_seq_push(mc_seq* q, const uint8_t* ev, int count, uint16_t dur_ms);
/* Push one wire frame: A5 | type | len:u16 | payload | crc8. */
int  mc_seq_push_frame(mc_seq* q, const uint8_t* frame, size_t bytes);
int  mc_seq_pending(const mc_seq* q);            /* ms of events queued   */
void mc_seq_flush(mc_seq* q);                    /* drop queue, resync    */
/* Renders from the sequencer, applying events at sample-accurate offsets. */
void mc_seq_render(mc_synth* s, mc_seq* q, int16_t* out, int frames, int* done);
uint32_t mc_seq_position(const mc_seq* q);

#endif /* MIDICORE_H */
