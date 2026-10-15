> 设计文档：多轨 Audio Output（Kontakt 式多输出乐器）
> 里程碑：**M3.0（feature/multitrack-output 的主功能）**
> 分支：`feature/multitrack-output`（已含 M2.5 播放后端解耦）
> 日期：2026-10-16
> 状态：**设计待评审**（尚未实现）

---

## 1. 需求（已与用户逐条冻结）

| # | 决策 |
|---|---|
| R1 | 插件对外声明 **可配置数量**的立体声输出总线，**默认 64 条**：`Out 1/2`（主总线）、`Out 3/4` … `Out 127/128` |
| R2 | 每个通道条**底部**新增**单向下拉**（Send 式单选）：`Master` / `Out 1/2` / `Out 3/4` …，**默认 `Master`** |
| R3 | **Master 条也有下拉**，但**不能选 Master 自己**，只能选实际总线（默认 `Out 1/2`） |
| R4 | **Master 条新增静音按钮**（复用轨道条 M 按钮：红底白字，不加 S）：屏蔽 Master 总线输出；输出到外部总线的子轨道照常播放 |
| R5 | 直出轨道保留自身 Gain/Pan/M/S；**混音 Master 推子只作用于 Master 总线**；**走带总音量作用于所有总线**；Master 表 = Master 总线（推子后、总音量前） |
| R6 | 未被宿主连接的总线在下拉中**标注"（未连接）"但仍可选**；主总线允许被宿主禁用；目标总线未连接的轨道 → **静音** |
| R7 | 轨道数 > 总线数：多轨共用同一总线，允许 |
| R8 | 关闭混音器：只有 Master 路径（送到 Master 的目标总线）；通道条下拉**置灰保留** |
| R9 | 路由**不保存**到宿主会话（换歌重置，与其它轨道参数一致）；本里程碑只做这一个功能 |
| R10 | standalone：一般只有 `Out 1/2` 有效，其余标注未连接 |
| R11 | 后端不支持"每轨隔离渲染"时，多输出降级为只有 Master 路径，UI 置灰（能力位驱动） |
| A | 总线数量的"可配置"= **构建期** CMake 变量 `VGMPLAY_OUTPUT_BUSES`（默认 64）—— VST3 总线布局是静态的 |
| B | Master 静音 = Master 条 M 按钮；静音时 Master 表归零 |
| C | 关闭混音器 + Master 静音 = 整首静音（此时只有 Master 路径） |
| D | 主总线在宿主中显示名统一为 `Out 1/2` |
| E | 目标总线未连接的轨道静音，**不与静音按钮联动**（下拉里已标注） |
| R12 | **所有输出总线一律立体声**：宿主请求 mono 时布局被拒（宿主会回退到 stereo）—— 换取代码简单可维护（ADR-7） |

---

## 2. 现状与落点

已有的基础（M2.5 交付，见 `docs/playback-backend-design.md`）：

- 每轨已经由**独立的 `ITrackPlayer` 实例**渲染（`IsolationMode::multiInstance`），混音求和发生在 `PlaybackEngine::render()`；
- 引擎的渲染签名目前是**固定双声道** `render(float* const* outputs, int numSamples)`；
- 音量链路已在渲染末尾成型：轨道 Gain/Pan →（Master 求和 + 混音 Master 推子）→ Master 表 → 走带总音量；
- 轨道状态是**每轨原子量**（gain/pan/mute/solo/peak），音频线程免锁读取；
- 两个离线测试（`vgm_mixer_test` 走 libvgm、`vgm_core_test` 走假后端且不链接 libvgm）。

本次改动**只落在**：`playback/PlaybackEngine.{h,cpp}`（路由 + 多总线渲染）、`PluginProcessor.{h,cpp}`（总线声明与缓冲区组装）、`ui/ChannelStrip.{h,cpp}` + `ui/MixerPanel.{h,cpp}`（下拉与 Master 静音）、两个测试、CMake。**端口层（`SongBackend.h`）与 `backends/` 一行都不用改** —— 这正是 M2.5 的收益。

---

## 3. 模型与信号流

```
                          ┌─────────────── Master 总线（内部求和，默认目标 Out 1/2） ───────────────┐
 track 1 ─ gain/pan/M/S ─▶│ 求和 ─▶ 混音 Master 推子 ─▶ Master 表 ─▶ Master 静音 ─▶ 目标总线选择  │
 track 2 ─ gain/pan/M/S ─▶│                                                                    │
 ...                      └────────────────────────────────────────────────────────────────────┘
 track k ─ gain/pan/M/S ────────────────────────────────────────────────▶ 目标总线（直出，绕过 Master）

 所有总线 ─▶ 走带总音量（插件的输出电平）──▶ 宿主
```

- **目标（destination）** 是单值：`Master`（内部）或某条真实总线（`Out n/n+1`）。
- **Master 不能指向自己**；Master 的默认目标是 `Out 1/2`（第 0 号总线），可改为任意其它总线。
- 直出轨道**不进 Master 求和**，但仍经过自身的 Gain/Pan/M/S（R5）。
- 未连接总线：渲染时直接跳过（不写、不清零也无所谓，因为缓冲区无效）→ 目标指向未连接总线的轨道**静音**（R6/E）。

---

## 4. 引擎设计（`playback/PlaybackEngine`）

### 4.1 新增边界常量与类型

```cpp
// playback/OutputRouting.h（新文件；只有常量与别名，不依赖任何格式引擎）
#ifndef VGMPLAY_OUTPUT_BUSES
	#define VGMPLAY_OUTPUT_BUSES 64			// 构建期可覆盖（CMake: -DVGMPLAY_OUTPUT_BUSES=16）
#endif

namespace vgmplay::playback
{
	/** 插件声明的立体声输出总线数量（Out 1/2 = 0 号总线）。 */
	inline constexpr int kOutputBusCount = VGMPLAY_OUTPUT_BUSES;

	/** 轨道/总线的输出目标：-1 = 内部 Master 总线，>= 0 = 真实总线序号。 */
	using OutputTarget = int;
	inline constexpr OutputTarget kMasterOutput = -1;
}
```

### 4.2 公开接口（新增）

```cpp
	// --- output routing (message thread writes, audio thread reads lock-free) ---
	/** 轨道输出目标（kMasterOutput 或总线序号）；越界值会被夹紧。 */
	void setTrackOutput (size_t trackIndex, playback::OutputTarget target);
	playback::OutputTarget getTrackOutput (size_t trackIndex) const;

	/** Master 总线的目标总线（只能是真实总线，不是 kMasterOutput）。 */
	void setMasterOutput (playback::OutputTarget busIndex);		// 越界/自指 -> 夹紧到 0
	playback::OutputTarget getMasterOutput (void) const { return masterOutput.load(); }

	/** Master 总线静音（R4/B）。 */
	void setMasterMuted (bool shouldBeMuted) { masterMuted.store (shouldBeMuted); }
	bool isMasterMuted (void) const { return masterMuted.load(); }
```

### 4.3 渲染签名（改为多总线）

```cpp
	/** 每条已连接总线一个立体声对；宿主未连接的总线 left/right 为 nullptr。 */
	struct OutputBusBuffers
	{
		float* left = nullptr;
		float* right = nullptr;
	};

	/** 音频线程：渲染到宿主提供的总线缓冲。numBuses 必须等于 kOutputBusCount。 */
	void render (const OutputBusBuffers* buses, int numBuses, int numSamples);
```

旧的 `render(float* const*, int)` 重载**删除**（唯一调用者是插件与本仓库测试，同步更新，避免两套语义）。

### 4.4 `render()` 内部（顺序即音量链路的权威描述）

1. **清零**：所有 `buses[i].left != nullptr` 的总线清零（未连接的跳过）。
2. 早退（未播放 / 未加载）时同样清零 + 发布静音电平（沿用 M2.5 的电平表修复）。
3. **逐轨渲染**（mixer 模式）：
   - 渲染进 `mixLeft/mixRight` 临时缓冲，算 track 峰值（不变）；
   - 施加 gain/pan/mute/solo（不变）；
   - 按该轨的 `OutputTarget` 分派：
     - `kMasterOutput` → 累加进 **`masterLeft/masterRight` 内部缓冲**；
     - 真实总线 `b` → 若 `buses[b]` 已连接则累加进去，否则**丢弃**（静音）；
   - 越界序号按"未连接"处理（防脏数据）。
4. **Master 段**：混音 Master 推子 → 测量 Master 表 → 若未静音，把 `masterLeft/Right` 累加进 **Master 目标总线**（未连接则丢弃）。
5. **走带总音量**：对所有已连接总线统一相乘（R5）。
6. 发布位置 / 播放结束判定（不变）。
7. **单实例模式（关闭混音器）**：跳过 3-4 的逐轨分派，player 输出直接进 `masterLeft/Right`，之后走同一段 Master 逻辑与总音量（R8：等价于"只有 Master"）。

要点：

- **不新增分配**：`masterLeft/masterRight` 在 `prepare()` 里按 `maxBlockSize` 分配一次（与现有 `mixLeft/mixRight` 同处），音频线程零分配、零加锁；
- 路由映射存在 `TrackState` 里（`std::atomic<int> output`），音频线程每块读一次到局部变量；
- 未连接总线零成本（不渲染、不清零），因此"声明 64 条但只用 2 条"与今天等价。

---

## 5. 插件层设计（`PluginProcessor`）

### 5.1 总线声明

```cpp
juce::AudioProcessor::BusesProperties props;
props = props.withOutput ("Out 1/2", juce::AudioChannelSet::stereo(), true);	// 主总线，默认激活
for (int bus = 1; bus < playback::kOutputBusCount; ++bus)
	props = props.withOutput (busName (bus), juce::AudioChannelSet::stereo(), false);	// 默认不激活（ADR-2）
```
`busName (0) = "Out 1/2"`，`busName (n) = "Out " + (2n+1) + "/" + (2n+2)`（R1/D）。

### 5.2 布局策略 `isBusesLayoutSupported()`

- **每条总线只允许 stereo 或 disabled**（R12 / ADR-7）：宿主请求 mono 一律拒绝，宿主会退回 stereo。
- **允许主总线被禁用**，只要至少还有一条总线启用（R6）。
- 其余情况自动成立（每条总线非 stereo 即 disabled）。

### 5.3 `processBlock()`

```cpp
	playback::OutputBusBuffers buses[playback::kOutputBusCount];
	for (int bus = 0; bus < playback::kOutputBusCount; ++bus)
	{
		auto* buffer = getBusBuffer (bufferToFill, /*isInput*/ false, bus);
		if (buffer->getNumChannels() >= 2)
			buses[bus] = { buffer->getWritePointer (0), buffer->getWritePointer (1) };
	}
	engine.render (buses, playback::kOutputBusCount, bufferToFill.numSamples);
```
（禁用总线在 JUCE 里通道数为 0 → 保持 `nullptr` = "未连接"。）

---

## 6. UI 设计

### 6.1 `ChannelStrip`

- **底部新增一行下拉**（`juce::ComboBox`，高 20 px），通道条 `preferredHeight` 170 → **196**（卡片最小高度同步 +26）。
- 选项由 `MixerPanel` 提供（避免通道条依赖引擎）：
  ```cpp
	struct OutputOption { playback::OutputTarget target; juce::String name; bool connected; };
	void setOutputOptions (const std::vector<OutputOption>& options, playback::OutputTarget selected);
	std::function<void (playback::OutputTarget)> onOutputChanged;
	```
- Master 条：下拉**不含** `Master` 项（R3），并**启用 M 按钮**（静音 Master 总线，R4/B；S 仍隐藏）。
- 下拉项文案：已连接 `Out 3/4`；未连接 `Out 3/4（未连接）`；关闭混音器时整行 `setEnabled(false)` 置灰保留（R8）。
- 目标未连接的轨道：下拉显示该项（带标注）+ 该轨静音（引擎保证）。

### 6.2 `MixerPanel`

- 刷新时组装 `OutputOption` 列表（哪些总线"已连接"由引擎上报：`PlaybackEngine::isOutputBusConnected (int)`，由插件每块写入的原子位图提供）；
- 把每轨 `getTrackOutput()` 与 `getMasterOutput()` / `isMasterMuted()` 喂给通道条（沿用现有 `setValues` 外挂两个参数的方法，不破坏既有签名）。
- Master 条的回调：`onOutputChanged` → `setMasterOutput`；`onMuteChanged` → `setMasterMuted`。
- 卡片头部（混音开关左侧）另有两个**路由快捷按钮**（后加的便利功能），它们设置的是引擎的**默认路由方案** `DefaultRouting`：
  - **分散输出**（`spreadAcrossBuses`）：轨道 *n* → 总线 *n*（`Out 1/2`、`Out 3/4`…）；超出总线数量的轨道留在 Master（没有总线可分）。
  - **全部回 Master**（`master`）：所有轨道目标改回 `Master`（**默认方案**，未触碰时即今天的行为）。
  - 点击 → `PlaybackEngine::setDefaultRouting()`：**立即作用于当前歌曲**，并且**之后加载的每首歌都以该方案开始**（轨道状态在新歌加载时由 `createPlayers()` 创建，随即套用该方案）。方案**跨文件保持**，与"不持久化到宿主会话"（R9）不冲突：它是本次运行期的偏好，不写入会话。
  - 两个按钮同时充当**单选指示**（当前方案高亮）；**关闭混音器时置灰**（此时路由不生效，R8）；点击后立即 `refresh()`，下拉同步更新。

### 6.3 引擎侧的"已连接"上报

插件（知道 JUCE 总线状态）在每块渲染时把已连接总线写入引擎的原子位图：

```cpp
	/** 宿主在最后一块里连接了哪些总线（bit *i* = 总线 *i*，总线 0 默认已连接）。
		引擎在 render() 里从拿到的缓冲区自行发布，UI 免锁读取。 */
	bool isOutputBusConnected (int bus) const;
```
> 实现细节（P1 实际采用）：引擎**不需要**插件的额外调用 —— `render()` 收到的总线缓冲本身就是答案，它每块把"哪些总线拿到了缓冲区"写进 `std::atomic<std::uint64_t> connectedBuses`（bit *i* = 总线 *i*，总线 0 默认已连接）。`kOutputBusCount <= 64` 由 `static_assert` 守住；若将来要声明的更多，就把它换成字节数组（接口不变）。

---

## 7. 测试与验收

### 7.1 硬门禁（逐样本一致）

**把各直出轨按目标归位后，所有总线之和 == 原来的单总线混音。**

- `vgm_mixer_test`（libvgm）：默认全部路由到 Master → 与 M2.5 的基准 `plain vs mixer` **逐样本一致（−1000 dB）**；再叠加"部分轨道直出"的配置，验证"求和 == 基准"。
- `vgm_core_test`（假后端，解析式）：三音轨，轨道 0 直出 `Out 3/4` → 断言总线上恰好是它自己的解析电平（0.176777），总线上剩余两音的合成电平也可解析验证。

### 7.2 功能验收清单

| # | 检查 |
|---|---|
| 1 | 默认全部 `Master` → 输出与改动前逐样本一致 |
| 2 | 单轨直出 → 该轨只出现在目标总线，且**不再**计入 Master 总线 |
| 3 | Master 目标改到 `Out 3/4` → Master 内容整体搬家，`Out 1/2` 静音 |
| 4 | Master 静音 → Master 总线（及其目标总线）只剩直出内容；Master 表归零 |
| 5 | 目标总线未连接 → 该轨静音，其余不受影响 |
| 6 | 关闭混音器 → 只有 Master 路径（含 Master 静音 = 整首静音） |
| 7 | 无隔离能力的后端（假后端提供 `IsolationMode::none` 变体）→ 只有 Master 路径，UI 置灰 |
| 8 | 路由切换实时生效、无爆音回归（切换后下一块即生效，原子读） |
| 9 | 未连接总线零成本（`vgm_core_test` 里对比"声明 64 / 只连 2"与"1 条总线"的渲染耗时） |

### 7.3 手工验收（宿主）

- 组件（VST3）：在宿主里启用若干额外输出总线 → 把轨道下拉分别指向它们 → 宿主侧应看到对应通道有独立音频；
- standalone：设备只有 2 通道时，只有 `Out 1/2` 有内容，其余下拉标注"（未连接）"。

---

## 8. 架构决策记录（ADR）

### ADR-1：总线数量构建期固定（CMake 变量，默认 64）
**上下文**：VST3 的总线布局在插件实例化时声明，运行期不可变。
**决策**：`-DVGMPLAY_OUTPUT_BUSES=64`（默认），编译进常量 `playback::kOutputBusCount`。
**备选**：运行期可配置（不可行：需重建插件）、写死 64（无法裁剪，小工程也背 128 通道）。
**后果**：改数量要重编译；换取零运行期复杂度与宿主兼容性。

### ADR-2：额外总线默认**不激活**，主总线默认激活
**决策**：`withOutput(..., isActivatedByDefault = false)`（主总线 true）。
**备选**：全部默认激活 —— 宿主会立刻分配 128 通道、界面冗长，且绝大多数用户只用主输出。
**后果**：与 Kontakt 等主流多输出乐器一致；用户要在宿主里按需启用额外总线（行业惯例，README/文档需说明）。

### ADR-3：`Master` 是**内部求和总线**，不是第 0 条真实总线
**决策**：`kMasterOutput = -1`；Master 自己的目标是真实总线（默认 0 号 = `Out 1/2`），不可自指。
**备选**：把 Master 当作 0 号总线、真实总线从 1 号开始 —— 会让"直出到 Out 1/2"与"发送到 Master"语义混淆。
**后果**：模型清晰（内部总线 + N 条真实总线），UI 下拉的"Master 不可选自身"自然成立。

### ADR-4：直出**绕过混音 Master 推子**，但**经过走带总音量**
**决策**：Master 推子是"Master 总线推子"；走带总音量是插件输出电平，对全部总线生效。
**备选**：直出全绕过（用户以为音量坏了）、全经过（Master 推子会莫名削弱直出轨）。
**后果**：与 Kontakt 的主音量语义一致；Master 表只反映 Master 总线。

### ADR-5：输出目标是**单选**，不是多条并行 Send
**备选**：多 Send + 每 Send 电平（本次范围过大，用户也明确只要 Kontakt 式单选）。
**后果**：模型与 UI 都保持简单；将来要做多 Send 时，`TrackState::output` 可升级为定长数组。

### ADR-6：路由映射为**每轨原子量**，音频线程每块读一次
**后果**：切换立即生效、无需加锁/重建播放器；"目标总线未连接"在渲染时按丢弃处理。

### ADR-7：**强制立体声**（所有总线 stereo 或 disabled）
**上下文**：宿主可能把乐器轨设成单声道，理论上可以接受 mono 并把 `L+R` 写进单通道。
**决策**：不接受 mono —— 每条总线只允许 stereo 或 disabled，宿主会回退到 stereo。
**备选**：仅主总线支持 mono（多一条 `right == nullptr` 的分支，渲染与测试都要覆盖两套语义）。
**后果**：代码路径只有一套（永远两个指针），可维护性优先；代价是单声道轨会以立体声加载（用户几乎无感，主流宿主均支持立体声）。

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 宿主对输出总线数量/显示有各自上限（Ableton、FL、Logic/AU 更紧） | 中 | 数量可配置；额外总线默认不激活；文档写明"需在宿主里启用额外输出" |
| 第 2 条起的总线多数宿主需手动启用 | 中 | 下拉标注"（未连接）"，行为明确（静音）而非静默失败 |
| 主总线被宿主禁用 | 低 | 允许；Master 目标指向禁用总线时该轨静音并在下拉标注 |
| 插件 I/O 签名变化 | 低 | 一次性重新扫描/重载；写进文档 |
| 通道条增高挤压卡片 | 低 | 卡片自适应逻辑已有（M2 的 `layoutStrips()`）；最小高度同步 +26 |
| 未连接总线被误当"静音"是 bug | 低 | 下拉标注 + 手工验收清单覆盖 |
| `kOutputBusCount > 64` 时位图溢出 | 低 | 连接位图设计为分块原子字节数组（ADR-6 说明） |

---

## 10. 里程碑 M3.0 分解（每步一个提交、每步可听可跑）

| 步 | 内容 | 验收 |
|---|---|---|
| **P1** | 引擎：`OutputRouting.h`、每轨/ Master 目标原子量、Master 静音、`masterLeft/Right` 内部缓冲、多总线 `render()`；同步更新两个测试为"1 条总线"调用 | `vgm_mixer_test` 与改动前**逐样本一致**；`vgm_core_test` 全绿 |
| **P2** | 插件：64 条总线声明（额外默认不激活）、`isBusesLayoutSupported`（含 mono 主总线）、`processBlock` 组装缓冲区、连接位图上报 | 组件与 standalone 均能加载；宿主里启用额外总线可见 |
| **P3** | UI：通道条底部下拉 + Master 静音按钮 + 未连接标注 + 关闭混音器置灰；`MixerPanel` 组装选项与刷新 | 手工验收：改路由后宿主通道内容随之变化 |
| **P4** | 测试补齐：直出/搬移/静音/未连接/降级/关闭混音器/性能对照 | §7.2 全绿 |
| **P5** | 文档（`CODEBUDDY.md`、`docs/mixer-milestones.md` 记录 M3.0）、dist 构建与冒烟 | 交付 |

**CMake 变量**：`VGMPLAY_OUTPUT_BUSES`（`CACHE STRING`，默认 `64`），以编译定义传给插件与两个测试目标。

**非目标（本里程碑不做）**：路由持久化（R9）、多 Send/每 Send 电平、每总线音量、离线导出多轨 WAV、运行期修改总线数量、M3 示波器/频谱（仍搁置）。

---

## 11. 实施记录（M3.0 已完成 2026-10-16）

| 步 | 内容 | 提交 |
|---|---|---|
| P1 | 引擎：`playback/OutputRouting.h`、每轨输出目标与 Master 目标/静音的原子量、Master 内部缓冲、多总线 `render()`；两个测试改为"1 条总线"调用 | `8362b23` |
| P2 | 插件：声明 `kOutputBusCount` 条总线（`Out 1/2` 激活，其余默认不激活）、布局策略（stereo 或关闭，主总线可禁用但至少一条启用）、`processBlock` 组装所有总线缓冲 | `25ff641` |
| P3+P4 | UI：通道条底部输出下拉（"（未连接）"标注、关闭混音器时置灰）、Master 条静音按钮；核心测试补齐路由/搬移/静音/未连接等解析式验收 | `f1c195b` |

**验证结果**

- `vgm_mixer_test`（libvgm 全链路）：默认全 Master 路由下输出与改动前 **逐样本一致（−1000.00 dB）**，其余检查数值不变 —— 即"多总线重写没有动音质"。
- `vgm_core_test`（假后端，不链接 libvgm）：新增 8 项路由检查全部通过，且数值与解析预期**完全吻合**（单音 0.176777、双音 0.250000、三音 0.306186）：单轨直出 0.176777 / 主总线剩 0.250000、所有总线之和仍等于未改动混音、Master 目标搬移、Master 静音（表归零、直出照常）、未连接总线上的轨道静音。
- 构建：VST3 + standalone 通过，源码守卫与冒烟测试通过。

**实现要点（与设计稿的差异/补充）**

1. **连接位图由引擎自行发布**：`render()` 从收到的缓冲区推断"宿主连接了哪些总线"并写入原子位图（bit *i* = 总线 *i*），插件无需额外调用 —— 设计稿 §6.3 的 `setConnectedOutputBuses()` 因此没有实现。
2. **未连接总线上的轨道静音，但轨道表仍跳动**：轨道表测的是轨道自身电平（与路由无关），下拉的"（未连接）"标注已说明它会静音。
3. `BusesProperties` 是 `AudioProcessor` 的 **protected** 嵌套类型 → 总线工厂必须写成**类内静态成员**（匿名命名空间里无法命名该类型）。
4. `kOutputBusCount > 64` 由 `static_assert` 守住（连接位图是 64 位）；要声明更多总线需把位图换成数组（接口不变）。
