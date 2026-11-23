# YM3812（OPL2）的可视化：调研与方案

> 2026-11-23。**未开工**。测试曲目在 `test_file/ym3812/`（12 首 `.vgz`）。
> 与 OPLL 那一轮（`2026-11-22-opl-visualisation.md`）同一条链路，但 patch 是**真寄存器**，不是音色号。

## 一、结论先说

**OPL2 走的是 OPN 那条路，不是 OPLL 那条路。** 它每个算子都有真实的 `0x20/0x40/0x60/0x80` 四个寄存器，
所以 `capturePatch` 要把它们抄下来——8 条算子寄存器加 `0xC0` 的连接/反馈，**9 条**。

**框架已经就位**：`Dialect` 结构、`NoteCache` 模式、`noteRateFor` 与 `fmOutputBits` 两张表、
`ChipModel::ym3812`、`deviceIdFor` 的 `case`——**上一轮为 OPLL 铺的东西这里全部复用**。

**真正要新写的只有两处**：解码器里的**每算子存储**，和驱动里的**第三个方言**。

## 二、已核实的事实（带出处）

| 事实 | 出处 |
|---|---|
| **默认核是 AdLibEmu**（"better than MAME"），MAME 与 Nuked 备选 | `oplintf.c:156-164` |
| 三个核都编进来了：`adlibemu_opl2.c.obj` / `fmopl.c.obj` / `nukedopl3.c.obj` | `build/**` |
| `adlibemu_opl2.c` 是 0 KB 的壳，实现全在 `adlibemu_opl_inc.c` | 文件尺寸 |
| **AdLibEmu 自带重采样**：`generator_pos/FIXEDPT` 累加器逐样本推进，无滤波 | `adlibemu_opl_inc.c:232-257` |
| 原生速率 `clock/72` = **49 716 Hz**，`noteRateFor(ym3812)` 已经是它 | `oplintf.c:311`、`VoicePatch.h` |
| `fmOutputBits(ym3812)` = **16，且注释写明"未实测"** | `VoicePatch.h:166` |
| `ChipModel::ym3812` 已存在，`deviceIdFor` 已映射 | `VoicePatch.h:25`、`FmVoiceDriver.cpp` |
| **`isOpll()` 只认 `ym2413`** → OPL2 的 patch 会落到 `kOpnDialect` | `FmVoiceDriver.cpp` |
| 解码器只存了**调制器**的 MULT 与 TL，**没有** AR/DR、SL/RR、载波、`0xC0` 的 CON 位 | `OplDecoder.cpp:553-591` |
| 槽位表 `kOperatorOfRegister[32]` 已有（0..17，偶=调制器），但 `modulatorChannel()` 只返回调制器 | `OplDecoder.cpp:45,538` |
| 行名是裸 `1`..`9` + `Bass Drum`…（libvgm 的 `channelNames` 覆盖了兜底） | 实测 `vgm_mixer_test` |
| `carriesFm(ym3812)` 已经是 `true` | `ChipPlans.cpp` |

**跑一遍现状**（`03 Pump Them with Bullets (Chapter 2).vgz`）：

```
Chips     : YM3812 (ELDA)
14 tracks over 1 chip(s)
[ 0] YM3812  1 … [ 8] YM3812  9     ← 裸数字
[ 9] Bass Drum … [13] Hi-Hat
peak 0.8168, rms 0.1682              ← 有声音
```

**音符、电平、MIDI 都正常，缺的是形状**——行里只有"一个周期"，因为 `fmKnown=true` 走的是
`shapeParameter` 打包路线，而 patch 不存在。

## 三、要改的四处

| 文件 | 改动 | 量 |
|---|---|---|
| `chips/OplDecoder.cpp` | 每算子 4 字节 ×2 + `0xC0` 存储；`capturePatch`（9 条，通道 0）；`NoteCache` 照 OPLL；`channelState` 的音量用载波 TL 而不是常数 15 | 中 |
| `backends/libvgm/FmVoiceDriver.cpp` | `kOpl2Dialect`：`noteOn` 写 `0xA0` 后 `0xB0`（key 在 bit 5、block 在 4-2、fnum 高位在 1-0），`noteOff` 清 key 位 | 小 |
| `backends/libvgm/LibvgmTrackPlayer.cpp` | native 速率名单加 `ym3812` | 一行 |
| `backends/libvgm/LibvgmSongInstance.cpp` | `tableNames` 的排除项从 `ym2413` 扩到 OPL2 家族 | 一行 |
| `Tests/` | `fmOutputBits` 峰值断言（照 `testOpllReachesFullScale`）、patch 断言 | 小 |

**`capturePatch` 的产物（提案，通道 0 的相对地址）：**

```
{ 0x20, op0 0x20 } { 0x40, op0 0x40 } { 0x60, op0 0x60 } { 0x80, op0 0x80 }
{ 0x23, op1 0x20 } { 0x43, op1 0x40 } { 0x63, op1 0x60 } { 0x83, op1 0x80 }   // 槽位 3 = 通道 0 的载波
{ 0xC0, fb << 1 | con }
```

**为什么不按歌曲的通道号捕获**：OPL2 的槽位公式是 `slot = (ch/3)*6 + ch%3 + 3*op`，从寄存器反推通道要走
`kOperatorOfRegister` 这层间接；而 OPLL 那一轮已经证明"**恒用通道 0**"少一整类错。代价是 patch 只对
"一颗芯片一个音色"成立——**而它本来就只渲染一个音符**。

## 四、需要你拍板的六件事

**D1（范围）· 只做 `ym3812`，还是把 `ym3526` / `y8950` 一起接？**
三者共用 `OplDecoder::Variant::opl2` 这**同一条分支**，解码器侧的 `capturePatch` 是免费的。
**建议一起**：patch 的 `model` 统一写 `ChipModel::ym3812`（三者的 FM 半兼容，且 `deviceIdFor` 只会把它映射到
`DEVID_YM3812` 的核）。**代价**：`ym3526`/`y8950` 的渲染用的是 YM3812 的核，timing 上略有差异，但音色一致。

**D2 · `capturePatch` 用"通道 0 的 9 条"还是"歌曲通道的绝对地址"？**
**建议通道 0**，理由见上：OPLL 那一轮已经踩过"按通道捕获"的间接成本。

**D3 · 行名改 `FM 1..9` 吗？**
现在是裸 `1`..`9`。**建议改**（与 YM2413 一致），鼓行保持 `Bass Drum`…不动。

**D4 · 速率策略**（**已推翻，见 §9**）
原建议：照 OPM/OPLL，导出让核跑 native 49 716，由 `WaveExport` 的 Blackman FIR 转换。
**实际实施后改成了"按文件速率直渲"**——FIR 让导出比播放暗 1.15 dB，而用户的判据是"导出 = 播放"。

**D5 · `fmOutputBits(ym3812)`**（**结论同 §9**）
原以为"测出满度即可"。实测暴露的是**这张表有两套约定**：芯片 DAC 满度（13）与混音里的声道电平（16）。
最终定 **16**，断言也从 `peak > 0.9` 改成"落在混音电平区间"。

**D6 · 单声道与音量**
OPL2 没有 pan，`render` 混的是 `(L+R)/2`；如果核把同一值写进两声道，**不需要 ×2**（要实测确认，YM2203 当初需要）。
另外 `channelState` 对非 OPLL 的旋律行**写死 `volume = 15`**（`OplDecoder.cpp:654`），**OPL2 的电平条现在是恒满**——
**建议一起修**（用载波 TL 反推），它和 patch 读的是同一批寄存器。

## 五、风险与已知坑

1. **`0xE0` 在 OPL2 上不存在**（waveform 固定正弦）。patch 里**不要写 `0xE0`**，否则 OPL3 核会把它当波形选择。
2. **`0xB0` 的 key 位是 bit 5**，与 OPLL 的 `0x20` bit 4 不同——方言里写错不会报错，只会不发声。
3. **rhythm 模式（`0xBD`）会借走通道 6-8**：那时它们不再是普通旋律行，`capturePatch` 要返回 false（现有 `channelState` 已有 rhythm 分支）。
4. **`kOperatorOfRegister` 的偶/奇约定**：偶数是调制器、奇数是载波；改成 `(channel, op)` 时必须保留"未使用窗口返回 -1"的语义（`0x26/0x27` 等）。

## 六、工作量与顺序

| 步 | 内容 | 验证 |
|---|---|---|
| 1 | 解码器存 9 字节 + `capturePatch` + 缓存 | 单元断言：写 9 个寄存器，patch 是 9 条；rhythm 行返回 false |
| 2 | `kOpl2Dialect` + `isOpl2()` | 手写 patch 能出声、音高对 |
| 3 | 测 `fmOutputBits` 并定值 | 峰值断言 |
| 4 | native 速率 + 行名 | 导出对比播放 |
| 5 | 面板/拖拽接通 + 截图基线 | 12 首 `.vgz` 目视 |

**粗估：1-4 步半天，第 5 步半天。** 比 OPLL 那一轮少一处不确定（不需要音色表），多一处存储（8 字节/通道）。

## 七、一句话

> **OPLL 的 patch 是一个字节，OPL2 的 patch 是九个字节——但两者共用同一条链路。**
> 这一轮真正的未知只剩一个：**AdLibEmu 的输出幅度**，而它正好是第一个该量的东西。
