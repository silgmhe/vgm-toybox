# DAW 同步 feature 评审（`feature/daw-sync`）

**评审对象**：`feature/daw-sync` @ `e20b375`（3 个提交：`2329b18` / `35fe634` / `e20b375`）
**范围**：相对 `master`（`05cd3c3`）的全部改动 —— 31 个文件，+2339 / −92
**门禁**：`.\build.ps1 -Target vgm_core_test,vgm_ui_test,vgm_app_test -Test` → **6 targets 全绿**（`daw-sync.txt` 含新增的空格断言）
**方法**：逐文件读实现（不是读 diff 摘要），每个结论都落到行号；对 libvgm 后端与 JUCE 的行为做了源码核对，不靠记忆

本文只记录问题与建议，**没有改任何代码**。

---

## 1. 一页结论

| 维度 | 评价 | 依据 |
|---|---|---|
| 分层与依赖方向 | **好** ✓ | `DawSync` 纯逻辑零 JUCE；`playback → ui` 仍零反向依赖；宿主适配只在 `PluginProcessor` 一处（§2.1） |
| 模块化 / 可扩展 | **一半好** ⚠ | 「加一个宿主格式」= 0 处改动 ✓；「改映射规则」集中在既有 seam ✓；「加一个同步控件」要动 `TransportPanel` 四五个地方 ✗（§5） |
| 可读性 | **好** ✓ | 公式与代码一一对应，注释写「为什么」而不是「做什么」；UI 的「动作 vs 开关」有明确取舍 ✓ |
| 可测试性 | **分三层，中间那层是空的** ⚠ | 纯映射：168 行断言 ✓；UI 状态：`daw-sync.txt` ✓；**处理器适配 + 引擎跟随：0 覆盖** ✗（§4 M-2） |
| 实时安全 | **有一个 UB** ⚠ | 不纠正时零锁 ✓；但跟随路径在锁外读了会被换歌改写的结构体（§4 M-1） |
| 正确性 | **有一个能听见的错** ✗ | 宿主每次「开始/继续」后的第一块先 seek 又被 `start()` 复位（§4 H-1） |
| 工程化 | **好** ✓ | 状态字段增量可选、不升版本；测试脚本进既有 harness，没有为它开新工具 |

**问题清单**（严重度按本仓定义：H = 用户能感知或流程无保护；M = 增加后续改动成本/风险；L = 一致性与整洁）

| # | 级别 | 位置 | 问题 | 建议 |
|---|---|---|---|---|
| H-1 | **H** | `PlaybackEngine.cpp:477-490` | seek 之后才 `start()`，libvgm 的 `start()` 会 `Reset()` 回歌首，seek 被抵消 | 把 `start()` 移到 seek 之前（1 行），或删掉这段（推荐先移动） |
| M-1 | M | `PlaybackEngine.cpp:454,463` | 锁外读 `trackPlayers` / 64 字节的 `cachedTimeline`，与换歌/改 loop 竞争 | 把决策挪进已有的 `ScopedTryLock` 之后（只有 sync 生效时才付这次锁） |
| M-2 | M | `AppHarnessTest.cpp:510-535`、`UiSnapshot.h:71-92` | 能注入假 play head，但**没有任何办法观察引擎位置** → 本 feature 的核心等式 0 断言 | `assert` 加 `value` 与 `assert engine position`，另写需要素材的 `daw-sync-follow.txt` |
| M-3 | M | `TransportPanel.cpp`（1125 行） | 面板第 6 个职责：同步框的几何/绘制/菜单/使能全在宿主类里 | 抽 `SyncGroup : Component`（约 200 行搬动） |
| M-4 | M | `PluginProcessor.cpp:19-58` | 唯一的宿主适配器（`readHostClock`）在匿名命名空间里，测试够不着 | 落到 `playback/HostClockSource.{h,cpp}`，用 `PositionInfo` 喂 |
| L-1 | L | `daw-sync-cue-design.md:165-173` | §7.1 的 API 表还是旧版（`hasCue`/`clearCue`/`isActive` 公式） | 按 `DawSync.h` 现状重写该表 |
| L-2 | L | `PlaybackEngine.h:117` | `getSongTimeline()` 零调用者（引擎自己用成员） | 删掉，或给它一个真正的消费者 |
| L-3 | L | `DawSync.h:77`、`DawSync.cpp:186` | `HostClock::sampleRate` 只写不读；读回也没人用 | 删字段，转换用的局部变量保留 |
| L-4 | L | `PluginProcessor.cpp:46-55` | 注释说「秒是安全单位」，然后拿宿主采样数除以**我们的**采样率 | 要么删掉 samples 分支，要么把不一致写进注释 |
| L-5 | L | `DawSync.cpp:121` | 纯函数 `evaluateSync` 依赖 `DawSync::kToleranceSeconds`（类常量） | 容差作为带默认值的参数传入 |
| L-6 | L | `TransportPanel.cpp:888`、`:895,922,721,755` | 「宿主在驱动」有 5 处各自判断（`isHostDriving()` 与裸 `sync.isActive()`） | 统一到 `isHostDriving()`（多加的 `syncAvailable` 现已冗余但无害） |

---

## 2. 事实基线

### 2.1 新增的四层（层次是对的）

```
UI          TransportPanel（Sync/Cue/旋钮 + 虚线框 + 菜单）   PlayView（空格让位）
             │ 读写 DawSync（消息线程）
             ▼
适配        PluginProcessor::readHostClock()  JUCE PositionInfo → HostClock（音频线程，每块一次）
             │
             ▼
策略        PlaybackEngine::followDawTransport()   decide() → seek/play/pause
             │
             ▼
纯逻辑      playback::DawSync + evaluateSync()   （零 JUCE、零 IO、可单测）
```

- `playback/DawSync.{h,cpp}`：179 + 205 行，**不 include 任何 JUCE 头**，`decide()` 是 `const`，全部状态是原子量。
- `PlaybackEngine` 持有 `DawSync`（`PlaybackEngine.h:121`）；处理器只**发布**时钟（`PluginProcessor.cpp:170`），不决策 —— 决策点在 `render()` 内部（`PlaybackEngine.cpp:1062`）。
- Standalone 不创建那三个控件，并把 `enabled` 清掉（`TransportPanel.cpp:386-397`），避免「没按钮能解开的锁」✓。
- 状态是**增量可选**属性、不升版本（`SessionState.h:46-52`、`SessionState.cpp:43-46,108-111`），读档顺序（量程 → 偏移 → CUE）写在了 `PlaybackEngine.cpp:295-303` ✓。

### 2.2 规模

| 文件 | 行数 | 备注 |
|---|---|---|
| `playback/DawSync.cpp` / `.h` | 205 / 179 | 本次新增的核心 |
| `playback/PlaybackEngine.cpp` | 1315 | 本次 +101（跟随 50 行 + 状态 9 行 + 缓存重构）；全仓最大 |
| `ui/TransportPanel.cpp` | 1125 | 本次 +692；全仓第二、UI 里最大 |
| `Tests/PlaybackCoreTest.cpp` | 981 | 本次 +168（`[10]` 一节） |
| `Tests/AppHarnessTest.cpp` | 772 | 本次 +138（`# wrapper` / `host` / `press ... unhandled`） |

### 2.3 测试覆盖（按层看，缺口在正中间）

| 层 | 有覆盖 | 证据 / 缺口 |
|---|---|---|
| `evaluateSync` 全部分支（N=1/2/0、无 loop、x<0、x=end、fade 尾、容差） | ✓ | `PlaybackCoreTest.cpp:811-931` |
| `DawSync` 状态机（latch、量程钳制、负值忽略） | ✓ | 同文件 `:935-976` |
| UI 状态（可用性、不发光、禁用规则） | ✓ | `scripts/daw-sync.txt` 28 条 `assert` + 2 次按键 |
| 空格让位 | ✓ | `press space unhandled` + 关闭后 `press space` |
| **`readHostClock`（JUCE → `HostClock`）** | **✗** | 匿名命名空间，core test 链接不到；只有 app harness 经过它，但不看结果 |
| **`followDawTransport`（seek/start/pause 的顺序）** | **✗** | 假后端的 `start/pause` 是空实现（`FakeBackend.cpp:38-39`），app harness 又不观察位置 → **H-1 正好落在这个盲区里** |
| 状态往返（4 个 sync 属性、`-1 → 0` 迁移） | ✗ | `SessionState` 往返测试存在，但没断言这几个字段 |

> 判读：**纯逻辑测得比多数项目都好，中间那层却完全空着**。这不是"少写了几个测试"，而是"能注入输入、不能观察输出"的工具缺口（§4 M-2）。

---

## 3. 做得好的地方（先说清楚，后面才好谈问题）

1. **公式与代码一一对应**：`design.md` §4 的 `L / C / N / F` → `SongTimeline` 的四个派生函数 → `evaluateSync` 的三个分支，读任何一处都能对上另两处。这种"可被审阅的正确性"比注释多更难做到。
2. **级联钳位那一段是真经验**（`DawSync.cpp:110-116`）：尾音期间引擎位置会超过可 seek 长度，比较前两边都钳到 `total`，否则每块都判成 3 秒误差、反复 seek。这种坑只有跑过才会知道，而且写进了代码。
3. **原子纪律写明了**：`DawSync.h:98-102` 说清楚"谁写谁读、输了竞争就下一块自愈"，`memory_order_relaxed` 的选择因此站得住（这些量互不推导，不需要跨变量的顺序）。
4. **空格是"查条件"不是"改绑定"**：`PlayView.cpp:112-131` —— 同步一关，空格立刻回到本地走带，没有两套状态要同步。而且把机制（返回 false → Windows 上 JUCE 把 `WM_KEYDOWN` 转投父窗口）写进了注释，而不是只写结论。
5. **Cue 是动作、不是开关**：`checked` 在三种状态下都为 false 并被脚本锁住（`daw-sync.txt:9,26,36,51,71`）。"这个编辑器里亮着 = 开关是开的"这条规则因此没有被例外污染。
6. **状态字段的兼容策略正确**：不升版本 + 全部可选 + 老值 `-1` 钳到 0（`PlaybackEngine.cpp:295-303`），旧工程能读回来。
7. **测试工具只做最小扩展**：`press <key> [unhandled]` 是一个后缀，不是新命令；`# wrapper vst3` 用 JUCE 自己的 `setTypeOfNextNewPlugin` 而不是 `const_cast` 骗过 `wrapperType` ✓。

---

## 4. 问题详述

### H-1 宿主每次「开始/继续」，第一块都从歌首播放 ⚠

**位置**：`plugin/Source/playback/PlaybackEngine.cpp:452-500`

```477:490:plugin/Source/playback/PlaybackEngine.cpp
	setPositionSecondsLocked (decision.position);

	if (decision.play)
	{
		if (! playing.load())
		{
			for (auto& player : trackPlayers)
			{
				player->start();
			}
		}
```

**为什么是错的**：`libvgm` 的 `start()` 不是"继续"，是"从头开始"：

- `LibvgmTrackPlayer::start()` → `playerEngine.Start()`（`backends/libvgm/LibvgmTrackPlayer.cpp:121-124`）
- `PlayerA::Start()` → `_player->Start()`
- `VGMPlayer::Start()` → `InitDevices(); Reset();`（`libs/libvgm/player/vgmplayer.cpp:963`）

于是"先 seek 到目标、再 start()"= **seek 被撤销**。实际时序（以宿主播到 cue+30 s 时启动为例）：

1. 第 N 块：seek(30 s) → `start()` 复位到 0 → 用 0 渲染这一块 → `publishPositionAndEnd()` 从 player 读回位置 ≈ 0（`PlaybackEngine.cpp:1298-1306`）
2. 第 N+1 块：目标 30 s、实际 0 → 再 seek 一次（此时 `playing` 已为 true，不再 `start()`）→ 正常

**能听见什么**：每次宿主开始/继续，先是**一小块歌首的声音**（并且 `InitDevices()` 会重初始化 sound core，带一次毫秒级断点），然后才是正确位置。宿主从中间按播放（DAW 里最常见的操作）每次都触发。

**为什么测试没抓到**：

- 假后端的 `start/pause` 是空实现（`FakeBackend.cpp:38-39`）→ `vgm_core_test` 结构上有覆盖、行为上无差别；
- app harness 跑了真实的 `processBlock`，但 `assert` 只能看 `visible/enabled/checked/text`（M-2）→ 看不见位置。

**修法**（按代价从小到大）：

1. **移动 1 行**：把 `player->start()` 挪到 `setPositionSecondsLocked()` 之前 —— 复位先发生，seek 后生效，语义正确，且保留 `ITrackPlayer::start()` 对其它后端的契约。
2. **删掉这段**（更贴近既有设计）：`playing` 标志本身就是 `render()` 的门（`PlaybackEngine.cpp:1066`），players 在加载时已 `start()` 过一次（`:886`），`pause()` 在 libvgm 是空实现（`LibvgmTrackPlayer.cpp:126-131`，`SongBackend.h:130-132` 也这么写）。删掉后需要在 `SongBackend.h` 明说 `start/pause` 只服务"需要自己线程推进"的后端。
3. 顺带把"要不要 `start()`"写成 `decision.play != engineWasPlaying`（`engineWasPlaying` 就是传给 `decide()` 的那个值）—— 现在 `playing.load()` 在 `decision` 之后又读了一次，同一个 `if` 里混着两个来源，容易被误读成"decision 的 play"。

**工作量**：方案 1 是 trivial；方案 2 需要先定 `ITrackPlayer` 的契约，半小时。

**验证**：按 M-2 加的位置断言，`daw-sync-follow.txt` 里"宿主从 cue+30 s 开始 → 位置等于 30 s"就会红→绿。**这条不需要真实宿主**，但需要一首素材（harness 加载的是真实后端 libvgm，不是假后端）—— 而仓库里目前一首都没有（`Get-ChildItem -Recurse -Include *.vgm,*.vgz` 为空），这也是 `-TestSong` 一直没进 CI 的原因。

**待确认**：本仓的宿主手工测试尚未做（`design.md` §12），以上是从 libvgm 源码推出来的；用 M-2 的脚本复现一次就能从"读代码判断"变成"红灯"。若真机听不出问题，说明我读错了 `start()` 的语义，这一条降级为 L。

---

### M-1 跟随路径在锁外读 `cachedTimeline` 与 `trackPlayers`（UB）⚠

**位置**：`plugin/Source/playback/PlaybackEngine.cpp:452-463`

```452:463:plugin/Source/playback/PlaybackEngine.cpp
void PlaybackEngine::followDawTransport (void)
{
	if (! dawSync.isActive() || ! loaded.load() || trackPlayers.empty())
	{
		return;
	}
	...
	const auto decision = dawSync.decide (cachedTimeline, positionSeconds.load(), playing.load());
```

**问题**：`render()` 自己是有纪律的 —— 碰 players 之前先 `ScopedTryLock`（`:1075`，"抢不到就整块静音"）。`followDawTransport()` 在这条纪律**之上**加了一个锁外的读：

- `trackPlayers.empty()`：与 `installSong()` 里的 `trackPlayers = std::move (built.players)`（`:905`，持锁）竞争；
- `cachedTimeline`（6 个 double + int + bool ≈ 64 字节）：与 `installSong`（`:912`）、`setLoopCount`/`setFadeSeconds` 引起的 `refreshCachedTimes()`（`:523-529`）竞争。

**为什么这次不能沿用"以前也这样"**：改动前锁外读的是**两个标量**（`cachedDurationSeconds`、`loopPointSeconds`），撕裂最多读到"略旧的数"；现在换成**结构体**，撕裂会得到**自相矛盾**的一组值（新歌的 `loopLengthSeconds` + 旧歌的 `singlePassSeconds`），`decide()` 会据此算出没有意义的目标位置。后果被 `setPositionSecondsLocked()` 的钳位兜住（不会崩、不会越界），但这是 **UB**，而且表现为"换歌瞬间偶发一次错误 seek"。

**修法**（推荐 a）：

- **a. 把决策挪进锁内**：`isActive()` 早退之后立刻 `ScopedTryLock`，再读 `trackPlayers`/`cachedTimeline` 并 `decide()`。代价是"同步生效期间每块一次 try-lock"（~20 ns/512 样本块），换来的是"音频线程读 players/timeline 一律在锁内"这条纪律的统一。
- **b. 双缓冲/快照**：音频线程自己持有一份 `SongTimeline`，消息线程在锁内更新后再原子发布。正确但比 a 复杂，收益只有省下那次 try-lock。

**顺带**：`PlaybackEngine.h:114-121` 的 `getSongTimeline()` 没有任何调用者（§4 L-2），而它的注释正是"Lock-free, like the cached duration" —— 这句如果是给后来者看的，会把人引向同一个坑。修 M-1 时把它一起处理掉。

**工作量**：30 分钟（a）+ 一次全量测试。

---

### M-2 harness 能注入假 play head，却不能观察结果 → 核心等式没有断言 ⚠

**位置**：`plugin/Tests/AppHarnessTest.cpp:510-535`、`plugin/Source/ui/UiSnapshot.h:71-92`

`host 12.5 playing` 会 `setPlayHead()` + `prepareToPlay()` + **真的跑一个 `processBlock`**（`:521-532`）—— 也就是说 H-1 所在的代码路径**每次 CI 都在跑**，只是没人看它的输出：

- `snapshot::line()` 输出 `id/type/bounds/visible/enabled/checked/text`，**没有数值**；
- `assert` 的字段表同样只有这五个（`:21-23` 的 help、`:567+` 的实现）。

于是 `engine.getPositionSeconds()`（或等价物）在整个测试体系里**不可观测**，本 feature 的核心等式 `position = host − cue` 没有任何端到端断言。

**建议**（一次投入，长期收益）：

1. `assert` 增加字段 `value`（对 `juce::Slider` 取 `getValue()`），`snapshot::line()` 一并输出 —— 这是通用能力，任何滑杆都能断言；
2. 给本 feature 再加一条更直接的：`assert engine position ~= <秒>`（harness 手里有 `processor`，读引擎的 `getPositionSeconds()` 即可），这样断言不必反推歌曲长度；
3. 新脚本 `scripts/daw-sync-follow.txt`（首行 `# wrapper vst3`）：

```
load $VGM_TEST_SONG          # 无素材时自动 skip，和 load-and-play 一样
host 10 playing
tick
click btn.cue                # CUE = 10 s
click btn.sync
host 12.5 playing            # 宿主往前走了 2.5 s
tick
assert engine position ~= 2.5
host 9.0 playing             # CUE 之前：应停在 0
tick
assert engine position ~= 0
```

**为什么值得**：第二条断言会把 H-1（第一块从 0 播）变成红灯 —— 有 bug 时这里是 0.01 而不是 2.5；也能挡住"改了映射忘了改引擎"这类回归。目前 `daw-sync.txt` 只断言 UI 使能状态，**UI 对了不代表声音对了**。

**工作量**：harness ~30 行 + 脚本 ~10 行。脚本无素材时自动 skip（和 `load-and-play.txt` 一样），所以 CI 要跑它就得先给它一首极小的曲子（`-TestSong`）—— 这正是上次评审 H-3 留下的那条尾巴，这次有了第二个理由。

---

### M-3 `TransportPanel` 已经承担 6 个职责，同步框是"画在别人身上的组件" ⚠

**位置**：`plugin/Source/ui/TransportPanel.cpp`（1125 行）

同步框相关的东西散在五处：

| 位置 | 干什么 |
|---|---|
| `:517-529` | 在 `resized()` 里算 `groupWidth` 并切出 `syncGroupBounds` |
| `:573-616` | 在 `paintCardContent()` 里画虚线框、名牌、裁切过的辉光 |
| `:1074-1123` | 在 `refresh()` 的调用链里控制使能/量程/数值显示 |
| `:959-990` | `syncGroupMouseDown()`：靠**父组件监听子组件**把两个按钮和旋钮的右键汇到一起 |
| `:992-1047` | 菜单项与回调（含 LookAndFeel、缩放、SafePointer） |

也就是说：**加第 4 个控件要改 4 个地方**（宽度公式、布局、绘制、使能），而且"框的边界"被三处代码共享（绘制/命中测试/重绘区域）。这正是 `CardComponent`、`ButtonPaint`、`Icons` 那类抽取想避免的形状。

**建议**：把 `SYNC`/`CUE`/旋钮/框/菜单合成 `ui/SyncGroup : juce::Component`：

- 内部自己 `resized()/paint()`/右键菜单，对外只暴露 `refresh()`、`bool isHostDriving() const`、`std::function<void()> onStateChanged`；
- `TransportPanel` 只剩 `syncGroup.setBounds(topRow.removeFromRight(...))` 与转发 `refresh()`；
- 父监听子组件的技巧随之消失（组件自己收子控件的鼠标事件是正常的）；
- `syncAvailable == false` 时直接 `std::unique_ptr<SyncGroup>` 为空，比"不 add 三个控件"更难写错。

代价 ~200 行搬动，风险低（无逻辑变更，`daw-sync.txt` 会验证）。**收益不是行数，而是"同步框以后能独立演进"**——比如 §5 提到的 "align to bar"、或把 CUE 位置显示成 `bar.beat`，都只需要动这一个组件。

---

### M-4 唯一的宿主适配器不可测 ⚠

**位置**：`plugin/Source/PluginProcessor.cpp:17-58`（匿名命名空间里的 `readHostClock`）

这里是**整个 feature 唯一与宿主格式有关的知识**：偏好 `timeSeconds`、回退 `timeSamples`、`usable` 的判据（playing / 非零时间 / bpm / ppq）、全零 context 的处理。`DawSync.h` 的注释甚至宣称"唯一的宿主相关之处就是时钟从哪来" —— 但这段代码在测试上**不可达**（匿名命名空间 + 依赖 `AudioProcessor`）。

**建议**：抽到 `playback/HostClockSource.{h,cpp}`：

```cpp
// 纯函数：输入是 JUCE 的 PositionInfo（或一个只含 4 个 optional 的 POD），输出 HostClock
HostClock readHostClock (const juce::AudioPlayHead::PositionInfo&, double localSampleRate);
```

`vgm_core_test` 就能覆盖四类输入：无 play head / 全零 context / 只有 samples / 播放中带 bpm+ppq，并顺带把 L-4 的采样率口径定下来。`PluginProcessor` 只剩两行（取 `PositionInfo`、调函数）。**这一层一测，DawSync 的输入域就是完整的了**。

---

### L-1 设计文档 §7.1 的 API 表已过期

`daw-sync-cue-design.md:165-173` 还写着 `hasCue/setCueFromHost/setCueSeconds/clearCue` 与 `isActive // enabled && hasCue && hostUsable`。`35fe634` 之后 CUE 恒存在、`hasCue/clearCue` 已删、`isActive = enabled && hostUsable`（`DawSync.h:154-157`）。§7.2 的"无锁返回副本"也建议改掉（与 M-1 同一处误导）。§10 的已知限制里还缺一条："同步生效时空格交给宿主，其它走带快捷键仍被窗口吃掉"（`PlayView.cpp:112-131`）。

### L-2 `getSongTimeline()` 是死接口

`PlaybackEngine.h:117`，零调用者（引擎自己用成员 `cachedTimeline`）。要么删，要么让它成为 M-1 方案 b 的读入口。

### L-3 `HostClock::sampleRate` 只写不读

`DawSync.h:77` 的字段、`DawSync.cpp:186` 的 `hostSampleRate` 存储：`readHostClock` 用的是自己的局部变量（`PluginProcessor.cpp:54`），`getHostClock()` 的调用方（`DawSync::setCueFromHost`）只看 `usable/seconds`。属于「加字段时顺手存的」，建议删掉，保持一致（与上次评审的 L-4 同类）。

### L-4 samples 回退口径与注释矛盾

`PluginProcessor.cpp:46-55` 注释说"秒是安全单位，因为 samples 是按宿主的采样率数的"，然后 `clock.seconds = *timeSamples / clock.sampleRate`，而 `clock.sampleRate` 是我们的采样率。宿主经过 SRC 时这条回退会系统性偏移。要么删掉 samples 分支（`timeSeconds` 缺失时 `usable=false`），要么把"best effort"写进注释并纳入 M-4 的测试矩阵。

### L-5 纯函数依赖类常量

`evaluateSync()` 是"可独立审阅的正确性核心"，但它在 `DawSync.cpp:121` 用了 `DawSync::kToleranceSeconds`。把容差作为参数（`double toleranceSeconds = DawSync::kToleranceSeconds`）能让纯函数不依赖类，也让"30 ms 是不是对所有宿主都合适"变成一个可测的问题（比如 96 kHz/大缓冲区下块本身就是 40 ms）。

### L-6 「宿主在驱动」的判断分散在 5 处

`TransportPanel.cpp:888`（`isHostDriving()`）、`:895`（`togglePlayStop`）、`:922`（`togglePlayPause`）、`:721`（`seekToSeconds`）、`:755`（`stopTransport`），其中前一个是 `syncAvailable && sync.isActive()`，其余是裸 `sync.isActive()`。两者**在今天等价**（standalone 里 `hostUsable` 永远为 false，`isActive()` 必为 false），但读代码的人要自己证一遍。建议统一成 `isHostDriving()`，并在它的注释里写明"这多出来的 `syncAvailable` 是防御性的"。

---

## 5. 扩展性预演（假设明天要加东西）

| 要做的事 | 要动几处 | 结论 |
|---|---|---|
| 支持新宿主格式（VST3 → AU/AAX/CLAP） | **0** | `readHostClock` 只吃 JUCE 的 `AudioPlayHead`，是最省的一类扩展 ✓ |
| 给同步加采样级块内起点（`design.md` §6 的 P2） | 2（`SyncDecision` 加块内偏移 + `render()` 里先补静音再渲染） | 映射层不动 ✓（`evaluateSync` 返回的是绝对位置），但要改的是每块都跑的热路径 |
| 改成 BAR/PPQ 时基（把 CUE 吸到小节线） | 5（`HostClock` +2 字段、`readHostClock`、`evaluateSync`、`SessionState` +1、UI 一处显示） | 都在既有 seam 上，但**必须承认 §10 第 4 条的现状**：`HostClock` 今天连 `ppq`/`bpm` 都丢掉了，是"CUE 存秒"这个决定把它挡在边界外 |
| 加一个同步控件（"align to bar" 按钮 / CUE 坐标显示） | **4~5（全在 `TransportPanel`）** | 这是 M-3 的实际代价：本该 1 处的改动 |

> 判读：**"宿主格式"和"映射规则"两个轴已经设计对了，"UI 控件"这个轴还压在面板上**。

---

## 6. 建议的修复顺序

1. **H-1**（1 行移动 + 注释）—— 先把能听见的错修掉。
2. **M-2**（`assert engine position` + `assert value` + 新脚本）—— 让 H-1 的修复可验证，也为后续所有引擎行为开一扇窗。**先加观测，再改行为**，顺序不要反。
3. **M-1**（决策挪进 try-lock）+ L-2（顺手删死接口）。
4. **M-4**（`HostClockSource` 抽出 + 4 类输入的单测，含 L-4 的口径）。
5. **M-3**（`SyncGroup` 抽取）—— 想继续在这个框里加东西就做，不加可以延后。
6. **L-1 §7.1、L-3、L-5、L-6** —— 一致性收尾，可以在同一次提交里做完。

前两项合起来约半天，能把"这个 feature 现在到底对不对"从**读代码判断**变成**CI 断言**。

---

## 7. 待拍板

1. **H-1 的修法**：移动 `start()`（小、保留后端契约），还是删掉 start/pause 只留 `playing` 标志（更贴近现有设计，但要先在 `SongBackend.h` 定契约）？
2. **M-1 的修法**：接受"同步生效期间每块一次 try-lock"（简单、统一纪律），还是做快照/双缓冲（省锁、多一层状态）？
3. **M-3 的时机**：现在就抽 `SyncGroup`，还是等真的出现第 4 个控件再抽（届时 `daw-sync.txt` 是现成的安全网）？
4. **下一步功能**：上一轮讨论的"把 CUE 坐标复制给宿主打 marker"/"吸附到小节"，一旦要做，`HostClock` 就得带上 `ppq`/`bpm` —— 要不要现在就按"未来会有音乐时基"来设计，还是维持 §10 第 4 条的"秒"不变？
