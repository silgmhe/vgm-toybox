# libvgm 可视化 feature 评审 · 及对后续三个功能的架构评估

**评审日期**：2026-11-14
**评审对象**：`feature/libvgm-visualisation`（相对 `origin/master`：255 提交 / 246 文件 / +33 238 −2 046）
**评审分支**：`review/libvgm-visualisation`（= `35aa4ed`）
**依据**：`.codebuddy/rules/code-quality.md`、`.codebuddy/rules/file-edits.md`、设计文档 `docs/desgin/libvgm-visualisation.md`

---

## 结论

**功能达标，可以合并。** 29 颗芯片的波形/波表可视化、拖拽导出、整曲批量导出三条主线全部落地且被 54 个 ctest 与逐芯片截图覆盖；唯一撤下的是 RF5C164（有据可查的放弃，见 §9.5.2 与 `docs/research/2026-11-14-rf5c164-post-mortem.md`）。

**但有 3 个必须在本分支还掉的债**（H-1…H-3），全部集中在"注释与分发"两处，与本轮踩过的坑同源：

| 级别 | 数量 | 摘要 |
|---|---|---|
| **H（必须修）** | 3 | `SongBackend.h` 两处文档错位/损坏；26 个收集器的复制粘贴分发 |
| **M（应修）** | 5 | `.vgz` 解压无上限；死代码保留；`PlaybackEngine` 过大；单文件测试；收集器重复的命名/去重 |
| **L（可延后）** | 3 | 取证工具与产品解析器的边界未写明；文档表格曾与代码不符；`LibvgmSongInstance.cpp` 职责混合 |

**对后续三个功能的判断**（详见第六节）：

| 功能 | 就绪度 | 真正缺的东西 | 量级 |
|---|---|---|---|
| 1. 乐器演奏视图 | UI 就绪，**音频无入口** | 一个"音色端口"（`IVoiceRig`）+ 混音注入点 | 中 |
| 2. FM 可视化（完整音符过程） | 数据模型**缺时间轴** | `ChannelState` 加 `noteAge`/`egLevel`，解码器加 trail | 小~中 |
| 3. SPC / GSF | 端口**已就绪**（NSF 是活证据） | `SongMemory` 伴随文件、每后端 `captureState`、扫描式 `samples()`、`inPlaceChannelMix` | 大（每格式一套） |

**一句话**：这个 feature 唯一的架构级教训是——**界面拿到的是一帧"瞬态快照"，而"一个音符的过程"从来没有对应的数据结构**。RF5C164 的四次失败与功能 2 要做的优化，是同一个缺口的两面。

---

## 一、评审方法

| 项 | 做法 |
|---|---|
| 静态审查 | 逐文件读接口与关键实现；对 26 个收集器、31 个解码器做同构性比对 |
| 规模度量 | 文件行数、重复模式出现次数（`hashOf` ×36、`SampleFormat` ×34） |
| 运行验证 | `54/54`；`vgm_chip_test`/`vgm_nsf_test` 断言；`vgm_app_test` 截图 |
| 规范对照 | 逐条走 `code-quality.md` 的 13 条与提交前检查清单 |
| 环境安全 | 该分支不涉及网络、认证、加密与远程输入；安全面只有**文件解析**（本地用户主动打开的文件） |

**不给"安全评分"。** 这是一个本地桌面/VST 音频程序，无人可越权，也没有不可信远端数据。把 OWASP 那套套上去只会产出噪声——唯一真实的攻击面是恶意构造的 `.vgz`/`.vgm`，M-1 就是那一条。

---

## 二、必须修（H）

### H-1 两个文档注释挂错了函数（规则 2、13）

`plugin/Source/playback/SongBackend.h:196-215`

```cpp
	/** Fills a snapshot of the chips' live state for display. ... return false ... */
	/** Every distinct sample the song has started, decoded and ready to be written. ... */
	virtual std::vector<DecodedSample> samples (void) const { return {}; }

	virtual bool captureState (ChipStateSnapshot& snapshot) const { ... }
```

第一段注释（"Fills a snapshot of the chips' live state"）描述的是 **`captureState`**，却贴在 `samples()` 上面；`samples()` 自己的注释从中间开始（"Called from the message thread while the audio thread is running"）。结果：`captureState` 一个字的说明都没有，而 `samples()` 顶着两段互相矛盾的说明。

**为什么这条是 H**：这个分支上最贵的一次误判（"键盘页空 = 路由没接上"）就是"读注释比读实现多"导致的。头文件是全项目唯一的接口契约，错位的契约比没有契约更贵。

**修**：把第一段移回 `captureState` 上方；`samples()` 保留自己的三段（语义、空值含义、线程）。

### H-2 文档里有一句话被编辑工具切成了两半

`plugin/Source/playback/SongBackend.h:222-225`

```cpp
		a single pass (IsolationMode::inPlaceChannelMix). Buffers holds 	rackCount stereo pairs,
		each 
	umSamples frames long; the backend writes all of them or none.
```

`trackCount` 中间插进了一个制表符（`t` + TAB + `rackCount`），`numSamples` 被换行劈成 `umN amples` 的前半段。显然是某次脚本替换留下的产物。

**为什么这条是 H**：它证明"用脚本改文件"这条路径仍然能用（`file-edits.md` 明令禁止），而**同一个工具已经在本轮造成过两起编译期可见的事故**（`if (a; b)`、bank 判断被吞）。文档里留一处伤，等于留一张"这样改也可以"的许可证。

**修**：恢复原句；提交前扫一遍被编辑文件里的可疑空白与半词。

### H-3 26 个收集器的分发是复制粘贴，而不是一张表（规则 4）

`plugin/Source/backends/libvgm/LibvgmSongInstance.cpp:271-460`

```cpp
	const bool hasC352 = songIr != nullptr
						 && std::any_of (songIr->chips.begin (), songIr->chips.end (),
										 [] (const midi::ChipSlot& slot)
										 { return slot.kind == midi::ChipKind::c352; });

	if (hasC352 && songIr != nullptr)
	{
		for (auto& sample : collectC352Samples (*songIr))
		{
			collected.push_back (std::move (sample));
		}
	}
```

同一个 12 行形状重复 26 次（C140/C352/QSound/K053260/K054539/…/X1-010），每份都要手抄一次枚举名、一次 `any_of`、一次 lambda。

**代价已经付过**：RF5C164 的收集器在撤下后仍然留在这张清单里没人发现（本项目 §13 那条"第二个写者"的经典形状）；加一颗新芯片要改两处（include + 块），并且**行序即优先级**这件事没有任何地方写下来。

**修**：一张表 + 一个循环。

```cpp
namespace
{
	struct CollectorRow
	{
		midi::ChipKind kind;
		std::vector<playback::DecodedSample> (*collect) (const midi::SongIr&);
	};

	const CollectorRow kCollectors[]
	{
		{ midi::ChipKind::c140,    &collectC140Samples },
		{ midi::ChipKind::c352,    &collectC352Samples },
		// ...
	};
}

for (const auto& row : kCollectors)
{
	if (irHasChip (*songIr, row.kind))
	{
		appendSamples (collected, row.collect (*songIr));
	}
}
```

`irHasChip()` 也顺手把 26 份 `any_of` 收成一份。NES DPCM 走的是另一个签名（字节 + 长度），在表外用一行显式调用即可——**例外要写在表旁边**，而不是混进 26 份复制里。

---

## 三、应修（M）

### M-1 `.vgz` 解压没有上限（健壮性）

`plugin/Source/backends/libvgm/LibvgmVgz.cpp:37-42`

```cpp
	const uint32_t plainSize = DataLoader_GetSize (loader);
	const uint8_t* plainData = DataLoader_GetData (loader);
	if (plainData != nullptr && plainSize > 0)
	{
		juce::MemoryBlock inflated (plainData, (size_t) plainSize);
```

libvgm 的 loader 已经把整个流解压进内存，这里再整体拷一份。压缩比 1:1000 的合法 gzip（几百 KB 的 `.vgz`）就能要求数 GB 分配。

**同类代码已经做对了**：`playback/SessionState.cpp:231` 给自己的 gzip 写了 `// Bounded on purpose: the decompressed size is not known up front and must not be trusted.` 同一条纪律没有延续到这里。

**修**：加一个上限常量（建议 64 MB，VGM 实机最大镜像远小于此），超限返回 `false` 并让上层给出"镜像过大"的错误，而不是让分配失败去崩。

### M-2 RF5C164 的死代码与它的断言还在树里（规则 5）

`plugin/Source/backends/libvgm/Rf5c68Samples.cpp`（418 行）产品路径已不调用；唯一使用者是 `plugin/Tests/ChipDecoderTest.cpp:7497`（`testRf5cSampleCollection`）与 `:7612`（`testRf5cStreamedCopyReachesTheSample`）。

**取舍写清楚**：留它的理由是"这是那一轮模型的记录"，`LibvgmSongInstance.cpp:344` 的注释也解释了。但按规则 5，版本库与 `backup/rf5c-2026-11-14/` 已经保管了它；留在编译单元里则是**第二份关于这颗芯片的说法**，而 §13 说得很直白：同一语义两个写者，迟早分叉。

**修（二选一，别都不做）**：
1. **推荐**：删收集器与两条断言，把模型留在 `docs/research/…post-mortem.md` 与 `tools/rf5c_forensic.py`（后者可执行、可复现，比死代码有用）；
2. 若一定要留：把它们编进一个显式的 `shelved` 目标，`main` 的 CI 不跑，文件名加 `_shelved` 后缀，并在头文件首行说明"不是产品路径"。

### M-3 `PlaybackEngine` 承担了四件事

`plugin/Source/playback/PlaybackEngine.h`（585 行）/ `.cpp`（1987 行）同时做：传输控制、混音（`mixTracks`/`finaliseMasterBus`/`applyDcBlock`）、MIDI 发射（`emitMidiForBlock`/`flushActiveNotesOff`）、工程状态序列化（`writeState`/`readState`）。

MIDI 发射是纯函数——输入（`MidiSequence`、块起点、帧数），输出（`MidiBlockSink` 事件）——它不需要 engine 的任何状态。抽出去之后，"某个轨被静音时该不该发 note-off"这类规则可以在不构引擎的前提下断言（规则 6）。

**修**：`playback/MidiEmitter.{h,cpp}`，engine 只持有它并转发一次。此项可与下一次 MIDI 相关改动合并，不必单独一轮。

### M-4 测试是单一编译单元（6742 行）

`plugin/Tests/ChipDecoderTest.cpp` 6742 行，容纳 31 个解码器与 26 个收集器的全部断言。

代价有三：增量构建每次都要编它；`git blame` 在"这颗芯片谁改的"上没有分辨力；**"哪些芯片没有断言"看不出来**（本项目 §7.5 要求核心逻辑必须有断言，而现状只能靠人记）。

**修**：按芯片族拆成 `ChipDecoderTest.Opn.cpp`、`.Opl.cpp`、`.Sample.cpp`…（或一个 `#include` 清单），并把"每颗芯片至少一条断言"变成可 grep 的事实。

### M-5 26 个收集器各自实现命名、哈希与去重

`hashOf`/FNV 常量在 `*Samples.cpp` 里出现 **36 次**，`SampleFormat` **34 次**；命名规则（`k054539_1A_003`、`segapcm_…`）与"同内容只留一份"的判据在每个文件里各写一遍。

这正是本项目付过四次账的形状（§13）。已经出过一次症状：OPN-ADPCM 那两版"同一记鼓两个文件"。

**修**：一个 `SampleSink`（append + 内容哈希去重 + 命名前缀 + 格式/速率校验），收集器只负责"把字节区间翻译出来"。26 个文件会明显变薄，去重语义也变成一处。

---

## 四、可延后（L）

### L-1 取证工具与产品解析器的重复是**故意的**，但没人写下来

`tools/rf5c_forensic.py`（343 行）、`tools/segapcm_forensic.py`（303 行）各自实现了一遍 VGM 命令解析，与 `backends/libvgm/VgmCommandReader.cpp`（596 行）平行。

这份重复是对的：**取证工具的职责是与产品解析器不一致时才指出问题**。但如果没人写下这句，下一个维护者会"顺手"把它改成复用，工具立刻失去意义。

**修**：两个文件头部各加一行："不共享产品的解析代码——它的价值在于独立实现；两边不一致时，先怀疑产品。"

### L-2 文档表格曾与代码不符

`docs/desgin/libvgm-visualisation.md` §9.1 中 RF5C164 一行在撤下导出后仍写着 `✓ collectRf5cSamples`，本次评审前一提交（`35aa4ed`）才修正。

**修**：评审清单加一条——**表格里出现的符号名（`collect*`、`k*`、文件名）必须 grep 得到**，否则视为文档腐烂。

### L-3 `LibvgmSongInstance.cpp`（1060 行）职责过多

设备映射、IR 构建、样本分发、编辑头信息四件事在一个类里。H-3 修完后，样本分发会变成一个函数，剩下的部分可以按"设备 / IR / 文本"再分一次。这一项不阻塞，等下一次触碰这个文件时顺手做。

---

## 五、达标项（点名保留）

这几处是本次 feature 真正立住的地基，后续功能依赖它们，**不要为了整洁把它们改掉**：

| 项 | 位置 | 为什么值得保留 |
|---|---|---|
| 三端口后端抽象 | `playback/SongBackend.h:24-44` | 从"插件 → PlaybackEngine → ISongBackend → ISongInstance → ITrackPlayer"一路只有值类型；且 `build.ps1:462-472` 有源码守卫，禁止 `Source/backends/` 之外的格式头文件泄漏——**这是 SPC/GSF 能接进来的前提** |
| 实时契约写进头文件 | `SongBackend.h:33-43` | 四条（无锁/无分配、消息线程、返回值语义、不回调）是可测的，不是口号 |
| 解码器只回答"现在像什么" | `chips/IChipDecoder.h:57-65` | 31 个解码器共用一套，音符提取与芯片解耦——这是"加一颗芯片只加一个文件"的真实原因 |
| 快照是拷贝不是视图 | `playback/ChipState.h:11-21` | 音频线程不借状态给界面，理由写在注释里 |
| `wave` 用 `shared_ptr<const>` | `ChipState.h:59-70` | 每帧 × 每通道，拷贝几十 KB 会直接把 UI 拖死；理由也写了 |
| 视图注册表 | `ui/ContentSlot.cpp:82-87` | 加一个视图 = 枚举值 + 表里一行 + 一个 label key；功能 1 的 UI 成本几乎为零 |
| 放弃的纪律 | `docs/desgin/libvgm-visualisation.md` §9.5.2 | 把一个做不到的功能撤下并写清原因，比交付一个"差不多对"的导出更符合规则 0 |

---

## 六、后续三个功能的架构评估

### 6.1 总表

| 功能 | 现有支撑 | 缺口 | 落点 | 量级 |
|---|---|---|---|---|
| 1. 乐器演奏视图 | 视图注册表、`ChannelState` 的 patch 字段、`IChipDecoder` | **没有"音色端口"**：`ITrackPlayer` 只能渲染整首歌，`IChipDecoder` 是只读的；**没有混音注入点** | 新增 `playback/VoiceRig.h` + 后端实现 + `PlaybackEngine` 一路 aux | 中 |
| 2. FM 可视化 | `fmKnown/fmMultiple/fmLevel/fmWaveform/fmFeedback`（`ChipTypes.h:184-204`） | **没有时间轴**：`ChannelState` 是一帧的瞬态，`wave` 是"一个周期" | `chips/ChannelState` 加法；解码器内 trail 环 | 小~中 |
| 3. SPC / GSF | 端口、注册表、`IsolationMode::inPlaceChannelMix`、`renderTracks`、`isAnalysing`，以及 NSF 这个**已跑通的第二后端** | `SongMemory` 无伴随文件；`samples()` 需扫描式实现；每后端 `captureState`；新核心依赖 | 每格式一个 `Source/backends/spc/`、`GSF/` | 大 |

### 6.2 功能 1：乐器演奏视图（FamiStudio 式音色快照 + 演奏）

**要什么**：从正在播放的歌曲里"抓住"某个通道的音色（寄存器级），之后在独立视图里用键盘弹它，实时出声。

**现在有什么**：
- 音色的**显示**字段齐全：`fmMultiple/fmLevel/fmWaveform/fmFeedback`、`patch`、`volume`、`duty`、`sampleRate`（`chips/ChipTypes.h:162-223`）；
- 视图挂载点便宜：`contentViews()` 一行（`ContentSlot.cpp:82-87`）；
- 手上有可直接驱动芯片核心的现成示例：`plugin/Tests/RenderProbe.cpp` 里 `SndEmu_Start2` + `devDef->Update` 已在生产之外的路径上跑通（同一手法在 RF5C 的静音掩码验证里用过：`none=3776 / ch0-muted=0 / all=0`）。

**缺什么（两处，都很明确）**：

1. **没有"音色"这个值类型**。`cms::ChannelState` 是给"画"用的归一化结果，回放一个音色需要的是**寄存器快照**（4 个 operator 的 TL/AR/DR/SR/RR/MUL/DT、算法、反馈、采样起止与循环点……）。给 `IChipDecoder` 加一个默认返回 `false` 的：

   ```cpp
   /** The registers that make this channel's voice, for a caller that wants to replay it later.
       Default false: a decoder that cannot serialise its voice says so instead of inventing one. */
   virtual bool capturePatch (int channel, VoicePatch& out) const { return false; }
   ```

   `VoicePatch` 只有两样东西：`deviceType` 与一串 `(offset, value)`（外加可选的 sample 字节）。**由解码器填写**——它已经是这颗芯片寄存器的唯一权威（规则 13），不要在 UI 里再拼一份。

2. **没有"在歌曲之外渲染一个音"的端口**。`ITrackPlayer` 的全部语义是"一首歌的一轨"。新增：

   ```cpp
   /** One chip voice, playable outside a song: the instrument view's engine. */
   class IVoiceRig
   {
   public:
       virtual ~IVoiceRig (void) = default;
       virtual bool load (const VoicePatch& patch) = 0;       // message thread
       virtual void noteOn (int midiNote, float velocity) = 0; // audio thread
       virtual void noteOff (int midiNote) = 0;
       virtual int render (float* left, float* right, int numSamples) = 0; // real-time contract
   };
   ```

   由 `ISongBackend` 提供工厂（`virtual std::unique_ptr<IVoiceRig> createVoiceRig (DeviceId)`），能力位 `BackendCapabilities::canAuditionVoices` 控制是否显示入口。**这样 SPC/GSF 接入时，音色视图不需要改一行**——它们各自实现 rig 即可。

3. **混音注入点**。`PlaybackEngine::render()` 目前只有"轨道和"这一条路（`mixTracks` / `renderPlainPlayer`）。音色视图的声音必须进同一个输出总线（否则 VST 宿主里听不到），但**不能**经过轨道推子/静音——它是"试听"，不是歌的一部分。建议在 `finaliseMasterBus` 之前加一路 `auditionLeft/auditionRight`，自带一个小推子；一个成员、一处求和。

**结论**：架构**没有为它预留接口**，但每一块地都在——UI 注册表、寄存器权威（解码器）、核心直驱示例、输出总线。工作量集中在"定义端口 + 写一个 libvgm 实现 + 接一路 audio"，不涉及重构。

**建议先做哪一半**：先做 `capturePatch` + 一个只读的"音色卡片"视图（显示快照、可复制成文本），把值类型定稳；出声的那一半随后加。这样即使 rig 拖后，用户也已经能用"看"来确认快照对不对。

### 6.3 功能 2：FM 可视化的"完整音符过程"

**问题本质**：现在的行画的是**一帧**——`channelState()` 返回弧线的四个数（`fmMultiple/fmLevel/fmWaveform/fmFeedback`），`waveform::` 用它们合成一个静态周期（`waveKind = 7`）。没有任何地方记录"这个音符从起音到现在，包络走到了哪里"。

**这不是 FM 特有的问题**。RF5C164 的四次失败是同一个缺口：`ChannelState` 只有"现在"，没有"过程"。当时的应对是"给每个通道加一条时间序列"，但做在了解码器之外（导出的包络分段、行的环），于是出现了两套时间观——这也是那一轮反复对不上的原因之一。

**建议的数据形状**（加在 `chips::ChannelState` 一侧，不动 `playback::` 那一层）：

```cpp
	/** How long the current note has been sounding, in output samples. -1 when unknown.
		A display that wants "the attack" needs this: the chip's registers say what the voice is,
		not how far into it we are. */
	int noteAgeSamples = -1;

	/** Per-operator envelope level (0 = silent, 1 = full), or `known = false` when the decoder
		does not model it. FM only for now; the field is chip-agnostic so a wavetable chip can fill
		it later without changing the panel. */
	struct OperatorLevel { bool known = false; float level = 0.0f; };
	OperatorLevel operators[4];
```

再给 `IChipDecoder` 加一个**可选**的轨迹读取（默认空，和 `capturePatch` 同样用"会不会"而不是"有没有"来表达）：

```cpp
	/** The last `count` samples this channel produced, oldest first, for a scrolling row.
		Default 0: a decoder that cannot produce a trace leaves the row on the cycle picture. */
	virtual std::size_t channelTrace (int channel, float* out, std::size_t count) const { return 0; }
```

**为什么放在解码器里**：OPN/OPM/OPL 的 EG 计算已经在解码器里（`advance()` 每帧跑一次），快照只是把内部状态暴露出来；放进 UI 就要重算一遍包络，等于第二份实现。

**成本**：三个解码器填 `noteAge`/`operators`（各约 40 行），加一条 trail（OPN 已有 `dacWindow` 的现成形状可抄），绘制侧扩展一处（`waveform::`）。**不需要动端口、不需要动导出**。

**同时建议**：顺手把 `fmKnown` 的"未知即不画"约定沿用到底——新的 `operators[i].known` 与 `noteAgeSamples < 0` 都必须是"宁可不画"，否则又会出现"看起来像真的"的默认值（这条在 §4 的设计讨论里已经立过规矩）。

### 6.4 功能 3：SPC / GSF

**已经就绪的证据**（不是猜测）：

| 证据 | 位置 |
|---|---|
| 端口头文件自己写着"an SNES/SPC engine tomorrow" | `SongBackend.h:17-18` |
| 后端注册是表，编译期装配 | `BackendRegistry.h:11-27`、`BuiltInBackends.cpp` |
| 第二个后端已完整跑通（NSF）：`captureState`、`samples()`、`isAnalysing`、自己的 MIDI 扫描 | `backends/nsf/NsfBackend.cpp:49-58`、`NsfPlayer.h:194,258,288`、`NsfPlayer.cpp:410-475` |
| 一次渲染产出 N 轨的通道已存在 | `SongBackend.h:222-236`（`renderTracks`）+ `PlaybackEngine.h:557-562`（`inPlaceMode`） |
| "还在分析"的 UI 约定已存在 | `ISongInstance::isAnalysing`（`SongBackend.h:255-262`），键盘页已实现"analysing"行 |
| 源码守卫禁止格式头文件外泄 | `build.ps1:462-472` |

**真正的缺口（四处，按风险排序）**：

1. **`SongMemory` 只有一个文件。** GSF 依赖伴随库文件（`.gsflib`，多首曲子共用一段 ROM），SPC 不需要但 GSF 必需。现在的结构（`SongBackend.h:55-68`）没有地方放它。建议加：

   ```cpp
   /** Files the song needs besides its own image: the GSF library, a multi-song archive, ... */
   std::vector<juce::File> companions;
   ```

   由加载点（`PlaybackEngine`）填写，后端自己决定怎么用。

2. **`samples()` 对"没有命令流"的格式意味着"扫一遍"。** SPC 的样本在 64 KB APU RAM 里、GBA 的在 ROM 里被 DMA 卷进来，两者都没有"这一段是样本"的声明。可行的路已经存在：NSF 就是**渲染整曲、边渲染边收集**（`NsfPlayer.cpp:410-475`），代价是加载时间——它用后台线程 + `isAnalysing` 化解。SPC/GSF 应照抄这个形状，**不要**在加载路径上同步扫描（本分支 RF5C 的教训：8 次整曲渲染让打开变慢十几秒）。

3. **每后端都要写 `captureState`。** 现在只有 2 个实现（libvgm、nsf），默认返回 `false`。对 SPC 是 DSP 的 8 组 voice 寄存器（volL/volR/pitch/srcn/adsr），对 GBA 是 6 个 SOUND 通道（vol/pan/freq/wave RAM）。端口不需要改，工作量在"把寄存器翻译成 `ChannelState`"——这正是本分支 31 个解码器做过 31 次的事，有大量先例可抄。

4. **混音分轨要走 `inPlaceChannelMix`。** SPC 有 8 个独立音量、GBA 有 6 个，天然可拆。若走 `multiInstance`（本分支 libvgm 的方式），8 个 SPC 模拟各渲染整曲会成倍吃 CPU。`renderTracks()` 与 `inPlaceMode` 的通道已经在（`PlaybackEngine.h:557-562`），但**只有一条路径被验证过**（NSF）——接入第二个格式时应把它当成主路径测一遍。

**另外两点成本，必须提前算**：

- **CPU 核心依赖**：SPC700 与 ARM7TDMI 都不在 `libs/` 里；NSF 用的是 `libs/nsfplay`。每个新格式 = 一个新核心 + 一个新后端，这是这个项目里最大的单笔投入（NSF 那次的规模可以参考：`NsfPlayer.cpp` 1251 行 + 后端若干）。
- **MIDI**：SPC/GSF 没有寄存器流可读，MIDI 只能"渲染一遍再识别"（NSF 的 `NsfPlayer.cpp:410` 就是这么做的）。`BackendCapabilities::canEmitMidi` 已经允许后端说"没有"，所以第一版可以直接关掉它，先把播放/可视化/导出做出来。

**结论**：**架构为 SPC/GSF 预留了正确的接缝**（端口、注册表、能力位、一次多轨渲染、分析中状态、源码守卫），唯一需要提前补的是 `SongMemory` 的伴随文件。剩下的是"每个格式一套后端 + 一个 CPU 核心"的体力活，而不是架构改造。

### 6.5 三个功能的共同建议

1. **先把"值类型"定稳，再写渲染。** `VoicePatch`（功能 1）与 `noteAge/operators`（功能 2）都是纯数据、可序列化、可测试的；先落地它们，UI 与音频可以分别推进，不会互相等。
2. **所有新能力都走"默认关闭 + 能力位"。** `canAuditionVoices`、`canEmitMidi`、`isAnalysing` 已经是这个模式，继续用——否则第二个后端一接进来就会显示一堆它做不到的按钮。
3. **不要在 UI 里重算芯片。** 本分支最贵的四次返工（RF5C）全部源于"界面自己推断芯片在做什么"。功能 2 若在面板里算包络，就是第五次。

---

## 七、检查清单逐条结论（`code-quality.md`）

| 规则 | 结论 | 依据 |
|---|---|---|
| 0 时间充足、不返工 | ⚠️ 部分 | RF5C 五版模型属于探索成本（可接受）；但 M-1/M-5 这类"先这样"的痕迹应现在清 |
| 1 名字说明意图 | ✅ | `kStepPerByte`、`singleChannelMuteOptions`、`rf5cDeviceFor` 等均可读 |
| 2 注释写为什么、只写一次 | ❌ **H-1、H-2** | 两处文档错位/损坏；收集器里同一理由重复多份 |
| 3 高内聚、低耦合 | ⚠️ | UI 不含芯片名（达标）；`LibvgmSongInstance` 四职责（L-3） |
| 4 加东西只改一处 | ❌ **H-3** | 26 份分发复制；视图与后端注册表则达标 |
| 5 不留技术债 | ⚠️ **M-2** | RF5C 死代码 + 断言保留；`.vgz` 无上限属健壮性欠账 |
| 6 纯逻辑可调用 | ✅ | 解码器/收集器/波形合成都传参可调；`MidiEmitter` 是下一步（M-3） |
| 7 颜色走主题 | ✅ | 本分支未引入字面量颜色（`theme::` 唯一来源） |
| 7.5 核心断言 + 界面截图 | ✅ | 54 ctest + 逐芯片截图；但测试集中在一个文件（M-4） |
| 8 提交即最小可读单元 | ✅ | 逐条提交说明写清"为什么、放弃了什么" |
| 9 重构不夹带行为 | ✅ | 回退提交明确写"行为回到搁置态" |
| 10 位域/掩码跨型号验证 | ⚠️ | 单通道隔离只在 RF5C 上验证过；同族（YM2610 联动 SSG）未在本次改动中复验 |
| 11 读状态先问所有权 | ⚠️ | `samples()`/`captureState` 的所有权说明恰好是 H-1 搞错的那段 |
| 12 位宽去实现里读 | ✅ | 大量注释引用被模拟代码的行号（`scd_pcm.c:325-335`、`rf5c68.c:159`） |
| 13 一份数据一个来源 | ❌ **M-2、M-5** | 死收集器是第二份说法；命名/去重分散在 26 个文件 |

---

## 八、建议的修复顺序（每个提交可独立验证）

| # | 内容 | 验收 |
|---|---|---|
| 1 | H-1 + H-2：修 `SongBackend.h` 两处文档 | 读头文件能一遍读懂 `samples` 与 `captureState` 的区别；全文无半词/制表符 |
| 2 | H-3：收集器改表驱动 | `vgm_app_test` 导出的样本数与名字逐字节不变（前后哈希对比） |
| 3 | M-2：删 RF5C 死收集器与两条断言 | `54/54`；RF5C 曲目导出仍为 0 样本（撤下状态保持） |
| 4 | M-1：`.vgz` 解压上限 | 构造 >64 MB 的 gz，得到错误信息而不是崩溃 |
| 5 | M-5：`SampleSink` | 26 个收集器行数下降；导出结果与第 2 步逐字节相同 |
| 6 | M-4：测试按芯片族拆分 | `54/54`；每颗芯片至少一条断言可 grep |
| 7 | M-3：`MidiEmitter` 抽出 | `PlaybackEngineTest` 的 MIDI 用例原样通过 |
| 8 | L-1/L-2：注释与文档纪律 | 工具头注释 + 表格符号可 grep |

前三项是**合并前**必须完成的；4–8 可以随后排。

---

## 九、给"后续功能"的一句话建议

- **功能 1**：先写 `VoicePatch` + `IChipDecoder::capturePatch` + 只读的音色卡片；出声的 `IVoiceRig` 放在第二步。不要在没有值类型之前先接音频。
- **功能 2**：把"时间"加进 `chips::ChannelState`（`noteAgeSamples` + `operators` + `channelTrace`），三个 FM 解码器填它；面板只画收到的数据。
- **功能 3**：现在就可以先做的一件事是给 `SongMemory` 加 `companions`，并把 `inPlaceChannelMix` 这条路径补上第二个格式的验证——SPC 接进来时这两处若无准备，会成为第一批阻塞项。

---

## 十、整改状态（2026-11-14）

合并前必须的三项已全部完成，外加两项应修项；`54/54`、芯片与 NSF 断言全绿，QSound 导出数与改前一致（2 个）。

| # | 项 | 状态 | 验收 |
|---|---|---|---|
| H-1 | `SongBackend.h` captureState/samples 文档错位 | ✅ 已修 | `captureState` 顶着自己的说明；`samples` 顶着自己的三段 |
| H-2 | `SongBackend.h` renderTracks 句子被切坏 | ✅ 已修 | `trackCount`/`numSamples` 恢复，无半词/制表符 |
| H-3 | 26 个收集器分发改表驱动 | ✅ 已修 | `LibvgmSongInstance.cpp` −597/+100；`irHas`/`clockOf`/`rateOf` 收口；OPN/YM2612/NES 作为有理由的例外保留 |
| M-1 | `.vgz` 解压上限 | ✅ 已修 | `LibvgmVgz.cpp` 加 `kMaxImageBytes = 64 MB`，与 `SessionState.cpp` 同一纪律 |
| M-2 | 删 RF5C 死收集器与两条断言 | ✅ 已修 | 删 `Rf5c68Samples.{h,cpp}` + CMake 4 处 + 两条测试 + include；模型留在 `backup/` 与 `docs/research/…post-mortem.md` |
| M-3 | `MidiEmitter` 抽出 | ⏳ 延后（修正判断） | 评审原说"纯函数"，实际 `emitMidiForBlock` 读写 ~10 个引擎成员（cursor、弯音跟踪、通道路由、mixer 状态），**有状态**；抽出是设计改动而非机械提取，留待专门一轮 |
| M-4 | 测试按芯片族拆分 | ⏳ 延后 | `ChipDecoderTest.cpp` 仍 6560 行；拆分是纯 churn 且有破坏绿测试套的风险，宜在加新芯片族时顺手做 |
| M-5 | `SampleSink` 收口 | ✅ 已修（比评审写的小） | 已有 `SampleCollector` 收口去重/命名/格式；只剩 10 份本地 `hashOf`（FNV-1a 同常量）抄写，已收进 `SampleCollector`，删 10 份本地定义 |
| L-1/L-2 | 工具头注释 + 文档纪律 | ✅ L-1 已修 | 两个取证工具头注明"不共享产品解析器"；L-2 作为评审清单项采纳（表格符号必须可 grep） |

**H-3 的取舍说明**：OPN 家族（YM2610/YM2608，两个收集器、两种 ChipKind）与 YM2612 DAC（构建共享的 `dacTable`）没有进表——前者是"两 kind 触发两收集器"，后者是"收集即产生副作用"。两者各留一个显式块并在注释里写明理由，比硬塞进表里更可读。NES DPCM 因签名不同（字节+长度，非 IR）同理。
