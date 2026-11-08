# NSF 曲目选择与 bank 判定分支评审

**评审对象**：`feature/libvgm-visualisation..fix/nsf-driver-init`，12 个提交，18 文件 +654/−20
**评审分支**：`fix/nsf-driver-init`
**依据**：`.codebuddy/rules/code-quality.md`
**方法**：读 diff 与关键函数，逐条对照规则取证（文件:行号）；功能面另用 `46/46`、芯片断言与
三首素材的行数/加载时间复核。

**结论（评审当时）：功能正确，但不能按现状合并。** 一处真实的 UI 线程卡顿、两处状态不一致、一处
"写的时候图快"的编码事故（规则 §0 表格里点名的同一类）。

> **整改状态见文末第八节**：P0-1…P0-7 与 S1、S2 已修，S3/S4/S5/S7 见该节的处置结论。

---

## 一、评审方法

逐条对照规则的 0–12 条与提交前检查清单，每一项都用**文件:行号**取证；没有证据的条目不算通过。
功能本身另用 `46/46` + 芯片断言 + 三首素材的行数/加载时间复核。

---

## 二、阻塞项

### P0-1 切曲目在 UI 线程同步重建，界面冻结约 200 ms

**现象**：`SongInfoPanel` 的下拉框回调直接走完整重建路径。

```
SongInfoPanel.cpp:145  switchToSelectedSong()
  → PlaybackEngine.cpp:121  selectSong()
    → loadImage() → buildSong()          // 同步，消息线程
```

**证据**：`07 Stage 2` 整曲重建 **271 ms**（`vgm_app_test` 加载计时），切曲目走同一条构树路径。

**为什么是缺陷**：文件打开当初特意做成异步（`PlaybackEngine.cpp` 的注释引用 "review M-1"：
读盘耗时不能让界面静默）。切曲目比读盘更重（要重建全部 player），却退回同步。

**规则**：§0（不赶工期，一次做对）、§9（行为变更必须单列并给证据）。

**修法**：新增 `selectSongAsync(index, onDone)`，复用 `loadFileAsync` 的 worker 池与
`loadGeneration` 语义；`songIndex` 不再作为可变成员，而是作为参数传给 `buildSong()`。

---

### P0-2 16 个文件被写入 BOM

```
docs/desgin/libvgm-visualisation.md              docs/memo/2026-11-08-nsf-super-mario-usa-single-event.md
plugin/Source/backends/nsf/NsfBackend.cpp        plugin/Source/backends/nsf/NsfBus.cpp
plugin/Source/backends/nsf/NsfBus.h              plugin/Source/backends/nsf/NsfPlayer.cpp
plugin/Source/backends/nsf/NsfPlayer.h           plugin/Source/playback/PlaybackEngine.cpp
plugin/Source/playback/PlaybackEngine.h          plugin/Source/playback/SongBackend.h
plugin/Source/ui/SongInfoPanel.cpp               plugin/Source/ui/SongInfoPanel.h
plugin/Tests/AppHarnessTest.cpp                  plugin/Tests/NsfProbe.cpp
plugin/assets/i18n/en.lang                       plugin/assets/i18n/zh-Hans.lang
```

**来源**：PowerShell `WriteAllText($path, $text, UTF8Encoding($true))` 的副作用，散落在几乎每个提交。

**影响**：

- 代码文件是纯噪 diff（`-#pragma once` → `+#pragma once`），让 `git blame` 与 review 都多一层噪音；
- `.lang` 文件**碰巧没坏**：`LanguagePack.cpp:116` 剥离 `0xFEFF`。但那是容错，不是契约。

**规则**：§0 表格第一行（"两个语言文件 539 行行尾噪音 —— 为了快，整份写回而不是改那两行"）、
§8（不把格式化与功能改动混在一起）。

**修法**：16 个文件剥掉 `EF BB BF`，单独一个 `chore:` 提交；之后所有文件编辑走
`replace_in_file`（保留原编码），不再整份写回。

---

### P0-3 `resetState()` 不清 `songCount` / `currentSongIndex`

`PlaybackEngine.cpp:1279` 的 `resetState()` 清了 `playing/loaded/song/tracks/...`，但没有清这两个
新成员。

**后果**：`unloadFile()` 之后 `getSongCount()` 仍返回上一首的 34。今天没暴露，是因为
`SongInfoPanel::refresh()` 先查 `isLoaded()` 就 `return` —— **靠调用方的检查顺序掩盖**。

**规则**：§5（缺陷不要用补丁掩盖）；§11（读一个状态前先问"上一次赋值是谁"）。

**修法**：`resetState()` 里把两者复位为 `1` / `0`。

---

### P0-4 `selectSong` 失败会静默停止播放，并把 `songIndex` 留在坏值

```cpp
PlaybackEngine.cpp:138   songIndex = index;        // 先写死
PlaybackEngine.cpp:141   return loadImage (...);   // 失败时 installLoaded 已 resetState()
```

- 失败后**播放已经停止**，而 UI 只把下拉框拨回原值（`SongInfoPanel.cpp:162`），用户看不到原因；
- `songIndex` 仍是失败值 → 之后任何 `rebuildPlayers()`（改采样率、改静音）都会再试一次坏索引。

**规则**：§5（兜底要暴露错误，不能换一种错法）。

**修法**：失败时把 `songIndex` 回滚到 `currentSongIndex`；`error` 冒泡到面板（状态栏或日志），
不要 `catch` 后丢弃。

---

### P0-5 一个概念两个默认值，其中一个已是死值

| 位置 | 值 | 注释说的语义 |
|---|---|---|
| `PlaybackEngine.h:434` `BuildConfig::songIndex` | `-1` | "用文件头指定的曲目" |
| `PlaybackEngine.h:575` `PlaybackEngine::songIndex` | `0` | "第一首，除非用户选了别的" |

`makeBuildConfig()` 永远从成员拷 `0`，所以 `-1` **再也不会出现**。读者要读三处才知道谁生效。

**规则**：§1（一处一个概念）、§2（注释不得与实现矛盾）。

**修法**：随 P0-1 一起，`songIndex` 变成 `buildSong()` 的参数，两个默认值合并成一个
（"调用方给什么就是什么"）。

---

### P0-6 注释与实现不符

`PlaybackEngine.h:75` 的文档写：

> Returns false when nothing is loaded **or the format has a single song.**

实现（`PlaybackEngine.cpp:121-144`）只检查 `index < 0` 与空镜像，**从不看 `songCount`**；
单曲格式收到 `selectSong(5)` 会照常重建。

**规则**：§2（注释解释为什么，前提是它得是真的）、§12-4（断言/说明写"应当"，不写观测到的现状）。

**修法**：要么实现里真的判断 `songCount <= 1` 并返回 false，要么改注释。

---

### P0-7 "新文件 = 第一首"被实现了两遍

```
PlaybackEngine.cpp:110-115   loadFile()       { lock; songIndex = 0; }
PlaybackEngine.cpp:202-207   loadFileAsync()  { lock; songIndex = 0; }
```

第三个加载入口出现时必然漏掉（规则 §4：加东西应该只改一处）。

**规则**：§4、§8（同一条规则写在两处，迟早在第三处失效）。

**修法**：随 P0-1 一起消掉——目标曲目作为 `loadImage()` 的入参，`songIndex` 不再是可变成员。

---

## 三、建议项（不阻塞，但应在本分支还清）

| # | 问题 | 位置 | 规则 |
|---|---|---|---|
| S1 | 卸载分支隐藏控件后**没有 `resized()`**，标题仍留着被挤窄的宽度（右侧多 142 px 空白） | `SongInfoPanel.cpp:78` | §5 |
| S2 | `switchToSelectedSong` 丢弃 `error`，至少应 `Logger` 或走状态栏 | `SongInfoPanel.cpp:162` | §5 |
| S3 | `NsfPlayer` 现有 5 个诊断入口（`onSoundWriteForDiagnostics`、`peekRam`、`statusReads`、`lastStatus`、`soundWrites`），建议聚成一个 `NsfDiagnostics` | `NsfPlayer.h` | §3/§4 |
| S4 | `NsfProbe` 的 lambda 内用了 `static previousClock / framesSeen`，进程内多次运行共享状态 | `NsfProbe.cpp` | §3 |
| S5 | `SongInfoPanel` 由 `const PlaybackEngine&` 变为可变引用，类注释没写"这个面板会触发整曲重建" | `SongInfoPanel.h` | §2/§3 |
| S6 | `installLoaded` 与 `rebuildPlayers` 写了两遍同样的两行（且一处有 null 判断、一处没有） | `PlaybackEngine.cpp:186,1261` | §4 |
| S7 | 缺 `songIndex → startSong` 与 `selectSong` 的断言；`NsfFormatTest` 只覆盖了 `isBanked` | `NsfFormatTest.cpp` | §6/§7.5 |
| S8 | `selectSong` 里对 `lock` 的使用与 `makeBuildConfig()` 的裸读不一致（都只发生在消息线程），加锁暗示了不存在的竞争 | `PlaybackEngine.cpp:132` | §1 |

---

## 四、达标项（点名保留）

- **纯函数 + 断言**：`isBanked()` 的两条新断言（无 bank 表 → 线性；单窗口 → banked）正是 §6 要的形状，
  且断言写的是"应当"（`a 256-byte image with one declared window is banked`），不是观测值；
- **注释写"为什么"**：`[level]`、`[frame]`、`[rom]` 三段诊断各自解释了"没有这个数会误判成什么"，
  不复述代码（§2）；
- **文案单源**：`song.track` 在 `en.lang` / `zh-Hans.lang` 各一行，没有第二份文案（§7）；
- **颜色走主题**：`songLabel` 用 `theme::secondaryText()`，且在 `lookAndFeelChanged()` 里设而不是构造函数（§7）；
- **提交说明**：`89bf64b` 把两次被否定的假设写进了正文，`0c1a7a6` 给出了修复前后的实测数字（§8）。

---

## 五、修复计划（4 个提交，逐个可验）

| 步 | 内容 | 提交类型 | 验收 |
|---|---|---|---|
| 1 | 16 个文件去 BOM | `chore:` | diff 里只剩真实改动；`46/46` |
| 2 | `selectSongAsync` + `songIndex` 变参数（消 P0-1 / P0-5 / P0-7） | `fix(playback):` | 切曲目不再冻结；行为变更段写明 |
| 3 | `resetState` 复位 + 失败回滚 + 错误冒泡（消 P0-3 / P0-4 / S2） | `fix(playback):` | 卸载后 `getSongCount()==1`；失败不改变当前曲目 |
| 4 | 注释订正 + 两条断言 + 卸载路径 `resized()`（消 P0-6 / S7 / S1） | `docs:` + `test:` | `NsfFormatTest` 覆盖 `songIndex → startSong` |

**合并前必须全绿**：`46/46`、芯片断言、`07 Stage 2` 16 行、`02 Green Hill Zone` 11 行、
`03 Scene Select` 24 行、加载 190–270 ms。

---

## 六、检查清单逐条结论

| 清单项 | 结论 |
|---|---|
| 有没有"为了快"写的地方 | **有**：BOM（P0-2）、同步重建（P0-1） |
| 三个月后能否只看函数名说出它做什么 | 是（`selectSong`/`switchToSelectedSong`/`isBanked`） |
| 算术/映射有测试能直接调用 | 部分：`isBanked` 有；`songIndex → startSong` 无（S7） |
| 新增同类东西只需改一处 | 否：加载路径两处重置（P0-7） |
| 留下"先这样"的补丁而不自知 | 有：失败静默（P0-4）、卸载不清状态（P0-3） |
| 注释是否在重复代码说清的事 | 否，注释质量达标 |
| 有没有第二份重复的表/常量/颜色 | 有：两个 `songIndex` 默认值（P0-5） |
| 新加颜色是否走四步 | 不涉及（复用 `theme::secondaryText()`） |
| 标题是 refactor 却夹带行为变更 | 不适用（本分支全是 `feat`/`fix`/`test`/`docs`） |
| 动了掩码/通道映射是否跨型号验证 | 不涉及（本次动的是 NSF 播放头与曲目索引） |
| 断言容差是否小于缺陷 | 本分支未新增带容差的断言 |
| 读的成员是否可能已被 move/clear | **有风险**：`songCount` 在 `resetState` 后仍是旧值（P0-3） |
| 界面与导出卖的是同一个对象 | 是（都读 `engine.getSongCount/getSongIndex`） |
| 截的位宽是否在实现里确认 | 不适用 |
| 固定缓冲大小从取值范围推 | 不适用 |
| 断言写的是"应当"还是观测现状 | 应当（`isBanked` 两条） |

## 八、整改状态（2026-11-08）

| 项 | 状态 | 说明 |
|---|---|---|
| P0-1 同步重建 | 已修 | `selectSongAsync` 走 `loadPool`；面板乐观选中、失败拨回 |
| P0-2 BOM | 已修 | 16 文件按字节剥离，16 insertions / 16 deletions 只差编码 |
| P0-3 `resetState` | 已修 | 复位 `songCount = 1` / `currentSongIndex = 0` |
| P0-4 失败语义 | **按文件打开语义**（用户裁定） | 失败即停播并在状态行报错；未加"保持原曲"分支，理由见下 |
| P0-5 双默认值 | 已修 | 成员删除，索引成为 `loadImage()` 参数；`BuildConfig::songIndex` 默认 0 |
| P0-6 注释不符 | 已修 | `selectSong` 校验 `0 ≤ index < songCount`，注释与实现一致 |
| P0-7 两处重置 | 已修 | 参数化后不再有重置点 |
| S1 卸载不重排 | 已修 | 隐藏选择器后补 `resized()` |
| S2 吞掉错误 | 已修 | `onSwitchFailure` → `PlayView::onEngineError` → 编辑器状态行 |
| S3 诊断入口分散 | 已修 | 合成 `NsfPlayer::StatusReads { count, lastValue }` |
| S4 lambda 内 `static` | 已修 | 改为 `main` 局部变量按引用捕获 |
| S5 面板副作用未写明 | 已修 | 类注释写明"这是唯一会改引擎的卡片" |
| S6 两处重复赋值 | 已修 | `recordSongs (const playback::SongInfo&)` |
| S7 缺断言 | 已修 | 新增 `NsfFile::startSongFor()`，`NsfFormatTest` 四条断言覆盖首/末/越界/负数 |
| S8 锁不一致 | 已说明 | 全部发生在消息线程；`takeSongForRebuild` 持锁只为复制镜像 |

### P0-4 的取舍

切曲目失败与打开文件失败**同样处理**：停止播放、在状态行报出原因、下拉框回到实际打开的曲目。
没有为它增加"失败后保持原曲"的回滚分支——索引已在 `takeSongForRebuild` 校验过、镜像已在内存，
这条路径的失败只可能来自后端自身错误，此时保留一个半坏的状态比停下来更危险；而给 `finishLoad`
加第三种语义会让"加载失败意味着什么"变得不可预测。

### 最终验收

`46/46`、芯片断言、`07 Stage 2` 16 行、`02 Green Hill Zone` 11 行、`03 Scene Select` 24 行；
NSF 显示 `Song [1]`（34 项）、VGM 隐藏两个控件；`song 1` / `34` 接受，`song 0` / `35` 拒绝；
`vgm_nsf_test` 新增四条映射断言；加载 190–270 ms。