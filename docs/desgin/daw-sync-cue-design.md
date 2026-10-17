# DAW 同步与 CUE 点（Transport）

> 分支：`feature/daw-sync`
> 日期：2026-10-17（同日按实现结果更新，§7 起以代码为准）
> 状态：**已实现**：P1 块粒度（§4 的数学模型、§7 的接口、§8 的界面、§9 的持久化）。
> P2 采样级起点未做（§6），故启动误差 ≤ 1 个音频块。
> 范围：**仅插件版**（VST3 / AU）。Standalone 不创建 `Sync` / `Cue` / 微调旋钮这三个控件。

## 1. 需求

| 编号 | 需求 |
|---|---|
| R1 | 插件版 Transport 上增加两个控件：`Sync`（启用/停用 DAW 同步）与 `Cue`（把当前 DAW 播放头设为同步点）。 |
| R2 | `Sync` 关闭时，行为与现在完全一致：插件自主走带，不读取宿主。 |
| R3 | `Sync` 打开时，位置与播放状态完全由 DAW 决定：DAW 不走带，插件不出声、不推进。 |
| R4 | `Cue` 是 DAW 时间轴上的一点，含义是「歌曲第 0 秒在 DAW 的第几秒」。 |
| R5 | DAW 播放头在 CUE 之前时，插件停在 0 并静音；越过 CUE 的那一时刻开始播放，之后保持同步。 |
| R6 | 歌曲走完之后按插件现有播放设置处理：播放一次 → 停在终点；多次 → 按循环次数；循环 → 按歌曲自身的 loop 点回卷。 |
| R7 | 同步是单向的：DAW → 插件。 |
| R8 | 不使用拍速、拍号、量化、小节线等任何音乐时基信息。 |
| R9 | 不做自动对轨、不检测 BPM。拍速由用户在插件外保证一致（VGM 导出 WAV → 测速）。 |

## 2. 结论

**可行**，而且实现比最初设想的简单：一旦把「歌曲位置」定义成「宿主时间的纯函数」，就不需要检测跳转、循环、定位，也不需要逐块校正。

关于单向同步：**单向不只是更好，而且是唯一能成立的方案**。VST3 没有任何让插件驱动宿主走带的接口（`AudioPlayHead::canControlTransport()` 恒为 false，`ProcessContext` 只读）。所以「插件调整不影响 DAW」不是设计取舍，而是协议本身的性质；插件只能做 slave。

需要接受的三个硬约束：

1. 宿主必须提供 process context。VST3 规范里它是 optional，个别宿主/版本可能不给。
2. 只能在 `processBlock()` 内读取走带，且不能保存指针；UI 必须读插件自己发布的快照。
3. 精度受音频块大小限制（P1 为 ±1 块，P2 做到采样级）。

## 3. 已核实的事实

| 事实 | 出处 | 对设计的影响 |
|---|---|---|
| `processContext` 是可选字段，宿主可以传空 | `ivstaudioprocessor.h` 的 `ProcessData::processContext` 注释 | 必须实现「宿主不可用」的降级路径 |
| 没有 context 时，JUCE 仍返回一个已赋值的 `PositionInfo`（时间 0、未播放） | `juce_audio_plugin_client_VST3.cpp` 的 process 包装 | 不能用 `hasValue()` 判断宿主是否支持；需要额外判据 |
| 只能在 `processBlock()` 里调 `getPlayHead()->getPosition()`，不能跨线程、不能存指针 | `juce_AudioProcessor.h`、`juce_AudioPlayHead.h` 注释 | 必须每块取样 → 发布无锁快照给 UI |
| 插件无法控制宿主走带 | VST3 无对应接口 | 单向是唯一解（R7） |
| 引擎的 `duration` = `GetTotalTime(PLAYTIME_LOOP_INCL \| PLAYTIME_TIME_PBK)`，**不含** fade | `LibvgmSongInstance.cpp`、`playera.cpp` | 真实发声结束点 = `total + fade`（见 §4） |
| fade 只在「歌曲有 loop 点且 loopCount > 0」时发生（loop 事件触发 `FadeOut()`），无 loop 点的歌曲不会淡出 | `playera.cpp` | `tail` 只在有 loop 点时存在 |
| 位置用 `PLAYTIME_LOOP_INCL` 读取时，**跨循环累加不回卷**；只有 `loopsForever` 分支用单遍模式，位置会跳回 loop 点 | `LibvgmTrackPlayer.cpp` | N≥2 时「引擎位置 = x」；N=0 时需要回卷公式 |
| `getLoopPointSeconds()` = `duration − loopLength`，只在 N≤1 时等于真正的 loop 起点；N≥2 时返回的是最后一圈的起点 | `PlaybackEngine.cpp` | 需要独立的 `SongTimeline`，顺带修掉这个陈年误差 |
| `render()` 用 `ScopedTryLock`，`setPositionSeconds()` 用阻塞 `ScopedLock` | `PlaybackEngine.cpp` | 不能每块 seek，也不能在音频线程直接 seek |
| 引擎的 `playing` / `positionSeconds` 是原子量 | `PlaybackEngine.h` | 音频线程可安全读写 |

## 4. 数学模型

### 4.1 符号

```
cue        DAW 时间轴上的同步点（秒），歌曲 0 秒对齐到这里
hostSec    宿主给的本块起始时间（秒）
x          = hostSec − cue，相对 CUE 的时间，可为负
L          单遍长度（秒），不含 fade 尾音
C          loop 长度（秒），0 = 歌曲没有 loop 点
loopStart  = L − C
N          loopCount：1 = 播放一次，2..n，0 = 无限
F          fadeSeconds
total      = (C > 0 && N > 1) ? L + (N−1)·C : L
tail       = (C > 0 && N > 0) ? F : 0        // libvgm 的淡出只发生在有 loop 点的歌
end        = total + tail
```

### 4.2 位置公式

```
if (x < 0)                    pos = 0;                       play = false
else if (C == 0 || N == 1)    pos = min(x, end);             play = x < end
else if (N == 0)              pos = x < L ? x
                                       : loopStart + fmod(x − L, C);
                                                             play = true
else                          pos = min(x, end);             play = x < end
```

`pos` 是「引擎坐标系」的位置：N≥2 时引擎位置跨循环累加，公式同样返回累加值，两者一致；N=0 时引擎位置会回卷，公式也回卷。

### 4.3 用户给的例子

L = 60，loopStart = 3（C = 57），N = 0（循环），cue = 10：

| DAW 时间 | x | 插件位置 | 状态 |
|---|---|---|---|
| 9.000 | −1.000 | 0.000 | 待命，静音 |
| 10.000 | 0.000 | 0.000 | 开始播放 |
| 69.999 | 59.999 | 59.999 | 播放 |
| 70.000 | 60.000 | 3.000 | 回卷到 loop 点（60 + 0 → 3） |
| 75.000 | 65.000 | 8.000 | 播放 |
| 127.000 | 117.000 | 3.000 | 第二圈开始（60 + 57 = 117） |

### 4.4 其它边界

| 场景 | 结果 |
|---|---|
| N = 1，F = 3，cue = 10 | 播放到 DAW 73 秒（L + F），位置停在 60，之后暂停 |
| N = 2，C = 57，F = 3 | `total` = 117，`end` = 120；DAW 127 秒起静音暂停 |
| 歌曲没有 loop 点，N = 0 或 2 | 退化为播放一次（`C == 0` 分支） |
| DAW 从 CUE 之后开始播放 | x > 0，直接定位到对应位置，不等下一次 CUE |
| DAW 循环区间回卷到 CUE 之前 | x < 0 → 归零待命；再次越过 CUE 时重新开始 |

## 5. 同步策略

**纯函数 + 自由跑 + 容差纠正**，不做逐块 seek。

1. 每块用宿主时间算出 `pos / play`（§4）。
2. 引擎自己连续渲染、自己处理 loop 与 fade。
3. 只有 `|pos − enginePos| > 30 ms`，或者 `play` 与引擎状态不一致时，才下发一次性定位/播放命令。
4. 因为位置是宿主时间的纯函数，**宿主跳转、宿主循环、用户拖动播放头都不需要单独检测**：它们只体现为一次较大的误差，被第 3 条覆盖。
5. 命令只在状态变化时下发；引擎侧再做一次判重（位置差 < 1 ms 且播放状态相同则忽略）。

为什么不做逐块 seek：`setPositionSeconds()` 要拿阻塞锁并让每个 track player 重新 seek，libvgm 的 seek 不重置振荡器，每块 seek 会咔哒响并拖慢渲染。

为什么 DAW 停止时用 pause 而不是 stop 到 0：位置应冻结在原地（CUE 之前的情况本来就是 0），DAW 再次从同一点继续时不需要重新对齐。

## 6. 采样级起点（P2）

P1 的启动误差最多一个音频块（512 @ 48 kHz ≈ 10.7 ms），对「对轨」来说偏大。P2 在块内切分：

```
n = clamp(round(-xAtBlockStart × sampleRate), 0, numSamples)
```

- 本块只渲染 `numSamples − n` 个采样，写入输出缓冲的第 `n` 个位置；
- 引擎位置从 0 开始推进，块结束时正好等于 `blockSize − n`，与 x 一致；
- 需要给 `render()` 增加 `startSample` 参数：`clearBusBuffers` 保持清零整块，`mixTracks` / `finaliseMasterBus` / `applyOutputVolume` 的目标偏移改为 `startSample`。

改动集中在渲染路径的 4 个函数，不动 DSP 本身。

## 7. 架构与接口

### 7.1 新增：`plugin/Source/playback/DawSync.{h,cpp}`

纯逻辑，不依赖 JUCE，可直接进 `vgm_core_test`（实际就是这样测的）。

```cpp
struct SongTimeline              // L / C / N / F 以及由它们推出的 total / tail / end
{
    double durationSeconds, singlePassSeconds, loopLengthSeconds, fadeSeconds;
    int    loopCount;            // 0 = 无限
    bool   loopsForever;
    bool   hasLoop() const noexcept;
    double loopStartSeconds() const noexcept;   // L − C
    double totalSeconds() const noexcept;       // (C>0 && N>1) ? L + (N−1)·C : L
    double fadeTailSeconds() const noexcept;    // 只有「有 loop 点且 N != 0」才有尾音
    double endSeconds() const noexcept;         // total + tail
};

SongTimeline makeSongTimeline (double duration, double singlePass, double loopLength,
                               int loopCount, double fade, bool loopsForever);

struct HostClock  { bool usable, playing; double seconds; };
struct SyncDecision { bool reposition, play; double position; };

inline constexpr double kSyncToleranceSeconds = 0.030;

SyncDecision evaluateSync (const SongTimeline&, const HostClock&, double cueSeconds,
                           double enginePosition, bool enginePlaying,
                           double toleranceSeconds = kSyncToleranceSeconds);

class DawSync                   // 全原子快照，音频线程与 UI 之间不加锁
{
    static constexpr double kMinKnobRangeMs = 1.0, kMaxKnobRangeMs = 2000.0;

    void publishHostClock (const HostClock&);   // 音频线程，每块一次
    HostClock getHostClock() const;
    bool isEnabled/setEnabled;  bool isHostUsable/isHostPlaying;
    bool isActive;                              // enabled && hostUsable（CUE 恒存在，没有第三个条件）
    bool setCueFromHost/setCueSeconds;          // 没有「清除」：0 s 就是 DAW 的开头
    double getCueSeconds, getCueOffsetMs/setCueOffsetMs, getKnobRangeMs/setKnobRangeMs;
    double getEffectiveCueSeconds;              // cue + offset
    SyncDecision decide (const SongTimeline&, double enginePosition, bool enginePlaying) const;
};
```

`kToleranceSeconds` 后来从类里提到命名空间（`kSyncToleranceSeconds`），并成为 `evaluateSync()` 的
带默认值参数：容差该多大取决于宿主的块大小，把它做成参数，测试才能问"0 ms 时会怎样"。

`makeSongTimeline` 负责后端可能没报的事实：`L` 缺失时按 `duration − (N−1)·C` 反推，`C > L` 时钳到 `L`。
`getLoopPointSeconds()` 改为使用 `loopStartSeconds()`，N≥2 时进度条上的 loop 标记顺带变正确。

`evaluateSync` 与 §4 的公式一致，另外补了一条实现细节：**比较**目标位置与引擎位置时，
两者都钳在可 seek 长度 `total` 内（尾音里引擎位置会超过 `total`，不钳的话每个块都会判成
3 秒误差并重复 seek）。

`setKnobRangeMs()` 会把当前偏移量按新量程重新钳制，所以旋钮永远不会显示一个它到不了的值。

### 7.2 `PlaybackEngine`

| 改动 | 说明 |
|---|---|
| 缓存 `SongTimeline`（取代 `cachedDurationSeconds` / `cachedLoopPointSeconds` / `loopsForever` 三个字段） | 每次换歌、改 loop、改 fade 时重算；**只在锁内被读**（`followDawTransport()` 自己读成员，不再有公开 getter——一个"无锁返回副本"的接口正是错的那一个） |
| 持有 `DawSync` | 「被跟随的东西」放在引擎里，`render()` 才能跟；它也因此成为工程状态的一部分 |
| `render()` 顶部调 `followDawTransport()` | 决策是纯原子运算，**不需要锁，不要纠正就直接返回**；要纠正时才 `ScopedTryLock`，抢不到下一块再试 |
| `setPositionSeconds()` | 不变（阻塞锁，只给消息线程用） |

为什么不在 `processBlock()` 里判断再投递命令：投递一个命令需要一个无锁信箱，而跟随本身不需要锁，
只需要「要纠正时才抢锁」。抢不到就跳过这一块——映射是绝对的，代价只有一块的偏移。

### 7.3 `PluginProcessor`

```cpp
void processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    // 只发布，不决策：跟随即在 engine.render() 内部发生
    engine.getDawSync().publishHostClock (playback::readHostClock (getPlayHead(), getSampleRate()));
    engine.render (buses, playback::kOutputBusCount, numSamples);
}
```

`readHostClock()` 在 `playback/HostClockSource.{h,cpp}`（它原本是 `PluginProcessor.cpp` 里的匿名
函数，测试够不着；见 `docs/review/2026-10-17-...` M-4）。它的可用性判据（不能只看 `hasValue()`）：

```
usable = bpm > 0 || ppq 有效 || timeInSeconds != 0 || timeInSamples != 0 || isPlaying
```

再加 `seenPlaying` 锁存（在 `DawSync::publishHostClock` 内部）：见过一次「宿主在播放」之后就认为
宿主可信。理由：没有 context 时 JUCE 给出的是全零对象，与「停在 0 点」无法区分；用 tempo/PPQ
可以区分绝大多数宿主。

### 7.4 UI

`PlayView` 把 `processor.wrapperType != wrapperType_Standalone` 作为 `syncAvailable` 传给
`TransportPanel`，后者据此决定要不要创建 `ui/SyncGroup`（判断在 `PluginEditor`，透传在 `PlayView`）。

`SyncGroup` 是一整个组件，不是面板上的三个控件：框（和它裁剪的辉光）、名牌、右键菜单、
以及"这一行要给它留多宽"是同一件事。面板只问它两个问题——`refresh()` 和
`isHostDriving()`——而它的菜单要把数值输入交给面板（那个覆盖层是**卡片**大小，比这个组大）。
理由与现状见 `docs/review/2026-10-17-...` M-3。

Standalone 里这个组**不创建**（不是隐藏）：没有宿主就没有可跟随的走带，留下的只会是死控件。
另外 standalone 会把 `DawSync` 的 enabled 强制清零——否则一个在 DAW 里存成「同步开」的工程，
在 standalone 打开后会锁住走带而没有任何按钮能解开。

## 8. UI 规格（已实现）

按钮行左半是走带，右端是一组**虚线框**围起来的控件，`DAW SYNC` 压在上沿正中：

```
播放 暂停 停止 | 循环 起始点   ╭┈┈┈┈┈ DAW SYNC ┈┈┈┈┈┈╮
                              ┆ [SYNC] [CUE] (旋钮) 0.0 ms ┆
                              ╰┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈╯
```

- 框由 `TransportPanel::paintCardContent()` 画（`syncGroupBounds`）：`drawGroupFrame()` 描一条
  1 px、3 on 3 off 的虚线圆角矩形，`PathStrokeType::createDashedStroke()` 一次描整条路径，所以
  虚线是绕着圆角走的。**没有填充、没有内阴影、没有铣边**——这组控件试过两版拟物的凹陷（描边、
  以及完整的槽底 + 双层铣边），在 40 px 的按钮旁边都读成「被选中的一块」，加上居中的铭牌更乱。
  §8 的结论是：这里只说明「这些是一组」，不声称自己是一个面。
- **铭牌压在框的上沿正中**（`drawGroupNameplate()`）：卡片色把字后面的虚线擦掉（fieldset 图例），
  缺口宽 = 量出的字宽 + 2×6，高 10 px。这样铭牌和框共用同一条边 —— 放进框里就必须占一行，控件
  只能整体下移，顶行会比左边的走带矮一截。擦除色必须是卡片色：框上只有虚线，再盖一块浮起来的
  「铭牌板」等于给同样的像素两种说法。
- **框不承载状态**：整圈染成强调色（哪怕 1 px）读起来仍是「这块被选中了」，所以框永远是
  `theme::groupFrame()`。同步是否生效由框**里面**的东西说：`SYNC` 按钮的强调色面与光晕，加上
  铭牌的字色。一处发光 = 一处开关，框本身不发光。
- 框里的控件和左边的走带按钮**共用一条基线**：`resized()` 里让出 `kFrameTopPadding`（9 px），否则
  走带按钮会比框内的按钮高一截，整个顶行看起来是歪的。铭牌的下半截只伸进框里 5 px，不会压到按钮。
- **框只占顶行，不缩短下面两行**：宽度要从「顶行」这块矩形上 `removeFromRight`，不能从贯穿整张卡片的
  `inner` 上取。从 `inner` 上拿走的宽度会让进度条和设置行一起变短——第一版就是这样，进度条停在卡片
  中间、右边空出一大块，看起来像布局坏了，而这组的宽度其实只该影响它自己所在的那一行。
- `SYNC` 的发光被裁在框内，且框画在光晕**之后**：不裁的话光晕会漫过虚线糊到卡片上，先画框的话
  光晕会把虚线染色。
- `Sync`（链环图标）与 `Cue`（旗标图标）是 40 px 图标按钮，面内图标下方各带一个小字 `SYNC` / `CUE`：
  链环和旗标本身不说明「这是跟宿主同步」，图标按钮的 `setCaption()` 就是为此加的（其它图标按钮不带字，
  仍是原来的 0.28 内缩）。
- 微调是 40 px 旋钮，**右边就是可输入的数值框** —— 它就是 `juce::Slider` 自己的文本框
  （`TextBoxRight`，62×22），和音量那个 `100 %` 是同一套控件、同一个 `LookAndFeel` 绘制。
  框里永远是带符号的偏移量（`0.0 ms`、`+3.5 ms`）；CUE 没有「不存在」这一态，所以这个框也没有
  第二种内容要显示。
- 这一组从按钮行**右端**整体布局，卡片变窄时先压缩它，走带按钮的大小不变。

| 控件 | 行为 |
|---|---|
| `Sync` | 切换开关，开启时发光。关闭即恢复自主走带，不打断试听（位置和播放状态原地不动）。 |
| `Cue` | 点击把**当前宿主播放头**设为 CUE。**只取点，不切换、也不发光**：再点一次是重新锚定（DAW 动了、或者换歌了）。宿主不可用时置灰。CUE 没有「有没有」这回事——它一直存在，默认就是 DAW 的 0 s。 |
| 旋钮拖动 | 在当前量程内连续调整 CUE 偏移（相对 CUE，不是绝对位置）。 |
| 旋钮右侧数值框 | 精确偏移量，直接输入（和音量输入百分比一样）。 |
| 旋钮双击 | 偏移量归零（拖动很难精确回到 `0.0`，「放回去」是微调控件唯一的常用手势）。数值框里的双击是编辑框自己的取词，与这里不冲突。 |
| 框内右键 | 菜单：`Exact offset...`（打开数值框）、`Knob range...`、`Reset cue to 0 s`。量程默认 ±10 ms，范围 1–2000 ms。菜单显式 `setLookAndFeel` 到 `ToyboxLookAndFeel`（见下）。 |

为什么 CUE 不再是切换：按钮发光对应的是**状态**，而点击是**动作**（把 CUE 放在这里）。
把两者绑成一个开关后，「重新取点」要两次点击，还会在两次点击之间留下一个「没有 CUE」的中间态。
现在把 CUE 归零走**整个框的右键菜单**，不绑在旋钮上：菜单管的是这一组的三件事（精确偏移、量程、
归零），量程本来就是组级设置，归零又和「拖动旋钮改偏移」是同一件事的两个方向。右键落在框内任何
位置——空白处、两个按钮、旋钮——都打开同一个菜单，位置在框里而不是在旋钮里，用户不用先猜「哪个
控件有菜单」。

**CUE 不存「有没有」**：它是一个位置，不是一个开关，所以位置本身就是它的全部状态，默认 0（DAW 的
开头）。菜单里的 `Reset cue to 0 s` 是**归零**，不是清除——归零之后 `sync.isActive()` 不变，`SYNC`
的灯也不变，因为唯一该决定同步是否工作的是 `SYNC`。早先的版本存了一个 `hasCue` 标志、菜单里是
`Clear cue`，结果是「清掉之后 SYNC 还在亮、走带却解锁了」：面板上同一时刻有两处在描述同步是否工作，
而且互相矛盾。现在状态只有一个字段（`enabled`）加一个数值（`cueSeconds`），不存在需要彼此同步的两个
真相。

`CUE` 按钮**不发光**：这个界面里「发光的按钮」只表示「这个开关是开的」，而 CUE 不是开关。
先做过一版「有 CUE 就亮」「同步开且有 CUE 才亮」，都是同一个问题——面板上出现两盏强调色的灯，
读起来像两个开关（实测反馈），而其中一盏还只在别的东西打开时才亮，含义要靠猜。
CUE 也**没有状态显示**（它没有「有没有」），旁边的旋钮只显示偏移量，不兼作 CUE 的存在指示灯。
强调色因此只属于 `SYNC`（以及框上那行小铭牌），一处发光 = 一处开关。

右键菜单的配色：`PopupMenu` 只认自己身上的 `LookAndFeel`（`PopupMenu::setLookAndFeel`，`findLookAndFeel()`
读的就是 `menu.lookAndFeel`），**不会**去问目标控件的 LookAndFeel，所以不设置就会落到平台的默认
`LookAndFeel_V4` 深色配色上；菜单因此必须显式挂到 `ToyboxLookAndFeel`（它的 `PopupMenu::*ColourId`
在 `refresh()` 里随主题重设）。

菜单的**尺寸**同样不跟着位置走，而是跟着 `Options::withTargetComponent()`：`MenuWindow` 的缩放因子
`= target ? target.getApproximateScaleFactorForComponent() : 1`，后者再乘 `Desktop::getGlobalScaleFactor()`
（桌面应用里恒为 1）。只写 `withTargetScreenArea()` 的话，菜单会按 1.0 画，在 150% 缩放的显示器上
比窗口里的控件明显小一圈（实测反馈「菜单太小了」）；所以两个都要给：屏幕区域决定**出现在哪**，
目标组件决定**多大**，顺序必须是 `withTargetComponent(...)` 在前、`withTargetScreenArea(...)` 在后
（后者覆盖前者设置的区域）。

数值输入不再有第二套入口：旋钮的读数就是 `Slider` 的文本框，点它就能改，`Exact offset...`
只是把它打开（`Slider::showTextBox()`），这样「怎么输入一个数」在整个界面里只有一种形状。
`ValueEntryOverlay`（卡片内部的覆盖层，不是 `AlertWindow`；理由见 `AboutOverlay.h`）因此只服务
`Knob range...` 这一项。

同步生效时的禁用规则（`sync.isActive()` = 同步开 **且** 宿主可信；CUE 恒存在，不参与判断）：

- 播放 / 暂停 / 停止 / 起始点开关 / 进度条 / 起始点拖动全部禁用；
- 但**禁用不等于看不见**：进度条被禁用时仍然画出已播放那一段，只是和轨道一起压到 60% 亮度
  （`drawLinearSlider`）。禁用是为了禁止拖动，不是为了把位置藏起来——把强调色整段去掉，那根条
  就变成一根空槽，用户会以为播放位置丢了（实测反馈「开启同步后强调色消失了」）；
- 循环次数、fade、音量**保持可用**（它们参与映射，改了立即生效）；
- **空格键交给宿主**：同步生效时窗口不再消费它（`PlayView::handleKey` 返回 false，条件由
  `TransportPanel::isHostDriving()` 给出），事件因此离开编辑器——Windows 上 JUCE 把没被用掉的
  `WM_KEYDOWN` 转投给父窗口（`juce_Windowing_windows.cpp` 的 `WM_KEYDOWN` → `forwardMessageToParent`）
  ——DAW 自己的播放/暂停快捷键照常工作。这里不设「解绑/重绑」两套状态：判断在每次按键时做，
  同步一关，空格就又是本地走带；
- 其它走带快捷键（Enter / Home / 方向键）仍被窗口吃掉（走带方法自己也会早退，因为快捷键不经过
  按钮）——它们没有对应到「宿主走带」上，所以没跟着让出去；
- 关闭同步时保持当前状态与位置继续（不打断试听）。

注意「同步开」本身不锁走带：宿主不可信时没有映射，插件仍自主走带，这也是 §10.1 的降级路径。
CUE 不是这里的条件——它一直在（默认 0 s），所以「开着同步却不起作用」只可能有一个原因：宿主没在
报时间。这也让「灯亮」和「真的在驱动」永远同时成立，不会出现面板上一处说开、一处说关。

状态灯（只有 `SYNC` 那一处发光，其余靠可用性）：

| 状态 | 表现 |
|---|---|
| Sync 关 | `SYNC` 与铭牌都是中性色，`CUE` 一直是中性色，其它控件正常 |
| Sync 开但宿主不可用 | `SYNC` 按钮与铭牌发亮，但走带照常可用（映射还没成立）；旋钮照常可用并显示偏移量 |
| Sync 开 + 宿主可信 | 走带禁用，位置完全由宿主决定；旋钮可用并显示偏移量 |
| 宿主可信（不管同步开关） | `CUE` 可用——取点需要一个宿主位置，没有它就无处可锚 |

## 9. 状态持久化

`SessionState` 根节点新增四个带默认值的属性，**不升版本号**（`SessionState.cpp` 是严格版本相等比较，
升版本会让旧工程读不回来）。属性都是可选的，旧版本读新工程会忽略它们：

```
syncEnabled      int     默认 0
syncCue          double  默认 0（秒）= DAW 的开头；没有「没有 CUE」这种取值
syncCueOffsetMs  double  默认 0.0
syncKnobRangeMs  double  默认 10.0
```

老工程里如果存过 `syncCue = -1`（上一版用负数表示「已清除」），读档时会钳到 0（见
`PlaybackEngine::readState`），也就是「清除」当时想表达的状态，所以不需要迁移代码。

读取顺序是量程 → 偏移 → CUE：`setKnobRangeMs()` 会按新量程钳制偏移，反过来会把记住的偏移量
按默认量程先钳一次。CUE 属于工程而不属于歌曲：换歌后沿用同一个 CUE（用户仍在同一小节线上对轨）。

## 10. 边界与已知限制

1. **宿主不提供走带**：`Cue` 置灰（框内右键菜单里的归零仍可用，它不需要宿主），`Sync` 可以打开但不起作用——走带照常可用。需要用户先在 DAW 里播放一小段，插件看到时间后才解锁（`seenPlaying` 锁存）。
2. **宿主停止时挂起处理**：部分宿主停止时根本不调用 `process`。插件只能停在最后一个块的位置，无法实时收到「已停止」；可接受。
3. **块粒度**：P1 启动误差 ≤ 1 块（512 @ 48 kHz ≈ 10.7 ms）；P2 采样级未做。
4. **改拍速**：CUE 存的是秒，改拍速后不会跟着小节线走，需要重新 `Cue`。这符合 R8（不使用音乐时基）。
5. **换歌**：映射是绝对的。若 DAW 已过 CUE，新歌会从中间开始；按 `Cue` 重新锚定即可。
6. **离线导出**：有 context 时正常同步；无 context 时按 `usable=false` 回退为自由走带，不能静音。
7. **无 loop 点的歌**：循环次数设置无效，退化为播放一次。
8. **fade**：单次播放会照常淡出；淡出期间位置数值会超过 `duration`，进度条按现有逻辑钳位。
9. **采样率变化**：CUE 以秒存储，仍成立。插件延迟报告为 0，其它轨道的延迟由宿主补偿。
10. **键盘**：同步生效时**空格交给宿主**（`PlayView::handleKey` 返回 false，事件离开编辑器，
    Windows 上 JUCE 把 `WM_KEYDOWN` 转投父窗口），DAW 自己的播放/暂停快捷键照常工作；Enter /
    Home / 方向键仍被窗口吃掉（它们在同步生效时本就早退），因为没有一个把它们对应到「宿主走带」上。

## 11. 实施计划

| 提交 | 内容 | 状态 |
|---|---|---|
| 1 | `SongTimeline` + `makeSongTimeline()`：L / C / N / F，修正 `getLoopPointSeconds()` | ✅ |
| 2 | `DawSync` 纯逻辑 + `evaluateSync()` + `DawSyncTest`（§4.3 全表与边界） | ✅ |
| 3 | 引擎 `followDawTransport()` + `processBlock` 只发布 host clock | ✅ |
| 4 | UI：`DAW SYNC` 虚线框内的 `Sync` / `Cue` / 旋钮 + 数值框、禁用规则、`SessionState`、Standalone 不创建 | ✅ |
| 5 | 采样级块内起点（§6） | ⬜ 未做 |

提交 3 之后就能验证各 DAW 的走带支持情况，再决定提交 5 的优先级。实际实现时，微调从
「右键菜单 ±1 / ±10 ms」改成了**旋钮 + 数值框**：菜单项给的是离散档位，而对轨要的是「听着一路拧」，
旋钮才是这个动作的形状。精确值一开始做成了「双击旋钮弹出覆盖层」，后来改成旋钮自带的文本框——
界面里已经有一个「输入一个数」的形状（音量的 `100 %`），同一个动作不该有两种控件；
双击空出来之后给了「归零」。CUE 也从「切换（再点清除）」改成纯动作，归零移到框内右键菜单，理由见 §8；
再后来连「有没有 CUE」这个状态也删了——CUE 恒存在（默认 0 s），`SYNC` 是唯一的开关。

## 12. 测试（已落地）

| 层级 | 覆盖 |
|---|---|
| `vgm_core_test` / `DawSyncTest` | §4.3 全表；N=1 含 fade；N=2；无限循环；无 loop 点；x<0；x=0 恰好；x=end；宿主不可用；`seenPlaying` 锁存；量程钳制偏移 |
| `vgm_core_test` / `SongTimeline` | N=1/2/0 下 L、C、loopStart 的换算；fade 尾音是否计入；后端少报单遍长度时的反推 |
| `vgm_core_test` / `[11] the host's clock` | 适配器（`HostClockSource`）的九类输入：无 play head、有 head 无 position、全零 context、只有 samples、seconds 优先、只有 bpm、只有 ppq、playing@0、NaN |
| `vgm_app_test` / `daw-sync.txt` | 用 `# wrapper vst3` 声明宿主版，`host 12.5 playing` 注入假 `AudioPlayHead` 并跑一个 `processBlock`；断言 `Cue` 的可用性（旋钮从一开始就可用，数字恒为偏移量：`0.0 ms` → `+2.5 ms`）、Cue 再点仍是取点（不是切换）、`CUE` 的 `checked` 在三种状态下始终为 false（不发光）、同步生效后走带被禁用、关闭同步后恢复且锚点还在、空格交给宿主（`press space unhandled`） |
| `vgm_app_test` / `daw-sync-follow.txt` | 端到端：CUE=2 s，宿主停在 2 s → 位置 0；宿主从 3 s 播放 → `assert engine position ~= 1.0`（这一条就是 H-1 的回归测试）；宿主退到 1 s → 回到 0；宿主停在 5 s → 位置 3.0 且不走带。需要素材（`VGM_TEST_SONG`），没有则整脚本跳过 |
| 手工 | REAPER / Ableton Live / FL Studio / Cubase 各验一次——**未做**，需要真实宿主 |

harness 的两条新语法（`# wrapper` 与 `host`）见 `docs/desgin/testing-architecture.md`：
前者必须在构造 processor 之前读，所以只能写成脚本首行的指令；后者说明 fake play head 与
`prepareToPlay` 的处理方式。

## 13. 当初的建议与结果

| # | 问题 | 建议 | 结果 |
|---|---|---|---|
| 1 | 同步开启时的本地走带 | 全部禁用；关闭同步时保持播放状态继续 | ✅ 已实现，但门槛是 `isActive()`（同步开 + 有 CUE + 宿主可信），不是「同步开」 |
| 2 | CUE 语义 | 歌曲 0 秒 ↔ DAW 播放头，不做歌曲内偏移 | ✅ 已实现 |
| 3 | 单次播放的淡出 | 保留淡出，进度停在终点 | ✅ 已实现（映射里 `tail` 只对有 loop 点的歌存在） |
| 4 | 换歌 | 不自动重对齐；`Cue` 就是重新锚定的手势，换歌后让 Cue 按钮脉冲提示 | 部分：不自动重对齐已实现，脉冲提示未做 |
| 5 | 微调 | 进第一版（右键菜单） | ✅ 已实现，改为**框内右键菜单 + 旋钮**（见 §8） |
| 6 | 目标 DAW | 先 REAPER，再 Ableton Live | 未做（需要真实宿主） |

补充两条实施建议：

- 先做 §4 的纯函数与单测，再接引擎，最后做 UI。块粒度的版本就足以验证宿主支持，不要一开始就改渲染路径。
- 采样级偏移（提交 5）以实测听感为准。DAW 常用块 64–256，误差 1.3–5.3 ms；若不可闻可以不做，避免动混音路径。
