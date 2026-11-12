# 调研：Game Boy 的 Wave 通道波形

**状态**：调研完成，未改代码
**测试文件**：`test_file/Game Boy DMG/` 十二首（Batman: The Video Game，1990）
**相关**：`chips/GbcApuDecoder.cpp`、`emu/cores/gb_mame.c`、`docs/issue/2026-11-12-the-router-that-rebuilt-the-write.md`

---

## 1. 现状（实测）

`01 Title`：四行齐全——Square 1/2 是**合成出来的方波**，Noise 是合成噪声，**Wave 行是空的**。

`assert engine wave.N` 四行都是 **0**：前三个通道不需要 `wave`（它们由 `period`/duty 合成），
Wave 通道需要一张表，而解码器没有。所以这次的改动比 WonderSwan 小得多——**命令通路已经在**，
只缺 `0x20-0x2F` 这 16 个字节和 `channelState` 的一个分支。

---

## 2. 芯片模型（`gb_mame.c`）

### 2.1 波形 RAM 的位置

`0xFF30-0xFF3F`，共 **16 字节 = 32 个 4 bit 采样**，与寄存器同一个数组
（`snd_regs[AUD3W0 + i]`，`gb_mame.c:992`）。VGM 的 `0xB3` 用“相对 `0xFF10` 的偏移”寻址，
所以波形数据就是 **offset `0x20-0x2F`**。

### 2.2 取样

```c
current_sample = gb->snd_regs[AUD3W0 + (offset/2)];
if (!(offset & 0x01)) current_sample >>= 4;    // 偶数步取**高**半字节
current_sample &= 0x0f;
current_sample -= 8;                           // 以 8 为中心
current_sample <<= 1;                          // 4 bit 值放大到 ±
```

**半字节顺序与 WonderSwan 相反**：GB 是**偶数步取高半字节**，WS 是偶数步取低半字节。
这条差异必须由断言钉住——写反不会报错，只会把每两步镜像（和 WS 那次一样的坑）。

归一化到行里是 `(nibble - 8) / 8`，范围 −1..+0.875，静音（nibble 8）为 0。

### 2.3 速率

```c
// frequency_counter 每 2 个 CPU cycle 加一，到 2048 回绕
snd->frequency_counter = (snd->frequency_counter + 1) & 0x7ff;
if (gb->gbMode == GBMODE_DMG && snd->frequency_counter == 0x7ff)
    snd->offset = (snd->offset + 1) & 0x1F;    // DMG：每个周期走一步
```

一步 = `2 × (2048 - f)` 个 CPU cycle，所以：

```
步进率 = clock / (2 * (2048 - f))       // f 为 11 位频率寄存器
音高   = 步进率 / 32 = clock / (64 * (2048 - f))
```

与现有 `GbToneTable` 的注释一致（“wave channel by 64”，pulse 是 32）。

### 2.4 音量与写入锁

- `NR32`（offset `0x0C`）的 bit 5-6 是**音量档**（0 静音 / 100% / 50% / 25%），只缩放振幅；
- **DMG 的波形 RAM 播放期间被锁**：`gb_wave_w` 只在通道关闭、或恰好处于“正在读该字节”的窗口时才写入
  （`gb_mame.c:278-300`），并且 DMG 还有 `gb_corrupt_wave_ram`（读时清零）的行为。

对“画形状”的影响：如果驱动在播放中改写波形，芯片会忽略这些写，而面板若照单全收就会**画出音频里没有的形状**。

---

## 3. 测试文件实测（12 首，前四首细看）

| 项 | 结果 |
|---|---|
| `0xB3` 写入 | 2 566 – 129 235 条 |
| 已覆盖的偏移 | `0x00-0x16`（NR10-NR52），**没有 `0x20-0x2F`** |
| 波形 RAM 写入 | 每首 **16 – 448** 条 |
| **播放中写入** | **0 条**（12 首都是先关通道再上传） |
| 不同表（含中间状态） | 15 – 44 张/首 |
| 通道类型 | 四行：Square 1/2（`gbPulse`）、Wave（`gbWave`）、Noise（`gbNoise`） |

“播放中 0 条”很关键：**这一批测试文件完全不需要处理写入锁**。锁仍然要写在注释里，因为别的驱动
可能不守规矩；但实现上可以先按“照单全收”做，并把这一条记成已知边界。

---

## 4. 方案

### A. 解码器（必做，约 80 行）

1. `uint8_t waveRam[16]`（随 `reset` 清零）；
2. `write()` 增加 `case 0x20 … 0x2F`：`waveRam[w.reg - 0x20] = value`（`0xB3` 已经到达解码器，
   不需要动 reader/loader/router）；
3. `channelState()` 的 wave 通道：
   - `wave` = 32 个半字节（**偶数步高半字节**）→ `(n - 8) / 8`；
   - `tableWave = true`（一个周期，导出走波表路径）；
   - `sampleRate = clock / (2 * (2048 - (f & 0x7FF)))`；`f` 为 11 位，`0x7FF` 是合法值；
4. 缓存：按 16 字节内容 hash，上限 **64**（本批最多 44 张）。

### B. 不做

- **DMG 写入锁**：实测 0 条播放中写入；镜像它还会让面板与 VGM 文件里真实存在的字节不一致（而且
  “是否正在读该字节”需要跟踪播放头）。在注释与文档里写明边界；
- CGB 双速模式（`GBMODE_CGB04` 的 offset 递增规则不同）：`0xB3` 是 DMG 通道，本批文件也没有 CGB；
- 音量档（只影响振幅）。

---

## 5. 风险

1. **半字节顺序**与 WonderSwan 相反——断言必须钉住“首字节的 0、1 两步”，否则镜像错误无法发现；
2. 频率 `0x7FF` → 分母 1 → 步进率 2 097 152，是合法的高音，不要当除零；
3. `wave` 只在 `gbWave` 通道上出现，另外三个通道保持 0（现有行为，不要“顺手补”成合成方波）；
4. 表会在换乐器时重写（15–44 张/首），缓存必须按内容；
5. 未触发/静音时是否显示表——这一批文件里波形先上传后触发，显示表更符合“装了什么就画什么”。

---

## 6. 验证计划

| 层 | 断言 |
|---|---|
| 解码器 | 32 步、偶数步高半字节、端点 −1 / +0.875、`(n-8)/8` |
| 速率 | `sampleRate = clock / (2*(2048-f))`，`f = 0x7FF` 不除零 |
| 行 | harness `engine wave.2 == 32` + `engine wave.2.peak == 0.875`（大小断言看不出空表）|
| 导出 | “Export wavetables” 对 Wave 行可用（2048 点单周期）|
| 回归 | 另外三行的形状与 `channelKind` 不变 |

---

## 7. 需要你确认

1. **DMG 写入锁不做**（实测 0 条播放中写入，注释写明边界）——可以吗？
2. **缓存上限 64**（本批最多 44 张）——够吗？
3. 未触发时也显示已装好的表（推荐）？
4. 只做 DMG 规则、不碰 CGB 双速（`0xB3` 就是 DMG 通道）——确认？

---

## 8. 已实现（2026-11-12）

按建议落地，四项决定全部照做（不做写入锁、缓存 64、未触发也显示、只做 DMG）：

- 解码器保存 `waveRam[16]`，`write()` 的 `default` 分支收下 `0x20-0x2F`；
- `channelState(wave)` 给 `wave`（32 步，**偶数步高半字节**）、`tableWave = true`、
  `sampleRate = clock / (2 * (2048 - f))`；按内容 hash 缓存 64 张；
- 头注释补了“Wave 的形状是 RAM 而不是寄存器”，并把 DMG 写入锁的边界写在取字节的注释旁；
- 验证：`testGbWaveTable`（半字节顺序、端点、速率）、`wave-gameboy.txt`（`wave.2 == 32` +
  `peak == 0.875`，并确认 `wave.0 == 0` 表示脉冲通道仍走合成路径）、截图 `gb-before.png` / `gb-after.png`。
