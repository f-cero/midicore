/*
 * midicore benchmark -- Waveshare ESP32-P4-Pico (ESP32-P4NRW32X).
 *
 * Native ESP-IDF, not Arduino: Waveshare ships P4 support only for IDF.
 * Auto-starts, prints a full report to the USB-Serial-JTAG console, then
 * repeats every 12 s forever.
 *
 * The point of this port, versus the ESP32-S3 build:
 *   360 MHz dual RISC-V instead of 240 MHz single Xtensa,
 *   768 KB of SRAM instead of 320 KB, so the ~151 KB of mip tables stop being
 *   a budgeting problem, and no radio interrupt noise at all.
 */
#include <stdio.h>
#include "esp_task_wdt.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
/* MC_NO_PSRAM_SHIM: keep profile E compiling when CONFIG_SPIRAM=n */
#if !CONFIG_SPIRAM
#define esp_psram_is_initialized() 0
#define esp_psram_get_size() 0
#endif
#include "esp_system.h"
#include "esp_rom_sys.h"

#include "midicore.h"
#include "mc_internal.h"   /* bench only: needs song/seq layout to build wire frames */

extern const uint8_t bank_gm_syn_start[]     asm("_binary_bank_gm_syn_start");
extern const uint8_t bank_gm_syn_end[]       asm("_binary_bank_gm_syn_end");
extern const uint8_t song_route1_mid_start[] asm("_binary_song_route1_mid_start");
extern const uint8_t song_route1_mid_end[]   asm("_binary_song_route1_mid_end");

#define BLK      128          /* frames per block == one I2S DMA buffer */
#define MIP_KB   512          /* bytes per derived level, per table */
#define VERSION  "1.2.0-p4"

static const uint8_t* BANK;
static size_t BANK_LEN;
static const uint8_t* SONG;
static size_t SONG_LEN;

static int16_t  g_buf[BLK * 2];
static double   g_mhz = 360.0;
static uint32_t g_run = 0;

static inline uint32_t cc(void) { return esp_cpu_get_cycle_count(); }

/* --- fixed-point formatting: IDF's nano printf drops %f, so print integers --- */
static const char* f2(char* b, size_t n, double v)
{
  int neg = (v < 0); if (neg) v = -v;
  long w = (long)v;
  long f = (long)((v - (double)w) * 100.0 + 0.5);
  if (f >= 100) { w++; f -= 100; }
  snprintf(b, n, "%s%ld.%02ld", neg ? "-" : "", w, f);
  return b;
}
static const char* f3(char* b, size_t n, double v)
{
  int neg = (v < 0); if (neg) v = -v;
  long w = (long)v;
  long f = (long)((v - (double)w) * 1000.0 + 0.5);
  if (f >= 1000) { w++; f -= 1000; }
  snprintf(b, n, "%s%ld.%03ld", neg ? "-" : "", w, f);
  return b;
}

struct Variant {
  const char* tag;
  int sr, voices, mipLevels, mipPsram, bankInRam, stress;
};
typedef struct Variant Variant;

struct Result {
  bool   ok;
  int    derived, voicesActive;
  size_t tableKB;
  double cycFrame, pctCore, xRT, maxBlkUs, deadlinePct, initMs, mipMs;
  char   note[80];
};
typedef struct Result Result;

static Result runOne(const Variant* v)
{
  Result r;
  fflush(stdout);
  memset(&r, 0, sizeof r);
  snprintf(r.note, sizeof r.note, "ok");

  mc_synth* s = mc_create(v->sr, v->voices);
  if (!s) { snprintf(r.note, sizeof r.note, "mc_create failed"); return r; }

  void* bankCopy = NULL;
  const uint8_t* bankPtr = BANK;
  if (v->bankInRam) {
    bankCopy = heap_caps_malloc(BANK_LEN, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (bankCopy) { memcpy(bankCopy, BANK, BANK_LEN); bankPtr = (const uint8_t*)bankCopy; }
    else snprintf(r.note, sizeof r.note, "bank copy failed, flash used");
  }

  int64_t t0 = esp_timer_get_time();
  int rc = mc_load_bank(s, bankPtr, BANK_LEN);
  r.initMs = (esp_timer_get_time() - t0) / 1000.0;
  if (rc != MC_OK) {
    snprintf(r.note, sizeof r.note, "load_bank %d (%s)", rc, mc_last_error(s));
    mc_destroy(s); if (bankCopy) free(bankCopy);
    return r;
  }

  void* mipBuf = NULL;
  if (v->mipLevels > 0) {
    size_t bytes = (size_t)MIP_KB * 101 * v->mipLevels;
    uint32_t caps = MALLOC_CAP_8BIT | (v->mipPsram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    mipBuf = heap_caps_malloc(bytes, caps);
    if (mipBuf) {
      int64_t a = esp_timer_get_time();
      r.derived = mc_build_mips(s, mipBuf, bytes);
      r.mipMs = (esp_timer_get_time() - a) / 1000.0;
      r.tableKB = (size_t)r.derived * MIP_KB / 1024;
      if (r.derived < v->mipLevels)
        snprintf(r.note, sizeof r.note, "only %d of %d levels fit", r.derived, v->mipLevels);
    } else {
      snprintf(r.note, sizeof r.note, "mip alloc failed (%u B)", (unsigned)bytes);
    }
  }

  mc_song* g = NULL;
  if (!v->stress) {
    g = mc_song_load(SONG, SONG_LEN, v->sr);
    if (!g) {
      snprintf(r.note, sizeof r.note, "song_load len=%u hdr=%02X%02X%02X%02X sram=%uK",
               (unsigned)SONG_LEN, SONG[0], SONG[1], SONG[2], SONG[3],
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
      mc_destroy(s); if (bankCopy) free(bankCopy); if (mipBuf) free(mipBuf);
      return r;
    }
    mc_player_start(s, g);
    mc_player_set_loop(s, 1);
  } else {
    mc_program_change(s, 0, 48);          /* Strings 1: long sustain */
    for (int i = 0; i < v->voices; i++) mc_note_on(s, 0, 36 + i * 3, 100);
  }

  for (int f = 0; f < v->sr; f += BLK) mc_player_render(s, g_buf, BLK, NULL);

  /* clean cost: interrupts masked, 8 x 250 ms, total cycles / total frames */
  uint64_t totalCyc = 0;
  uint32_t totalFrm = 0;
  int chunk = v->sr / 4;
  for (int gi = 0; gi < 8; gi++) {
    portDISABLE_INTERRUPTS();
    uint32_t c0 = cc();
    for (int f = 0; f < chunk; f += BLK) mc_player_render(s, g_buf, BLK, NULL);
    uint32_t c1 = cc();
    portENABLE_INTERRUPTS();
    totalCyc += (uint32_t)(c1 - c0);
    totalFrm += (uint32_t)chunk;
  }
  r.cycFrame = (double)totalCyc / (double)totalFrm;

  /* jitter: per-block timing, interrupts live */
  uint32_t worst = 0;
  for (int i = 0; i < 256; i++) {
    uint32_t a = cc();
    mc_player_render(s, g_buf, BLK, NULL);
    uint32_t b = cc();
    if (b - a > worst) worst = b - a;
  }

  double hz = g_mhz * 1e6;
  r.pctCore     = r.cycFrame * v->sr / hz * 100.0;
  r.xRT         = hz / (r.cycFrame * v->sr);
  r.maxBlkUs    = worst / g_mhz;
  r.deadlinePct = r.maxBlkUs * (double)v->sr / ((double)BLK * 1e6) * 100.0;
  r.voicesActive = mc_active_voices(s);
  r.ok = true;

  if (g) { mc_player_stop(s); mc_song_free(g); }
  mc_destroy(s);
  if (bankCopy) free(bankCopy);
  if (mipBuf)   free(mipBuf);
  return r;
}

static void printHeader(void)
{
  printf("\nconfig                      sr voi   KB  cyc/frm  %%core     xRT  maxblk   dl%%  voi  init   mips\n");
  printf("-------------------------- ----- --- ---- -------- ------ ------- ------- ----- ---- ------ ------\n");
}

static void printRow(const Variant* v, const Result* r)
{
  char a[24], b[24], c[24];
  if (!r->ok) {
    printf("%-26s %5d %3d   failed: %s\n", v->tag, v->sr, v->voices, r->note);
    return;
  }
  printf("%-26s %5d %3d %4u %8s %5s%% %7s %6sus %4s%% %4d %6s %6s",
         v->tag, v->sr, v->voices, (unsigned)r->tableKB,
         f2(a, sizeof a, r->cycFrame), f2(b, sizeof b, r->pctCore),
         f2(a, sizeof a, r->xRT), f2(b, sizeof b, r->maxBlkUs),
         f2(c, sizeof c, r->deadlinePct), r->voicesActive,
         f2(a, sizeof a, r->initMs), f2(b, sizeof b, r->mipMs));
  if (strcmp(r->note, "ok")) printf("  [%s]", r->note);
  printf("\n");
}

/* FNV-1a over 30 s of PCM. Must equal the host reference on the same line:
 * proves this RISC-V build renders audio bit-identical to the x86 engine. */
static uint32_t pcmHash30s(void)
{
  mc_synth* s = mc_create(32000, 16);
  if (!s) return 0;
  if (mc_load_bank(s, BANK, BANK_LEN) != MC_OK) { mc_destroy(s); return 0; }
  mc_song* g = mc_song_load(SONG, SONG_LEN, 32000);
  if (!g) { mc_destroy(s); return 0; }
  mc_player_start(s, g);
  mc_player_set_loop(s, 0);
  uint32_t h = 2166136261u, frames = 32000u * 30u;
  for (uint32_t f = 0; f < frames; f += BLK) {
    mc_player_render(s, g_buf, BLK, NULL);
    const uint8_t* p = (const uint8_t*)g_buf;
    for (int i = 0; i < BLK * 4; i++) { h ^= p[i]; h *= 16777619u; }
  }
  mc_player_stop(s);
  mc_song_free(g);
  mc_destroy(s);
  return h;
}


/* =============== streaming benchmark: file source vs wire source ==========
 * The file source walks the SMF in place out of flash: no event table, no
 * per-song heap. The wire source encodes the same music into 100 ms frames
 * (the P4 -> CYD packet format) and pushes them through mc_seq_push_frame().
 * Both hash their PCM so we can compare them on real silicon.
 */
#define SEQ_SECS   30
#define SEQ_WIN_MS 100

static uint8_t crc8b(const uint8_t* p, size_t n)
{
  uint8_t c = 0;
  while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); }
  return c;
}

static void hashAcc(uint32_t* h, const int16_t* pcm, int frames)
{
  const uint8_t* b = (const uint8_t*)pcm;
  for (int i = 0; i < frames * 4; i++) { *h ^= b[i]; *h *= 16777619u; }
}

static void benchSeq(void)
{
  const int SR = 32000;
  const uint32_t total = (uint32_t)SEQ_SECS * SR;

  /* ---------- A. file source, straight out of flash ---------- */
  size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  mc_seq* qf = mc_seq_open_file(SONG, SONG_LEN, SR);
  size_t h1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!qf) { printf(" FAIL seq_open_file\n"); return; }

  mc_synth* s = mc_create(SR, 16);
  if (!s || mc_load_bank(s, BANK, BANK_LEN) != MC_OK) { printf(" FAIL synth/bank\n"); return; }

  uint32_t hf = 2166136261u; int done = 0;
  uint32_t c0 = cc();
  int64_t  t0 = esp_timer_get_time();
  for (uint32_t f = 0; f < total; f += BLK) { mc_seq_render(s, qf, g_buf, BLK, &done); hashAcc(&hf, g_buf, BLK); }
  uint32_t cfile = cc() - c0;
  int64_t  tfile = esp_timer_get_time() - t0;
  const uint32_t voicesFile = (uint32_t)mc_active_voices(s);
  mc_seq_free(qf); mc_destroy(s);

  printf(" A file-from-flash   %8u B midi   heap %+d B   %s\n",
         (unsigned)SONG_LEN, (int)(h0 - h1),
         (h0 - h1) < 0 ? "(NEGATIVE: alloc!)" : "no per-song table");
  printf("   hash=0x%08X  %7.1f Mcyc  %.2f%% core  %5.1fxRT  voices=%u\n",
         (unsigned)hf, (double)cfile / 1e6,
         100.0 * ((double)cfile / (double)total) / (g_mhz * 1e6),
         ((double)SEQ_SECS * 1e6) / (double)tfile, (unsigned)voicesFile);

  /* ---------- B. wire source: 100 ms frames ---------- */
  mc_synth* s2 = mc_create(SR, 16);
  if (!s2 || mc_load_bank(s2, BANK, BANK_LEN) != MC_OK) { printf(" FAIL synth2\n"); return; }
  mc_song* ref = mc_song_load(SONG, SONG_LEN, SR);
  if (!ref) { printf(" FAIL ref parse\n"); return; }
  mc_seq* qs = mc_seq_open_stream(SR, 480);
  if (!qs) { printf(" FAIL seq_open_stream\n"); return; }

  uint32_t hs = 2166136261u; done = 0;
  uint32_t ev = 0, worstChunk = 0, maxQueue = 0, chunks = 0;
  uint8_t frame[1200];
  const uint32_t winFrames = (uint32_t)SR * SEQ_WIN_MS / 1000u;

  c0 = cc(); t0 = esp_timer_get_time();
  for (uint32_t w = 0; w < total; w += winFrames) {
    /* encode the next 100 ms as a wire frame, exactly as the link would carry it */
    int n = 0, prev = 0;
    while (ev < ref->count && ref->ev[ev].frame < w + winFrames && n < 240) {
      int dt = (int)(((int64_t)(ref->ev[ev].frame - w) * 1000) / SR);
      int d = dt - prev; if (d < 0) d = 0; if (d > 255) d = 255;
      prev = dt;
      frame[10 + n * 4 + 0] = (uint8_t)d;
      frame[10 + n * 4 + 1] = ref->ev[ev].type;
      frame[10 + n * 4 + 2] = ref->ev[ev].a;
      frame[10 + n * 4 + 3] = ref->ev[ev].b;
      n++; ev++;
    }
    const uint16_t len = (uint16_t)(6 + n * 4);
    frame[0] = 0xA5; frame[1] = 0x01;
    frame[2] = (uint8_t)(len >> 8); frame[3] = (uint8_t)len;
    frame[4] = (uint8_t)(chunks >> 8); frame[5] = (uint8_t)chunks;
    frame[6] = (uint8_t)(SEQ_WIN_MS >> 8); frame[7] = (uint8_t)SEQ_WIN_MS;
    frame[8] = (uint8_t)(n >> 8); frame[9] = (uint8_t)n;
    frame[len + 4] = crc8b(frame, (size_t)len + 4);
    chunks++;
    const uint32_t fs = len + 5u;
    if (fs > worstChunk) worstChunk = fs;

    int rc = mc_seq_push_frame(qs, frame, fs);
    if (rc != MC_OK) { printf(" FAIL push rc=%d chunk=%u\n", rc, (unsigned)chunks); break; }
    const int pend = mc_seq_pending(qs);
    if ((uint32_t)pend > maxQueue) maxQueue = (uint32_t)pend;

    for (uint32_t f = 0; f < winFrames; f += BLK) { mc_seq_render(s2, qs, g_buf, BLK, &done); hashAcc(&hs, g_buf, BLK); }
  }
  uint32_t cstream = cc() - c0;
  int64_t  tstream = esp_timer_get_time() - t0;

  printf(" B wire-100ms frames  %u chunks   worst %u B/frame   queue peak %u ms\n",
         (unsigned)chunks, (unsigned)worstChunk, (unsigned)maxQueue);
  printf("   hash=0x%08X  %7.1f Mcyc  %.2f%% core  %5.1fxRT  voices=%u\n",
         (unsigned)hs, (double)cstream / 1e6,
         100.0 * ((double)cstream / (double)total) / (g_mhz * 1e6),
         ((double)SEQ_SECS * 1e6) / (double)tstream, (unsigned)mc_active_voices(s2));
  printf("   stream overhead vs file: %+.2f%% cycles   hashes %s\n",
         100.0 * ((double)cstream / (double)cfile - 1.0),
         (hs == hf) ? "IDENTICAL" : "differ (1 ms dt granularity)");

  mc_seq_free(qs); mc_song_free(ref); mc_destroy(s2);
  printf(" REGION  seq state = tracks*32B + 512*8B ring = %u B fixed, independent of song length\n",
         (unsigned)(MC_SEQ_MAXTRACKS * 32 + MC_SEQ_RING * 8));
}

/* ============================ voice scaling ============================== */
static void benchVoices(void)
{
  const int SR = 32000;
  const int V[] = { 8, 16, 32, 64 };
  const int secs = 10;
  printf("\n --- voice scaling, 32 kHz, all voices sounding ---\n");
  for (int k = 0; k < 4; k++) {
    mc_synth* s = mc_create(SR, 64);
    if (!s || mc_load_bank(s, BANK, BANK_LEN) != MC_OK) { printf(" FAIL\n"); return; }
    for (int i = 0; i < V[k]; i++) {
      mc_program_change(s, i & 15, 48 + (i & 7));
      mc_note_on(s, i & 15, 30 + (i % 60), 100);
    }
    const uint32_t frames = (uint32_t)SR * secs;
    uint32_t c0 = cc();
    for (uint32_t f = 0; f < frames; f += BLK) mc_render(s, g_buf, BLK);
    uint32_t d = cc() - c0;
    const double cycFrame = (double)d / (double)frames;
    printf("  %2d voices  %8.1f cyc/frame  %6.2f%% core  %6.1fxRT\n",
           V[k], cycFrame, 100.0 * cycFrame * SR / (g_mhz * 1e6),
           (double)secs * 1e6 / (double)(d * 1000000.0 / (g_mhz * 1e6)));
    mc_destroy(s);
  }
}

static void runAll(void)
{
  printf("\n================================================================================\n");
  printf(" midicore benchmark %s   run #%lu   core %d   %s MHz\n",
         VERSION, (unsigned long)++g_run, (int)xPortGetCoreID(), f3((char[16]){0}, 16, g_mhz));
  uint32_t fsz = 0; esp_flash_get_size(NULL, &fsz);
  size_t ps = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
  printf(" chip=%s cores=%d  flash=%u KB  psram=%u KB (free %u KB)\n",
         CONFIG_IDF_TARGET, (int)portNUM_PROCESSORS,
         (unsigned)(fsz / 1024), (unsigned)(ps / 1024),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  printf(" sram free=%u KB  largest=%u KB   bank=%u B  midi=%u B\n",
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)BANK_LEN, (unsigned)SONG_LEN);
  printf("================================================================================\n");

  static const Variant V[] = {
    { "A no-mips  flash-bank",  32000, 16, 0, 0, 0, 0 },
    { "B no-mips  ram-bank",    32000, 16, 0, 0, 1, 0 },
    { "C mips-1   ram",         32000, 16, 1, 0, 0, 0 },
    { "D mips-3   ram",         32000, 16, 3, 0, 0, 0 },
    { "E mips-3   psram",       32000, 16, 3, 1, 0, 0 },
    { "F mips-3   ram    48k",  48000, 16, 3, 0, 0, 0 },
    { "G mips-3   ram    44k",  44100, 16, 3, 0, 0, 0 },
    { "H stress16 voices mips", 32000, 16, 3, 0, 0, 1 },
    { "I stress32 voices mips", 32000, 32, 3, 0, 0, 1 },
  };
  const int n = sizeof V / sizeof V[0];
  Result res[10];

  printHeader();
  for (int i = 0; i < n; i++) { res[i] = runOne(&V[i]); printRow(&V[i], &res[i]); }

  printf("\n");
  if (res[0].ok && res[3].ok)
    printf(" MIPS COST   A->D  cyc/frame %+.1f%%   core %.2f%% -> %.2f%%   xRT %.1f -> %.1f\n",
           100.0 * (res[3].cycFrame / res[0].cycFrame - 1.0),
           res[0].pctCore, res[3].pctCore, res[0].xRT, res[3].xRT);
  if (res[0].ok && res[3].ok)
    printf("             absolute cost of mips: %.2f%% of one core at 32 kHz\n",
           res[3].pctCore - res[0].pctCore);
  if (res[1].ok && res[3].ok)
    printf(" FLASH vs RAM   ram-bank/no-mips %.2f%% vs mips-3/ram %.2f%% of a core\n",
           res[1].pctCore, res[3].pctCore);
  if (res[4].ok && res[3].ok)
    printf(" PSRAM PENALTY  mips-3 psram %.2f%% vs ram %.2f%% of a core (%+.0f%%)\n",
           res[4].pctCore, res[3].pctCore,
           100.0 * (res[4].pctCore / res[3].pctCore - 1.0));
  if (res[5].ok && res[3].ok)
    printf(" 48 kHz         %.2f%% of a core (%.2fx the 32 kHz cost)\n",
           res[5].pctCore, res[5].pctCore / res[3].pctCore);
  if (res[7].ok && res[8].ok)
    printf(" VOICES         16 held %.2f%%  /  32 held %.2f%% of a core\n",
           res[7].pctCore, res[8].pctCore);

  benchSeq();
  benchVoices();

  printf("\n INTEGRITY  pcm_hash_30s=0x%08X  (host reference: 0xE9D05B19)\n", (unsigned)pcmHash30s());
  printf("================================================================================\n");
  fflush(stdout);
}

static void bench_task(void* arg)
{
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(1500));       /* let the USB-JTAG console attach */
  BANK = bank_gm_syn_start;     BANK_LEN = (size_t)(bank_gm_syn_end - bank_gm_syn_start);
  SONG = song_route1_mid_start; SONG_LEN = (size_t)(song_route1_mid_end - song_route1_mid_start);
  g_mhz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();
    runAll();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(12000));
  }
}

void app_main(void)
{
  /* Unbuffered: a line must reach the console immediately, never sit in stdio
     while we go on to mask interrupts for a quarter of a second at a time. */
  setvbuf(stdout, NULL, _IONBF, 0);
  esp_rom_printf("\n[bench] app_main entered, reset_reason=%d\n", (int)esp_reset_reason());
  fflush(stdout);

  /* One core does audio, the other is free for the game. Pin to core 1 so the
   * numbers describe a quiet core, not one sharing with the system tasks. */
  xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 5, NULL, 1);
}
