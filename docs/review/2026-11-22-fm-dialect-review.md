# `refactor/fm-voice-dialect` 全面审查

> 2026-11-22。范围：30 个提交，`plugin/` 下 1733 增 / 695 删（另 1159 行文档）。
> 依据：`.codebuddy/rules/code-quality.md`（第 0–13 条 + 提交前自检）、上一轮审查报告五项已闭合。
>
> 结论：**行为正确、注释质量高于平均**（每处取舍都写了理由，四处"一个值两个含义"的坑都被记录）。
> **但有两处 HIGH：一处在导出驱动里会让刚修好的 OPM 滋滋复发；一处是 `ctest` 里有一个测试在跑七天前的二进制。**

## 修复进度

| 编号 | 内容 | 状态 |
|---|---|---|
| HIGH-1 | `exportDriver` 在"请求速率为 0"时复用旧核 | ✅ `e55ac81` |
| HIGH-2 | `vgm_core_test` 编译不了，`core` 测试跑陈旧二进制 | ✅ `e55ac81` |
| **HIGH-3** | **`vgm_mixer_test` 26/28 红，`54/54` 一直是空的** | **待决定（见 §2.5）** |
| MEDIUM-1 | 批量导出不传 `targetRate`，与拖拽不一致 | 待修 |
| MEDIUM-2 | FIR 每样本每抽头重算 `sin`/`cos` | 待修 |
| MEDIUM-3 | `kExportNoteRateFallback = 8000` 是"谎报"的默认值 | 待修 |
| MEDIUM-4 | `fmOutputBits` 被两处以不同对象为键各读一次 | 待修 |
| MEDIUM-5 | 单声道 ×2 无钳位 | 待修 |
| LOW-1..5 | 见 §4 | 待修 |

**HIGH-1 与 HIGH-2 已修并有断言**：`vgm_mixer_test` 里新增三条驱动真实 `renderNoteAt` 的检查
（48 kHz 档、YM2151 转芯片速率、再切回 48 kHz），中间那条在旧代码上会红。

---

## 1. HIGH-1：`exportDriver` 会带着上一次的速率继续用

`LibvgmTrackPlayer.cpp:1287-1297`：

```cpp
const double wantedRate = (chosen->model == chips::ChipModel::ym2151) ? 0.0 : shape.rate;

const bool wrongRate = exportDriver != nullptr && wantedRate > 0.0
                       && std::abs (exportDriver->rate () - wantedRate) > 0.5;

if (exportDriver == nullptr || wrongRate)
{
    exportDriver = makeFmVoiceDriver (true, wantedRate);
}
```

**`0.0` 在这里有两个意思**：对工厂它是"跑芯片自己的速率"，对这段判断它变成了"不关心，别重建"。
`wantedRate > 0.0` 这一半把 OPM（永远 0）和"设置选芯片速率"（也是 0）排除在重建之外。

两条可达路径：

1. **导出速率设置从"跟随宿主"改成"芯片速率"** → `shape.rate = 0` → 不重建 → 驱动里仍是
   `exportRate = 48000` → `load` 按 48000 启动核。**设置被静默忽略**，文件仍是 48 kHz
   （而且 `target = source`，低通不生效，所以听上去没问题，只是承诺错了）。
2. **同一会话里先导一颗非 OPM、再导 YM2151** → OPM 分支给 0 → 不重建 → 驱动仍按 48 kHz 启动
   **`nukedopm`** → 它内部的无滤波线性插值回来 → **`9777dd7` 修掉的那层滋滋复发**。

第 2 条正是这一轮最后三份文档的主题，而它被一个 `> 0.0` 重新打开。

**修法**：比较"请求值"而不是"生效值"。存一份 `mutable double exportRateRequested = -1.0;`，
`shape.rate != exportRateRequested` 就重建，并把 `shape.rate` 原样交给工厂（0 由 `load` 解释为芯片速率）。

**断言**：`renderNoteAt` 是纯函数式的，可以直接驱动——先以 `shape.rate = 48000` 渲一次 YM2612，
再以 `shape.rate = 0` 渲一次 YM2151，断言第二次的 `RenderedNote::rate == noteRateFor (ym2151)`。
今天会红。

## 2. HIGH-2：`ctest` 里的 `core` 在跑七天前的二进制

```
build/plugin/vgm_core_test.exe   11-15 16:13
FmVoiceDriver.cpp                11-22 16:51
plugin/CMakeLists.txt:693        Source/backends/libvgm/FmVoiceDriver.cpp
```

`cmake --build --target vgm_core_test` 失败：

```
FmVoiceDriver.cpp(6): fatal error C1083: 无法打开包括文件: "emu/EmuStructs.h"
```

它没有 libvgm 的 include 目录（`vgm_opn_test` 有 `target_link_libraries (... ${LIBVGM_TARGETS})`，
它没有），而 `CMakeLists.txt:683-687` 的注释还写着：

> **Deliberately links no libvgm code**: if the core still depended on the third-party engine, this target would not link.

**这句话和它下面第 693 行的源列表直接矛盾。** 结果是：

- `ctest -N` 里它是 `Test #3: core`，54 项之一；
- 它**跑的是 11-15 那个可执行文件**，本分支一行代码都没进去；
- `54/54` 里有 1 项是空的。

**这正是本项目反复付账的形状**：没有被断言守着的路径会裂，**被断言守着但跑的是旧二进制的路径连裂都看不见**。

**修法**（二选一）：给它补 `target_link_libraries (... ${LIBVGM_TARGETS})` 并修正注释；
或者把 `FmVoiceDriver.cpp` 从源列表里去掉、并把 `core` 从 ctest 注册里去掉——**前提是确认它真的不需要**。
无论哪条，**`PlaybackCoreTest.cpp` 里那句"这条断言在 `vgm_opn_test`"才说明得通**。

## 2.5 HIGH-3：`54/54` 一直是空的

修 HIGH-1 时，`vgm_mixer_test` 的二进制也被重建了一次。**它跑出 26/54 失败**，而全部 28 个
`engine.*` 里只有 2 个是绿的。失败集中在同一处：

```
seek to 1.00 s -> position 1.00 s, spread 1.0000 s
FAIL every track sits on the seek target          (PlaybackEngineTest.cpp:1084)
after looping: position 2.00 s, spread 2.0000 s
FAIL every track lands on the same position after looping   (:1093)
```

`spread == position` 的含义是：至少一个 track 的 `getTrackPositionSeconds` 返回 **0**，
而引擎本身的位置是对的。**每一个 `engine.*` 都这样**，包括 `engine.psg`、`engine.opx`。

**归属已用对照实验确认：与本分支无关。** 把 `plugin/` 回退到 `feature/fm-visualisation`
重编同一个 `vgm_mixer_test`，`engine.ym2610` 报**完全相同的两条**、`spread` 同为 1.0 / 2.0。
分支的 diff 也确实没有触碰位置代码（`PlaybackEngine` 只改了 `renderNoteAt` 的签名，
`LibvgmTrackPlayer` 只改了导出路径）。

**所以这是仓库级的既有问题**：这套集成测试"很久以来"只在**陈旧二进制**上显示为绿。
本分支没有制造它，但**它是 `54/54` 这个数字不可信的原因**，而这一轮的所有结论都引用过那个数字。

**它不该在这个分支里修**——`inPlaceMode` / 一 player 一 track 的构造 / `seekAllPlayersPlayTo`
都要读一遍，而它与 FM 导出无关。**但任何"全绿"的结论，在它修好之前都不成立。**

下一步建议单独一轮：先让 `vgm_mixer_test` 在一个 fixture 上真正跑通（哪怕先只修 `spread`），
再考虑它在 ctest 里的 28 个注册项。**在此之前，本分支的验证依据应当是**：

| 依据 | 可信度 |
|---|---|
| `vgm_opn_test`（音高、位宽、滤波器、OPM 出声） | ✅ 重建后真跑 |
| `vgm_core_test`（54 项里的 `core`） | ✅ 重建后真跑（本分支修好） |
| `vgm_chip_test`（OPM stride、capturePatch） | ✅ 重建后真跑 |
| 面板截图 `vgm_app_test` | ✅ 真跑 |
| **`vgm_mixer_test` 的 28 个 `engine.*`** | ❌ **陈旧/红，不作为依据** |

## 3. MEDIUM

### MEDIUM-1：两条导出路径对速率的处理不对称

`ContentSlot.cpp:1732` 调的是

```cpp
playback::exportWavetables (drawn, ..., mode, seconds, bits);   // 没有 targetRate
```

而拖拽在 `:1460-1464` 明确传了 `targetRate`，并且 `:1457` 的注释写着：

> **The same rate the batch export writes at** ... a drag that produced a different file from File > Export would be a bug the user could hear rather than see.

**这句话现在是假的。** `exportWavetables` 没有 `targetRate` 参数，所以 `writeNoteFile` 里
`target = source`；对 YM2151（核跑 native 55930）意味着：

| 手势 | 采样率 |
|---|---|
| 拖拽 | **48000**（低通后抽取） |
| `Export snapshot` | **55930**（不重采样） |

两个都对（55930 不需要重采样），但**同一行、两个手势、两种文件**，而代码说它们应当一致。

**修法**：给 `exportWavetables` 加 `targetRate`，批量路径传与拖拽同一个值。非 OPM 那档
source 已等于 target，滤波器是恒等，行为不变。

### MEDIUM-2：滤波器每样本每抽头重算 `sin`/`cos`

`WaveExport.h:173-190`。权重只依赖 `t`（`x = 2.0 * normalised * t`，`position = t / half`），
与 `i` 无关——**它是一组常数**，却在 89 489 个样本上各算 31 次，一次音符约 830 万次超越函数调用。
批量导出八行 ≈ 秒级。

**修法**：进入循环前把 31 个权重算进 `std::array<double, 31>`，循环里只做乘加。数值完全等价，
这一条是纯性能，不改变任何样本。

### MEDIUM-3：`kExportNoteRateFallback = 8000` 是"谎报"的默认值

`VoicePatch.h:107` 与 `WaveExport.cpp:373`：

```cpp
const auto source = (channel.noteWaveformRate > 0.0f) ? (double) channel.noteWaveformRate
                                                      : chips::kExportNoteRateFallback;   // 8000
```

`noteWaveformRate` 为 0 时，写出的文件声明 **8 kHz**——**正好是面板速率，也正好是这句话上面
那段注释所说的谎**（"declaring 8 000 about samples made at 53 267"）。

今天所有 FM 路径都会填 `noteWaveformRate`，所以它没有被触发；但**默认值挑了一个"看起来合理、
实际最坏"的数**，和第 5 条的兜底语义问题同形。`channel.voicePatch` 就在手边，
改成 `chips::noteRateFor (model)`，或干脆让 0 表示"拒绝写"。

### MEDIUM-4：`fmOutputBits` 被两处以不同对象为键各读一次

- `FmVoiceDriver::render`（`:532`）用 `loadedModel` 决定缩放；
- `NoteWaveform::render`（`:121`）用 `patch.model` 决定量化步数。

两者今天必然相等（`load` 就是从同一个 patch 设的 `loadedModel`），但**"这个音符的精度"有两个读取点、
两个键**，是第 13 条的典型形状。缩放归一化到 `1/2^(bits-1)` 而量化到 `2^(bits-1)-1`，
一旦哪天不等，症状是"音量差 6 dB"或"多一层噪声"，都不会报错。

**修法**：让 `FmVoiceDriver::noteWidth()` 暴露一个值，或把 bits 作为 `render` 的入参传下去。

### MEDIUM-5：单声道 ×2 没有钳位

`WaveExport.cpp:397-406`。注释解释了为什么是固定增益而不是归一化，理由成立；
但它把样本直接乘 2 而不钳位——**一个峰值 0.6 的 YM2203 patch 会写出 >1.0 的 float 样本**，
在 DAW 里就是削波。既然这里是"补一个声道差额"，补完以后超出满幅的部分应当就地告诉调用者
（钳位 + 计数，或退回 1.5 倍这类保守值）。

## 4. LOW

1. **`FmVoiceDriver.h:65-73` 有两段构造函数的文档注释**，前一段是旧的且已失效
   （"exportQuality true to run the core at the chip's own rate"——`exportRate` 现在可以覆盖它）。
   第 2 条规则：一个坑只解释一次，重复的会各自漂移。
2. **`NoteWaveform.cpp:118-127` 说量化"applied last so full scale stays full scale"**——
   `writeNoteFile` 之后又按 16 位量化了一次（`WaveExport.cpp:408-414`），所以它不是最后一个。
   两段量化串联目前无害（14 位栅格落到 16 位栅格上），但注释已经不成立。
3. **`VoicePatch.h:144` 的"fourteen for the OPN2, sixteen for the rest of the family"**
   与它下面 `ym2610 → 15`、`ym2151 → 14` 矛盾，而文件顶部的表是对的。改首句即可。
4. **`PlaybackCoreTest.cpp` 的 `#include "playback/WaveExport.h"` 已经是死引用**（断言搬走后没人用）。
5. **`FmVoiceDriver.cpp:458` 用 `loadedRate != wantedRate` 精确比较浮点**，而同一文件在
   `LibvgmTrackPlayer` 里用的是 `> 0.5` 容差。值来自同一表达式，所以今天没问题——两处形状不一致而已。

## 5. 遗漏（不是缺陷，是缺口）

| 缺口 | 现状 |
|---|---|
| **两条导出路径都没有断言** | `exportWavetables` 与 `startWaveDrag` 都没被任何测试驱动；「选目录」与「导出」尚未拆开 |
| **`core` 测试是空的** | 见 HIGH-2，占 54 项的 1 项 |
| **OPL 家族与 VRC7 的位宽未测** | 表里写着"未测"，是诚实的；接 OPL 时按 `nukedopm` 的结论一起处理 |
| **OPX 不在两表里** | `noteRateFor`/`fmOutputBits` 都落到 OPN/16 的兜底，注释已说明 |
| **两条导出路径速率一致性无断言** | MEDIUM-1 修好后应当有一条 |

## 6. 规范对照

| 条款 | 结果 |
|---|---|
| 0 时间充足、返工不是 | ⚠️ HIGH-2 是"没跑就以为绿了"的典型 |
| 2 注释写为什么、只写一次 | ⚠️ LOW-1 / LOW-2 / LOW-3 三处陈旧或重复 |
| 3 高内聚低耦合 | ✅ `ui/` 仍不含芯片名，`Dialect` 收得干净 |
| 4 加东西只改一处 | ✅ 加 OPL 只需一个 `kOplDialect` |
| 5 不留技术债 | ⚠️ 坏掉的测试目标、死 include、`= 8000` 的兜底 |
| 6 可测试 | ✅ 滤波器做成内联、音高断言可调用 |
| 7 颜色走主题 | ✅ 无新增字面量颜色 |
| 9 重构不夹带行为 | ✅ `72e1220` 行为未变（截图逐像素一致） |
| 12 位宽去实现里读 | ✅ 四个型号都是实测值，来源写在表里 |
| 13 一份数据一个来源 | ⚠️ MEDIUM-4、MEDIUM-1 |
