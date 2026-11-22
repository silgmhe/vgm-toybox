# YM2413（OPLL）的可视化：调研与方案

> 2026-11-22。**未开工**，先解决"要做什么、代价多大、哪几个岔路要人拍板"。
> 前一轮修的是"变体没人传"（`2026-11-22-the-variant-nobody-passed.md`），那是 bug；
> 这一轮补的是"OPLL 没有 `capturePatch`"，那是功能。

## 一、结论先说

**比预想的便宜一个数量级，因为有一个前提被搞错了。**

之前的判断是"OPLL 的音色在内置 ROM 里，要读 ROM 或抄一张 15 音色的表"。**核对核的源码之后，两条都不必要**：

```1419:1421:libs/libvgm/emu/cores/emu2413.c
	case 0x30:
		set_patch(opll, reg - 0x30, (data >> 4) & 15);
		set_volume(opll, reg - 0x30, (data & 15) << 2);
```

**写 `0x30 + ch = 音色号 << 4 | 音量`，核自己就去 `default_inst` 里把预置载入。** 而我们的渲染驱动本来就是"起一颗新核、写寄存器、发声"——**所以只要把这个字节交给它，预置音色自动到位，一张表都不用抄。**

**"OPLL 没有逐算子的参数"这句话是对的，但它不等于"没有 patch"。音色号就是它的 patch。**

## 二、已核实的事实（都带出处）

| 事实 | 出处 |
|---|---|
| 写 `0x30+n` 的音色号会载入 ROM 预置 | `emu2413.c:1419`，表在 `:77 default_inst` |
| 三个核（emu2413 / MAME / Nuked）**都编进来了** | `build/**/{emu2413,nukedopll,ym2413,2413intf}.c.obj` |
| 默认用 **emu2413**（列表里排第一） | `2413intf.c:54` "default, because it's better than MAME" |
| **它自带重采样，且只有 2 抽头（线性）** | `emu2413.c:1166 EOPLL_RateConv_new(f_inp, f_out, 2)` |
| 原生速率 = `clock / 72` = **49 716 Hz** | `emu2413.c:1563-1564` |
| `deviceIdFor` 的兜底是 **DEVID_YM2612** | `FmVoiceDriver.cpp:49`——加 `ChipModel` 必须加 `case`，否则会静默按 YM2612 渲染 |
| `ChipModel` 里**没有 `ym2413`** | `VoicePatch.h:19-27`（有 `ym3812`、`vrc7`） |
| `OplDecoder::Impl::Channel` 不存音色号与音量 | `OplDecoder.cpp:105-120`，注释说"the chip genuinely does not say" |
| **`channelState` 把旋律行的音量写死成 15** | `OplDecoder.cpp:579 state.volume = 15;` |

**最后一条是个独立的可见缺陷**：OPLL 的 `0x30` 低四位就是音量，而电平条永远画满。截图里九行都是满蓝条，正是它。

## 三、要改的四处

| 文件 | 改动 | 风险 |
|---|---|---|
| `chips/VoicePatch.h` | 加 `ChipModel::ym2413`；`noteRateFor` → 49 716；`fmOutputBits` → **待测** | 低；但 `deviceIdFor` 必须同步 |
| `backends/libvgm/FmVoiceDriver.cpp` | `deviceIdFor` 加 `case`；新增 `kOplDialect`（2 算子）；OPLL 的速率走 **native** | 中；`Dialect` 是给 4 算子写的，2 算子要先确认 `opPatch` 的取法 |
| `chips/OplDecoder.cpp` | `Impl::Channel` 加 `instrument`/`volume`；实现 `capturePatch`；`channelState` 用真实音量 | 低 |
| `Tests/` | 峰值断言（照 `testOpmReachesFullScale`）、patch 断言、速率断言 | 低 |

**`capturePatch` 的产物（提案）：**

```
预置音色：{ {0x30 + ch, inst << 4 | vol} }                       // 1 条
用户音色：{ {0x00, v0} … {0x07, v7}, {0x30 + ch, 0 << 4 | vol} }  // 9 条
鼓（9..13）：返回 false
```

**它和 OPN 的 patch 是同一个约定**：偏移是"这颗芯片的寄存器地址"，`load()` 原样写回。**区别只是 OPLL 的 patch 恰好很短。**

## 四、需要你拍板的六件事

**D1（核心）· patch 用"音色号"还是"展开的算子参数"？**
我建议**音色号**。它是芯片的语义，且不需要从 ROM 抄表（抄表就有抄错的风险，而且要跟着核的版本走）。
代价：patch 离开这颗芯片就没有意义——**但 `VoicePatch` 本来就是"这颗芯片的寄存器"**。

**D2 · 顺手修音量那个 bug 吗？**
`state.volume = 15` 写死，OPLL 九个通道的电平条永远是满的。**建议一起修**（同一个字段，同一个函数），但它会改变面板上已有的画面，需要重做一次截图基线。

**D3 · 范围只做 OPLL，还是带 OPL2/OPL3？**
OPL2（`ym3812`）有逐算子寄存器，`capturePatch` 是**另一条路径**（更像 OPN）；OPL3 还要两个 bank。
**建议只做 OPLL**，把 `kOplDialect` 留在"2 算子、可扩展"的形状，下一轮接 OPL2 时只加 `capturePatch`。

**D4 · 导出的速率策略**
emu2413 自带 2 抽头线性插值 → **和 OPM 完全同构**：导出让核跑 **native 49 716**，由我们的 Blackman FIR 转换；面板仍 8 kHz（核内插值，够画）。
**建议确认这一条**，它是唯一"照抄 OPM 结论"的地方。

**D5 · 行名 `1`..`9` 要不要改成 `FM 1`..`FM 9`？**
现在是 libvgm 的 `DeviceChannelNames` 覆盖了我们的 `carriesFm` 兜底，所以是裸数字。**建议改**，但会动到按名字匹配的断言（`expectNote` 用 group+name），要一起看。

**D6 · 用户音色（`inst = 0`）支持吗？**
`0x00..0x07` 是**全局**的 8 字节自定义音色，写一次所有 `inst=0` 的通道共用。**建议支持**——成本是 8 条寄存器，而没有它的话用户音色的曲子会退化成通用正弦。

## 五、工作量与顺序（每步都能独立验证）

| 步 | 内容 | 验证 |
|---|---|---|
| 1 | `ChipModel::ym2413` + `noteRateFor` + `deviceIdFor` | 编译；`deviceIdFor` 不再是 YM2612 兜底 |
| 2 | 测 `fmOutputBits(ym2413)` | 峰值断言（TL 全开的 patch） |
| 3 | `kOplDialect` + native 速率 | 一个手写 OPLL patch 能出声、音高对 |
| 4 | `OplDecoder` 存 `instrument`/`volume` + `capturePatch` | 单元断言：`0x30 = 0x10` 得到 1 条寄存器 |
| 5 | 面板 / 拖拽 / 快照导出接通 | 截图：九行出现真实包络形状 |
| 6 | （可选）D2 音量、D5 行名 | 截图基线 |

**粗估**：1-4 步是纯代码与单元断言，**半天**；5 步要重做截图基线并手验**半天**。

## 六、未知与风险

1. **`fmOutputBits(ym2413)` 完全未知**。三个核都编进来了，但只有 emu2413 会跑；它 `mix_output_stereo` 的幅度只能实测。**这是第一步就要量的，不是最后。**
2. **OPLL 是单声道**。`mix_out[0]` 与 `[1]` 大概相同（`EOPLL_calcStereo` 里 pan 固定），所以导出可能和 YM2203 一样需要 ×2。**要实测**：如果 L==R 且峰值只有一半，就是同一件事。
3. **音量是衰减，`0` 最响**：`0x30` 低四位和 OPN 的 TL 同方向，**不是**"数字越大越响"（我第一版写反了，直接把测试音写成了静音）。面板的电平条要的是反向，所以 `channelState` 用 `15 - volume`。
4. **rhythm 模式会借走通道 6-8**：那时它们的 `instrument` 不再是音色号，`capturePatch` 必须返回 false（现有 `channelState` 已有 rhythm 分支，照它做）。
5. **`Dialect` 的 `opPatch` 取法**：现在按 4 算子写的（`op 0..3`，bank 3..9），2 算子要先读一遍 `load`/`noteOn` 才敢改。

## 七、不做的事

- **CSM 模式**（OPM 那份文档已经这么定过，同理）
- **测试寄存器**（`0x0F`）
- **鼓的五个波形**：它们是 one-shot 脉冲，`channelState` 已经画成脉冲，没有 patch 可言
- **Nuked / MAME 两个核**：默认用 emu2413，另外两个只在 emu2413 被关掉时才轮到；不针对它们做校准

## 八、实现记录（做完之后回填）

| 项 | 实测/结论 |
|---|---|
| `fmOutputBits(ym2413)` | **12**——声明 16 时单个用户音色峰值只有 `0.063`（1/16 满幅）；改 12 后 `1.013` |
| 用户音色的字节序 | **按寄存器交错**：`0x00/0x01` = 两个算子的 `0x00`，`0x02/0x03` = 两个算子的 `0x20`，依次类推（`EOPLL_patchToDump`）。**按 OPL2 的"先算子后寄存器"写法会让 AR 一直是 0，结果是完全静音** |
| 渲染通道 | **恒为 0**：patch 自带 `0x30`，方言写 `0x10`/`0x20`，两者落在同一通道上 |
| 音量 | `0` 最响；patch 原样携带（文件该多轻就多轻），面板取反显示 |
| 速率 | 导出跑 native 49 716 + 我们的 FIR（核内是 2 抽头线性） |
| 行名 | `FM 1..9`（libvgm 的表给的是裸数字，按 D5 覆盖） |

**一条留给下一颗的**：`emu2413` 的 `0x30` 同时决定音色与音量，而 `0x20` 的 bit 5 在这份核里是
"sustain flag"、在解码器里被当作 key-off——**两处语义要各自确认，别照抄。**

## 九、一句话

> **OPLL 的 patch 是一个字节（音色号），不是十五张表。**
> 因为它"没有算子寄存器"就认定"没有 patch"，是把"寄存器里有什么"和"芯片会发什么"混成了一件事——
> 而这正好是这一轮那个 bug 的同一种误读：**以为缺的东西，其实只是没去问对对象。**
