/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 midicore contributors
 */
#include <stdio.h>
#include <stdlib.h>
#include "mc_internal.h"
int main(int argc, char** argv) {
  size_t n; FILE* f = fopen(argv[1], "rb"); fseek(f,0,SEEK_END); n=ftell(f); fseek(f,0,SEEK_SET);
  void* b = malloc(n); fread(b,1,n,f); fclose(f);
  mc_synth* s = mc_create(32000, 16);
  printf("bank load -> %d (%s)\n", mc_load_bank(s,b,n), mc_last_error(s));
  printf("hasBank=%d numPatches=%u tables=%u\n", s->hasBank, s->bank.numPatches, s->bank.numTables);
  const mc_patch* p = mc_bank_patch(&s->bank, 0);
  printf("patch0: opCount=%d algo=%d gain=%d velDepth=%d\n", p->opCount, p->algorithm, p->gain, p->velDepth);
  for (int k=0;k<p->opCount;k++)
    printf("  op%d tbl=%d mul=%d lvl=%d atk=%d dec=%d sus=%d rel=%d out=%d\n",
      k,p->op[k].table,p->op[k].mul,p->op[k].level,p->op[k].attack,p->op[k].decay,
      p->op[k].sustain,p->op[k].release,p->op[k].out);
  printf("table0[0..7] = %d %d %d %d %d %d %d %d\n",
    s->bank.tables[0],s->bank.tables[1],s->bank.tables[2],s->bank.tables[3],
    s->bank.tables[4],s->bank.tables[5],s->bank.tables[6],s->bank.tables[7]);
  printf("attInc[100]=%d decMul[100]=%d sine64[16]=%d\n", g_attInc[100], g_decMul[100], g_sine64[16]);
  mc_program_change(s,0,0);
  mc_note_on(s,0,60,100);
  printf("after note_on: active voices=%d\n", mc_active_voices(s));
  mc_voice* v=&s->voices[0];
  printf("  v.active=%d ch=%d note=%d opCount=%d patch=%p\n", v->active,v->ch,v->note,v->opCount,(void*)v->patch);
  printf("  op0.inc=%u op0.env=%d op0.stage=%d sus=%d\n", v->op[0].inc, v->op[0].env, v->op[0].stage, v->op[0].sus);
  static int16_t buf[64*2];
  for (int blk=0; blk<6; blk++) {
    mc_render(s, buf, 64);
    printf("  blk%d mixL=%d out=%d env=%d stage=%d act=%d\n", blk, s->mixL[0], buf[0],
           v->op[0].env, v->op[0].stage, mc_active_voices(s));
  }
  return 0;
}
