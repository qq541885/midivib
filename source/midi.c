/* midi.c -- Standard MIDI File 解析
 *
 * 处理范围：
 *   - format 0 / 1 / 2 都能读（多轨就是逐轨累积 tick，再全局按 tick 排序）
 *   - tempo meta 0x51 全部收集，tick -> 秒 走分段积分，支持中途变速
 *   - division 为 SMPTE 时基（高字节非 0）时按 fps * ticksPerFrame 换算
 *   - running status
 *   - note on 力度 0 视为 note off
 *   - 没有对应 off 的 on 给 0.3 秒默认时长
 *
 * 不处理（对本用途无意义）：
 *   - 音色库、控制器曲线、pitch bend 的曲线值（只用来定位事件边界）
 *   - RIFF RMID 容器（.rmi），只接受裸 SMF
 */

#include "midi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEMPO   4096
#define MAX_RAW     (MIDI_MAX_NOTES * 2)
#define MAX_FILE_SZ (64 * 1024 * 1024)

typedef struct { uint64_t tick; uint32_t us; } TempoMark;
typedef struct { uint64_t tick; uint8_t status, note, vel; } RawEv;

static TempoMark g_tempo[MAX_TEMPO];
static int       g_ntempo;
static double    g_tpq;
static double    g_smpte_tps;

static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* MIDI 变长整数：每字节 7 位，高位为续读标志 */
static uint32_t varlen(const uint8_t **pp, const uint8_t *end) {
  uint32_t v = 0;
  const uint8_t *p = *pp;
  int guard = 0;
  while (p < end && guard++ < 5) {
    uint8_t b = *p++;
    v = (v << 7) | (uint32_t)(b & 0x7F);
    if (!(b & 0x80)) break;
  }
  *pp = p;
  return v;
}

static int cmp_tempo(const void *a, const void *b) {
  const TempoMark *x = (const TempoMark *)a, *y = (const TempoMark *)b;
  if (x->tick < y->tick) return -1;
  if (x->tick > y->tick) return 1;
  return 0;
}

static int cmp_raw(const void *a, const void *b) {
  const RawEv *x = (const RawEv *)a, *y = (const RawEv *)b;
  if (x->tick < y->tick) return -1;
  if (x->tick > y->tick) return 1;
  /* 同一 tick 上先处理 note off，再处理 note on，这样同音重复不会互相吞掉 */
  int xo = ((x->status & 0xF0) == 0x90 && x->vel > 0) ? 1 : 0;
  int yo = ((y->status & 0xF0) == 0x90 && y->vel > 0) ? 1 : 0;
  return xo - yo;
}

static int cmp_note(const void *a, const void *b) {
  const MidiNote *x = (const MidiNote *)a, *y = (const MidiNote *)b;
  if (x->t < y->t) return -1;
  if (x->t > y->t) return 1;
  return 0;
}

/* tick -> 秒：按 tempo map 分段积分 */
static double tick2sec(uint64_t tick) {
  if (g_smpte_tps > 0.0) return (double)tick / g_smpte_tps;
  if (g_tpq <= 0.0) return 0.0;

  double   sec  = 0.0;
  uint64_t prev = 0;
  uint32_t us   = 500000; /* 默认 120 BPM */

  for (int i = 0; i < g_ntempo; i++) {
    if (g_tempo[i].tick >= tick) break;
    sec  += ((double)(g_tempo[i].tick - prev) / g_tpq) * ((double)us / 1e6);
    prev  = g_tempo[i].tick;
    us    = g_tempo[i].us;
  }
  sec += ((double)(tick - prev) / g_tpq) * ((double)us / 1e6);
  return sec;
}

int midi_load(const char *path, MidiFile *out) {
  memset(out, 0, sizeof(*out));

  FILE *f = fopen(path, "rb");
  if (!f) return -1;

  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long sz = ftell(f);
  if (sz <= 14 || sz > MAX_FILE_SZ) { fclose(f); return -1; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

  uint8_t *d = (uint8_t *)malloc((size_t)sz);
  if (!d) { fclose(f); return -1; }
  size_t got = fread(d, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) { free(d); return -1; }

  const uint8_t *p   = d;
  const uint8_t *end = d + sz;

  if (memcmp(p, "MThd", 4) != 0) { free(d); return -1; }
  uint32_t hlen = rd32(p + 4);
  if (hlen < 6 || p + 8 + hlen > end) { free(d); return -1; }

  out->format   = rd16(p + 8);
  out->tracks   = rd16(p + 10);
  out->division = rd16(p + 12);
  p += 8 + hlen;

  g_ntempo = 0;
  g_tpq = 0.0;
  g_smpte_tps = 0.0;

  if (out->division & 0x8000) {
    int fps = 256 - (out->division >> 8);
    int tpf = out->division & 0xFF;
    if (fps <= 0 || tpf <= 0) { free(d); return -1; }
    g_smpte_tps = (double)fps * (double)tpf;
  } else {
    g_tpq = (double)(out->division ? out->division : 96);
  }
  out->tpq       = g_tpq;
  out->smpte_tps = g_smpte_tps;

  RawEv *raw = (RawEv *)malloc(sizeof(RawEv) * MAX_RAW);
  if (!raw) { free(d); return -1; }
  int nraw = 0;

  /* ---- 扫描所有 track ---- */
  while (p + 8 <= end) {
    if (memcmp(p, "MTrk", 4) != 0) break;
    uint32_t tlen = rd32(p + 4);
    p += 8;
    const uint8_t *te = p + tlen;
    if (te > end) te = end;

    uint64_t t       = 0;
    uint8_t  running = 0;

    while (p < te) {
      t += varlen(&p, te);
      if (p >= te) break;

      uint8_t st;
      uint8_t b = *p;
      if (b & 0x80) {
        st = b;
        p++;
        /* 只有 channel 消息会更新 running status */
        if (st < 0xF0) running = st;
      } else {
        st = running;
        if (!st) break;
      }

      if (st == 0xFF) {
        if (p >= te) break;
        uint8_t  mt = *p++;
        uint32_t ml = varlen(&p, te);
        if (p + ml > te) break;
        if (mt == 0x51 && ml == 3 && g_ntempo < MAX_TEMPO) {
          uint32_t us = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
          g_tempo[g_ntempo].tick = t;
          g_tempo[g_ntempo].us   = us ? us : 500000;
          g_ntempo++;
        }
        p += ml;
        continue;
      }

      if (st == 0xF0 || st == 0xF7) {
        uint32_t sl = varlen(&p, te);
        if (p + sl > te) break;
        p += sl;
        continue;
      }

      uint8_t hi = st & 0xF0;

      if (hi == 0xC0 || hi == 0xD0) {
        if (p + 1 > te) break;
        p += 1;
        continue;
      }

      if (hi == 0x80 || hi == 0x90 || hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
        if (p + 2 > te) break;
        uint8_t d1 = p[0];
        uint8_t d2 = p[1];
        p += 2;

        int on;
        if (hi == 0x90)      on = (d2 > 0);
        else if (hi == 0x80) on = 0;
        else                 continue;   /* A0/B0/E0 不是音符事件 */

        if (nraw < MAX_RAW) {
          raw[nraw].tick   = t;
          raw[nraw].status = st;
          raw[nraw].note   = d1 & 0x7F;
          raw[nraw].vel    = d2 & 0x7F;
          nraw++;
        }
        continue;
      }

      /* 未知 status，跳一个字节防止死循环 */
      p++;
    }
    p = te;
  }

  free(d);

  if (nraw == 0) { free(raw); return -1; }

  qsort(g_tempo, (size_t)g_ntempo, sizeof(TempoMark), cmp_tempo);
  qsort(raw, (size_t)nraw, sizeof(RawEv), cmp_raw);

  out->notes = (MidiNote *)calloc((size_t)nraw, sizeof(MidiNote));
  if (!out->notes) { free(raw); return -1; }

  int     nn = 0;
  int32_t start[16][128];
  for (int c = 0; c < 16; c++)
    for (int n = 0; n < 128; n++)
      start[c][n] = -1;

  for (int i = 0; i < nraw; i++) {
    int ch = raw[i].status & 0x0F;
    int nt = raw[i].note;
    int on = ((raw[i].status & 0xF0) == 0x90 && raw[i].vel > 0);

    if (on) {
      /* 同音重复触发：先把上一个收尾，避免被覆盖丢事件 */
      if (start[ch][nt] >= 0) {
        int si = start[ch][nt];
        out->notes[nn].t    = tick2sec(raw[si].tick);
        out->notes[nn].dur  = 0.05;
        out->notes[nn].note = raw[si].note;
        out->notes[nn].vel  = raw[si].vel;
        out->notes[nn].ch   = (uint8_t)ch;
        nn++;
      }
      start[ch][nt] = i;
    } else if (start[ch][nt] >= 0) {
      int   si = start[ch][nt];
      start[ch][nt] = -1;
      double bt = tick2sec(raw[si].tick);
      double et = tick2sec(raw[i].tick);
      out->notes[nn].t    = bt;
      out->notes[nn].dur  = (et > bt) ? (et - bt) : 0.05;
      out->notes[nn].note = raw[si].note;
      out->notes[nn].vel  = raw[si].vel;
      out->notes[nn].ch   = (uint8_t)ch;
      nn++;
    }
  }

  /* 没有配对的 note on */
  for (int c = 0; c < 16; c++) {
    for (int n = 0; n < 128; n++) {
      if (start[c][n] < 0) continue;
      int si = start[c][n];
      out->notes[nn].t    = tick2sec(raw[si].tick);
      out->notes[nn].dur  = 0.30;
      out->notes[nn].note = raw[si].note;
      out->notes[nn].vel  = raw[si].vel;
      out->notes[nn].ch   = (uint8_t)c;
      nn++;
    }
  }

  free(raw);

  out->count  = nn;
  out->length = 0.0;
  for (int i = 0; i < nn; i++) {
    double e = out->notes[i].t + out->notes[i].dur;
    if (e > out->length) out->length = e;
  }

  qsort(out->notes, (size_t)nn, sizeof(MidiNote), cmp_note);
  return 0;
}

void midi_free(MidiFile *m) {
  if (!m) return;
  free(m->notes);
  m->notes = NULL;
  m->count = 0;
  m->length = 0.0;
}
