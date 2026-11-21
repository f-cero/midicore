/*
 * midicore benchmark -- ESP32-S3. Auto-starts, loops forever, prints to
 * Serial @115200. Every number here is measured on the chip, not modelled.
 *
 * What it answers: how much of one core the synth costs at 32 kHz with the
 * band-limited mip levels enabled, versus the old single-table path, and
 * whether putting the mip tables in PSRAM instead of internal SRAM matters.
 */
#include <Arduino.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>
#include "midicore.h"

extern "C" {
extern const uint8_t  bank_gm[];
extern const uint32_t bank_gm_len;
extern const uint8_t  song_route1[];
extern const uint32_t song_route1_len;
}

#define BLK     128          /* frames per block == one I2S DMA buffer */
#define INTMEM  0
#define PSRMEM  1
#define VERSION "1.1.0"

static int16_t  g_buf[BLK * 2];
static double   g_mhz = 240.0;
static uint32_t g_run = 0;

static inline uint32_t cc(void) { return esp_cpu_get_cycle_count(); }

struct Variant {
  const char* tag;
  int sr, voices, mipLevels, mipMem, bankInRam, stress;
};

struct Result {
  bool   ok;
  int    derived, voicesActive;
  size_t tableKB;
  double cycFrame, pctCore, xRT, maxBlkUs, deadlinePct, initMs, mipMs;
  char   note[72];
};

static Result runOne(const Variant& v)
{
  Result r;
  memset(&r, 0, sizeof r);
  snprintf(r.note, sizeof r.note, "ok");

  mc_synth* s = mc_create(v.sr, v.voices);
  if (!s) { snprintf(r.note, sizeof r.note, "mc_create failed"); return r; }

  void* bankCopy = NULL;
  const uint8_t* bankPtr = bank_gm;
  if (v.bankInRam) {
    bankCopy = heap_caps_malloc(bank_gm_len, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (bankCopy) { memcpy(bankCopy, bank_gm, bank_gm_len); bankPtr = (const uint8_t*)bankCopy; }
    else snprintf(r.note, sizeof r.note, "bank copy failed, flash used");
  }

  int64_t t0 = esp_timer_get_time();
  int rc = mc_load_bank(s, bankPtr, bank_gm_len);
  int64_t t1 = esp_timer_get_time();
  r.initMs = (t1 - t0) / 1000.0;
  if (rc != MC_OK) {
    snprintf(r.note, sizeof r.note, "load_bank %d (%s)", rc, mc_last_error(s));
    mc_destroy(s); if (bankCopy) free(bankCopy);
    return r;
  }

  void* mipBuf = NULL;
  if (v.mipLevels > 0) {
    size_t bytes = (size_t)512 * 101 * v.mipLevels;
    uint32_t caps = MALLOC_CAP_8BIT | (v.mipMem == PSRMEM ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    mipBuf = heap_caps_malloc(bytes, caps);
    if (mipBuf) {
      int64_t a = esp_timer_get_time();
      r.derived = mc_build_mips(s, mipBuf, bytes);
      int64_t b = esp_timer_get_time();
      r.mipMs = (b - a) / 1000.0;
      r.tableKB = (size_t)r.derived * 512 / 1024;
      if (r.derived < v.mipLevels)
        snprintf(r.note, sizeof r.note, "only %d of %d levels fit", r.derived, v.mipLevels);
    } else {
      snprintf(r.note, sizeof r.note, "mip alloc failed (%u B)", (unsigned)bytes);
    }
  }

  mc_song* g = NULL;
  if (!v.stress) {
    g = mc_song_load(song_route1, song_route1_len, v.sr);
    if (!g) {
      snprintf(r.note, sizeof r.note, "song_load failed");
      mc_destroy(s); if (bankCopy) free(bankCopy); if (mipBuf) free(mipBuf);
      return r;
    }
    mc_player_start(s, g);
    mc_player_set_loop(s, 1);
  } else {
    mc_program_change(s, 0, 48);           /* Strings 1: long sustain */
    for (int i = 0; i < v.voices; i++) mc_note_on(s, 0, 36 + i * 3, 100);
  }

  /* warm up caches and envelopes */
  for (int f = 0; f < v.sr; f += BLK) mc_player_render(s, g_buf, BLK, NULL);

  /* ---- clean cost: interrupts off, 8 x 250 ms, sum cycles / sum frames */
  uint64_t totalCyc = 0;
  uint32_t totalFrm = 0;
  int chunk = v.sr / 4;
  for (int gi = 0; gi < 8; gi++) {
    noInterrupts();
    uint32_t c0 = cc();
    for (int f = 0; f < chunk; f += BLK) mc_player_render(s, g_buf, BLK, NULL);
    uint32_t c1 = cc();
    interrupts();
    totalCyc += (uint32_t)(c1 - c0);
    totalFrm += (uint32_t)chunk;
  }
  r.cycFrame = (double)totalCyc / (double)totalFrm;

  /* ---- jitter: per-block timing with interrupts live, worst case matters */
  uint32_t worst = 0;
  for (int i = 0; i < 256; i++) {
    uint32_t a = cc();
    mc_player_render(s, g_buf, BLK, NULL);
    uint32_t b = cc();
    if (b - a > worst) worst = b - a;
  }

  double hz = g_mhz * 1e6;
  r.pctCore     = r.cycFrame * v.sr / hz * 100.0;
  r.xRT         = hz / (r.cycFrame * v.sr);
  r.maxBlkUs    = worst / g_mhz;
  r.deadlinePct = r.maxBlkUs * (double)v.sr / ((double)BLK * 1e6) * 100.0;
  r.voicesActive = mc_active_voices(s);
  r.ok = true;

  if (g) { mc_player_stop(s); mc_song_free(g); }
  mc_destroy(s);
  if (bankCopy) free(bankCopy);
  if (mipBuf)   free(mipBuf);
  return r;
}

static void header(void)
{
  Serial.println();
  Serial.println("config                     sr voi   KB  cyc/frm  %core    xRT  maxblk   dl%  voi  init   mips");
  Serial.println("------------------------- ----- --- ---- -------- ------ ------ ------- ----- ---- ----- ------");
}

static void printRow(const Variant& v, const Result& r)
{
  if (!r.ok) {
    Serial.printf("%-25s %5d %3d   failed: %s\n", v.tag, v.sr, v.voices, r.note);
    return;
  }
  Serial.printf("%-25s %5d %3d %4u %8.2f %5.2f%% %6.1f %6.1fus %4.1f%% %4d %5.1f %6.1f",
                v.tag, v.sr, v.voices, (unsigned)r.tableKB, r.cycFrame, r.pctCore,
                r.xRT, r.maxBlkUs, r.deadlinePct, r.voicesActive, r.initMs, r.mipMs);
  if (strcmp(r.note, "ok")) Serial.printf("  [%s]", r.note);
  Serial.println();
}

/* FNV-1a over 30 s of rendered PCM. Must match the host build byte for byte:
 * proves the on-chip engine produces the same audio as the reference renderer. */
static uint32_t pcmHash30s(void)
{
  mc_synth* s = mc_create(32000, 16);
  if (!s) return 0;
  if (mc_load_bank(s, bank_gm, bank_gm_len) != MC_OK) { mc_destroy(s); return 0; }
  mc_song* g = mc_song_load(song_route1, song_route1_len, 32000);
  if (!g) { mc_destroy(s); return 0; }
  mc_player_start(s, g);
  mc_player_set_loop(s, 0);
  uint32_t h = 2166136261u, frames = 32000u * 30u;
  for (uint32_t f = 0; f < frames; f += BLK) {
    mc_player_render(s, g_buf, BLK, NULL);
    const uint8_t* b = (const uint8_t*)g_buf;
    for (int i = 0; i < BLK * 4; i++) { h ^= b[i]; h *= 16777619u; }
  }
  mc_player_stop(s);
  mc_song_free(g);
  mc_destroy(s);
  return h;
}

static void runAll(void)
{
  Serial.println();
  Serial.println("================================================================================");
  Serial.printf(" midicore benchmark %s   run #%lu   %.0f MHz\n", VERSION, (unsigned long)++g_run, g_mhz);
  Serial.printf(" chip=%s cores=%d  flash=%u KB  psram=%u KB free=%u KB\n",
                ESP.getChipModel(), ESP.getChipCores(),
                (unsigned)(ESP.getFlashChipSize() / 1024),
                (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  Serial.printf(" sram free=%u KB  largest block=%u KB   bank=%u B  midi=%u B\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)bank_gm_len, (unsigned)song_route1_len);
  Serial.println("================================================================================");

  static const Variant V[] = {
    { "A no-mips  flash-bank", 32000, 16, 0, INTMEM, 0, 0 },
    { "B no-mips  ram-bank",   32000, 16, 0, INTMEM, 1, 0 },
    { "C mips-1   ram",        32000, 16, 1, INTMEM, 0, 0 },
    { "D mips-3   ram",        32000, 16, 3, INTMEM, 0, 0 },
    { "E mips-3   psram",      32000, 16, 3, PSRMEM, 0, 0 },
    { "F no-mips  flash 44k",  44100, 16, 0, INTMEM, 0, 0 },
    { "G mips-3   ram    44k", 44100, 16, 3, INTMEM, 0, 0 },
    { "H stress16 voices mips", 32000, 16, 3, INTMEM, 0, 1 },
    { "I stress32 voices mips", 32000, 32, 3, INTMEM, 0, 1 },
  };
  const int n = sizeof V / sizeof V[0];
  Result res[16];

  header();
  for (int i = 0; i < n; i++) {
    res[i] = runOne(V[i]);
    printRow(V[i], res[i]);
  }

  Serial.println();
  if (res[0].ok && res[3].ok) {
    Serial.printf(" MIPS COST   A->D  cyc/frame %+.1f%%   core %.2f%% -> %.2f%%   xRT %.1f -> %.1f\n",
                  100.0 * (res[3].cycFrame / res[0].cycFrame - 1.0),
                  res[0].pctCore, res[3].pctCore, res[0].xRT, res[3].xRT);
    Serial.printf("             absolute cost of mips: %.2f%% of one core at 32 kHz\n",
                  res[3].pctCore - res[0].pctCore);
  }
  if (res[1].ok && res[3].ok)
    Serial.printf(" FLASH vs RAM   ram-bank/no-mips %.2f%% vs mips-3/ram %.2f%% of a core\n",
                  res[1].pctCore, res[3].pctCore);
  if (res[4].ok)
    Serial.printf(" PSRAM PENALTY  mips-3 psram %.2f%% vs ram %.2f%% of a core (%+.0f%%)\n",
                  res[4].pctCore, res[3].pctCore,
                  100.0 * (res[4].pctCore / res[3].pctCore - 1.0));
  if (res[7].ok && res[8].ok)
    Serial.printf(" VOICES         16 distinct voices %.2f%%  /  32 voices %.2f%% of a core\n",
                  res[7].pctCore, res[8].pctCore);

  uint32_t h = pcmHash30s();
  Serial.printf("\n INTEGRITY  pcm_hash_30s=0x%08X  (host reference: 0xE9D05B19)\n", (unsigned)h);
  Serial.println("================================================================================");
  Serial.flush();
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  g_mhz = (double)ESP.getCpuFreqMHz();
  runAll();
}

void loop()
{
  delay(12000);
  runAll();
}
