/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 midicore contributors
 */
/* esp32_midicore.h -- I2S output glue for midicore on ESP32-S3. */
#pragma once
#include "midicore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int pinBclk, pinLrck, pinDout;
  int sampleRate;
  int voices;
  int blockFrames;      /* I2S DMA block, 128..512 */
  int core;             /* core to pin the audio task to */
} mc_esp32_config;

/* Starts an audio task that owns the synth. Returns 0 on success.
 * The task renders forever and pulls events from the player. */
int mc_esp32_start(const mc_esp32_config* cfg);

/* Thread-safe: queue a song for the audio task, replacing the current one. */
int mc_esp32_play(const void* midi, size_t len);
void mc_esp32_stop(void);
int  mc_esp32_voices(void);
int  mc_esp32_load_bank(const void* bank, size_t len);

#ifdef __cplusplus
}
#endif
