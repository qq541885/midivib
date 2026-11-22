/* main.c -- MIDI 手柄震动播放器（Switch homebrew，中文图形界面）
 *
 * 做什么：
 *   浏览 SD 卡上的 .mid / .midi 文件，选中后把音符事件按时间轴驱动成
 *   Joy-Con / Pro 手柄的 HD 震动。音高决定震动频率，力度决定振幅。
 *
 * 为什么是做震动而不是放声音：
 *   Switch 的 HD 马达频响覆盖 41.7–626 Hz（低频段），正好对得上
 *   钢琴 A0(27.5Hz) 到 C8(4186Hz) 的对数区间压缩——低音沉、高音尖，
 *   节奏和旋律都能"摸"出来。
 *
 * 中文渲染：Switch 系统 SharedFont，不往 nro 里塞字体。
 * 按键读取：直接用 libnx pad，不经过 SDL 的映射层，避免按钮错位。
 *
 * MIT license.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "midi.h"

/* ------------------------------------------------------------------ */
#define SCR_W   1280
#define SCR_H   720
#define PAD     40

#define ROOT_DIR   "sdmc:/"
#define MAX_ENTRY  512
#define MAX_POLY   64

/* 震动频响边界（libnx 文档值） */
#define VIB_FMIN   41.7f
#define VIB_FMAX   626.0f
#define VIB_FRATIO (VIB_FMAX / VIB_FMIN)

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
  char name[256];
  int  is_dir;
} FsEntry;

static FsEntry g_ent[MAX_ENTRY];
static int     g_nent   = 0;
static int     g_esel   = 0;
static int     g_etop   = 0;
static char    g_cwd[512] = ROOT_DIR;

static MidiFile g_midi;
static int      g_loaded = 0;

enum { MODE_BROWSE = 0, MODE_PLAY, MODE_DONE };

static int      g_mode = MODE_BROWSE;
static int      g_playing = 0;
static uint64_t g_start_ms = 0;
static int      g_next_note = 0;

typedef struct { double end; uint8_t note; uint8_t vel; } Active;
static Active g_act[MAX_POLY];
static int    g_nact = 0;

/* 统计用 */
static int    g_hit_notes = 0;
static float  g_cur_amp = 0.0f;
static float  g_cur_freq = 0.0f;
static char   g_status[256] = "";

/* SDL */
static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static TTF_Font     *g_f24 = NULL;
static TTF_Font     *g_f20 = NULL;
static TTF_Font     *g_f16 = NULL;

/* 震动 */
static HidVibrationDeviceHandle g_vib[2] = { 0, 0 };
static int g_vib_n = 0;

/* ------------------------------------------------------------------ */
/* 系统字体                                                            */
/* ------------------------------------------------------------------ */

static void *g_font_data = NULL;
static size_t g_font_size = 0;

static int load_system_font(void) {
  const u32 types = PlSharedFontType_Standard | PlSharedFontType_ChineseSimplified;
  if (R_FAILED(plInitialize(types))) return -1;

  PlFontData fd;
  if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_ChineseSimplified))) {
    if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) return -1;
  }
  g_font_data = fd.address;
  g_font_size = fd.size;
  return 0;
}

static TTF_Font *open_font_at(int px) {
  if (!g_font_data || !g_font_size) return NULL;
  SDL_RWops *rw = SDL_RWFromConstMem(g_font_data, (int)g_font_size);
  if (!rw) return NULL;
  return TTF_OpenFontRW(rw, 1, px);
}

/* ------------------------------------------------------------------ */
/* 震动                                                                */
/* ------------------------------------------------------------------ */

static void vib_init(void) {
  g_vib_n = 0;

  if (R_SUCCEEDED(hidInitializeVibrationDevices(g_vib, 2,
        HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
    g_vib_n = 2;
    return;
  }
  if (R_SUCCEEDED(hidInitializeVibrationDevices(g_vib, 2,
        HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
    g_vib_n = 2;
    return;
  }
  if (R_SUCCEEDED(hidInitializeVibrationDevices(g_vib, 1,
        HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft))) {
    g_vib_n = 1;
    return;
  }
  if (R_SUCCEEDED(hidInitializeVibrationDevices(g_vib, 1,
        HidNpadIdType_No2, HidNpadStyleTag_NpadJoyRight))) {
    g_vib_n = 1;
  }
}

static void vib_send(float amp, float flo, float fhi) {
  if (g_vib_n <= 0) return;

  if (amp < 0.0f) amp = 0.0f;
  if (amp > 1.0f) amp = 1.0f;
  if (flo < VIB_FMIN) flo = VIB_FMIN;
  if (flo > VIB_FMAX) flo = VIB_FMAX;
  if (fhi < 82.0f) fhi = 82.0f;
  if (fhi > 1252.0f) fhi = 1252.0f;

  HidVibrationValue v;
  memset(&v, 0, sizeof(v));
  v.ampLow  = amp;
  v.freqLow = flo;
  v.ampHigh  = amp * 0.55f;
  v.freqHigh = fhi;

  HidVibrationValue vals[2];
  vals[0] = v;
  vals[1] = v;
  hidSendVibrationValues(g_vib, vals, (size_t)(g_vib_n > 2 ? 2 : g_vib_n));
}

static void vib_off(void) {
  if (g_vib_n <= 0) return;
  HidVibrationValue z;
  memset(&z, 0, sizeof(z));
  z.freqLow  = VIB_FMIN;
  z.freqHigh = 82.0f;
  HidVibrationValue vals[2] = { z, z };
  hidSendVibrationValues(g_vib, vals, (size_t)(g_vib_n > 2 ? 2 : g_vib_n));
}

/* MIDI 音高 -> 震动频率：按 A0..C8 做对数映射，均匀铺满频响区间 */
static float note_to_freq(int note) {
  float t = ((float)note - 21.0f) / 87.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return VIB_FMIN * powf(VIB_FRATIO, t);
}

/* ------------------------------------------------------------------ */
/* 目录浏览                                                            */
/* ------------------------------------------------------------------ */

static int is_midi_name(const char *n) {
  size_t l = strlen(n);
  if (l >= 5 && !strcasecmp(n + l - 5, ".midi")) return 1;
  if (l >= 4 && !strcasecmp(n + l - 4, ".mid"))  return 1;
  return 0;
}

static int cmp_entry(const void *a, const void *b) {
  const FsEntry *x = (const FsEntry *)a, *y = (const FsEntry *)b;
  if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir; /* 目录优先 */
  return strcasecmp(x->name, y->name);
}

static void list_dir(void) {
  g_nent = 0;
  g_esel = 0;
  g_etop = 0;

  DIR *d = opendir(g_cwd);
  if (!d) { snprintf(g_status, sizeof(g_status), "打不开目录"); return; }

  struct dirent *de;
  while ((de = readdir(d)) != NULL && g_nent < MAX_ENTRY) {
    if (de->d_name[0] == '.') continue;

    char full[768];
    snprintf(full, sizeof(full), "%s%s", g_cwd, de->d_name);

    struct stat st;
    int isdir = 0;
    if (stat(full, &st) == 0) isdir = S_ISDIR(st.st_mode);

    if (!isdir && !is_midi_name(de->d_name)) continue;

    snprintf(g_ent[g_nent].name, sizeof(g_ent[g_nent].name), "%s", de->d_name);
    g_ent[g_nent].is_dir = isdir;
    g_nent++;
  }
  closedir(d);

  /* 加一个".."用于返回上级 */
  if (strcmp(g_cwd, ROOT_DIR) != 0) {
    if (g_nent < MAX_ENTRY) {
      snprintf(g_ent[g_nent].name, sizeof(g_ent[g_nent].name), "..");
      g_ent[g_nent].is_dir = 1;
      g_nent++;
    }
  }

  qsort(g_ent, (size_t)g_nent, sizeof(FsEntry), cmp_entry);
  snprintf(g_status, sizeof(g_status), "%d 项", g_nent);
}

static void enter_parent(void) {
  size_t l = strlen(g_cwd);
  if (l <= strlen(ROOT_DIR) + 1) {
    snprintf(g_cwd, sizeof(g_cwd), "%s", ROOT_DIR);
    return;
  }
  if (g_cwd[l - 1] == '/') g_cwd[--l] = 0;
  char *slash = strrchr(g_cwd, '/');
  if (!slash) {
    snprintf(g_cwd, sizeof(g_cwd), "%s", ROOT_DIR);
  } else if (slash == g_cwd + 4) {          /* "sdmc:/" */
    snprintf(g_cwd, sizeof(g_cwd), "%s", ROOT_DIR);
  } else {
    slash[1] = 0;
  }
}

/* ------------------------------------------------------------------ */
/* 播放控制                                                            */
/* ------------------------------------------------------------------ */

static void stop_playback(void) {
  g_playing = 0;
  g_nact = 0;
  vib_off();
}

static int begin_playback(const char *path, const char *display) {
  if (g_loaded) { midi_free(&g_midi); g_loaded = 0; }

  if (midi_load(path, &g_midi) != 0) {
    snprintf(g_status, sizeof(g_status), "解析失败：%s", display);
    return -1;
  }
  g_loaded = 1;
  g_next_note = 0;
  g_nact = 0;
  g_hit_notes = 0;
  g_cur_amp = 0.0f;
  g_cur_freq = 0.0f;
  g_start_ms = SDL_GetTicks64();
  g_playing = 1;
  g_mode = MODE_PLAY;
  return 0;
}

static void update_playback(void) {
  if (!g_playing || !g_loaded) return;

  double now = (double)(SDL_GetTicks64() - g_start_ms) / 1000.0;

  /* 触发到期的音符 */
  while (g_next_note < g_midi.count && g_midi.notes[g_next_note].t <= now) {
    const MidiNote *n = &g_midi.notes[g_next_note];
    if (g_nact < MAX_POLY) {
      g_act[g_nact].end  = n->t + (n->dur > 0.03 ? n->dur : 0.03);
      g_act[g_nact].note = n->note;
      g_act[g_nact].vel  = n->vel;
      g_nact++;
    }
    g_hit_notes++;
    g_next_note++;
  }

  /* 过期音符出栈 */
  int w = 0;
  for (int i = 0; i < g_nact; i++)
    if (g_act[i].end > now) g_act[w++] = g_act[i];
  g_nact = w;

  if (g_nact == 0) {
    /* 没有任何音符在响 -> 静震，但节拍之间本来就会有间隙 */
    g_cur_amp = 0.0f;
    vib_off();
  } else {
    /* 振幅取最大值，频率跟最新触发的音符走（听感/手感最接近旋律） */
    float  amp  = 0.0f;
    const int latest = g_nact - 1;
    for (int i = 0; i < g_nact; i++) {
      float a = (float)g_act[i].vel / 127.0f;
      if (a > amp) amp = a;
    }
    float freq = note_to_freq(g_act[latest].note);
    g_cur_amp  = amp;
    g_cur_freq = freq;
    vib_send(amp, freq, freq * 2.0f);
  }

  /* 结束判定 */
  if (g_next_note >= g_midi.count && g_nact == 0 && now > g_midi.length + 0.25) {
    stop_playback();
    g_mode = MODE_DONE;
  }
}

/* ------------------------------------------------------------------ */
/* 绘制                                                                */
/* ------------------------------------------------------------------ */

static void draw_text(const char *s, int x, int y, SDL_Color c, TTF_Font *f) {
  if (!f || !s || !*s) return;
  SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
  if (!surf) return;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
  if (tex) {
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(g_ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
  }
  SDL_FreeSurface(surf);
}

static void draw_rect(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_ren, r, g, b, a);
  SDL_Rect rc = { x, y, w, h };
  SDL_RenderFillRect(g_ren, &rc);
}

static void fmt_time(double s, char *buf, size_t n) {
  if (s < 0) s = 0;
  int m = (int)(s / 60.0);
  double r = s - m * 60.0;
  snprintf(buf, n, "%d:%05.2f", m, r);
}

/* --- 浏览界面 --- */
static void render_browse(void) {
  SDL_Color white = { 235, 238, 245, 255 };
  SDL_Color grey  = { 150, 158, 172, 255 };
  SDL_Color accent= { 120, 190, 255, 255 };
  SDL_Color dim   = {  90,  96, 110, 255 };

  draw_rect(0, 0, SCR_W, 84, 30, 34, 44, 255);
  draw_text("MIDI 手柄震动播放器", PAD, 22, white, g_f24);
  draw_text(g_cwd, PAD, 56, dim, g_f16);

  const int row_h   = 44;
  const int list_y  = 104;
  const int max_rows = (SCR_H - list_y - 96) / row_h;

  if (g_nent == 0) {
    draw_text("这个目录里没有 .mid / .midi 文件", PAD, list_y + 20, grey, g_f24);
    draw_text("把 MIDI 放 SD 卡任意目录，用方向键浏览", PAD, list_y + 70, dim, g_f20);
  } else {
    if (g_esel < g_etop) g_etop = g_esel;
    if (g_esel >= g_etop + max_rows) g_etop = g_esel - max_rows + 1;
    if (g_etop < 0) g_etop = 0;

    for (int i = 0; i < max_rows && g_etop + i < g_nent; i++) {
      int idx = g_etop + i;
      int y = list_y + i * row_h;
      int sel = (idx == g_esel);

      if (sel) draw_rect(PAD - 12, y - 4, SCR_W - 2 * PAD + 24, row_h - 6, 60, 110, 175, 255);

      char line[320];
      if (g_ent[idx].is_dir)
        snprintf(line, sizeof(line), "[ %s ]", g_ent[idx].name);
      else
        snprintf(line, sizeof(line), "%s", g_ent[idx].name);

      draw_text(line, PAD, y + 6, sel ? white : (g_ent[idx].is_dir ? accent : grey), g_f20);
    }

    if (g_nent > max_rows) {
      char pos[64];
      snprintf(pos, sizeof(pos), "%d / %d", g_esel + 1, g_nent);
      draw_text(pos, SCR_W - PAD - 90, 22, grey, g_f20);
    }
  }

  draw_rect(0, SCR_H - 72, SCR_W, 72, 26, 30, 38, 255);
  draw_text("上下 选择     A 进入/播放     B 上一级     + 退出", PAD, SCR_H - 50, grey, g_f20);
  if (g_status[0]) draw_text(g_status, SCR_W - PAD - 320, SCR_H - 50, dim, g_f16);
}

/* --- 播放界面 --- */
static void render_play(void) {
  SDL_Color white = { 235, 238, 245, 255 };
  SDL_Color grey  = { 150, 158, 172, 255 };
  SDL_Color accent= { 120, 190, 255, 255 };
  SDL_Color dim   = {  90,  96, 110, 255 };

  draw_rect(0, 0, SCR_W, 84, 30, 34, 44, 255);
  draw_text("正在震动播放", PAD, 22, white, g_f24);

  double now = (double)(SDL_GetTicks64() - g_start_ms) / 1000.0;
  double total = g_midi.length > 0.01 ? g_midi.length : 1.0;
  double p = now / total;
  if (p < 0) p = 0;
  if (p > 1) p = 1;

  /* 进度条 */
  const int bar_y = 130, bar_h = 26;
  draw_rect(PAD, bar_y, SCR_W - 2 * PAD, bar_h, 40, 46, 58, 255);
  draw_rect(PAD, bar_y, (int)((SCR_W - 2 * PAD) * p), bar_h, 80, 150, 220, 255);

  char tb[64], tt[64], line[256];
  fmt_time(now, tb, sizeof(tb));
  fmt_time(total, tt, sizeof(tt));
  snprintf(line, sizeof(line), "%s / %s", tb, tt);
  draw_text(line, PAD, bar_y + bar_h + 12, grey, g_f20);

  snprintf(line, sizeof(line), "音符 %d / %d", g_hit_notes, g_midi.count);
  draw_text(line, PAD + 260, bar_y + bar_h + 12, grey, g_f20);

  /* 实时震动参数 */
  int y = 260;
  draw_text("当前震动强度", PAD, y, dim, g_f20);
  draw_rect(PAD + 200, y + 4, 420, 20, 40, 46, 58, 255);
  draw_rect(PAD + 200, y + 4, (int)(420 * g_cur_amp), 20, 220, 140, 90, 255);
  snprintf(line, sizeof(line), "%.0f%%", g_cur_amp * 100.0f);
  draw_text(line, PAD + 640, y, grey, g_f20);

  y += 56;
  draw_text("当前震动频率", PAD, y, dim, g_f20);
  snprintf(line, sizeof(line), "%.1f Hz", g_cur_freq);
  draw_text(line, PAD + 200, y, grey, g_f20);

  y += 56;
  snprintf(line, sizeof(line), "活跃音符 %d（最多 %d）", g_nact, MAX_POLY);
  draw_text(line, PAD, y, dim, g_f20);

  y += 56;
  snprintf(line, sizeof(line), "音高范围 %d 个   格式 %d   轨道 %d",
           g_midi.count, g_midi.format, g_midi.tracks);
  draw_text(line, PAD, y, dim, g_f16);

  if (g_vib_n <= 0) {
    y += 44;
    draw_text("没有检测到可取用的震动设备（手持模式或连上 Joy-Con 再试）", PAD, y,
              (SDL_Color){ 230, 120, 120, 255 }, g_f20);
  }

  draw_rect(0, SCR_H - 72, SCR_W, 72, 26, 30, 38, 255);
  draw_text("B 停止并返回     + 退出", PAD, SCR_H - 50, grey, g_f20);
}

/* --- 结束界面 --- */
static void render_done(void) {
  SDL_Color white = { 235, 238, 245, 255 };
  SDL_Color dim   = {  90,  96, 110, 255 };

  draw_rect(0, 0, SCR_W, 84, 30, 34, 44, 255);
  draw_text("播放完成", PAD, 22, white, g_f24);

  char line[256];
  snprintf(line, sizeof(line), "共触发 %d 个音符", g_hit_notes);
  draw_text(line, PAD, 160, white, g_f24);

  char t[64];
  fmt_time(g_loaded ? g_midi.length : 0.0, t, sizeof(t));
  snprintf(line, sizeof(line), "时长 %s", t);
  draw_text(line, PAD, 210, dim, g_f20);

  draw_rect(0, SCR_H - 72, SCR_W, 72, 26, 30, 38, 255);
  draw_text("任意键返回，A 再播一次     + 退出", PAD, SCR_H - 50, dim, g_f20);
}

static void render(void) {
  SDL_SetRenderDrawColor(g_ren, 18, 20, 26, 255);
  SDL_RenderClear(g_ren);

  switch (g_mode) {
    case MODE_PLAY: render_play(); break;
    case MODE_DONE: render_done(); break;
    default:        render_browse(); break;
  }
  SDL_RenderPresent(g_ren);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  hidInitialize();

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    consoleInit(NULL);
    printf("SDL_Init 失败: %s\n按 + 退出\n", SDL_GetError());
    consoleUpdate(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 1;
  }

  g_win = SDL_CreateWindow("MIDI 震动播放器",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           SCR_W, SCR_H, SDL_WINDOW_SHOWN);
  g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!g_win || !g_ren) {
    SDL_Quit();
    return 1;
  }

  if (TTF_Init() == 0 && load_system_font() == 0) {
    g_f24 = open_font_at(24);
    g_f20 = open_font_at(20);
    g_f16 = open_font_at(16);
  }

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  vib_init();
  list_dir();

  int running = 1;

  while (running && appletMainLoop()) {
    padUpdate(&pad);
    u64 down = padGetButtonsDown(&pad);

    /* 退出 */
    if (down & HidNpadButton_Plus) {
      running = 0;
      break;
    }

    if (g_mode == MODE_BROWSE) {
      if (down & HidNpadButton_Up)   { if (g_esel > 0) g_esel--; }
      if (down & HidNpadButton_Down) { if (g_esel + 1 < g_nent) g_esel++; }
      if (down & (HidNpadButton_StickLUp))   { if (g_esel > 0) g_esel--; }
      if (down & (HidNpadButton_StickLDown)) { if (g_esel + 1 < g_nent) g_esel++; }

      if (down & HidNpadButton_B) {
        if (strcmp(g_cwd, ROOT_DIR) != 0) {
          enter_parent();
          list_dir();
        }
      }

      if (down & HidNpadButton_A) {
        if (g_nent > 0) {
          if (g_ent[g_esel].is_dir) {
            if (strcmp(g_ent[g_esel].name, "..") == 0) {
              enter_parent();
            } else {
              size_t l = strlen(g_cwd);
              size_t room = sizeof(g_cwd) - l - 1;
              strncat(g_cwd, g_ent[g_esel].name, room);
              l = strlen(g_cwd);
              if (l + 1 < sizeof(g_cwd)) { g_cwd[l] = '/'; g_cwd[l + 1] = 0; }
            }
            list_dir();
          } else {
            char full[768];
            snprintf(full, sizeof(full), "%s%s", g_cwd, g_ent[g_esel].name);
            if (begin_playback(full, g_ent[g_esel].name) != 0) {
              /* 解析失败留在浏览界面，状态栏已写明原因 */
            }
          }
        }
      }
    } else if (g_mode == MODE_PLAY) {
      if (down & HidNpadButton_B) {
        stop_playback();
        g_mode = MODE_BROWSE;
        snprintf(g_status, sizeof(g_status), "已停止");
      }
    } else if (g_mode == MODE_DONE) {
      if (down & HidNpadButton_A) {
        /* 再播一次当前文件 */
        if (g_loaded) {
          g_next_note = 0;
          g_nact = 0;
          g_hit_notes = 0;
          g_start_ms = SDL_GetTicks64();
          g_playing = 1;
          g_mode = MODE_PLAY;
        }
      } else if (down & ~(HidNpadButton_Plus)) {
        g_mode = MODE_BROWSE;
        snprintf(g_status, sizeof(g_status), "");
      }
    }

    update_playback();
    render();
    SDL_Delay(8);
  }

  stop_playback();
  if (g_loaded) midi_free(&g_midi);

  if (g_f24) TTF_CloseFont(g_f24);
  if (g_f20) TTF_CloseFont(g_f20);
  if (g_f16) TTF_CloseFont(g_f16);
  TTF_Quit();
  plExit();
  SDL_DestroyRenderer(g_ren);
  SDL_DestroyWindow(g_win);
  SDL_Quit();
  return 0;
}
