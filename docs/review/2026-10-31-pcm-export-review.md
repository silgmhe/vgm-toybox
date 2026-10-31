# PCM 采样导出 Review

**评审对象**：`feature/pcm-sample-export`（`master` → `8af2780`，7 个提交，28 个文件 +1644/−638）
**基线**：`master @7ee84b7`
**依据**：`.codebuddy/rules/code-quality.md`（12 条）
**方法**：逐提交读 diff，对每条规则单独取证；`git diff --numstat` 定位"改动规模与功能规模不符"的文件

## 结论

| 级别 | 数量 | 一句话 |
|---|---|---|
| 必须修 | 2 | 语言文件被整体改行尾；VGM 采集器没有任何断言 |
| 应修 | 4 | 菜单两套渲染、非 NES 白扫全流、跨后端命名空间、同一理由写两遍 |
| 可延后 | 3 | 一处行为微变、一个薄包装、一处锁的嵌套 |

## 修复记录

评审当天全部处理，**六个提交，行为未变**（`46/46`，`Entry080` 仍是 2 个采样，VGZ 仍是 5 个）。

| 编号 | 处理 | 提交 |
|---|---|---|
| H-1 语言文件行尾 | 已修：从 master 恢复，按键位插入 4 条键。**净差异从 539 行降到 2 行** | `3d07486` |
| H-2 VGM 无断言 | 已修：`dpcm_five_samples.vgz` + `pcm-samples.txt`，断言数量与首个采样的名字 | `d1c1dec` |
| M-1 菜单两份渲染 | 已修：`getMenuForIndex` 改为调用 `renderEntries` | `b4b89fe` |
| M-2 非 NES 白扫全流 | 已修：`deviceInfo` 里没有 `DEVID_NES_APU` 就不扫 | `cbe5273` |
| M-3 跨后端命名空间 | 已修：移到 `chips/NesDpcmSample.h/.cpp`，命名空间 `toybox::chips::dpcm` | `c3a2906` |
| M-4 理由写两遍 | 已修：`ScanResult::samples` 保留理由，`dpcmSamples()` 只指向它 | `915b3e7` |
| L-1 `describe` 空分支 | **保留**：`paddedRight` 对空串结果相同，且它现在只在一个函数里 | — |
| L-2 `dpcmRate` 薄包装 | **保留**：五个调用点，删除的收益小于改动面；`M-3` 之后它在 `NsfPlayer` 内部，不再跨文件 | — |
| L-3 `currentSamples` 持锁 | **保留**：无死锁（音频线程不取引擎锁），实测无感；注释已写明 | — |

**H-1 的根因值得单独记一笔**：两个 `.lang` 文件在仓库里是 CRLF，而编辑工具整份按 LF 写入，一次两行的改动变成 539 行。修的时候用了一次脚本——**这是 `file-edits.md` 允许的例外**（需要精确控制行尾，原生编辑做不到），脚本读写都按字节，跑完即删。

**功能是对的，两条路径都验过。** NSF 的 `Entry080` 与 VGM 的 `Good Morning` 都读出了与 FamiStudio 一致的采样数，端到端断言也在。问题集中在**工程完整性**：一个提交把格式化混进了功能，一条新链路只靠手工验证。

---

## 一、必须修

### H-1 两个语言文件被整体改行尾，2 行功能淹没在 539 行里（规则 8）

```
plugin/assets/i18n/en.lang          +275 −264
plugin/assets/i18n/zh-Hans.lang     +271 −260
```

实际只加了 4 条键（各 2 条）。**其余全部是行尾变更**：

```
仓库 master 里：CRLF 264 行
HEAD 里：        LF 275 行
```

`c2761f6`（`feat(ui)`）用编辑工具写入时把 CRLF 转成了 LF，于是整个文件被判为改写。

**影响**：
1. **规则 8 明确禁止**"把格式化、重命名和功能改动混在一起"——这里混的是最没道理的一种。
2. 以后 `git blame` 这两份文件，**每一行都会指向这次提交**，而它们本来只该有 2 行归属这次。
3. 评审时无法从 diff 看出改了哪几条键，只能靠 grep 找回来。

**修法**：把两个文件转回 CRLF 并单独提交，标题写 `fix(i18n): restore the line endings the menu commit rewrote`，说明里给出"真实改动只有 4 条键"。

**这次不是第一次**：`6022fcc`（`selectLastSong`）和一次 `CMakeLists.txt` 的改动都在收尾时发现过同类问题。**写文件之后应该看一眼 `--numstat`，而不是只看编译过了。**

### H-2 VGM 采集器零测试（规则 6、7.5）

`NesDpcmBank.cpp` 214 行，是本次**最大的一个新增单元**，而它没有任何断言。验证方式只有：

```
vgm_app_test 的 `samples` 命令 → 打印 5 行 → 人眼看
```

**`commandSamples()` 只 `printf`，不 `check`。** 它证明了"跑起来是对的"，没证明"改坏了会红"。

对比 NSF 那条路：`NsfFormatTest` 有 14 条算术断言 + 3 条端到端（含"不播放也能拿到 2 个"）。

**这条链路上已经出现过 3 次错误**（`0x70-0x7F`、`0x80-0x8F`/`0x68`、`0xC0-0xDF` 的命令长度），**每一次都是靠"数字不对"发现的**——而下一次不会有 `5` 这个数字摆在眼前。

**修法**：把 `01 Good Morning (Opening).vgz` 拷进 `plugin/Tests/assets/song/`（`dpcm_five_samples.vgz`），加一个 `vgm_libvgm_test` 或在 `vgm_app_test` 里加 `samples.count` 断言，覆盖：

1. 5 个采样，名字是 `dpcm_C000_1008` / `dpcm_C400_1008` / `dpcm_CC00_1008` / `dpcm_CF00_432` / `dpcm_DB00_1024`
2. 每个的 `values.size() == 长度 × 8`
3. `$C000` 的速率是 16884.6 Hz（速率索引来自 `$4010`，不是默认值）

**第 3 条最容易漏**：速率是这一支唯一"读寄存器但不解码"的部分，写错不会有任何声音上的异常，只会让文件播放速度不对。

---

## 二、应修

### M-1 菜单有两份渲染逻辑（规则 4）

```cpp
// MainMenu.h:295  getMenuForIndex —— 一份
for (const auto& entry : groups[index].entries)
{
    if (entry.separatorBefore) { menu.addSeparator(); }
    if (! entry.children.empty()) { menu.addSubMenu (entry.label, renderEntries (entry.children), entry.enabled); continue; }
    // ...八行构造 Item
}

// MainMenu.h:385  renderEntries —— 另一份，同样的八行
```

顶层走前者，子菜单走后者。**两处内容逐行相同**，只差入口。

**规则 4 说的正是这个**："新增一种格式、一个视图……应当是加一行或加一个文件，而不是在四处改代码并祈祷它们的顺序一致。" 这里将来给菜单项加一个属性（比如 `tooltip`），**必须记得改两处**，而漏掉一处的症状是"子菜单项和顶层项长得不一样"——只有点开才看得见。

**修法**：`getMenuForIndex` 改成 `return renderEntries (groups[index].entries);`，把循环体交给那个函数。

### M-2 每个 VGM 都全流扫描一遍，不管有没有 2A03（规则 5 · 性能）

`LibvgmSongInstance::prepare()` 无条件调用：

```cpp
dpcmSamples = std::make_shared<const std::vector<playback::DecodedSample>> (
    collectNesDpcmSamples (playerMemory.data, playerMemory.size));
```

`collectNesDpcmSamples` 的第一段是 `while (reader.next (command))`——**遍历整个命令流**。

对一个 YM2612 或 SPC 的 VGM，这次遍历**注定找到 0 个采样**，代价是白读一遍文件。147 KB 的曲子几毫秒，**十几 MB 的录屏 VGM 就是几十毫秒**，而它花在一个没有 2A03 的文件上。

`enumerate()` 就在这次调用**之前**（`LibvgmSongInstance.cpp:184`，收集在 `:200`），`deviceInfo` 已经填好，判断条件是现成的：

```cpp
const bool hasNes = std::any_of (deviceInfo.begin(), deviceInfo.end(),
    [] (const PLR_DEV_INFO& device) { return device.type == DEVID_NES_APU; });
```

**修法**：`if (hasNes)` 再收集，否则留空 `shared_ptr`（`samples()` 已经处理了 `nullptr`）。

### M-3 `DpcmSample.h` 被跨后端引用（规则 3）

```cpp
// backends/libvgm/NesDpcmBank.cpp:21
namespace dpcm = toybox::backends::nsf::dpcm;
```

**libvgm 后端 include 了 nsf 后端的头**。它在注释里写清了理由（"两首 2A03 的曲子，同一套算术"），`memo` 里也记了该移到 `chips/`——**但注释解释不了一个违反规则的依赖方向**。

`backends/nsf/` 和 `backends/libvgm/` 是**同级后端**，一个依赖另一个意味着：如果将来移除 NSF 后端，libvgm 的 DPCM 导出会编译不过，而错误会指向一个"和 VGM 无关"的文件。

**修法**：把 `DpcmSample.h/.cpp` 移到 `chips/NesDpcmSample.h/.cpp`（芯片解码器都在那），命名空间改 `toybox::chips::dpcm`。代价是四处 `CMakeLists.txt` 源文件列表 + 两个 include。

**这条和 `memo` 里那条是同一件事**——`memo` 把它写成"下次顺手做"，而规则 5 说"有就当场还"。**它是本次唯一一处"先这样"**，应该在这一轮还掉，而不是留给下一次。

### M-4 同一个理由写在两处注释里（规则 2）

```cpp
// NsfPlayer.h  ScanResult::samples
//   Carried here because the walk runs on a player of its own: its Bus is gone by the time
//   anyone exports, and the playback player has only seen the samples the user happened to reach.

// NsfPlayer.cpp  dpcmSamples()
//   The scan's list first: it walked the whole song. This player's own list is what the user has
//   reached so far - a shorter answer to the same question, and the reason an export used to
//   depend on how long the song had been playing.
```

**说的是同一件事**（"扫描走整首，播放只到用户听到的"）。

规则 2："一个坑只解释一次。重复的解释会在两处逐渐不一致。"

**修法**：`ScanResult` 那里保留"为什么采样随结果发布"（数据流），`dpcmSamples` 那里只留"优先用哪个"（选择），把"为什么"删掉并指向 `ScanResult`。

---

## 三、可延后

### L-1 `describe()` 丢掉了空 shortcut 的分支（规则 9）

原来：

```cpp
const auto shortcut = EditorCommands::describe (entry.key);
if (shortcut.isNotEmpty()) { text << shortcut.paddedRight (' ', 10); }
else                        { text << juce::String().paddedRight (' ', 10); }
```

现在：`EditorCommands::describe (entry.key).paddedRight (' ', 10)`。

**结果相同**（空串 `paddedRight` 也是十个空格），**但这是一次行为简化夹在 `feat(ui)` 里**。规则 9 要求重构不夹带行为变更，这次夹带的是"等价但要论证等价"的简化。

`c2761f6` 的标题是 `feat(ui)`，可以接受；**如果再出现一次，就该拆开**。

### L-2 `dpcmRate` 成了一行薄包装（规则 1）

```cpp
float dpcmRate (uint8_t index, bool pal)
{
    return (float) dpcm::rate (index, pal);
}
```

`NsfPlayer` 内部还在用它，而 `NesDpcmBank` 直接调 `dpcm::rate`。**两个名字指同一件事**，其中一个只是加了个 `float` 转换。

**留着它的理由**是 5 个调用点都要改；**删掉它的理由是**规则 1"一处一个概念"。如果 M-3 要动 `DpcmSample`，顺手改调用点即可。

### L-3 `currentSamples()` 在引擎锁内调下游（规则 3）

```cpp
std::vector<playback::DecodedSample> currentSamples (void)
{
    const juce::ScopedLock guard (lock);          // 音频线程也用这把锁
    ...
    return trackPlayers.front()->samples();       // 里面还要再取 scanResultMutex、拷 64 KB
}
```

**没有死锁**：音频线程只取 `scanResultMutex`，不取引擎锁，不构成环。**但锁内做了 64 KB 拷贝 + 解码**，把音频线程可能等待的时间从"取个指针"拉长到"几次 memcpy"。

实测无感（导出是一次性操作），**但规则 3 说"谁拥有状态谁负责"**——引擎不该在下游调用期间持锁。

**修法**：锁内取 `ITrackPlayer*`，解锁后再调 `samples()`。**需要一个"指针在解锁后仍活着"的保证**——`trackPlayers` 只在换歌时重建，而换歌也持同一把锁，所以可以，但要写清楚。

---

## 四、检查过、无问题

留这一节是为了让这份文档不只列问题：以下是逐条查过、**确认没问题**的项。

| 检查 | 结论 |
|---|---|
| **规则 11**：三处"读当前状态"的所有权 | `LibvgmTrackPlayer::samples()`、`PlaybackEngine::currentSamples()`、`NsfPlayer::dpcmSamples()` **都返回值**，没有一处把内部容器的指针交出去 |
| **规则 12**：`$4013` 位宽 | `lengthFromRegister` 用的是 `value * 16 + 1`，与 `nes_dmc.cpp` 和 NotSoFatso 一致 |
| **规则 12**：固定缓冲大小 | `CapturedSample::bytes` 是 4096，**从寄存器上限 4081 推出来的**，不是从某首歌的观测值 |
| **规则 11**：`applyScan` 的 `std::move` | 参数 by value，move 进成员后**不再被读**；moved-from 的 `scan` 没有后续使用 |
| **规则 7**：颜色 | 本次没有新增任何颜色，菜单与报告浮层都走既有主题 |
| **规则 10**：跨型号 | 不适用（只接了 2A03），但 `memo` 里写明了其余 11 颗芯片的边界 |
| **`writeSamples` 的失败处理** | 一个文件写失败只减计数，其余继续——`failed` 会算出差额并显示 |
| **采样为空时不弹选择器** | `exportSamples()` 在 `samples.empty()` 时直接出报告，不会让用户选完目录才发现没东西 |

---

## 五、建议顺序

1. **H-1**（十分钟，纯行尾，但它污染了 `blame`）
2. **H-2**（一小时，VGM 的 5 个采样断言——这条链路上已经错过三次）
3. **M-3 → M-1 → M-2**（M-3 是一次移动，顺手把 L-2 一起清掉；M-1 是删重复；M-2 是加一个 `if`）
4. **M-4**（改注释，五分钟）
5. **L-1 / L-3** 视情况，或写进文档作为已知取舍

**H-1 和 H-2 修完，这一支就到了可以合并的状态**；M 级的四条里，M-3 和 M-1 值得在合并前做——它们都是"以后每加一样东西都要多改一处"的类型。
