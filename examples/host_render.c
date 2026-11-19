/*
 * host_render.c -- render a MIDI file to a WAV on the PC, using the exact
 * engine that runs on the target. Same call sequence a firmware would use.
 *
 *   host_render bank.syn song.mid out.wav [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "midicore.h"

static void* slurp(const char* path, size_t* n) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  void* p = malloc((size_t)sz);
  if (!p || fread(p, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(p); return NULL; }
  fclose(f);
  *n = (size_t)sz;
  return p;
}

static void wav_hdr(FILE* f, uint32_t frames, int rate) {
  const uint32_t bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  uint32_t c = 36 + bytes; fwrite(&c, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  uint32_t fmt = 16; fwrite(&fmt, 4, 1, f);
  uint16_t pcm = 1, ch = 2; fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
  uint32_t r = (uint32_t)rate; fwrite(&r, 4, 1, f);
  uint32_t br = (uint32_t)rate * 4; fwrite(&br, 4, 1, f);
  uint16_t ba = 4, bits = 16; fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s bank.syn song.mid out.wav [seconds] [rate] [voices]\n", argv[0]);
    return 1;
  }
  const int rate   = argc > 5 ? atoi(argv[5]) : 32000;
  const int voices = argc > 6 ? atoi(argv[6]) : 16;

  size_t bn = 0, mn = 0;
  void* bank = slurp(argv[1], &bn);
  void* midi = slurp(argv[2], &mn);
  if (!bank || !midi) return 1;

  mc_synth* s = mc_create(rate, voices);
  if (!s) { fprintf(stderr, "mc_create failed\n"); return 1; }
  if (mc_load_bank(s, bank, bn) != MC_OK) {
    fprintf(stderr, "bank rejected: %s\n", mc_last_error(s));
    return 1;
  }
  mc_song* g = mc_song_load(midi, mn, rate);
  if (!g) { fprintf(stderr, "midi parse failed\n"); return 1; }

  double secs = argc > 4 ? atof(argv[4]) : mc_song_frames(g) / (double)rate + 2.0;
  if (secs > 600) secs = 600;
  const uint32_t total = (uint32_t)(secs * rate);

  FILE* out = fopen(argv[3], "wb");
  wav_hdr(out, total, rate);

  static int16_t buf[512 * 2];
  uint32_t done = 0, peak = 0;
  double sumSq = 0;
  const int flood = (voices > 2 && mc_song_tracks(g) >= 2) && 0;  /* keep all tracks */
  (void)flood;
  mc_player_start(s, g);
  int finished = 0;

  while (done < total) {
    uint32_t n = total - done;
    if (n > 512) n = 512;
    mc_player_render(s, buf, (int)n, &finished);
    for (uint32_t i = 0; i < n * 2; i++) {
      const int v = buf[i] < 0 ? -buf[i] : buf[i];
      if ((uint32_t)v > peak) peak = (uint32_t)v;
      sumSq += (double)buf[i] * buf[i];
    }
    fwrite(buf, 2, n * 2, out);
    done += n;
    if (finished) {
      const uint32_t pad = (uint32_t)(0.25 * rate);
      static int16_t zero[512 * 2];
      uint32_t z = 0;
      while (z < pad) {
        uint32_t k = pad - z > 512 ? 512 : pad - z;
        fwrite(zero, 2, k * 2, out);
        z += k;
      }
      break;
    }
  }
  fclose(out);

  const double rms = (done > 0) ? (sumSq / ((double)done * 2)) : 0.0;
  fprintf(stderr, "rendered %.2f s  peak %.3f  rms %.4f  voices_peak_state=%d\n",
          done / (double)rate, peak / 32768.0, sqrt(rms), mc_active_voices(s));
  fprintf(stderr, "song: %d tracks, %.2f s notated\n",
          mc_song_tracks(g), mc_song_frames(g) / (double)rate);

  mc_player_stop(s);
  mc_song_free(g);
  mc_destroy(s);
  free(bank);
  free(midi);
  return 0;
}
