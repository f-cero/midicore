/*
 * esp32_sd_player -- MIDI jukebox on an ESP32-S3 with an SD card.
 *
 *   /bank.syn      instrument bank (66 KB, from tools/sf2_to_bank.py)
 *   /midi/*.mid    songs
 *
 * Wiring: I2S BCLK=5, LRCK=6, DOUT=7 into a PCM5102A / MAX98357A.
 *         SD on SPI: CS=10, MOSI=11, SCK=12, MISO=13.
 */
#include <SPI.h>
#include <SD.h>
#include "midicore.h"
#include "esp32_midicore.h"

static uint8_t*  g_bank = nullptr;
static uint8_t*  g_song = nullptr;

static void* loadFile(const char* path, size_t* outLen) {
  File f = SD.open(path, FILE_READ);
  if (!f) { Serial.printf("[sd] missing %s\n", path); return nullptr; }
  const size_t n = f.size();
  void* p = ps_malloc(n);          /* PSRAM */
  if (!p) { f.close(); return nullptr; }
  const size_t rd = f.read((uint8_t*)p, n);
  f.close();
  if (rd != n) { free(p); return nullptr; }
  *outLen = n;
  return p;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  SPI.begin(12, 13, 11, 10);
  if (!SD.begin(10, SPI, 10000000)) {
    Serial.println("[sd] mount failed");
    return;
  }

  size_t bn = 0;
  g_bank = (uint8_t*)loadFile("/bank.syn", &bn);
  if (!g_bank) { Serial.println("[bank] missing /bank.syn"); return; }
  Serial.printf("[bank] %u bytes\n", (unsigned)bn);

  mc_esp32_config cfg = { 5, 6, 7, 32000, 16, 256, 0 };
  if (mc_esp32_start(&cfg) != 0) { Serial.println("[i2s] start failed"); return; }
  while (mc_esp32_load_bank(g_bank, bn) != 0) delay(10);

  size_t sn = 0;
  g_song = (uint8_t*)loadFile("/midi/theme.mid", &sn);
  if (g_song) mc_esp32_play(g_song, sn);
  Serial.println("[ok] playing");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("[stat] voices=%d\n", mc_esp32_voices());
  }
  delay(10);
}
