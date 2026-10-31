# DPCM 通道的波表：从示意图形到真实采样

- **日期**：2026-10-29
- **对象**：`plugin/Source/playback/WaveShape.cpp`、`backends/nsf/`、`backends/libvgm/`、`chips/`
- **目的**：判断"波表展示当前音符触发的那个采样"是否可行，以及 NSF 与 VGM 两条路径的差距
- **相关**：`2026-10-19-midi-output-and-visualisation.md`
- **后续更正（2026-10-31）**：4.2 的长度最初按四位写，而播放核心与 NotSoFatso 都按八位算，已改正；同时补上当时还不知道的两步收尾——尾部填充要裁、公式里的 `+1` 是每个采样自带的 padding 字节。原因见 `docs/memo/2026-10-30-the-register-width-nobody-checked.md`。

## 1. 结论

三句话：

1. **显示与导出的通路早就存在。** `ChannelState::wave` 就是为此准备的字段，`KeyboardRowPaint` 与 `WaveExport` 都优先读它，FDS 和 N163 已经在填。缺的不是机制，是 DPCM 没有往里递数据。
2. **NSF 的 2A03 DPCM 可以取到真实采样，成本小。** `NsfBus` 就是 CPU 地址空间本身，`Read()` 会处理 bank 切换；采样地址来自 `$4012`、长度来自 `$4013`，剩下的只是积分。
3. **VGM 取不到，而且不是"提取"这一层的问题。** `LibvgmBackend` 没有实现 `captureState`，所以那条路径上键盘页压根没有通道行；`NesApuDecoder` 不接收 ROM；VGM 的 `0x80` data block 也没有加载点。三处都缺。

## 2. 现在画的是什么

`WaveShape.cpp:101-135`，`Kind::delta` 分支：

```cpp
uint32_t state = 0x9E3779B9u ^ (uint32_t) shapeParameter;

for (int i = 0; i < 32; ++i)
{
    state ^= state << 13;  state ^= state >> 17;  state ^= state << 5;   // xorshift
    const float step = (state & 1u) ? 1.0f : -1.0f;                      // 1 bit
    const float envelope = 1.0f - (float) i / 31.0f * 0.72f;             // 衰减
    level = level * 0.90f + step * 0.62f * envelope;                     // 积分 + 泄漏
    cycle.push_back (clampTo (level, -1.0f, 1.0f));
}
```

**伪随机的 ±1 步进，积分后带衰减与向中心回拉。** 它模拟的是"1-bit 数据流被 DAC 积分"这件事的**外观**，与任何实际采样无关。

种子在 `NsfPlayer.cpp:874`：

```cpp
state.shapeParameter = (tone << 4) ^ (rate * 7919);
```

`tone` 是采样地址页号，`rate` 是播放速率。**同一个采样换个性高播放会画出另一张图** —— 这一半合理（变调确实改变听感），一半可疑（指纹本该由采样内容决定，而非播放速度）。

注释记录了动机：更早的版本用固定 `i % 7`，一个规则阶梯**和两行之下的 VRC6 锯齿分不清**，改成按身份播种之后不同采样才长得不一样。

**面板与导出共用 `cycleFor`**（`KeyboardRowPaint.cpp:251`、`WaveExport.cpp:80`），所以导出的 DPCM 文件就是屏幕上那张噪波 —— 有诊断价值，没有音乐用途。

## 3. 通路

| 环节 | 位置 | 状态 |
|---|---|---|
| 字段 | `ChipState.h:40-42` `std::vector<float> wave` | 存在 |
| 面板 | `KeyboardRowPaint.cpp:223` `if (! channel.wave.empty ())` | 优先使用 |
| 导出 | `WaveExport.cpp:75` `if (channel.wave.size () >= 2)` | 优先使用 |
| 填充 | `NsfPlayer.cpp:1052`（FDS 64 级）、`1122`（N163） | 已实现 |
| 填充 | DPCM / MMC5 PCM | **缺** |

字段注释写得很清楚：*Empty for chips that generate their shape rather than read it from memory - the display synthesises those.*

DPCM 恰好属于"从内存读"，却被归类成"生成"。

## 4. NSF：可做

### 4.1 数据在哪

`NsfBus.h:25`：

```cpp
class NsfBus : public xgm::NES_MEM
{
    bool Read (uint32_t adr, uint32_t& val, uint32_t id = 0) override;
    std::array<uint8_t, 8> banks {};      // 八个 4KB 窗口
};
```

**它是 6502 地址空间本身**，`Read()` 负责那八个 bank 窗口（`$5FF8-$5FFF` 选择）。DPCM 采样落在 `$C000-$FFFF`，所以任意字节都可读。

NSF 文件即 ROM 镜像，**采样必定存在**，不存在"文件没带"的情况。

### 4.2 边界

| 量 | 来源 |
|---|---|
| 起始地址 | `$4012` 页号 → `$C000 + tone * 64` |
| 长度 | `$4013` → `reg * 16 + 1` 字节，最大 4081；再裁掉尾部填充与那个 padding 字节 |

**字节不是波形，是增量。** 每字节 8 个 1-bit delta，1 → `+2`、0 → `-2`（7-bit 域），**必须积分**才是 PCM。这与 `Kind::delta` 的语义一致。

**DMC 只播一次，不循环，播完即止。** 所以"当前音符触发的这个采样"= 从起始地址起的整段，不需要滚动窗口。

### 4.3 改动范围

1. `NsfPlayer` 的 `kShapeDelta` 分支（`NsfPlayer.cpp:860`）读 `[$C000+tone*64, +len)`，积分后填 `state.wave`
2. `cycleFor` 的 `delta` 分支保留为无数据时的兜底（MMC5 PCM 就还需要它）
3. 断言：`wave` 非空、长度等于 `$4013` 推出值裁掉填充后的结果、导出值与 ROM 字节的积分一致

**MMC5 的 PCM 不受益**：它是驱动实时写 `$5011`，没有 sample bank，仍然是合成图形。

## 5. VGM：三层都缺

| 层 | 现状 |
|---|---|
| 显示/导出 | `LibvgmBackend` / `LibvgmTrackPlayer` **没有 `captureState`** |
| 解码器 | `NesApuDecoder` 的 `kRequirements` **没有 `hasMemory`**，不接收 ROM |
| 数据 | VGM 的 `0x80` data block（NES ROM）**没有加载点** |

第三条的基础事实：

```cpp
// SongBackend.h:163
virtual bool captureState (ChipStateSnapshot& snapshot) const { /* 默认 false */ }
```

只有 `NsfBackend.cpp:47` 重写了它。**所以 VGM 格式的键盘页没有通道行** —— 没有地方显示，也没有东西可导出。

另外 `NesApuDecoder` 的唯一消费者是 `MidiSequenceBuilder`（`NoteExtractor` 那条线只产音高），它拿不到、也不需要内存。

**还有一个前提差异**：VGM 的 2A03 采样来自 data block，**取决于制作工具是否写入**。没写的 VGM 里没有采样数据，任何代码都取不到。NSF 没有这个不确定性。

## 6. 待定的设计选择

| | 整段采样 | 当前位置的窗口 |
|---|---|---|
| 画面 | 稳定，像 FDS 表 | 滚动，示波器 |
| 15 Hz 刷新下 | 静止可辨认 | **闪烁、看不清** |
| 导出意义 | 真实采样，可用 | 一段任意片段 |

**建议整段。** 依据是 DMC 不循环，"形状"就是整段本身；而面板每秒重绘有限次，滚动窗口在 15 Hz 下只会变成闪烁。

**降采样到 128 点左右**即可 —— 面板宽度约 120 px，更多点画不出来。注意鼓的瞬态在前几百字节，降采样窗口应覆盖全长而不是只取开头。

## 7. 附表：其他带 sample bank 的芯片

`chips/*Decoder` 里有一批**已经持有完整 ROM** 的解码器，`attachMemory()` → `setRom()` 来自 VGM 的 `0x8C`/`0x8D`/`0x8E` data block：

```
SegaPcm  C140  C352  K053260  K054539  QSound  GA20  OKIM6295  RF5C68  MultiPcm  Es5503  Ymz280b
```

`SegaPcmDecoder.cpp:119` 的 `impl->rom` + `byteAt(address)` 就是现成的读取器，`cycleFor(start, length)` 已经在读它估算音高。

**但它们的 `channelState()` 产出的是 `chips::ChannelState`（音高、门、音量），唯一消费者是 `NoteExtractor` → MIDI。** 与显示层的 `playback::ChannelState` 是两个不同的结构，中间没有线。

**所以这条路的障碍不在"取数据"，在"少一根从解码器到显示层的线"。** 属于独立工程，与 DPCM 不同一批。

## 8. 未确认

- VGM 路径上键盘页没有通道行这一结论来自静态阅读（`captureState` 无实现），**没有实际打开一个 VGM 观察**
- 多芯片 PCM 若要接上显示层，装配点在哪一层尚未查（`NoteExtractor` 是现有唯一消费者，但它服务于 MIDI，不适合兼作显示总线）
