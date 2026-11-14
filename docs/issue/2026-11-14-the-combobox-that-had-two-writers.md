# 下拉框选了没生效：一个选中值被两个写者争抢

> 用户说"经常选了但没改成功"。日志给出的现场是同一毫秒的四行：
> `setSelectedId(4)` → `box=4 -> 3 (engine said 2)` → `onChange id=3`。
> 也就是说：**用户的选择被写进盒子，又在同一个瞬间被引擎的旧值写了回去**，
> 而回调读到的已经是回滚后的值。这不是下拉框的 bug，是"谁有权写这个控件"没有定清楚。

## 一、症状

| 项 | 内容 |
|---|---|
| 报告 | 混音台每轨的 MIDI 通道下拉框，"经常"选了没生效 |
| 频率 | 约每三次有一到两次；不是必现，不是完全坏 |
| 长期性 | "很早就有了，不是当前 feature 引入" |
| 后续线索 | 用户补充：**NSF 多曲目选择的下拉框好像也有同样问题** |
| 环境 | 独立版与 VST3 都能复现（与音频/MIDI 输出无关，界面层） |

"经常"这个词很关键：它把排查方向从"逻辑错"推向"时序错"。必现的错误通常是状态机或索引写错；
概率性失效，尤其和点击节奏相关的，几乎总是**两个写者**在抢同一份数据。

## 二、先走通链路，再做排除

在动任何代码之前，先把"用户点一下"到"引擎收到值"的完整路径读出来。

```
点击
 └─ MidiChannelBox::showPopup()                     ChannelStrip.cpp
     ├─ makeMidiChannelMenu(off, channels, getSelectedId(), icon)   MidiMenu.h
     └─ menu.showMenuAsync(..., SafePointer<MidiChannelBox>)        ← 异步
         └─ 回调: hidePopup(); setSelectedId(result)                ← 默认 sendNotification
             └─ ComboBox::onChange                                  ← 排队派发
                 └─ channel = getSelectedId() - 1
                     └─ onMidiChannelChanged(channel)
                         └─ engine.setTrackMidiChannel(index, channel)   ← 真正写入
```

与此同时，另一个完全独立的路径**每秒跑 10–15 次**：

```
PluginEditor 定时器
 └─ MixerPanel::refresh() → refreshValues()
     └─ strips[i]->setMidiState(engine.getTrackMidiChannel(i), ...)
         └─ if (midiBox.getSelectedId() != 值 + 1)
                midiBox.setSelectedId(值 + 1, dontSendNotification)   ← 回读覆盖
```

两条路径都合理，问题在于它们之间没有约定。

### 已排除的嫌疑（每条都有依据，不是感觉）

| 嫌疑 | 结论 | 依据 |
|---|---|---|
| 锁 | 排除 | `setTrackMidiChannel` 只有一次 `atomic::store` + 一次 `fetch_add`，没有竞争 |
| 索引错位 | 排除 | `rebuildStrips` 里 `for i < tracks.size()`，`index` 就是 `i`，与回调捕获的一致 |
| item id 映射 off-by-one | 排除 | 盒子 `1=Off, channel+1`；菜单 `1=Off, index+2`；两端逐个核对过 |
| 歌曲重建覆盖用户值 | 排除 | `rebuildPlayers()` 明确恢复了 `midiChannel`（`PlaybackEngine.cpp`，与 gain/pan/mute/solo 并列） |
| `setSelectedId` 短路 | 日志排除 | 见第五节，日志里每次都有 `after setSelectedId` |

排除法不是"我觉得没问题"，而是每一行都要能指到具体代码。这份表后来直接变成了文档的骨架。

## 三、仪器化：为什么必须打日志

三个候选嫌疑（见下）都发生在**消息循环的不同相位**，静态阅读无法区分：

1. `SafePointer` 失效 —— 菜单打开期间 strip 被重建，选择被静默丢弃；
2. `rebuildMidiItems()` 在菜单打开时 `clear()` 了 item 列表；
3. `setSelectedId` 因 `lastCurrentId` 已等于 `result` 而短路，不触发 `onChange`。

它们对用户是同一个现象，对代码是三条不同的路。**必须有时间戳级别的现场**。

### 为什么不用窗口脚本复现

harness 有 `set` 和 `click` 两条指令，但都到不了目标：

| 指令 | 实际路径 | 为什么不行 |
|---|---|---|
| `set mixer.midi.0 5` | 直接 `setSelectedId(..., sendNotification)` | 绕开了 `showPopup`，race 不存在 |
| `click mixer.midi.0` | `triggerClick()` | `ComboBox` 的菜单开在 `mouseDown` 里，`triggerClick` 不触发 |

实测也印证了：`set` 连跑 12 次，日志里 `onChange` 次次正常，一次 `OVERRIDE` 都没有。
**这类 bug 只能在真实事件循环里出现。**

### 日志设计（四条原则）

1. **写文件，不写 stderr**。Windows 的 Standalone 是 GUI 子系统，`fprintf(stderr)` 看不到。
   落到 `%TEMP%\vgmtoybox-midi-diag.log`。
2. **毫秒时间戳**。这是排序 bug，没有时间戳就无法判断"重建发生在点击前还是点击后"。
3. **每个跨界点都打**：进入 `showPopup`、回调（含 `result` 与 `safeNull`）、`setSelectedId` 之后、
   `onChange` 入口、回读覆盖的那一行、strip 析构、strips 重建。
4. **分段标记**。每次启动插一行 `=== session start ===`，否则多轮运行的日志混在一起无法读。

一个细节：覆盖那一行的日志措辞直接写结论，而不是打印变量让读的人自己推：

```cpp
midiDiag::log ("setMidiState OVERRIDE ... box=" + ... + " -> " + ... + " (engine said ...)");
```

日志里的这行字，后来直接成了提交信息里的一句话。

## 四、取证：同一毫秒的四行

用户复现一次后，日志给出完整现场（节选，`mixer.midi.0` 指第 0 轨）：

```
20:58:21.350  showPopup mixer.midi.0 id=3 items=17 enabled=1
20:58:22.089  popup-callback result=4 safeNull=0 idBefore=3
20:58:22.089  popup-callback after setSelectedId id=4
20:58:22.089  setMidiState OVERRIDE mixer.midi.0 box=4 -> 3 (engine said 2)
20:58:22.098  onChange mixer.midi.0 id=3 channel=2 hasCallback=1
```

读法：

| 行 | 说明 |
|---|---|
| `result=4` | 用户点了 Ch.3（id 4 = channel 3） |
| `after setSelectedId id=4` | 盒子**接受**了选择，显示已是 Ch.3 |
| `OVERRIDE box=4 -> 3` | **同一毫秒**，定时器回读引擎值 2（Ch.2），把盒子改回 Ch.2 |
| `onChange id=3 channel=2` | 排队的通知此时才派发，读到的是被回滚后的 id → 写入 Ch.2 |

`setSelectedId` 到 `onChange` 之间隔了 9 ms —— 这 9 ms 就是 `ChangeBroadcaster` 队列在消息循环里的等待。

**结果**：用户选 Ch.3，引擎收到 Ch.2，界面停在 Ch.2。三次里中招一到两次，取决于定时器相位。

## 五、扩面：一个 bug，五个控件

拿到根因后，用户补了一句"NSF 曲目下拉框好像也这样"。于是把 `plugin/Source/ui/**` 里所有
`setSelectedId` 全部列出来，按"是否有定时回读 + 用户通知是否异步"分类：

| 控件 | 定时回读点 | 用户通知 | 后果 |
|---|---|---|---|
| `midiBox`（每轨） | `ChannelStrip::setMidiState` | 异步 | 写回旧值 |
| `outputBox`（每轨） | `ChannelStrip::setOutputOptions` | 异步（JUCE 内置） | 同上 |
| `songBox`（多曲目） | `SongInfoPanel::refresh`，**无条件** | 异步（JUCE 内置） | **完全无效** |
| `loopBox` | `TransportPanel::refresh` | 异步（JUCE 内置） | 同上 |
| `fadeBox` | `TransportPanel::refresh` | 异步（JUCE 内置） | 同上 |

低风险（只在打开面板/初始化时同步，无定时回读）：
`SettingsOverlay` 的 `rateBox` / `startNote` / `middleC` / `languageBox`。

### 曲目下拉框为什么最重

```cpp
// SongInfoPanel::refresh()，每个 tick 无条件执行
songBox.setSelectedId (engine.getSongIndex () + 1, juce::dontSendNotification);
```

它连"值是否不同"都不判断。用户选第 5 首 → 通知排队 → 下一 tick 无条件写回 `getSongIndex()`
（还是旧的那首）→ 通知派发时：

```cpp
const int index = songBox.getSelectedId () - 1;
if (index < 0 || index == engine.getSongIndex ())
    return;                     // ← 直接返回，一次写入都没有
```

MIDI 至少还"写了一个旧值"，曲目选择是**彻底静默无效**，界面立刻弹回原曲目。
同一份竞态，在 `midiBox` 上表现为"改错了"，在 `songBox` 上表现为"啥也没发生"——
**同一个根因，两种截然不同的用户体感**，这正是它长期没被当成一个 bug 的原因。

### `songBox` 还有第二层

即使通知能及时派发，`selectSongAsync` 也是**真异步**（后台构建 200 ms 量级），
`getSongIndex()` 在整个构建期间继续报旧歌。所以 400 ms 的宽限期也救不了它，
必须有一个"切换进行中，别碰这个控件"的显式状态。

## 六、根因

### 6.1 直接原因：异步通知撞上同步回读

```cpp
// ChannelStrip.cpp（修复前）
safe->setSelectedId (result);      // 默认 NotificationType::sendNotification
```

JUCE 的 `ChangeBroadcaster::sendChangeMessage()` **只是把通知排进消息队列**，
`onChange` 要等消息循环下一轮才执行。而定时器的回读是**同步**的。

于是存在一个窗口：

```
t0  用户点击 → setSelectedId(4)   [盒子已是 4，通知在队列里]
t1  定时器跑 → 读到引擎 2 → 盒子改成 3
t2  通知派发 → onChange 读到 3 → 写入引擎 2
```

窗口宽度 = 通知在队列里等待的时间（实测 9 ms），而定时器间隔 66–100 ms，
所以这是一个**相位概率**：点击时刻决定中不中招。

### 6.2 结构性原因：一个控件，两个写者

真正该被记住的不是"要加 `sendNotificationSync`"，而是：

> **界面的选中值是"用户意图"的一次表达。当它同时被"引擎状态的镜像"写时，
> 这两者必须有先后约定，否则谁后写谁赢，而"后"取决于消息循环的相位。**

`code-quality.md §13`（一份数据只能有一个写者）在数据层已经执行得很严，
但**控件状态**这一层当时没有被纳入同一条纪律——它有 `onChange` 回调、有 `dontSendNotification`、
有定时刷新，看起来"本来就该这样"。这个 issue 是把 §13 推到 UI 状态层的一次补课。

### 6.3 为什么长期没被发现

| 原因 | 说明 |
|---|---|
| 概率性 | 三分之一的点击是好的，"有时候不灵"容易被归为手滑或卡顿 |
| 两种体感 | MIDI 表现为"改错了"，曲目表现为"没反应"，看不出同源 |
| 静态不可见 | 两条路径分别看都正确，只有带时间戳的现场才能把顺序还原 |
| 无自动化覆盖 | 窗口脚本的 `set` 绕开 `showPopup`，天然测不到这条路径 |
| 注释曾"解释"过它 | `SongInfoPanel` 里那行"乐观设置"的注释说明作者意识到要做乐观更新，但没意识到镜像会把它抹掉 |

## 七、解决方案

### 7.1 不给五个控件各打一个补丁

同一个规则写五遍，就是下一个"两边不一致"的源头（这个项目已经被"第二个写者"伤过一次）。
抽一个类，把规则收进去：

```
plugin/Source/ui/SyncedComboBox.{h,cpp}
```

### 7.2 三类调用，三个语义

| 方法 | 语义 | 用在哪 |
|---|---|---|
| `noteUserPick(id)` | 记录"用户刚选了什么"并**同步**应用 | `showPopup` 的回调里，唯一一处 |
| `syncTo(id)` | 引擎镜像：选择在途时**不覆盖** | 所有定时刷新 |
| `forceTo(id)` | 无条件写入 | 列表刚重建，必须落在有效项上 |
| `holdOff()` | 外部否决（异步引擎） | `songBox` 在 `switchingSong` 期间 |

`noteUserPick` 里只有一行是关键：

```cpp
void SyncedComboBox::noteUserPick (int itemId)
{
	pickInFlight = true;
	lastPickMs = clock ();

	// 同步派发：返回时引擎已持有新值，下一次刷新无从覆盖。
	setSelectedId (itemId, juce::sendNotificationSync);
}
```

`syncTo` 是第二条规则的落点：

```cpp
void SyncedComboBox::syncTo (int itemId)
{
	if (holdOff != nullptr && holdOff ()) return;

	const auto now = clock ();
	if (pickInFlight && (now - lastPickMs) < pickGraceMs) return;   // 选择还在路上

	pickInFlight = false;
	if (getSelectedId () != itemId) setSelectedId (itemId, juce::dontSendNotification);
}
```

### 7.3 为什么需要三个方法而不是一个

一开始想只加 `sendNotificationSync`，但它解决不了两种情形：

- **列表重建**：`clear()` 之后盒子没有选中项，此时若被宽限期挡住，界面会停在空白；
- **异步引擎**：曲目切换的 200 ms 里引擎值一直是旧的，任何宽限期都太短。

所以规则必须是"谁在写、为什么写"的三分，而不是一条时间常量。

### 7.4 迁移清单

| 文件 | 改动 |
|---|---|
| `ChannelStrip.h/.cpp` | 删 `MidiChannelBox`（并入基类）；`outputBox`/`midiBox` 换类型；`makeMenu` 注入 DIN 菜单 |
| `MixerPanel.cpp` | `setMidiState` / `setOutputOptions` 的回读走 `syncTo` / `forceTo` |
| `SongInfoPanel.h/.cpp` | `songBox` 换类型；加 `switchingSong` 与 `holdOff`；`refresh` 分重建/镜像两支 |
| `TransportPanel.h/.cpp` | `loopBox` / `fadeBox` 换类型；刷新走 `syncTo`，语言重建走 `forceTo` |
| `CMakeLists.txt` | 登记新源文件（主目标与 `vgm_ui_test`） |

## 八、验证

### 8.1 让竞态可以在无窗口的单测里复现

竞态的触发条件是"时间过了多久"，所以把时钟做成注入的：

```cpp
std::function<juce::uint32 (void)> clock = [] { return juce::Time::currentTimeMillis (); };
```

测试里 `now` 是一个变量，宽限期靠**步进**跨过，而不是 `sleep`：

```cpp
box.noteUserPick (3);                 // 用户选了 3
box.syncTo (1);                       // 同一瞬间，定时器拿旧值回读
check (box.getSelectedId () == 3, "a refresh right after a pick leaves the pick alone");

now += SyncedComboBox::pickGraceMs;   // 选择落地
box.syncTo (2);
check (box.getSelectedId () == 2, "after the grace window the engine's value wins again");
```

六个断言覆盖：`forceTo` 生效、选择立即显示、宽限期内不覆盖、宽限期外覆盖、`holdOff` 生效、解除后恢复。

### 8.2 断言必须被验证会红

把 `syncTo` 里的宽限期判断临时短路成 `if (false && ...)`，跑一次：

```
FAIL a refresh right after a pick leaves the pick alone  (UiButtonTest.cpp:456)
```

确认它精确命中那条规则，再恢复。**没有验证过会失败的断言，等于没有断言。**

### 8.3 回归

```
54/54 ctest · 芯片 PASS · NSF PASS · UI 断言全过
dist: Standalone 21:09:24 / VST3 21:09:18
```

用户复测：MIDI 下拉、曲目下拉、音频输出、loop/fade 预设，全部正常。

## 九、可复用的规则

这四条比修复本身更值钱。

### 规则一：界面上"乐观更新 + 定时回读"必须显式定义 pending

只要一个控件同时满足：① 用户操作后先改界面、② 有定时器从数据源回读，就必须回答
"回读要不要等用户这次提交落地"。答案要么是同步提交（`sendNotificationSync`），
要么是一个 pending 位。**两者都没有，就是本 issue。**

### 规则二：JUCE 的 `setSelectedId` / `setValue` / `setToggleState` 默认都是排队通知

`NotificationType::sendNotification` 走 `sendChangeMessage()`，不在当前调用栈里执行。
需要"改完就生效"的地方必须显式用 `sendNotificationSync`。
同理，任何"改完立刻读回自己刚写的值"的代码，都要怀疑这里。

### 规则三：概率性失效先找"两个写者"，不要先怀疑锁

锁问题的特征是"数据损坏/崩溃"，而"选不上/回弹/偶发丢值"几乎总是两份代码在写同一份状态。
定位方法：把**所有**写这个状态的点列出来（`grep setSelectedId`），逐个看它属于谁。

### 规则四：修一处之前先扫同构

用户只报了 MIDI，实际是五个控件。**"同一个模式出现在几处"比"这一处怎么修"更重要**——
因为把它抽出来之后，第五处、第六处以后自动免疫。

### 规则五：时间相关的逻辑，把时钟做成参数

`Time::currentTimeMillis()` 直接调用意味着不可测。注入之后，宽限期、去抖、超时全都能在
单测里步进验证，不需要 `sleep`，也不需要在真机上手动复现。

## 十、下次的检查清单

遇到"控件选了没生效 / 偶发回弹 / 下拉框时好时坏"：

- [ ] 列出该控件的**所有**写入点（`grep setSelectedId` / `setValue` / `setToggleState`）
- [ ] 每个写入点标注：谁触发（用户 / 定时器 / 引擎回调）、是否同步
- [ ] 若存在"用户异步 + 定时同步"的组合 → 直接判定为本类问题
- [ ] 检查通知类型是否 `sendNotification`（默认）而非 `sendNotificationSync`
- [ ] 检查定时回读是否无条件执行（`SongInfoPanel` 那行就是反例）
- [ ] 检查数据源本身是否异步产出（后台构建、异步解码）——若是，需要 `holdOff` 而非宽限期
- [ ] 修完给"时间"注入时钟，补一条能步进的单测，并**验证它不修就会红**

## 十一、结论

> 下拉框没有 bug。它只是被两个都"有理"的写者同时写：
> 一个代表用户此刻的意图（走异步通知），一个代表数据源此刻的事实（走同步回读）。
> 谁后写谁赢，而"后"由消息循环的相位决定 —— 于是表现为三分之一的点击失效。

修复没有引入新概念，只是把这条约定写下来并收进一个类：
**用户的意图同步落地，数据源的镜像礼让在途的选择，异步数据源则显式否决镜像。**

真正需要被记住的是它揭示的边界：`code-quality.md §13` 说"一份数据只能有一个写者"，
这条纪律过去只被执行在数据层。UI 控件的选中值同样是数据，同样只能有一个写者。

---

## 附录 A：诊断日志原文（关键段）

```
20:58:17.839  rebuildStrips tracks=13 stripsBefore=8
20:58:17.840  dtor mixer.midi.7
...
20:58:18.941  showPopup mixer.midi.0 id=1 items=17 enabled=1
20:58:19.663  popup-callback result=2 safeNull=0 idBefore=1
20:58:19.663  popup-callback after setSelectedId id=2
20:58:19.667  onChange mixer.midi.0 id=2 channel=1 hasCallback=1
20:58:21.350  showPopup mixer.midi.0 id=3 items=17 enabled=1
20:58:22.089  popup-callback result=4 safeNull=0 idBefore=3
20:58:22.089  popup-callback after setSelectedId id=4
20:58:22.089  setMidiState OVERRIDE mixer.midi.0 box=4 -> 3 (engine said 2)
20:58:22.098  onChange mixer.midi.0 id=3 channel=2 hasCallback=1
```

十二条 UI 操作里中招一次；`safeNull` 全程为 0、`dtor` 未与任何 `popup-callback` 相邻，
说明"SafePointer 失效"和"item 列表重建"两个候选都不成立 —— 只有第三条（回读覆盖）站得住。

## 附录 B：涉及文件

| 文件 | 角色 |
|---|---|
| `plugin/Source/ui/SyncedComboBox.{h,cpp}` | 新：三条规则的唯一落点 |
| `plugin/Source/ui/ChannelStrip.{h,cpp}` | MIDI / 输出两个选择器 |
| `plugin/Source/ui/SongInfoPanel.{h,cpp}` | 曲目选择器 + `switchingSong` |
| `plugin/Source/ui/TransportPanel.{h,cpp}` | loop / fade 预设 |
| `plugin/Source/ui/MidiMenu.h` | MIDI 菜单（带 DIN 图标），未改动 |
| `plugin/Tests/UiButtonTest.cpp` | `checkTheSyncedCombo`，六条断言 |
| `plugin/CMakeLists.txt` | 新源文件登记（主目标与 `vgm_ui_test`） |

## 附录 C：排查用过的临时诊断代码

排查阶段加过一套临时日志（`ui/MidiDiag.h` + 十余处 `midiDiag::log(...)` 调用），
写入 `%TEMP%/vgmtoybox-midi-diag.log`，带毫秒时间戳与会话分隔线。
**定位完成后已全部删除**，未进入任何提交。

保留下来的是方法：*当两条路径分别看都正确、合起来却出错时，先让它们在同一份时间轴上说话。*
