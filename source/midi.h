/* midi.h -- 最小可用的 Standard MIDI File 解析器
 *
 * 只做一件事：把一个 .mid 里的音符事件抽成 (开始时间, 时长, 音高, 力度)，
 * 时间单位是秒，并且正确处理 tempo 变化和 SMPTE 时基。
 *
 * 不依赖任何第三方库。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define MIDI_MAX_NOTES 65536

typedef struct {
  double  t;     /* 开始时间，秒 */
  double  dur;   /* 时长，秒 */
  uint8_t note;  /* MIDI 音高 0..127 */
  uint8_t vel;   /* 力度 0..127 */
  uint8_t ch;    /* 通道 0..15 */
} MidiNote;

typedef struct {
  MidiNote *notes;
  int       count;
  double    length;    /* 总时长，秒 */
  uint16_t  format;
  uint16_t  tracks;
  uint16_t  division;
  double    tpq;       /* ticks per quarter；0 表示用了 SMPTE 时基 */
  double    smpte_tps; /* SMPTE 每秒 tick 数 */
} MidiFile;

/* 成功返回 0；notes 由 midi_free 释放 */
int  midi_load(const char *path, MidiFile *out);
void midi_free(MidiFile *m);
