/* esp32_midicore.cpp -- I2S output for midicore (ESP32-S3, IDF 5.x / Arduino 3.x). */
#include "esp32_midicore.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static i2s_chan_handle_t s_tx = nullptr;
static mc_synth* s_synth = nullptr;
static mc_esp32_config s_cfg;
static TaskHandle_t s_task = nullptr;

static volatile const void* s_pendingMidi = nullptr;
static volatile size_t s_pendingLen = 0;
static volatile const void* s_pendingBank = nullptr;
static volatile size_t s_pendingBankLen = 0;
static volatile bool s_stopReq = false;

static void audio_task(void*) {
  const int frames = s_cfg.blockFrames;
  int16_t* buf = (int16_t*)malloc((size_t)frames * 2 * sizeof(int16_t));
  if (!buf) { vTaskDelete(nullptr); return; }

  for (;;) {
    if (s_pendingBank) {
      const void* b = (const void*)s_pendingBank;
      const size_t n = s_pendingBankLen;
      s_pendingBank = nullptr;
      mc_load_bank(s_synth, b, n);
    }
    if (s_pendingMidi) {
      const void* m = (const void*)s_pendingMidi;
      const size_t n = s_pendingLen;
      s_pendingMidi = nullptr;
      mc_song* g = mc_song_load(m, n, s_cfg.sampleRate);
      if (g) mc_player_start(s_synth, g);
    }
    if (s_stopReq) { s_stopReq = false; mc_player_stop(s_synth); }

    int done = 0;
    mc_player_render(s_synth, buf, frames, &done);
    size_t written = 0;
    i2s_channel_write(s_tx, buf, (size_t)frames * 2 * sizeof(int16_t), &written,
                      portMAX_DELAY);
  }
}

int mc_esp32_start(const mc_esp32_config* cfg) {
  if (!cfg) return -1;
  s_cfg = *cfg;

  s_synth = mc_create(cfg->sampleRate, cfg->voices);
  if (!s_synth) return -2;

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 6;
  chan.dma_frame_num = cfg->blockFrames;
  if (i2s_new_channel(&chan, &s_tx, nullptr) != ESP_OK) return -3;

  i2s_std_config_t std = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)cfg->sampleRate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)cfg->pinBclk,
      .ws   = (gpio_num_t)cfg->pinLrck,
      .dout = (gpio_num_t)cfg->pinDout,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { false, false, false },
    },
  };
  if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return -4;
  if (i2s_channel_enable(s_tx) != ESP_OK) return -5;

  xTaskCreatePinnedToCore(audio_task, "midicore", 4096, nullptr, 24, &s_task, cfg->core);
  return 0;
}

int mc_esp32_load_bank(const void* bank, size_t len) {
  s_pendingBank = bank; s_pendingBankLen = len; return 0;
}
int mc_esp32_play(const void* midi, size_t len) {
  s_pendingMidi = midi; s_pendingLen = len; return 0;
}
void mc_esp32_stop(void) { s_stopReq = true; }
int  mc_esp32_voices(void) { return s_synth ? mc_active_voices(s_synth) : 0; }
