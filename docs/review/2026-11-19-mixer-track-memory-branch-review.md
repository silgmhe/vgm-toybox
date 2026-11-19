# 混音器轨道内存与后端实例化分支评审

**评审对象**：`feature/fm-visualisation..issue/mixer-track-memory`，11 个提交，22 文件 **+830 / −61**
**评审分支**：`issue/mixer-track-memory`
**依据**：`.codebuddy/rules/code-quality.md`
**方法**：逐条对照规则的 0–13 条与提交前检查清单，每一项用**文件:行号**取证；内存用
`vgm_app_test` 的峰值采样复核，渲染一致性用 `vgm_nsf_probe --check` 复核。
**结论**：**方向与实现都成立，但不建议按现状合并。** 有四处需要先处理：一处死代码、
一处音频线程上的重活、三处历史缺陷没有回归测试、一处 UI 高度写死。

> 这个分支修了两个很难查的 bug（`pulse.muted` 粘滞、N163 通道镜像），两处修复本身是对的，
> 也留下了写给下一个人的注释。问题不在修复质量，在**修散文里承认过的坑没有一道断言**。

---

## 一、评审方法

规则的每一条都用**文件:行号**取证，没有证据的条目不算通过。功能面另用：

| 证据 | 覆盖什么 |
|---|---|
| `ctest` 54/54 | 既有行为未回退 |
| `vgm_nsf_probe --check` 0.000000 / 4096 帧 | 逐通道总和 == 混合输出 |
| `vgm_app_test` 峰值内存 | `07` 20.7 MB、`01` 27.3 MB |
| `vgm_nsf_probe --scan` | 扫描耗时 2.67 s / 18.03 s |

**没有覆盖的**：`pulse.muted` 是否每帧复位、N163 的通道顺序、`SilentTrackPlayer` 的输出、
两条渲染入口是否都轮询。这四项见 P0-2。

---

## 二、阻塞项

### P0-1 `visualiser.analysing` 成了死键

`KeyboardRowPaint.cpp` 改成"用灰色画音符"之后，这个键不再被任何代码引用：

```powershell
Select-String -Path plugin\Source -Recurse -Pattern 'visualiser.analysing'
# （无结果）
```

但两份语言文件里都还在：

```
plugin/assets/i18n/en.lang
plugin/assets/i18n/zh-Hans.lang
```

**规则**：§5「删掉不再被引用的代码、文案键、常量；『以后可能有用』的东西由版本库保管」。

**修法**：两个文件各删一行。若确实想留给将来的标记，就在 `KeyboardRowPaint.cpp` 里留一句
注释指明去向——但按规则，删掉，需要时从版本库取。

---

### P0-2 三个历史缺陷，一个断言都没有

这是本分支最贵的两处修复，也是最该被钉住的两处：

| 缺陷 | 修复位置 | 现有测试 |
|---|---|---|
| `pulse.muted` 只置不清，一条通道整首消失 | `NesApuDecoder.cpp` `clockSweep` | **无** |
| N163 `mask` 位未镜像，通道全反 | `nes_n106.cpp` `RenderChannels` | **无** |
| 扫描轮询只在 `render()`，逐通道路径不轮询 | `NsfPlayer.cpp:202` | **无** |

`ChipDecoderTest.cpp` 里所有 `muted` 断言都是别的芯片（OPX 的 -96 dB 组、MSM5232 的
`channelState`），**NES APU 的 sweep 静音一条都没有**。`PlaybackEngineTest` 里的 `muted`
全是轨道静音，与此无关。

**规则**：§7.5「核心逻辑必须有单元测试，覆盖边界与**历史缺陷**」；§0 点名的四件事里，
「`$4013` 位宽错了四轮」正是同一个形状——**修好了，但没有一行代码记得它修过**。

**为什么这次尤其不能省**：`muted` 的修复是一行 `pulse.muted = false`，删掉不会有任何测试变红。
而它的症状是"有可视化、没声音"——**排查花了整个分支的时间**。

**修法**（三条，都可写进 `ChipDecoderTest.cpp`）：

1. **sweep 静音每帧重算**：构造一个 `$4001` 配置让 `target > 0x7FF` 的流，断言那一帧
   `channelState` 静音、**下一帧恢复非静音**。没有第二句，这条测试等于没测。
2. **N163 通道顺序**：`RenderChannels` 写入第 0 个位置时，`GetTrackInfo(0)` 报的通道名必须是
   同一个。这是 §10「通道映射类改动必须跨型号验证」的直接应用，而且**只需要一个芯片就够**
   ——要断言的是"写入位置与读取位置的编号约定一致"。
3. **两条入口都轮询**：断言 `renderChannels()` 在扫描落地后 `isAnalysing()` 变 false。
   可以直接驱动 `NsfPlayer`，不需要 backend。

**关于 `--check` 的盲区**：它比对总和，而一致的置换求和相同，所以**它永远抓不到 N163 这个
bug**。这一点已写进提交说明，但更该写进测试文件的注释里，否则下一个人会以为 `--check` 通过
就代表通道顺序正确。

---

### P0-3 `skipHint` 高度写死 160，长文案会被截断

```cpp
// SettingsOverlay.cpp, VgmSettingsPage::resized
skipSwitch.setBounds (area.removeFromTop (24));
area.removeFromTop (6);
skipHint.setBounds (area.removeFromTop (160));
```

中文文案 480 字、英文 780 字符，12.5 px 字号，行高约 17 px。宽度取决于窗口：

| 窗口 | `pageContent` 宽 | 中文行数 | 需要 | 英文行数 | 需要 |
|---|---|---|---|---|---|
| 默认 820×650 | ≈ 550 px | ≈ 11 | **187 px** | ≈ 9 | 153 px |
| 放大 1500×900 | ≈ 830 px | ≈ 8 | 136 px | ≈ 6 | 102 px |

**默认窗口下中文会截断**，英文勉强放下。放大窗口后就够了——**这正是"写死高度"的典型症状：
它只在某一个窗口尺寸上正确**。

`NesSettingsPage` 的 `mixerHint` 给的是 86，文案约 250 字，同一张表算下来默认窗口也需要约
96 px。**所以这不是新问题，是这个分支把它放大了两倍。**

**规则**：§7.5「界面用眼睛」。上面的数字是估算，**结论要用截图确认**：

```powershell
build\plugin\vgm_app_test <脚本>   # shot 到 build/shots/vgm-settings.png
```

脚本需要能切到 VGM 页——目前 harness 没有这个命令（见 S6）。

**修法**：给 `View` 加一个 `hintHeight (const juce::Label&, int width)`，用
`juce::TextLayout` 或 `Label::getHeightForWidth` 算高度，`resized()` 里现算。硬编码的高度
只要文案一改就错，而文案是最常改的东西。

---

### P0-4 `vgm_chip_test` 构建不了，而 `ctest` 一直在跑它的旧二进制

**这一条是在补 P0-2 时发现的，它把 P0-2 从"没写测试"变成"没有地方写测试"。**

```
vgm_chip_test 的源列表 : Source/backends/libvgm/OpnVoiceDriver.cpp 等
target_include_directories(vgm_chip_test PRIVATE Source)      ← 只有 Source
target_link_libraries      : 没有（vgm_opn_test 才有 ${LIBVGM_TARGETS}）
```

`OpnVoiceDriver.cpp` 第 6 行要 `emu/EmuStructs.h`，于是：

```
fatal error C1083: 无法打开包括文件: "emu/EmuStructs.h": No such file or directory
```

目标是 `EXCLUDE_FROM_ALL`，所以 `cmake --build` 全量时不会碰它；而 `ctest` **不构建**，所以
`Test #1: chip` 跑的是一份更早的 `vgm_chip_test.exe`——**时间戳早于本分支对
`NesApuDecoder.cpp` 的修改**。

**结论**：`54/54` 里那一项从未验证过 `pulse.muted` 的修复，也从未验证过 N163 的镜像。
它是绿的，因为它测的是旧代码。

**这是本分支最该修的一条，也是"54/54 通过"这句话为什么不能单独作为证据。**

**修法**（二选一，都需要单独一轮，因为它会改变回归基线）：

1. 把 `backends/libvgm/` 的源从 `vgm_chip_test` 移出——它自己的注释就写着 "no libvgm"，
   而列表里有七个 libvgm 源；
2. 或者补 `libs/libvgm` 的 include 并链接 `${LIBVGM_TARGETS}`——但那样它就变成了第二个
   `vgm_opn_test`。

**无论选哪个，修完后第一次跑 `chip` 才算真的跑了这个分支的改动。**

---

## 三、应修项

### S1 `setNonlinearMixer` 在音频线程重建 65536 项的表

```cpp
// NsfPlayer.cpp —— 由 captureState 调用，而 captureState 每块都跑
void NsfPlayer::setNonlinearMixer (bool enabled) const
{
    if (enabled == nonlinearMixer) { return; }   // 值不变就返回，挡掉了 99.9%
    ...
    dmc->SetOption (xgm::NES_DMC::OPT_NONLINEAR_MIXER, enabled ? 1 : 0);
}
```

```cpp
// nes_dmc.h:33
uint32_t tnd_table[2][16][16][128];              // 65536 项 = 256 KB
```

`SetOption` 无条件调用 `InitializeTNDTable`，后者把 65536 个格子全部重算，每个格子含一次浮点
除法。**这发生在音频线程**——`captureState` 由 `PlaybackEngine::render` 在块末尾调用。

**已做的缓解是对的**（值不变就返回），所以只在用户拨开关后的**第一块**触发一次，约 0.5–1 ms。
在一个 512 采样的块预算（11.6 ms）里它不致命，但它是**一次可避免的音频线程重活**。

**为什么没有更好的位置**：NSF 的设置页没有"重载当前歌曲"的回调（VGM 页现在有了）。所以
`captureState` 是唯一能"让已开的歌立刻生效"的出口。

**两个选项**：

1. **给 NSF 页也接上重载**（和 VGM 一样），然后从 `captureState` 里删掉这次推送。干净，但
   NSF 重载要重建 6502 与扫描，代价远大于 1 ms 的表重建——**不划算**。
2. **保留**，在注释里写明"这一块会花约 1 ms，是刻意的取舍"。**我倾向这个。**

**无论选哪个，现在缺的是那句注释**——代码里写的是"only applied when the value changes"，
没说"变化这一次会让音频线程跑 65k 次除法"。

---

### S2 `pollScan` 有两个调用点，将来会有第三个

```cpp
NsfPlayer.cpp:748    pollScan (frames);    // render()
NsfPlayer.cpp:1612   pollScan (frames);    // renderChannels()
```

这正是这个分支修的 bug 的形状：**逻辑只有一份，但调用点每加一个入口就要记得加一次**。

**规则**：§4「加东西应该只改一处」。

**修法**：把两个入口的共同前缀收进一个 `prepareBlock (int frames)`，里面做 `pollScan` 与
`clocksPerSample` 的计算。加第三个入口时，它要么调用 `prepareBlock`，要么根本不该存在。

---

### S3 `reloadForPitchChange` 现在承担两个语义

```cpp
PluginEditor.cpp:209   settingsOverlay.onSampleRootChanged     = [this] { reloadForPitchChange(); };
PluginEditor.cpp:213   settingsOverlay.onSkipSilentTracksChanged = [this] { reloadForPitchChange(); };
PluginEditor.cpp:813   void ToyboxAudioProcessorEditor::reloadForPitchChange (void)
```

函数名只说 pitch，现在也服务"跳过静音轨"。**规则 §1「名字说明意图」**。

**修法**：改名 `reloadCurrentSong()`，两处调用与函数内注释一起改。改名不改行为，可以独立提交。

---

### S4 `SilentTrackPlayer::render` 返回 `numSamples`

```cpp
SilentTrackPlayer.h:38
int render (float* left, float* right, int numSamples) override
{
    std::fill (...) ; std::fill (...) ;
    return numSamples;
}
```

返回"渲染了这么多"但实际只清零。**这个返回值是对的**——`PlaybackEngine` 用 `rendered > 0`
决定是否继续算峰值与混音，返回 0 会让它走 `continue` 分支、**跳过把清零后的缓冲混进母线的
那一步**。而那条缓冲是共享的，跳过就会留下上一条轨的音频。

**这不是缺陷，但它是一个反直觉的正确**。注释只解释了"清零而不是跳过"，没解释"为什么返回
非零"。**补一句**，否则下一个人会把它改成 `return 0` 以求"诚实"。

---

### S5 `trackHasEvents` 的语义改了两轮，名字没变

这个名字现在表示"这条轨在事件流里出现过"，**且是否采用它由设置决定**：

```cpp
LibvgmSongInstance.cpp:771  （skip 判断）
LibvgmSongInstance.cpp:782  （SilentTrackPlayer）
```

同一份数据，两个决定因素（事件、设置）。**这不是缺陷**（§13 说的是"两个写入点"，这里是
一个写入点 + 一个读取时的开关），但名字没有体现后半句。

**修法**：`trackHasEvents` → `trackProducedEvents`，并在声明处写一句"是否据此跳过由
`settings::libvgm::skipSilentTracks` 决定"。改名可以独立提交。

---

### S6 harness 打不开设置页的某一页

`shot` 能拍覆盖层（`export-options-shot.txt` 就是这么拍的），但设置侧栏的树节点没有
componentID，脚本点不到 VGM 页。**这正是 P0-3 无法当场验证的原因**，也是它该被补上的理由。

**修法**：给 `SettingsOverlay` 的 `CategoryItem` 设一个 `settings.tree.<id>` 的 componentID，
脚本就能 `click settings.tree.vgm`。约 5 行，纯测试基础设施。

---

## 四、观察项

### O1 `libs/nsfplay` 的改动没有集中记录

```cpp
// libs/nsfplay/xgm/devices/Sound/nes_n106.cpp  RenderChannels
mask = ~(1 << (7 - i));
```

第三方目录被改了。规则没有禁止（`docs/review/2026-10-24-nsfplay-integration.md` 说明过这是
vendored 源码），但**升级 nsfplay 时这行会被覆盖**。

**建议**：在 `docs/` 已有的 nsfplay 集成文档里维护一张"我们对第三方代码做了哪些修改"的表，
一行一条。这比在每个 `// **...**` 注释里解释更可靠——注释跟着文件走，表跟着升级流程走。

### O2 `settings::libvgm` 与 `backends::libvgm` 同名不同域

`SettingsDefaults.h` 里新增了 `namespace libvgm`。与 `settings::nsf` / `backends::nsf` 的
对称性一致，**这是对的**；但它与 `toybox::backends::libvgm` 在同一份代码里同时可见，
`settings::libvgm::skipSilentTracks` 与 `backends::libvgm::...` 需要读者分辨前缀。
**不改，记一笔**。

### O3 `.lang` 里的 `\n` 是本项目首例

```cpp
LanguagePack.cpp:38    case 'n': result << juce::newLine; break;
```

解析器支持，但现在**没有其他文案用过**。`VgmSettingsPage` 的 hint 是第一个。这意味着多行
文案的渲染路径从未被验证过——与 P0-3 一起看。

### O4 提交 `5047734` 的标题

`revert(libvgm): a track is judged by its own events again` —— 内容是"回退判据"，标题准确。
但它**同时**依赖前一个提交 `f61d205`（修 `muted`）才成立。单独 cherry-pick 会退回误判。
提交说明里已经写明这条依赖，**保持现状**。

---

## 五、逐条规则对照

| 规则 | 结论 | 证据 |
|---|---|---|
| §0 时间是充足的 | **通过** | 无"先这样"；两处排查过程写进了 `docs/issue/2026-11-19-*.md` |
| §1 名字说明意图 | **S3、S5** | `reloadForPitchChange`、`trackHasEvents` |
| §2 注释写为什么、只写一次 | **通过** | 见"做对了的地方" |
| §3 高内聚低耦合 | **通过** | 设置在 store，后端读；UI 里没有芯片名 |
| §4 加东西只改一处 | **S2** | `pollScan` 两个调用点 |
| §5 不留技术债 | **P0-1** | `visualiser.analysing` 死键 |
| §6 可测试 | **P0-2** | 三个缺陷无断言 |
| §7 颜色走主题 | **通过** | 未新增颜色；`analysing` 用 `theme::secondaryText()` |
| §7.5 核心用断言、界面用眼睛 | **P0-2、P0-3** | 同上 |
| §8 提交即最小可读单元 | **通过** | 11 个提交各自一件事，`--scan` 单独一个 |
| §9 重构不夹带行为 | **通过** | `e89579a` 的契约变更在说明里单列 |
| §10 掩码改动跨型号验证 | **部分** | N163 修了，但只在 N163 上验证（§10 要求另一颗通道数不同的芯片） |
| §11 读状态前问所有权 | **通过** | 未新增 `std::move` 后的读取 |
| §12 位宽去实现里读 | **通过** | `pulse.muted` 的判据来自 nsfplay 实现，不是规格记忆 |
| §13 一份数据一个来源 | **通过** | `trackHasEvents` 单点填写；默认值只在 `SettingsDefaults.h` |

---

## 六、做对了的地方

不写这一节，评审就只剩指责。这个分支有四处值得保留为范例：

**1. 注释写了"为什么"，而且写的是当时不知道的部分**

```cpp
// NsfPlayer.cpp  pollScan
// It used to live inside `render()` alone, which was correct while every song mixed through
// one player - and stopped being correct when `inPlaceChannelMix` made `renderChannels()` the
// only entry point that runs.
```

**它承认了自己是第二次修**，并指出了失效条件。这正是 §2 想要的。

**2. 把验证数据抄进了注释**

`NsfBackend.cpp` 的 `caps.isolation` 变更处直接写着：

```
channels sum vs mixed: worst difference 0.000000 over 4096 frames
```

不是"已验证"，是**可复核的数字**。下一个人可以重跑同一条命令。

**3. 空对象替代空指针，并把契约写进接口**

`SongBackend.h` 的 `createPlayer` 现在承诺"只在这一种情况下返回 null"，`SilentTrackPlayer`
的注释解释了"第九处会被用户遇到"。**这是把一个事实从 N 个调用点收回一个地方的范例。**

**4. 事故写成了文档**

`docs/issue/2026-11-19-a-muted-flag-that-never-cleared.md` 记了四轮排查、两轮弯路、六条教训。
§0 里那四件事，这一次没有重复。

---

## 七、整改顺序

| 顺序 | 项 | 成本 | 状态 |
|---|---|---|---|
| 1 | **P0-1** 删死键 | 2 行 | ✅ `b129f09` |
| 2 | **S6** `settings <pageId>` | 约 30 行 | ✅ `4e11b1f` |
| 3 | **P0-3** `WrappedLabel` + 实测高度 | 约 40 行 | ✅ `f4e7931` |
| 4 | **S3、S5** 改名 | 纯改名 | ✅ `ca2b4d4` |
| 5 | **S2** `prepareBlock` | 约 15 行 | ✅ `d22fa4a` |
| 6 | **S1** 补注释 | 3 行 | ✅ `d22fa4a` |
| 7 | **P0-4** 让 `vgm_chip_test` 能构建 | 未知 | ⬜ **下一步** |
| 8 | **P0-2** 三条回归测试 | 每条 30–60 行 | ⬜ 依赖 P0-4 |
| 9 | **O1** 第三方改动表 | 文档 | ⬜ |

**P0-4 与 P0-2 完成前不建议合并**：现在合并，`54/54` 里那个 `chip` 项仍然指向旧二进制，而
这个分支最贵的两处修复就永远没有断言。

---

## 八、整改状态

**已完成**（`b129f09` … `d22fa4a`）：

| 项 | 提交 | 证据 |
|---|---|---|
| **P0-1** 删 `visualiser.analysing` | `b129f09` | `Select-String` 无结果 |
| **S6** `settings <pageId>` 命令 | `4e11b1f` | `OK  settings page vgm` |
| **P0-3** `WrappedLabel` + 实测高度 | `f4e7931` | 中英两语言截图，字号与邻行一致 |
| **S3** `reloadForPitchChange` → `reloadCurrentSong` | `ca2b4d4` | 纯改名 |
| **S5** `trackHasEvents` → `trackProducedEvents` | `ca2b4d4` | 同上 |
| **S2** `prepareBlock` 收拢两个入口 | `d22fa4a` | `--check` 0.000000 / 4096 帧 |
| **S1** 补音频线程重建表的说明 | `d22fa4a` | 注释 |

**P0-3 的结论与评审时写的相反**：不是截断，是**压缩**。`Label` 用 `drawFittedText`，放不下时
缩小字号而不是报告溢出，所以 160 像素在英文下刚好、在中文下把字压小了。两个故障叠在一起，
只有截图能同时看见它们。

**已完成（第二批）**：

| 项 | 提交 | 证据 |
|---|---|---|
| **P0-4** `vgm_chip_test` 能构建了 | `f35881c` | 删掉只被 `LibvgmTrackPlayer` 引用的 `OpnVoiceDriver.cpp`；第一次真构建，无既有失败 |
| **P0-2a** sweep 静音每帧重算 | `f71b23d` | **把修复注释掉后该条 FAIL、退出码 1**；恢复后 PASS |

P0-4 的修法是删一个源文件，不是补 include：那个文件只被 `LibvgmTrackPlayer.cpp` 用到，而后者
不在这个 target 里。补 include 会让 `vgm_chip_test` 变成第二个 `vgm_opn_test`，与它自己注释里
写的 "no libvgm" 相悖。

**P0-2a 的断言是这样验证的**（规则 §12 第 4 条：断言里只写"应当"）：

```
OK    the pulse sounds until the sweep runs
OK    a sweep that overflows silences the channel
FAIL  and clearing the sweep brings it back - the flag describes this frame
Result : FAILURES                              ← 退出码 1
```

**只有第三条能分辨对错**：sweep 开着时每一帧都会重新溢出，"仍然静音"是正确实现和粘滞实现给出
的同一个答案。把 sweep 关掉，两者才分道。

**已完成（第三批）**：

| 项 | 提交 | 证据 |
|---|---|---|
| **P0-2b** N163 通道顺序 | `6f08836` | `--order` 八行逐一对齐；去掉镜像后 row 6/11 MISMATCH |
| **P0-2c** 两条入口都轮询 | `6f08836` | `--poll` 发布前后 `analysing` 1→0；绕过 `prepareBlock` 后保持 1 |

这条落地在 `vgm_nsf_probe` 而不是 `vgm_chip_test`，因为两件事都发生在 `NsfPlayer` 层：N163 的
顺序是 `renderChannels` 与 `captureState` 之间的事，轮询是渲染入口之间的事。**这正是它们当初
没被想到的原因**——`--check` 证明了求和正确，而一个一致的置换求和也正确。

**两条都验证过会失败**，数据记在提交说明里：

```
--order，镜像放回去：
row 6  Wave 7  freq 1244.49  rms 0.000000  MISMATCH
row 11 Wave 2  freq    0.00  rms 0.007355  MISMATCH      ← 6 + 11 = 17，镜像的两个位置

--poll，绕过 prepareBlock：
before publish: analysing=1
after  publish: analysing=1
poll: the per-channel path never saw the scan            exit 1
```

**未完成**：

| 项 | 原因 |
|---|---|
| **O1** 第三方改动表 | 文档工作，可以随时补 |

**P0-2 的三条全部落地**；`54/54` 里的 `chip` 项现在跑的是当前代码，`--order` 与 `--poll` 是两个
可以单独运行的断言。
