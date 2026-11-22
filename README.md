# midivib — MIDI 手柄震动播放器（Nintendo Switch）

导入 `.mid` / `.midi`，把音符事件按时间轴驱动成 Joy-Con / Pro 手柄的 HD 震动。
音高决定震动频率，力度决定振幅。

## 用法

1. 把 `midivib.nro` 放 `/switch/midivib.nro`
2. 把任意 `.mid` / `.midi` 放 SD 卡任意目录
3. 启动后方向键浏览目录，A 进入目录 / 播放文件，B 返回上一级，+ 退出

## 震动映射

| MIDI | 震动 |
|---|---|
| 音高 A0(21) .. C8(108) | 频率 41.7 .. 626 Hz，对数均匀铺满 |
| 力度 0..127 | 振幅 0..100% |
| Note Off / 时长到期 | 振幅归零 |

频响边界取自 libnx 的 HD 震动规格：freqLow 41.7–626 Hz，freqHigh 82–1252 Hz。
低音沉、高音尖，旋律走向和节奏都能摸出来。

## MIDI 支持范围

- format 0 / 1 / 2
- tempo meta 0x51 全程收集，支持中途变速（分段积分换算秒）
- SMPTE 时基（division 高字节非 0）
- running status
- note on 力度 0 视为 note off
- 无配对 note off 的给 0.3 秒默认时长
- 不支持 RIFF RMID（.rmi）容器

## 实现要点

- 中文用 Switch 系统 SharedFont，nro 里不带字体
- 按键直接读 libnx `pad`，不经过 SDL 的映射层
- 震动走 `hidInitializeVibrationDevices` + `hidSendVibrationValues`

## 构建

devkitPro + 以下 portlibs，`make`。CI 已配好。
`switch-sdl2 switch-sdl2_ttf switch-libpng switch-freetype switch-zlib switch-mesa switch-glad switch-libdrm_nouveau switch-harfbuzz switch-bzip2 switch-lzma`

链接行（顺序重要）：

```
-lSDL2_ttf -lfreetype -lharfbuzz -lpng -lz -lbz2 -llzma
-lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm
```
