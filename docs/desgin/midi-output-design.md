# 设计：混音器每轨 MIDI 输出


- **状态**：基础版已实现（工作区）；引擎侧与窗口侧均已自动化覆盖（`vgm_mixer_test` `[10]`、`app.midi`），待 REAPER 实测；MIDI-BUS 暂不做
- **日期**：2026-10-19
- **分支**：`feature/midi-output`
- **来源**：`docs/research/2026-10-19-midi-output-and-visualisation.md`
- **一句话**：在每个混音通道条的音频输出下拉框下方增加一个 MIDI 通道下拉框，把该轨的演奏事件实时送到插件 MIDI 输出总线；能产生音高的芯片通道发 note + pitch bend，只能 one-shot 的通道发固定 trigger，并用图标提示。

---

## 1. 需求与范围

### 1.1 用户可见行为

- 混音器里每个**芯片通道轨道**的底部，在现有音频输出下拉框下方，增加一个 MIDI 输出通道下拉框；
- 下拉框内容：`Off`、`1` … `16`；**默认全部为 `Off`**；
- 选择 `Off` 表示该轨不输出 MIDI；选择 `1–16` 表示该轨的事件发送到对应 MIDI 通道；
- 对只能 one-shot 的通道（ADPCM-A、ADPCM-B、DAC、noise、部分采样通道），在 MIDI 下拉框旁显示一个**one-shot 图标**，tooltip 说明“只发触发音符，不跟踪音高”；
- 对该芯片暂不支持 MIDI 的通道，下拉框禁用并显示 `Not available`；
- **MIDI 跟随音频可听状态**：轨道被 mute，或处于“其它轨 solo、自己未被 solo”的状态时，该轨 MIDI 输出停止并发送 all-notes-off；重新可听后从当前位置继续。要单纯禁止 MIDI，请把该轨的 MIDI 下拉框设为 `Off`，而不是用 mute/solo；
- **Master 轨道也有 MIDI 下拉框**，默认 `Off`；它输出的是所有音频可听且 MIDI 已启用的轨道合并后的 MIDI 总输出；
- **混音器总开关关闭时不输出任何 MIDI**；轨道/Master 的 MIDI 下拉框保留数值但灰显；
- MIDI 输出是**实时**的：不做 MIDI 文件导出，不做离线渲染；
- **优先 VST3**：插件在支持插件 MIDI 输出的 VST3 宿主中，把 MIDI 输出路由到一条 MIDI 轨并内录；
- 后续可视化直接复用同一套事件序列，不需要再解析一次 VGM。

### 1.2 不在本 feature 内

- MIDI 文件导出；
- MIDI 输入/外部键盘演奏；
- 自动识别采样根音、自动鼓映射编辑；
- 为所有 libvgm 芯片实现 MIDI；
- **linked SSG 独立轨**（YM2610/YM2608/YM2203 的 3 个 SSG 当前没有独立 mixer track）——第一版不做，方案记在附录 A；
- **每轨 MIDI Bus/端口**：VST3 插件只有一个 MIDI 输出事件总线，插件内部不能枚举/选择宿主端口；每轨一个 bus 下拉框需要多个 VST3 事件输出总线，JUCE wrapper 当前不支持，第一版不做；
- 插件端虚拟 MIDI 端口（只在宿主不支持插件 MIDI 输出时作为后备方案另行评估）。

### 1.3 关键约束

- **不能影响音频线程**：MIDI 事件不能加锁、分配、打日志；不能因为 MIDI 输出失败而静音或卡住音频；
- **不能改变现有混音行为**：MIDI 通道设置不改变 gain/pan/输出路由；mute/solo 会按音频可听状态同时影响 MIDI（见 §3.2），这是唯一联动；
- **插件已经声明 MIDI 输出总线**：`NEEDS_MIDI_OUTPUT TRUE`、`producesMidi()` 已经为 true，`processBlock()` 目前清空 MIDI buffer；本 feature 只是把生成的事件放进去，不需要改 CMake 的 MIDI 能力开关。

---

## 2. 可行性结论

**可行，而且插件的 MIDI 输出基础已经在了。**

现有代码：

- `plugin/CMakeLists.txt:166-171` 已设置 `NEEDS_MIDI_INPUT TRUE` 和 `NEEDS_MIDI_OUTPUT TRUE`；
- `PluginProcessor.h:33-37` 的 `acceptsMidi()` / `producesMidi()` 都为 true；
- `PluginProcessor.cpp:110-117` 每个 block 先 `midiMessages.clear()`，注释明确说输出总线是为未来的 MIDI-out 预留的，并防止输入事件回显；
- JUCE 的 VST3 wrapper 在 `JucePlugin_ProducesMidiOutput` 打开时会把 `processBlock` 留下的 MIDI buffer 转成 VST3 `outputEvents`（`juce_audio_plugin_client_VST3.cpp:3733-3735`）；AU/AUv3 wrapper 也支持 `MIDIOutputCallback`（`juce_audio_plugin_client_AU_1.mm:729-744`、`juce_audio_plugin_client_AUv3.mm:1647`）。

因此：
- **不需要**再改 `NEEDS_MIDI_OUTPUT`；
- **不需要**为了 MIDI 输出修改 libvgm 的音频路径；
- 需要做的是：在 `processBlock` 里把事件按 sample offset 填进 `midiMessages`，以及把事件序列、轨道通道映射和 UI 做出来。

**真正的风险不在“能不能发 MIDI”，而在：**
1. 宿主是否支持把插件的 MIDI 输出路由到轨道（VST3/AU 标准支持，但不同 DAW 的暴露方式不同）；
2. 每轨事件提取是否足够准确（尤其 one-shot 采样）；
3. 16 个 MIDI 通道是否够用（YM2610B 正好用满 16 个）；
4. 循环/seek/停止时不能挂音。

---

## 3. 用户交互设计

### 3.1 通道条布局

当前 `ChannelStrip` 的底部顺序是：

```
[名称]
[pan]
[音量推子 / 电平表 / 刻度]
[M] [S]
[音频输出下拉框]   ← 现有
```

新增后：

```
[名称]
[pan]
[音量推子 / 电平表 / 刻度]
[M] [S]
[音频输出下拉框]   ← 现有
[MIDI 通道下拉框] [能力徽标]   ← 新增，位于最底部
```

- `ChannelStrip::preferredHeight` 目前是 `204`（`ChannelStrip.h:29`）；新增一行 26 px + 间距 4 px，建议改为 **234**；
- 通道条宽度仍是 88 px，内部宽度 76 px；MIDI 行里 `ComboBox` 约 44 px，右侧 17 px 给能力徽标（1 px 间距）；
- Master strip 同样增加这一行，但没有能力徽标；它的 MIDI 行同样位于音频输出下拉框下方；
- 图标和下拉框都使用现有的 `ToyboxLookAndFeel` 风格；MIDI 行不新增卡片，只增加一个控件行；
- 这条改动会让混音卡片的最小内容高度增加约 30 px；小窗口下多出的高度由现有 viewport 滚动承担（`MixerPanel::layoutStrips()` 已经按 `preferredHeight` 计算）。

### 3.2 下拉框行为

- 条目：`Off`（id 1）、`Ch.1` … `Ch.16`（id 2–17）；`selectedId - 1` 映射为 0–16，0 = Off，1–16 = MIDI 通道。**文字前缀 `Ch.` 不是装饰**：混音器里一个裸数字 `3` 既可能是音频通道也可能是音色号，加了前缀用户才知道这是 MIDI 通道（键 `mixer.midiChannel`，`Ch.{0}`）；
- 弹窗里每个通道条目左侧画一枚 **MIDI 标志**（环 + 五个触点 + 底部缺口，`drawMidiGlyph`），`Off` 不带——它背后没有 MIDI 通道。`juce::ComboBox::addItem` 只能存文字，所以 `MidiChannelBox` 覆写 `showPopup()`，用 `makeMidiChannelMenu()`（`Source/ui/MidiMenu.h`）自己建菜单，`PopupMenu` 的 `Drawable` 图标接口负责把标志缩放到条目高度；
- **初始值全部为 `Off`**；用户显式选择通道后才输出 MIDI；
- 选择后立即写入 `PlaybackEngine`，不需要“应用”按钮；
- 选择 `Off` 时，若该轨有正在发声的 MIDI note，先发 note-off/all-notes-off，再停止；
- **MIDI 跟随音频可听状态**：轨道 mute，或 solo 状态下自己未被 solo 时，该轨直出 MIDI 停止并发送 all-notes-off，同时从 Master 合并中排除；恢复可听后从当前播放位置继续，不补发不可听期间的音符；
- 用户若要禁止某轨 MIDI，应把该轨 MIDI 设为 `Off`；mute/solo 是“音频可听状态”，`Off` 是“MIDI 不输出”，两者语义不同；
- **Solo 跟随音频**：有 solo 时，只有 solo 轨的 MIDI 直出和 Master 合并保留；取消 solo 后恢复。solo 状态变化会触发一次 all-notes-off + 重新同步；
- 两个轨道选同一个 MIDI 通道时**允许合并**（MIDI 本身就是多源合并的）；tooltip 可以提示“与其它轨共用通道”，不做硬性阻止；
- 音频输出选择 `Master` / `Out n` 不影响 MIDI 通道；MIDI 始终走插件的 MIDI 输出总线；
- **混音器总开关关闭时不输出任何 MIDI**：直出和 Master 都停，先发送 all-notes-off；MIDI 下拉框保留数值但灰显，重新打开混音器后恢复。

### 3.3 Master MIDI

- Master strip 的底部也增加同样的 MIDI 下拉框（`mixer.master.midi`），默认 `Off`；
- Master 的 MIDI 是**所有音频可听、且自身 MIDI 已启用的轨道**的合并信号；
- Master 把合并信号重新发送到 Master 选择的 MIDI 通道；各轨自身的 MIDI 通道仍然保留直出；
- 因此，若某轨既启用了直出、又启用了 Master，同一音符会在两个通道上出现一次（直出一次、Master 合并一次）。这允许用户自由选择“分轨直出”“单轨总谱”或两者都要；Master 默认 `Off`，不想要 Master 时设为 `Off` 即可；
- Master 的 `M`（master mute）同时静音音频和 Master MIDI 合并；轨道 mute 只影响该轨的直出和它在 Master 合并中的贡献；
- Master MIDI 不做能力徽标（它只是一个合并出口，不改变源轨的能力，也不是“旋律”或“触发”）。

### 3.4 MIDI Channel / MIDI Bus / MIDI Port

先把三个词分开：

| 词 | 含义 | 本设计 |
|---|---|---|
| **MIDI Channel** | 1–16，事件里的通道号 | 每轨一个下拉框；已经有 |
| **MIDI Port / Device** | 操作系统里的 MIDI 输出设备（loopMIDI、IAC、硬件端口） | 全局设置；standalone 可选设备；VST3 由宿主路由 |
| **VST3 MIDI Bus** | 插件暴露给宿主的事件输出总线 | 本插件只有一条；宿主把它路由到一个端口/轨道 |

- **VST3 下没有“每轨 MIDI Bus”下拉框**：插件只有一个 MIDI 输出事件总线（JUCE 的 `NEEDS_MIDI_OUTPUT` 创建的就是它）。这个总线由宿主路由，插件内部无法枚举或选择宿主端口；
- 一个插件实例要同时指向多个宿主端口，需要多个 VST3 事件输出总线；JUCE 的 VST3 wrapper 目前只处理 index 0 的输入/输出事件总线，不支持多 bus。自己改 wrapper 会引出跨宿主兼容问题，第一版不做；
- **推荐做法**：每轨选 MIDI Channel；VST3 的端口选择交给 REAPER 的插件 MIDI 输出路由。若要同时去多个端口，加载多个 VST3 实例，或在 REAPER 里对 MIDI 轨做路由；
- **standalone** 才需要全局的 MIDI 设备/端口下拉框（`juce::MidiOutput` 的设备列表），放在 Settings → MIDI 输出子面板，而不是每个通道条；
- 因此，用户提出的“在通道条下再加一个 MIDI-BUS 下拉框”**第一版不实现**；如果以后要做，优先做全局设备选择，不做每轨 bus。
- 如果你说的“MIDI Bus”其实是 **MIDI 通道组**，那每轨的 MIDI 下拉框已经覆盖。

### 3.5 能力徽标（键盘 / 采样波形）

- 徽标说的是“这条通道输出的是哪种 MIDI”，不是“有没有 MIDI”：
  - `midiCapability == melodic` → **钢琴键盘**（`drawMelodyGlyph`）：三白键两黑键，音符带真实音高；
  - `midiCapability == oneShot` → **采样波形**（`drawSampleGlyph`）：一个满幅起音 + 向右衰减的脉冲串，音符只是触发；
- 两者都是 `juce::Path` 矢量绘制（`Source/ui/Icons.cpp`），颜色 `theme::secondaryText()`，在徽标的 17×26 区域里按正方形居中——键盘一旦被拉成长方形就像栅格，而不像键盘；
- 尺寸经验（都按 1× 真实像素截图定过）：15 px 见方时用**三个白键 + 两枚黑键**。四条约束都是踩出来的：
  - 分隔线**贯穿键盘整个高度**（`top`→`bottom`），黑键后画、把它覆盖住：可见部分正好从黑键的底边开始，中间不留空白段——真实键盘上两者本来就共线，只画下半段会留下“中间空了一块”的观感（第一版就是这样被指出来的）；
  - 分隔线要用**与外框同粗细**的笔并**对齐到像素列**（`std::floor (x) + 0.5f`）；1 px 线落在两列之间会被摊成两个半亮灰像素，在 15 px 下等于没有线（最初连“有没有线”都要放大 20 倍才看清）；
  - 黑键要明显比分隔线宽（宽度比 0.20 : 0.06），否则上半段只是“粗一点的线”，整枚图标变成条形码；四个白键三个黑键在 15 px 放不下这个比例，所以只画三个白键；
  - 波形在同样大小下要把振幅拉到 0.5、放慢衰减，否则只剩一条折线；
- 图标不可点击，只提供 tooltip：
  - melodic：EN “Melodic channel: every note carries the pitch the chip plays.” / ZH “旋律通道：每个音符都带芯片实际播放的音高。”
  - oneShot：EN “One-shot channel: MIDI note C4 is a trigger; pitch is not tracked.” / ZH “一次性通道：以 C4 作为触发音符，不跟踪音高。”
- 组件 ID `mixer.midi.<index>.badge`（带轨道索引，理由同 §3.6），方便 AppHarness 断言；
- 若通道不支持 MIDI，不显示徽标（下拉框已显示 `Not available`，再画一个标记只是重复），下拉框禁用。

### 3.6 可访问性/测试 ID

- 每轨 MIDI 下拉框：`mixer.midi.<index>`（如 `mixer.midi.0`、`mixer.midi.3`）；
- 每轨能力徽标：`mixer.midi.<index>.badge`；
- Master MIDI 下拉框：`mixer.master.midi`；Master 不显示能力徽标（其徽标 ID 为 `mixer.master.badge`，始终不可见）；
- **索引是必需的**：组件 ID 必须唯一，否则 `findByID` 只返回树里的第一个匹配（`AppHarnessTest.cpp:114-138`），脚本就只能断言轨 0，无法验证“噪声轨（one-shot）显示徽标”这类每轨差异。音频输出框沿用旧的 `mixer.output`，没有索引（现无脚本按轨断言它）；
- AppHarness 的 `set <componentId> <index>` 已经能驱动 ComboBox（`plugin/Tests/AppHarnessTest.cpp:514-527`），可以直接写脚本验证通道切换。

---

## 4. 数据模型

### 4.1 `TrackInfo` 增加能力位

`plugin/Source/playback/SongBackend.h:67-77` 目前有 `TrackInfo`。建议增加：

```cpp
enum class MidiCapability
{
    unavailable,   // 该芯片/通道暂不支持 MIDI
    melodic,       // 能给出音高：FM / PSG / 方波等
    oneShot        // 只能触发：ADPCM-A/B、DAC、noise、采样通道
};

struct TrackInfo
{
    // ... existing fields ...
    MidiCapability midiCapability = MidiCapability::unavailable;
};
```

`TrackInfo` 是格式无关的，后端负责填。这样 UI 不需要知道 `DEVID_*`，将来 SPC 后端也能用同一套能力位。

### 4.2 事件序列

在 `plugin/Source/playback/` 增加不依赖 libvgm 的类型：

```cpp
enum class MidiEventType { noteOn, noteOff, pitchBend, controlChange };

struct MidiEvent
{
    double timeSeconds;
    MidiEventType type;
    std::uint8_t data1;
    std::uint8_t data2;
    std::uint32_t sourceTrack;   // TrackInfo::id
};

struct MidiSequence
{
    std::vector<MidiEvent> events;   // 按 timeSeconds 排序
    // 可选：每轨的默认 one-shot note、名称等
};
```

`ISongInstance` 增加可选访问：

```cpp
virtual const MidiSequence* getMidiSequence() const { return nullptr; }
```

默认返回 nullptr，不破坏现有后端；`LibvgmSongInstance` 覆盖它。引擎在 `song` 存活期间持有这个指针，序列在加载时构建，之后只读。

### 4.3 每轨 MIDI 通道

`PlaybackEngine::TrackState`（`PlaybackEngine.h:213-223`）增加：

```cpp
std::atomic<int> midiChannel { 0 };   // 0 = Off, 1..16
```

`PlaybackEngine` 增加：

```cpp
void setTrackMidiChannel (size_t trackIndex, int channel);   // clamp 0..16
int getTrackMidiChannel (size_t trackIndex) const;

void setMasterMidiChannel (int channel);                     // clamp 0..16
int getMasterMidiChannel (void) const;
```

通道值是 UI 和音频线程共享的原子量；事件调度读它，不需要锁。

Master 的 MIDI 通道单独存一个 `std::atomic<int> masterMidiChannel { 0 }`，不占 `TrackState` 的数组位置。

### 4.4 持久化

`SessionTrackState`（`SessionState.h:15-23`）增加：

```cpp
int midiChannel = 0;   // 0 = Off, 1..16
```

`SessionState` 增加：

```cpp
int masterMidiChannel = 0;   // 0 = Off, 1..16
```

`writeSessionState()` 在 `<track>` 上写可选的 `midiChannel` 属性，在 `<mixer>` 上写可选的 `masterMidiChannel`；`readSessionState()` 缺省读 0。**不提升 `currentVersion`**：这是可选的加法字段，旧版本会忽略它，新版本读旧工程得到 Off。若以后改变语义，再升版本。

`PlaybackEngine::readState()` 在恢复每轨 gain/pan/mute/solo/output 的同一处恢复 `midiChannel`，并在恢复 master 设置时恢复 `masterMidiChannel`；新歌加载时默认 Off。

---

## 5. 事件提取

### 5.1 为什么放在后端，而不是改 libvgm

推荐第一版在 `LibvgmSongInstance` 里基于 `playerMemory` 构建序列，不改 `libs/libvgm`：

- 序列在加载时构建，不在音频线程；
- 可视化、MIDI 调度、以后的 MIDI 文件导出共享同一份数据；
- 不依赖 libvgm 的私有 `CHIP_DEVICE`/写函数；
- 对 `.vgz` 已经解压好的 `playerMemory` 直接解析，和实际播放的字节一致。

如果以后要覆盖 S98/DRO/GYM，再考虑在 libvgm 的 `PlayerBase` 增加一个“寄存器写 tap”，而不是为每种格式各写一套解析器。第一版不做这个上游改动，因为：

- VGM/VGZ 是项目的主格式；
- 事件序列必须在播放前完整可用（可视化/seek），实时写 tap 只能拿到“已经播过的”事件；
- 上游改动会扩大维护面，且不能解决 one-shot 语义。

### 5.2 解析器职责

建议新增：

```
plugin/Source/backends/libvgm/MidiSequenceBuilder.{h,cpp}
```

职责：

- 读 VGM header：data offset、loop offset、chip clock；
- 遍历命令流：`0x50–0x5F`、`0xA0–0xAF`、`0xB?`（第二芯片）、`0x61/0x62/0x63/0x7n` 等待、`0x80–0x8F`（YM2612 DAC 流，**低 4 位是等待采样数**，字节从 PCM bank 取）、`0x67` 数据块、`0xE0`（PCM bank 寻址）、`0x66` 结束；
- `0x67` 数据块里 type 为 `0x00` 的是 YM2612 的 PCM bank，`0x8n` 按顺序从中取字节写到 `0x2A`——不保留这些 payload，DAC 轨就永远只有音频、没有 MIDI。上层看到的是一条 `0x52`（`0x2A` = 字节）加上它自带的等待两条命令；
- **凡是带等待的命令都必须推进时钟**，包括不产生寄存器写的那几条。`0x8n` 只在 DAC 流里出现，容易在“这是寄存器写”的思路下被跳过：它不推进，MD 曲目的 MIDI 时间就会短一大截（Green Hill Zone 52.9 s → 21.4 s），音符越跑越快、序列提前结束。命令时钟读完之后应当正好等于 header 的 `total_samples`（`VgmCommandReader::timeSamples()`，`vgm_mixer_test` 的 `[11]` 直接比对这两个数）；
- 维护每种芯片的影子寄存器；
- 对支持的芯片调用对应的 `ChipMidiDecoder`；
- 输出 `MidiEvent`，`sourceTrack` 用已经建立的 `TrackInfo::id`；
- 时间统一为秒：`sampleCount / 44100.0`；VGM 的 tick 是 44.1 kHz 采样，不需要 tempo map。

### 5.3 每芯片解码器

第一版只做研究文档里的 A 档，以及明确标记的 one-shot：

| 芯片 | 处理 |
|---|---|
| YM2610 / YM2610B | FM：key on/off + FNUM/BLOCK + TL/pan；ADPCM-A：one-shot trigger；ADPCM-B：DELTA-N → 音高（有 `0x82` ROM 时） |
| YM2612 | 6 FM；DAC 为 one-shot |
| YM2608 | 6 FM；ADPCM-A/B one-shot（SSG 见 §6.3） |
| YM2203 | 3 FM（SSG 见 §6.3） |
| YM2151 | 8 FM |
| YM2413/YM3526/YM3812/YMF262 | FM；rhythm 通道 one-shot |
| AY8910 / SN76489 | tone 为 melodic；noise 为 one-shot |
| NES APU / GB DMG | pulse/triangle/wave melodic；noise/DMC one-shot |
| C6280 / K051649 / SAA1099 / POKEY / WSWAN | 波形/方波 PSG：周期→音高；噪声按速率取鼓键；声音 DMA 与 16 位成对同样由解码器自己处理 |
| 其他 | 暂不产生事件，`midiCapability = unavailable` |

要按「寄存器流」判断的音高，一律走 §14 的两层模型：音符由寄存器算出的移调量决定，采样自身的根音是可选的第二层。**音频不作为音高的来源**，只用于事后核对（§10）。

### 5.4 FM 的 note 生成

以 YM2610 为例：

- `0x28` 的 bit 4–7 是 operator key on/off，低 2 位选通道，bit 2 选高/低三通道组；
- `0xA0–0xA6` + `0xA4–0xA6` 给 FNUM/BLOCK；
- 用芯片时钟、FNUM、BLOCK 算频率，再转成“最近 MIDI note + pitch bend”；
- 频率公式要按芯片的 prescaler 校准，**不要直接复用参考项目的 `/72`**（见研究文档 §3.2）；
- 一个通道从 key-off 到 key-on 发 note-on；key-on 期间 FNUM 变化只更新 pitch bend；note 变化超过 pitch bend 范围时先 note-off 再 note-on；
- 力度用载波 TL/通道音量近似；pan 用 `0xB4` 的 L/R 位发 CC10；
- 同一个通道重复 key-on（没有先 key-off）不应重复发 note-on；用 `keyMask` 记录 operator 位。

### 5.5 one-shot 的 note：不做 GM 鼓映射

第一版**不做 GM 鼓映射**，也不做采样根音分析。所有能确定音高的通道（FM、PSG tone）都输出**原始音高**；one-shot 通道没有可靠音高，统一发送 **C4（MIDI note 60）** 作为触发标识：

| 来源 | note |
|---|---:|
| ADPCM-A 1–6 | 60 |
| ADPCM-B（有 `0x82` ROM 且采样有周期） | 采样自身音高 |
| YM2612 DAC | 60 |
| noise | 60 |
| rhythm（OPLL/OPL） | 60 |

- 这些 note **只是触发标识**，不代表采样本身的真实音高；不要把 60 当成鼓映射，也不要声称它还原了采样音高；
- 用户如果需要在 DAW 里做鼓映射，可以自行加 MIDI 效果器；插件不内置 drum map；
- **ADPCM-B 已按根音处理**（不再是 one-shot）：`DELTA-N` 是真正的音高寄存器，码率 `delta * clock / 65536` 字节/秒（一字节两个 nibble），采样周期从 `0x82` 块解码后测得，音符 = 周期与 `DELTA-N` 共同决定；`0x82` 块缺失或采样测不出周期时退回 C4 触发。电平寄存器写 0 即收音符（驱动就是这么结束声部的）；
- 用户只选 MIDI 通道，不选 note；
- note-off 来源：key-off、采样结束/停止位，或一个默认 gate（例如 80 ms）；三者取先到者；
- 图标和 tooltip 提示用户“这是 one-shot，C4 只是 trigger，不跟踪音高”。

### 5.6 循环与 seek

- 序列只保存**一遍**，并记录 `loopStartSeconds`/`loopEndSeconds`（来自 `SongInfo`）；
- 引擎报的位置是它**真正播放**的位置，和这份序列不是同一个坐标系：N ≥ 2 次循环时位置会一路数过第一遍的结尾，无限循环时位置直接跳回 loop start。两者都要用 `playback::songTimeWithinPass()` 折回“这一遍”里（与 `evaluateSync()` 用的折法一致，fade 尾巴算多一遍）；
- 折回之后，“位置往回跳”与 seek 是同一件事：游标用 `lower_bound` 重新定位、先发 all-notes-off，然后照常发事件。所以循环不需要单独的代码路径，`midiResetGeneration` 也不必为它递增；
- seek/load/stop 时把 `midiResetGeneration` 加一；音频线程看到新 generation 后先结束仍在发声的 note，再把游标定位到新位置；
- “结束”是逐个 note 发它自己的 note-off（引擎记着哪些 note 发过 note-on，见 §6.5），再对仍有 note 的通道补一条 All Notes Off（CC123）。不向 16 个通道广播：没路由过的通道不是本引擎改写的对象；也不发 All Sound Off（CC120），那会砍掉释放尾巴，和“结束音符”是两件事；
- 不折回的后果是：第一遍结束之后游标越过序列末尾，音符全部停止而音频继续播放——循环听不见第二遍。`vgm_mixer_test` 的 `[12]` 在带 loop 点的曲目上数“第一遍之后是否还有 note-on”；
- 仓库里绝大多数资产没有循环点，`[12]` 会打印 `loop check skipped` 跳过——**只有 `loop.vgm` 例外**：它存在的唯一理由就是携带循环点，所以拿不到 loop 点时 `[12]` 必须报错（`loop check DID NOT RUN`），否则改坏资产头部又能让这条检查静默失效（它就是因此空转了一整年）；
- 这样可以避免“循环后旧 note 还按着”和“seek 后挂音”。

---

## 6. 实时 MIDI 调度

### 6.1 插件：走宿主 MIDI 输出总线

这是本 feature 的主路径：

1. `ToyboxAudioProcessor::processBlock()` 开头 `midiMessages.clear()`（现有）；
2. `engine.render(...)` 同时接收一个可选的 `MidiBlockSink*`；
3. 引擎在渲染音频时，按 block 起始位置和 sample rate 扫描 `MidiSequence`，把事件转成 `(sampleOffset, status, data1, data2)` 交给 sink；
4. sink 把事件加到 `midiMessages`；
5. JUCE 的 VST3/AU wrapper 把它们转成宿主可见的 MIDI 输出事件。

伪代码：

```cpp
void ToyboxAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    midiMessages.clear();   // drop incoming events; output bus is ours
    playback::MidiBufferSink sink (midiMessages);
    engine.render (buses, playback::kOutputBusCount, numSamples, &sink);
}
```

- sample offset 用 `round((event.timeSeconds - songTime) * sampleRate)`；
- `blockStartSeconds` 必须是本次 `render()` 真正使用的起点（DAW sync 修正后的起点），不能在外面先读一个旧位置；`songTime = songTimeWithinPass(timeline, blockStartSeconds)` 是同一个起点折回序列那一遍之后的坐标（见 §5.6），扫事件、比时间、算 offset 都用它；
- 引擎在 `render()` 的 try-lock 成功后、渲染音频之前扫描事件，保证和音频同一时间基准；
- `MidiBufferSink` 不分配内存：在 `prepareToPlay()` 里预留最大事件数，`processBlock` 只写；如果宿主 buffer 容量未知，使用成员 `juce::MidiBuffer` + `swapWith`，或至少保证 `ensureSize` 在非音频线程完成；
- 如果 `try-lock` 失败（输出静音），本 block 不发 MIDI；下一个成功 block 通过 generation 重新同步，不补发已错过的 note（避免时间错位）。

### 6.2 Standalone

- standalone 没有宿主 MIDI 输出总线，需要 `juce::MidiOutput` 打开一个设备；
- 第一版可以**不做** standalone 实时输出，只做插件；如果做，建议用独立的高优先级 MIDI 线程，从 lock-free 队列取事件，调用 `sendMessageNow`，不要在音频线程直接打开/查找设备；
- 设备选择放在 Settings 的 Audio/MIDI 页，属于后续工作。

### 6.3 宿主兼容性

- **VST3 是主路径**：JUCE wrapper 支持 output event bus；宿主必须启用/路由它；
- **首选实测宿主是 REAPER（VST3）**；用户自行验证内录流程；
- AU/AUv3 是次要路径：JUCE wrapper 支持 `MIDIOutputCallback`，但 Logic/GarageBand 等宿主是否把 MIDI 输出暴露成可录制的路由，需要实测；
- AAX：当前 JUCE AAX wrapper 没有对应的 `ProducesMidiOutput` 路径，**不承诺**；
- 需要准备一张“宿主 × 格式”测试表（REAPER 优先，其次 Cubase/Nuendo、Bitwig、Studio One、Ableton Live、Logic）；
- 如果宿主不支持，后备方案是 standalone/虚拟 MIDI 端口，但不在本 feature 的第一版。

### 6.4 通道映射与 Master 合并

- 事件序列里存 `sourceTrack`，不预先写死 MIDI 通道；
- 调度时读 `TrackState.midiChannel`：
  - 0 -> 该轨的直出丢弃；
  - 1..16 -> 写到对应通道；
- **Master 合并**：再读 `masterMidiChannel`：
  - 0 -> 不产生 Master 合并输出；
  - 1..16 -> 把所有“音频可听（未 mute，且 solo 状态下自己 solo）且自身 midiChannel != Off”的轨道的同一批事件，复制到 Master 通道；
- 直出和 Master 是两条并行输出：同一轨若两者都启用，会发两次（一次直出、一次 Master 合并）。这是用户自由度的一部分；Master 默认 Off，不想要就关掉；
- 如果轨道通道或 Master 通道在发声过程中被改：先对旧通道发 all-notes-off，再用新通道重新开始；第一版建议立即 all-notes-off，避免挂音；
- 多个轨道共用同一个 MIDI 通道时，事件合并；如果两个轨道同时发同一个 note，note-off 可能互相截断，这是 MIDI 合并的固有问题，UI 可提示。

### 6.5 停止、暂停与挂音

- 引擎在发 note-on 时把 `(通道, note)` 记进一张 16 × 128 位的活动表（`midiActiveNotes`），发 note-off 时清除；Master 合并的那份拷贝是另一个通道上的另一个 note，同样记录。表只在音频线程读写（`emitMidiForBlock()` 写、`flushActiveNotesOff()` 清），不加锁；
- 停止/暂停/播完/卸载时 `render()` 走空闲分支，先把表里剩下的 note 逐个发 note-off（发在它当初的通道上、用它自己的编号），再对有 note 的通道补一条 CC123；表随即清空，所以只有第一个空闲 block 说话，之后每个静音 block 什么都不发；
- 为什么不能只发 CC123：All Notes Off 是“请求”，相当一部分接收端会忽略它——MIDI 监视器里停止后仍亮着的 note 就是这种情况；而对一个被明确告知要发声的 note，它自己的 note-off 是接收端必须执行的消息；
- 为什么逐通道而不是 16 通道广播：note-off 只能落在真正发过 note-on 的通道上，没路由过的通道不是本引擎改写的对象。`PluginProcessor` 里“停止时广播 16 个通道的 CC123/CC120”因此被删掉——既多余，CC120 还会砍掉释放尾巴；
- note-off 事件本身可能被过滤（轨道在 note 期间被 mute、通道被改），活动表是“该发而没发”的兜底：这些 note 会在停止时被正式结束，而不是一直挂在宿主里。

---

## 7. 每芯片能力与 one-shot 标记

### 7.1 后端如何填 `midiCapability`

在 `LibvgmSongInstance::enumerate()` 里，拿到 `PLR_DEV_INFO` 和 `channelNames` 后，按 `device.type` 和 `channelIndex` 问 `midiCapabilityFor()`（`LibvgmSongInstance.cpp`，第一个匹配的 `case` 生效）。一张表比十个散点好核对，范围按通道号：

| 设备类型 | `melodic` | `oneShot` | 备注 |
|---|---|---|---|
| YM2610 | 0–5（非 B 只有 1/2/4/5） | 6–12 | 6–11 ADPCM-A（触发）、12 ADPCM-B（有 `0x82` ROM 时按 DELTA-N 出真实音高，否则 C4 触发） |
| YM2608 | 0–5 | 6–12 | 6–11 ADPCM-A、12 ADPCM-B |
| YM2612 | 0–5 | 6 | Ch6 是 DAC |
| YM2203 | 0–2 | — | SSG 是 linked，不在 track 列表（§7.2） |
| YM2151 | 0–7 | — | |
| YM2413 | 0–8 | 9–13 | 9–13 是 5 个鼓 |
| YM3526 / YM3812 | 0–8 | 9–13 | |
| Y8950 | 0–8 | 9–14 | 14 是 Delta-T |
| YMF262 / YMF278B | 0–17 | 18–22 | YMF278B 只接 FM 半（§11.2 行 22） |
| YMF271 | 0–11 | — | 12 个 group；某组是 FM 还是 PCM 由歌曲写的 sync 模式决定，能力位是静态的，所以按 FM 报，PCM 组由解码器自己发 one-shot 标记 |
| AY8910 | 0–2 | — | noise 没有独立 track |
| SN76496 | 0–2 | 3 | |
| RF5C68 | — | 0–7 | 也覆盖 RF5C164 |
| YMZ280B | — | 0–7 | **搁置**，见 `docs/memo/2026-10-20-ymz280b-shelved.md` |
| X1_010 | 0–15 | — | PCM 模式由解码器自己发 one-shot 标记 |
| NES_APU | 0–2 | 3–4 | noise / DMC |
| GB_DMG | 0–2 | 3 | |
| C6280 | 0–5 | — | 末两通道可切噪声 |
| K051649 | 0–4 | — | SCC1 / SCC+ |
| SAA1099 | 0–5 | — | |
| POKEY | 0–3 | — | 16 位成对时只报低通道 |
| WSWAN | 0–3 | — | 噪声/语音走同一组通道 |
| C140 | 0–23 | — | 采样播放器；音符 = 播放率 / 采样自身周期（§11.2 行 6） |
| C352 | 0–31 | — | 采样播放器；键开是独立的 `0x202` 执行写（§11.2 行 24） |
| K053260 | 0–3 | — | 采样播放器；键开是共享掩码 `0x28` 的上升沿，鼓（无周期采样）由解码器自己发 one-shot 标记（§11.2 行 39） |
| K054539 | 0–7 | — | 采样播放器；采样在 `0x80` 标记处结束，音量是衰减（§11.2 行 40） |
| QSound | 0–18 | — | 采样播放器；没有键开位，rate 与 volume 同时非零才发声（§11.2 行 37） |
| GA20 | 0–3 | — | 采样播放器；地址 16 字节一单位，采样在 `0x00` 处结束，字节率 = `clock/4 / (256-rate)`（§11.2 行 38） |
| SEGAPCM | 0–15 | — | 采样播放器；寄存器即播放状态，控制字节 bit0 清零才发声（§11.2 行 9） |
| ES5503 | 0–31 | — | 波表芯片；音符 = `freq × (clock/8/(n+2)) / 2^(9+res-size)` 再除以表自身的周期（§11.2 行 51） |

范围之外（`channelIndex` 超出上表，或设备类型不在表里）一律 `unavailable`。这张表只说“最多到哪一路”，实际出声与否由 `[13]`（有声轨必须报音符）把关。

### 7.2 linked SSG：第一版不做

- libvgm 把 OPN 的 SSG 作为 **linked AY8910**（`opnintf.c:48-52`）；
- `LibvgmSongInstance` 的 `isTrackDevice()` 只保留 `parentIdx == (UINT32)-1` 的设备（`LibvgmSongInstance.cpp:31-36`），所以 YM2610/YM2608/YM2203 的 SSG **没有自己的 mixer track**；
- 当前 YM2610 的 13 个 track 是 6 FM + 6 ADPCM-A + 1 ADPCM-B，SSG 不在其中；
- **第一版接受这个缺口**：不给 linked SSG 做独立 MIDI 通道，也不在 UI 上伪造轨道；
- 后续可选方案（MIDI-only lane、真正可隔离 track）见 **附录 A**，作为备忘。

### 7.3 非 B 的 FM 静默通道

- YM2610 非 B 实际只输出 `CH[1], CH[2], CH[4], CH[5]`（`fmopn.c:3621-3624`）；
- mixer 仍显示 FM 1–6；其中 FM 1/4 可能是静默的；
- 建议 `midiCapability = unavailable` 给非 B 的 0/3 号 FM（按 libvgm 的 channel index），避免用户选了通道却没有事件；
- YM2610B 全部 6 个 melodic；
- 具体索引以 `TrackInfo::channelIndex` 为准，后端负责映射。

---

## 8. 视觉与布局

### 8.1 尺寸

- `ChannelStrip::preferredHeight`: 204 -> **234**（新增 26 px 行 + 4 px 间距）；
- `kMidiHeight = 26`，`kMidiGap = 4`；
- `resized()` 中从底部依次 `removeFromBottom`：MIDI 行、音频输出行、M/S 行；
- 音频输出行仍然在最上面；MIDI 行在最底部，符合“音频输出下拉框下方”；
- 小窗口下由 MixerPanel 的 viewport 滚动，不压缩推子；`layoutStrips()` 已按 `preferredHeight` 计算最小高度。

### 8.2 控件

- `juce::ComboBox midiBox`，条目 `Off` + `1..16`；轨道的 ID 是 `mixer.midi.<index>`，Master 的 ID 是 `mixer.master.midi`；
- 轨道右侧 `MidiCapabilityBadge`（`juce::Component` 子类，按 `setCapability()` 调 `drawMelodyGlyph` 或 `drawSampleGlyph`，`setTooltip`）；Master 不显示这个 badge；
- 宽度分配：轨道内部约 62 px；combo 44 px，gap 1 px，badge 17 px；Master 没有 badge，combo 用满整行；
- `midiBox` 使用 `ToyboxLookAndFeel` 的 compact combo 属性（和现有输出框一致）；
- 禁用态：`setEnabled(false)`，显示 `Not available`，badge 隐藏；
- 语言切换时只重建 item 文本，保持当前选择；`refreshLanguage()` 里加 MIDI 相关 tooltip；
- `setMixerActive(false)` 时 MIDI 下拉框灰显（值保留）；`setMixerActive(true)` 时恢复。混音器关闭期间不产生任何 MIDI 事件；

### 8.3 i18n key

- `mixer.midiTooltip`
- `mixer.midiOff`
- `mixer.midiUnavailable`
- `mixer.midiOneShotTooltip`
- `mixer.midiMelodyTooltip`
- `mixer.midiChannel`（`Ch.{0}`）

中英文语言包都要加；`plugin/Tests/scripts/language.txt` 的完整性检查会覆盖。

---

## 9. 引擎接口与线程

### 9.1 `PlaybackEngine`

新增：

```cpp
void setTrackMidiChannel (size_t trackIndex, int channel);
int getTrackMidiChannel (size_t trackIndex) const;
playback::MidiCapability getTrackMidiCapability (size_t trackIndex) const;

void setMasterMidiChannel (int channel);
int getMasterMidiChannel (void) const;

/** Optional sink; render() calls it with sample offsets while it holds the render lock. */
void render (const playback::OutputBusBuffers* buses, int numBuses, int numSamples,
             playback::MidiBlockSink* midiSink = nullptr);
```

`TrackState` 增加 `std::atomic<int> midiChannel`；引擎另有 `std::atomic<int> masterMidiChannel`；`LoadedSong` 增加 `const playback::MidiSequence* midiSequence` 和 MIDI 游标状态。

### 9.2 线程规则

- `MidiSequence` 在加载时构建，之后只读；不修改；
- `midiChannel` 是原子量，UI 写、音频线程读；
- `midiResetGeneration` 是原子量，seek/loop/stop、mute/solo 变化、通道变更时递增；音频线程看到变化后发 all-notes-off；
- 音频线程只读序列、读原子量、调 sink；不分配、不加锁、不写日志；
- 事件游标只属于音频线程；消息线程不碰它；
- 如果 `render()` 的 try-lock 失败，本 block 不推进 MIDI 游标；下一成功 block 按 generation/位置重新同步。

### 9.3 all-notes-off

在以下时点发：

- stop / pause；
- 轨道 mute/unmute（只发该轨使用的通道）；
- 轨道 solo 状态变化（全局 all-notes-off + 重新同步）；
- 混音器总开关关闭（停掉全部 MIDI 并 all-notes-off）；
- seek；
- loop 回跳；
- 轨道 MIDI 通道从 A 改到 B；
- Master MIDI 通道变化；
- 歌曲卸载；
- 宿主 transport 跳转（DawSync 的 cue/loop）。

发送内容建议：CC123（All Notes Off）+ CC120（All Sound Off）+ pitch bend center，覆盖该实例用到的通道；不要发到全部 16 通道以外。

**已实现的两处偏离**：center 只在开启弯音时发（关闭时输出里不该有 `0xE0`）；关闭弯音的那一刻，如果引擎发过 `0xE0`，给用到的通道补一次 center，否则上一条颤音会作为永久失谐留着。

### 9.4 pitch bend 范围

14 位弯音值只有配合接收乐器的灵敏度才有意义，而 MIDI 没有查询它的消息。因此：

- 引擎把“每侧半音数”当作输入（1..24，默认 12），用它换算每个弯音值；
- 演奏前（以及通道新投入使用、范围被改动时）向用到的通道发 **RPN 0**（CC101/100=0，CC6=半音数，CC38=0，随后 CC101/100=127 取消选择）——这是 General MIDI 音源读取的消息，支持 RPN 的音源会跟随；不认识 RPN 的音源按自己的固定范围解释，此时唯一能对齐的办法是让用户把两边都设成同一个值；
- 关闭弯音后序列里的 pitch bend 事件被丢弃，音符落在最近的半音上，`0xE0` 一条都不发；
- 设置存 `SettingsStore`（`settings.midi.*`），不进工程状态：它描述的是外部乐器，换工程不该改变它；`ToyboxAudioProcessor` 在构造时和 store 广播时把它交给引擎，这样宿主不打开窗口也能拿到正确的值；
- 播放中修改范围只重发 RPN，不切音：滑块的每一步都切音比短暂的偏差更糟；已经在响的音在各自通道下一次弯音前保持旧值。

---

## 10. 测试计划

### 10.1 单元测试

**已实现**：`PlaybackEngineTest`（`vgm_mixer_test`）`[10] MIDI output`，用真实歌曲走 `MidiSequenceBuilder` + 引擎 + sink，断言的是宿主实际会收到的消息：

- 全 `Off` 时不产生任何 note；
- 第一次渲染先发 CC123 + CC120 × 16 通道（清掉上一段播放的挂音）；
- 轨 0 → 通道 1：只有通道 1 有事件，SN76489 的 440 Hz 解出 MIDI 69（A4）；
- mute 后事件停止（通道值仍保留）；其它轨 solo 时本轨事件同样停止；
- Master 通道复制同样的音符序列；Master 与轨道同通道时不重复发送；Master 静音只停合并那份；
- 混音器总开关关闭时没有任何 note；
- seek 之后重新发 CC123 × 16；
- 第一个音符之前先发 RPN 0（CC101/100=0、CC6=12、CC38=0）；
- 关掉弯音后没有任何真实弯音：`0xE0` 只允许是“关闭那一刻补发”的 center，且最多每通道一条；
- 一个 block 内的事件按 sampleOffset 单调。

**尚未自动化**（需要每颗芯片一份合成 VGM，成本较高，暂靠 REAPER 手工验证）：YM2610 的 key-on/off、FNUM/BLOCK、pan、ADPCM-A trigger，非 B FM 通道映射（1/2/4/5）与 B 的 6 通道，one-shot 的 C4 trigger 与 gate，时钟换算（各芯片 master clock / 分频）。

这些用例只回答“解码器是否和自己写下的常量一致”；常量本身对不对，由 §10.2 回答。

### 10.2 期望值来自芯片（`vgm_opn_test`）

`vgm_chip_test` 里的 FM 用例把寄存器含义写成常量——这正是 `0x40-0x4F` 那处 bug 能活下来的原因：用例和解码器出自同一个错误读法（`0x43` 当“通道 0 的算子 4”），两边互相印证，一起和硬件不符。`plugin/Tests/OpnCoreOracleTest.cpp`（`vgm_opn_test`）把期望值换成硬件：libvgm 自己的 YM2612 核播放同一串寄存器写，用核的静音掩码一次只放一个通道并测 RMS，解码器的 `gate` 必须和“这个通道有没有声音”一致。

- 两个地址被直接点名。写 `0x43`：芯片不出声，解码器也不许出声（旧写法会把它记成通道 0 的算子 4，凭空多一个音）。写 `0x4C`：芯片出声，解码器必须承认（旧写法把它记到通道 3，真正的载波停在 `0x7F`——FM5 有音频没音符的那个 bug）。
- 8 种算法 × 4 个“只开一个算子” × 6 个通道 = 192 次比较，端口 1 的偏移在每一次里都被走过；再叠 32 个随机 patch（同一次里各通道算法/音量都不同，写错通道才会暴露），共 384 次，其中约 220 次真的有声。
- 只比 `gate`，不比音符：gate 是解码器对“有没有声音”的断言，音符提取器只决定这个断言活多久；gate 错了，音符一定错。
- 它已被验证抓得住这个 bug：把 `totalLevel` 的位序改回旧写法，`vgm_opn_test` 立刻变红（`0x43` 幽灵音 + 每个“算子 4 是载波”的用例都报“芯片 rms 5778.5，解码器 gate off”），改回来即绿。
- 它链接 libvgm（不是“只测 chips”的目标），运行 0.2 s，所以默认不构建、但进 CTest：`ctest -R opn`。

### 10.3 窗口脚本

`plugin/Tests/scripts/midi.txt`（CMake 自动注册为 `app.midi`，测试曲目同 `load-and-play.txt`）：

- `dump mixer.midi.0` / `dump mixer.master.midi` 能打印这两个控件；
- `set mixer.midi.0 0` -> `Off`；`set mixer.midi.0 1` -> `Ch.1`；`set mixer.midi.0 16` -> `Ch.16`；
- `set mixer.master.midi 3` -> Master `Ch.3`；
- `minimal.vgm` 是单颗 SN76489：轨 0–2 可旋律化，轨 3 是噪声（one-shot）——`mixer.midi.3.badge visible == true`、`mixer.midi.0.badge visible == true`（键盘与波形都显示，只是形状不同），Master 没有徽标（`mixer.master.badge visible == false`）；
- 不支持轨的 combo disabled（测试曲目里没有这种轨，用 `VGM_TEST_SONG` 指向含 S98/DRO 的文件时才会出现；代码路径见 `LibvgmSongInstance.cpp` 的 `midiCapabilityFor`）；
- mute/solo 不改下拉框的选择值（值归引擎所有，条子只显示）——脚本只能驱动有 componentID 的控件，条子里的 M/S 按钮没有 ID，因此这一条由 `PlaybackEngineTest` `[10]` 覆盖；
- `click mixer.enable` 关掉混音器后 `mixer.midi.0 enabled == false`、`mixer.master.midi enabled == false`（Master 的输出选择框仍为 true——它的路由在两种模式下都有效），值保留；再点开恢复；
- **`click` 之后必须跟 `sleep`**：`Button::triggerClick()` 是投递的，`settle()` 只睡不泵消息循环，脚本会看不出任何变化（本轮就踩到了：断言拿到 `enabled == true`）。自翻转按钮由 harness 直接改状态，所以 `chip.0` 那些脚本不需要 `sleep`；
- 语言切换后 item 文本更新，选择不变（key 的完整性由 i18n 检查覆盖；harness 没有切换语言的命令，这一条靠手工/`language` 相关脚本，选择值本身存在引擎里，切语言只重建条目文本）；
- 状态保存/恢复后 `midiChannel` / `masterMidiChannel` 一致（`vgm_core_test` 已覆盖，见 `PlaybackCoreTest.cpp`）。

`plugin/Tests/scripts/settings-pages.txt` 覆盖设置页 *Settings* → *MIDI*：树第 4 行是它、开关和滑块的初始值、关掉弯音后范围变灰但保留数值，以及 **`assert engine bend*`**——页面只写设置文件，真正的值要经过 store 广播到处理器的监听器再进引擎，断在页面上会漏掉“监听器没接上”这种错误。

`AppHarnessTest` 的 `click`/`toggle` 命令在修复前只 `triggerClick()` 再 `settle()`：`triggerClick()` 只是投递，自翻转按钮在投递的处理器里改状态并直接返回、不调 `onClick`，所以开关看起来按下了、值从未写进 store（实测：`settings.midi.bend` 一直是 true）。现在命令投递后跑 `runDispatchLoopUntil()`，脚本里的开关才真的生效。

### 10.4 宿主集成

- 在 REAPER 中加载 VST3，启用插件 MIDI 输出，路由到 MIDI 轨，录音（用户实测）；
- 验证 4 个 FM 通道、ADPCM-A trigger、ADPCM-B trigger 的时间/通道；
- 验证 mute、solo、Master 合并和 Master Off；
- stop/seek/loop 后无挂音；
- 如果宿主不支持插件 MIDI 输出，记录到兼容性表，暂不实现虚拟端口。

### 10.4 回归

- `ctest` 全绿；
- 音频输出路由、mute/solo、混音器布局截图无回归；
- `preferredHeight` 增加后，小窗口滚动正常。

---

## 11. 实施阶段

| 阶段 | 内容 |
|---|---|
| P1 | `MidiCapability`/`MidiSequence`/`TrackInfo` 扩展；`PlaybackEngine` 每轨 + Master MIDI 通道原子量与持久化 |
| P2 | `MidiSequenceBuilder` + YM2610/YM2612/YM2151/AY/SN 解码器；单元测试 |
| P3 | `render()` 的 MIDI sink；`processBlock` 填充 `midiMessages`；VST3 宿主内录验证 |
| P4 | `ChannelStrip` 轨道/Master MIDI combo + one-shot badge + i18n + AppHarness 脚本 |
| P5 | 更多芯片（覆盖面目标与清单见 §11.2）、ADPCM-B 根音、standalone/virtual port、可视化复用；Settings → MIDI 输出子面板 |

第一版不做：S98/DRO/GYM、linked SSG 独立轨、MIDI 文件、虚拟端口、自动鼓映射。

### 11.1 后续：Settings → MIDI 输出子面板

**已落地（P3，见 §14.5）**：MIDI 页上已有「按采样根音移调」开关（`pitch.useSampleRoot`，默认关）与其说明文字——那是音高模型的两层之间的开关，也是这一页的第一项。

后续在设置面板增加一个 **MIDI 输出** 子页，集中放可配置项，不把这些控件塞进通道条：

- one-shot 通道的 trigger note 映射（ADPCM-A/B、DAC、noise；默认 C4，允许改 note）；
- ADPCM-B 的根音/采样率覆盖，或切换为 melodic；
- **每个 mixer 通道的 MIDI 分析策略**：寄存器流（现状）或音频频谱（对“流式播放的采样/整曲一个 sample”这类寄存器流无音符的通道）——现在不做，方案与待定项见 `docs/memo/2026-10-21-per-channel-midi-analysis-strategy.md`；
- 每轨默认 MIDI 通道 / 自动分配策略；
- standalone / 虚拟端口的 MIDI 设备选择（VST3 不需要）；
- Master 合并的默认开关/默认通道；
- 可能还有“MIDI 输出通道冲突”提示样式。

第一版只做通道下拉和固定 C4 trigger（不做 GM 鼓映射）；这些设置项属于 P5 之后，不阻塞实时 MIDI 输出。

### 11.2 芯片覆盖：现状与目标

**目标**：MIDI 输出的芯片覆盖面最终不低于参考项目 MDPlayer-stable 的 `EnmChip` 主表——**54 项**（`reference/MDPlayer-stable/MDPlayer/MDPlayerx64/common.cs`；`S_*` 是同一芯片的第二实例，不单列）。

判据是「能出 MIDI 事件」而不是「音频能出声」：每一项要么旋律化（note-on + 弯音），要么 one-shot（C4 触发 + note-off）。只有音频、没有事件的通道不计入。

**现状（2026-10）**

| 口径 | 数量 |
|---|---:|
| 音频核心（libvgm `DEVID_*`，全部可播） | 49 |
| MIDI 设备类型（`midiCapabilityFor` 的 `case` 数，含已搁置的 YMZ280B） | 31 |
| 上表对应的 MDPlayer 主表条目 | 33 / 54 |
| 有音频核心、缺解码器 | 5（另有 YMZ280B：解码器已写但搁置） |
| 连音频核心都没有 | 15 |
| libvgm 有、MDPlayer 没有（不在对齐范围内） | 13 |

「上表对应条目」的口径固定为：下面 54 行明细里 MIDI 现状既不是「—」也不是「✗」的行数。第 3 行（YM2612Ch6）与第 18 行（DMC）借用父行的核心，但各自要占一条 MIDI 通道，所以也计入；YMZ280B（第 36 行）标「搁置」，不计入。

**54 项明细**（「MIDI 现状」见 §7.1；「—」= 有核心没解码器，「✗」= 无核心）

| # | MDPlayer `EnmChip` | libvgm 核心 | MIDI 现状 | 说明 / 下一步 |
|---:|---|---|---|---|
| 1 | SN76489 | SN76496 | 旋律 1–3 + 噪声触发 | |
| 2 | YM2612 | YM2612 | 旋律 1–6 + Ch6 DAC 触发 | |
| 3 | YM2612Ch6 | （同 2，通道） | 触发 | 不是独立核心 |
| 4 | RF5C164 | RF5C68 | 触发 ×8 | 16 位变体，同一核心 |
| 5 | PWM | 32X_PWM | **不支持**（无音高） | 流式 DAC：`0xB2` 写的左右/单声道输出寄存器就是音频本身，`cycle` 设的是 DAC 采样率而不是音高，不存在“音符”这个事件。2026-10-22 决定不做 |
| 6 | C140 | C140 | 旋律 ×24 | 采样播放器，有音高。频率寄存器是播放率：`frequency * clock / 288 / 65536` 字节每秒。**音符由采样自身周期决定**：键开时在 loop 区（无 loop 则前 2 KB）做自相关，取「值 ≥ 最强局部峰 −0.02 的最短 lag」再按「loop 是整数个周期」吸附，得到该采样一个周期的字节数，音符 = `播放率 / 周期`（`chips/C140Decoder.h`）。周期测不出来的（鼓/噪声、μ-law、无 ROM）才退回 `chips::quantisePlaybackRate()` 的 8 kHz 根音（根音是参数，§11.1；时钟翻倍即八度）。ROM 来自 `0x67` 的 `0x8D` 块（`MidiSequenceBuilder::assembleRom()` 按各块自带的 ROM 偏移拼），寻址按头字节 `0x96` 的 banking 折叠——System 2 是 `((adrs&0x200000)>>2)|(adrs&0x7ffff)`，**不折叠的话 1 MB ROM 里根本没有 bank 0x14，所有音符会静默退回根音**。结束地址（`0x18/19`）决定音符长度，键开时与 `0x1A/1B` 一起锁存——核心就是这么复制 start/end 的，所以中途改结束地址只影响下一个键。loop 位（`0x15` 的 bit 4）让指针回卷，越过结束地址也不停，只有写 mode 去掉 bit 7（驱动的 key-off）能结束。`0xD4` 是 16 位大端偏移，高字节落在 `port` 里，所以 `0x100` 以上的声部要靠两半拼回；`c140.vgm` 用 voice 0/16 差八度、两种结束方式各 4 个音符，`[13]` 实测 2 轨各 4 音符、rms 0.17 |
| 7 | OKIM6258 | MSM6258 | **不支持**（无音高） | 流式 DAC：`0x01` 是数据口，`0x0C` 只是采样率档位，既无起止地址也无样本号。2026-10-22 决定不做 |
| 8 | OKIM6295 | MSM6295 | 旋律 ×4（鼓为触发） | 采样播放器，**无音高寄存器**：固定 `clock/165`，音高只能来自采样自身的周期（§14.3），标 `sampleInferred`；寄存器是两字节命令端口 `0x00`（bit7 选样本、下一字节 bits 4-7 启动、低 4 位为衰减音量，0 最响），起止地址在 ROM 的 `sample*8` 处各 3 字节 → 长度精确。命令 `0xB8`，ROM 为 `0x8B` 块（256 KB）。八首真实曲目全部通过 `[13]` |
| 9 | SEGAPCM | SEGAPCM | 旋律 ×16（鼓为触发） | 采样播放器（Sega System 16），M4 第七颗。16 路 8-bit PCM，**寄存器就是播放状态**：`8n+2/3` 左右音量、`8n+4/5` 循环地址（256 字节单位）、`8n+6` 结束地址（核心比 `addr>>16`，所以停在 `(reg+1)*256`）、`8n+7` 步进、`0x80+8n+4/5` 当前地址（24 位位置的 bit8-15/16-23，低 8 位是芯片自己的小数部分）、`0x80+8n+6` 控制（**bit0 清零才发声**、bit1 关循环、其余是 bank）。速率 = `pitch * clock / 32768` 字节/秒（核心按 `clock/128` 出样本，一个步进是 1/256 字节），音符仍由采样自身周期决定。命令是 `0xC0 <offset lo> <offset hi> <value>`，ROM 走 `0x80` 块。两处与前面几颗不同、各有单测：①**没有独立的键开寄存器**——驱动先写地址/音量/结束/步进，再把控制字节的 bit0 清 0，解码器只在这个「从关到开」的跳变起音符；②**结束地址是 256 字节单位**，`(reg+1)*256`，循环声部只由控制位收尾。**测量**：周期从采样**四分之一处**开始测（Slap Bass 的开头是 slap 本身的噪声，直接测会得到 0），窗口 2 KB。**真实曲目**：`test_file/SEGAPCM/03 Scene Select.vgz`（Galaxy Force II，19.2 秒、104 次键开、7 个 `0x80` 块、时钟 4,026,987）104 次键开里 **39 次**测出音高：Ch 3 是 **E1(28)** 的 slap bass（start 0x0000、pitch 128、周期 388 字节），同一采样在 Ch 5 用 pitch 160 得到 **G#1(32)**（+4 半音，与 160/128 完全吻合），Ch 4 是另一个采样上的 G#2；其余通道（Ch 1/2/7/8）是鼓，全部 C4 触发 |
| 10 | YM2151 | YM2151 | 旋律 1–8 | |
| 11 | YM2608 | YM2608 | 旋律 1–6 + ADPCM 触发 | |
| 12 | YM2203 | YM2203 | 旋律 1–3 | SSG 未接（§7.2） |
| 13 | YM2610 | YM2610 | 旋律（B：1–6）+ ADPCM 触发 | 非 B 只有 CH1/2/4/5 |
| 14 | AY8910 | AY8910 | 旋律 1–3 | |
| 15 | HuC6280 | C6280 | 旋律 1–6 + 噪声触发 | Ch5/6 可切噪声 |
| 16 | YM2413 | YM2413 | 旋律 1–9 + 5 鼓触发 | |
| 17 | NES | NES_APU | 旋律 1–3 + 噪声/DMC 触发 | |
| 18 | DMC | （同 17，通道） | 触发 | 不是独立核心 |
| 19 | FDS | ✗ | — | libvgm 无核心 |
| 20 | MMC5 | ✗ | — | libvgm 无核心 |
| 21 | YMF262 | YMF262 | 旋律 1–18 + 5 鼓触发 | |
| 22 | YMF278B | YMF278B | 旋律 1–18 + 5 鼓触发（仅 FM） | FM 半复用 OPL3 解码器（`0xD0`，端口 0/1）；wavetable 半（端口 2–5）无解码器，将来只能 one-shot |
| 23 | VRC7 | ✗ | — | OPLL 变体，有核心后可复用 YM2413 解码器 |
| 24 | C352 | C352 | 旋律 ×32 | 采样播放器，有音高，C140 的近亲。频率寄存器同样是播放率（`frequency * clock / (288 * 65536)`，与 C140 同一个除数），音符同样由采样自身周期决定（`chips::estimateCycleLength()`，与 C140 共用）。三处不同各有一个用例：①**键开是独立的写**——flag 里的 `0x4000` 不起作用，offset `0x202` 的执行写才启动所有请求过的声部（键关同理：`0x2000` 加执行写），执行后请求位被清掉，第二次执行写不再触发；②**寄存器是 16 位整字**，`0xE1` 一次带 16 位大端 offset（bit 15 选第二片）与 16 位大端值，读取器把值的高/低字节放进 `RegisterWrite::value`/`value2`；③**采样地址是 `(bank << 16) | start`** 并按 ROM 容量（2 的幂）取掩码，所以 bank 1 是同一偏移再向里 64 KB。结束地址结束不循环的声部；置 loop 位（`0x0002`）后只有键关能停。μ-law 采样先按 `c352.c` 的表解码再测周期；测不出周期的退回 `chips::quantisePlaybackRate()`，根音取芯片自身码率 `clock / 576`（0x8000 即中央 C，寄存器顶高一个八度）。ROM 来自 `0x67` 的 `0x92` 块（`assembleRom(reader, 0x92, 32 MB)`）；头部时钟按 libvgm 的读法 `hdr[0xDC] * 72 / hdr[0xD6]`。`c352.vgm` 用一次执行写同时键开两个声部（voice 0 靠结束地址、voice 1（循环、采样在 bank 1）靠键关），`[13]` 实测两轨各 4 个音符、A4/A5、rms 0.21。**真实曲目**：`test_file/c352` 的两首 Galaga 小曲（Ship Captured / Ship Recovered，头部时钟 24 576 000、divider 72）实测 3 轨全部有声且音符对齐（Ch 1/2/3 = 21/20/12 个音符），并暴露了一个生成资产抓不到的问题——**整首曲子靠一个 16 字节的单周期波表**（`wave_start` 0x23A、`wave_end` 0x249、loop 回 0x23A、ROM 块只传 16 字节载荷），比自相关窗口还短，测不出周期就退回根音，于是全曲低了 40 个半音（Ch 1 报 36，实际是 E5）。修法：loop 短于 256 字节（`kSingleCycleBytes`）且测不出周期时，把 loop 长度当作一个周期；同时 `wave_end` 是**闭区间**（核心先取样再判断结束，`c352.c` 的 `C352_fetch_sample`），区域与 loop 都要 +1 字节——16 与 15 字节差一个半音。修复后两首曲子是同一段下行三度旋律的 C 大调与 A 大调版本（Ch1 76/72/74/71/72/69…），单测 `a one-cycle wavetable loop of sixteen bytes at 7040 a second is A4` 钉住这条规则 |
| 25 | YM3526 | YM3526 | 旋律 1–9 + 5 鼓触发 | |
| 26 | Y8950 | Y8950 | 旋律 1–9 + 5 鼓/Delta-T 触发 | |
| 27 | YM3812 | YM3812 | 旋律 1–9 + 5 鼓触发 | |
| 28 | K051649 | K051649 | 旋律 1–5 | SCC1 / SCC+ |
| 29 | N163 | ✗ | — | libvgm 无核心 |
| 30 | VRC6 | ✗ | — | 有核心后脉冲部分可复用 NES 解码器 |
| 31 | FME7 | ✗ | — | AY 同构，有核心后可直接复用 AY 解码器 |
| 32 | RF5C68 | RF5C68 | 触发 ×8 | |
| 33 | MultiPCM | ✗ | — | libvgm 无核心 |
| 34 | uPD7759 | uPD7759 | **不支持**（无音高） | 语音合成器：start 上升沿 + 样本号是真实事件，但整颗芯片没有音高，一个 C4 只会把语音当乐器。2026-10-22 决定不做 |
| 35 | YMF271 | YMF271 | 旋律 ×12 | 4 算子 FM，12 个 group × 4 个 bank，4 种 sync 模式（0 = 四算子一组，写 bank 0 即复制到其余三个；1 = 两两成对；3 = 四路独立 PCM）。VGM 命令是 `0xD1`（端口字节 bit 7 是芯片实例、低 7 位是 bank），读取器把 bank 留在 `port` 里传下来——忽略它就会漏掉「只有 bank 0 的写会传播」这条。地址低四位是 group，3/7/11/15 是空洞。核心在 ROM 未分配时连 FM 组也不发声，所以资产要带 `0x67` type `0x85` 的 ROM 块；`opx.vgm` 用 group 0/1 各出 A4/A5，`[13]` 实测 Ch1/Ch2 各 4 个音符、rms 0.318 |
| 36 | YMZ280B | YMZ280B | 触发 ×8（**搁置**） | 采样播放器。解码器已写但**不计入覆盖、不进 ctest**：vgmrips 上这颗芯片的成套录音实际是“一条长样本、一路发声”，没有可断言的每声部音符。见 `docs/memo/2026-10-20-ymz280b-shelved.md` |
| 37 | QSound | QSOUND | 旋律 ×19（鼓为触发） | 采样播放器（Capcom DL-1425），有音高 + 回声，M4 第五颗。16 路 8 位 PCM + 3 路 4 位 ADPCM。频率是 4.12 定点步进：每个输出样本把 `rate` 加到 28 位位置上，所以字节率 = `rate * clock / 10223616`（输出率 `clock / 2 / 1248`；VGM 头部写的老 4 MHz 时钟在 libvgm 里 ×15 成 60 MHz，解码器两种写法都接受）。音符仍由采样自身周期决定（`chips::estimateCycleLength()`）。三处与前面几颗不同、各有用例：①**没有任何键开位**——声部只要 rate 和 volume 都非零就在发声，驱动先写 start/end/volume、最后写 rate 起音，写 rate=0 或 volume=0 停音；解码器按这个上升沿起音符、按同一条件关门；②**bank 寄存器属于下一个声部**（核心 `init_register_map()`：`(n<<3)+0` 写的是 `voice[(n+1)%16].bank`）；③**loop 是长度不是地址**——位置到 `end` 时 `new_phase -= loop_len<<12`，所以循环区是 `end-loop_len .. end`，`loop_len = 0` 表示不循环、到 `end` 结束。另外：`end` 是 int16 但地址无符号，采样跨 64 KB bank 边界（`end < start`）时要按回绕算；pan 不是静音（任何 pan 都出一侧）；ADPCM 三路固定 8 kHz，由 `0xD6+n` 写非零启动、到自己的 end 地址停。ROM 走 `0x67` 的 `0x8F` 块；命令是 `0xC4`（16 位大端**值在前**，偏移一个字节）。**打击乐**：SF2 的鼓组占同几路声部，测不出周期（或高于 C8）报 C4 触发。`qsound.vgm` 两轨各 4 个音符（A4 与 C4 触发，靠 rate 写起停），`engine.qsound` 通过。**真实曲目**：`test_file/QSound/35 Guile Stage I.vgz`（79.5 秒、446 KB、15 个 `0x8F` 块、26,609 条 QSound 写入、ADPCM 未使用）19 条轨中 11 条有声且全部与音频一致：PCM 2 是贝斯（37/25/42/44…）、PCM 4/5/6 是同一采样的三音和弦（64/61/56）、PCM 7–14 是鼓（全 C4 触发）；并修掉了「`end` 按有符号比较导致跨 bank 采样测不出周期」与「循环声部 gate 立刻关闭」两个 bug。**Logo 曲目**（`test_file/QSound/01 QSound Logo.vgz`，9.4 秒、6,566 条写入）暴露了另外两个：①**ROM 上限写死 4 MB**——芯片用 15 位 bank 寻址，实际可到 **8 MB**，这首曲子的采样在 `0x520000`/`0x640000`，被截断后读到的全是 0，于是每个声部都测不出周期、整首曲子只剩 C4 触发（builder 现已改成 8 MB，与 libvgm 的 ROM 声明一致）；②**每个非零 rate 写入都当成键开**——驱动在一个持续音里改写 rate 九次做颤音/滑音，旧代码把每个音符炸成 5–36 个触发音；现在只在「从静音到发声」的上升沿起音，播放中改 rate 视为滑音（音高仍按新 rate 报，音高真正变化时由抽取器切开）。修完之后同一采样的 rate 比例与音符差**精确吻合**（4576→74、4864→75、5440→77，即 +1 与 +3 个半音；6496/4864→41 即 +5），10 条有声轨全部与音频一致 |
| 38 | GA20 | GA20 | 旋律 ×4（鼓为触发） | 采样播放器（Irem/Nanao），M4 第六颗。4 路 8-bit PCM，寄存器 = `通道 * 8 + n`：0/1 起始地址、2/3 结束地址（核心从不读）、4 速率、5 音量、6 控制（bit1 键控）。三处与前面几颗不同、各有单测：①**地址单位是 16 字节**（`start = ((reg1<<8)\|reg0) << 4`，`irem_ga20_w`）；②**采样在 ROM 的 `0x00` 字节处结束**（`irem_ga20_cache_samples` 的终止符，没有 loop，end 寄存器形同虚设）；③**速率是分频，但分母是芯片自己的时钟**：计数器每 `256-rate` 个样本进一个字节，而核心的样本率是 `clock / 4`（`iremga20.c` 把 `cfg->clock / 4` 交给 `INIT_DEVINF`，libvgm 之后再降采样到输出率），所以字节率 = `clock / 4 / (256-rate)`，**芯片时钟必须参与**。头部 0xE0（`_CHIPCLK_OFS` 里 `DEVID_GA20` 那一格）这首是 3,579,545 Hz → 894,886 字节钟，rate 131 ≈ 7,159 B/s。旧代码用输出率 44,100 当字节钟（353 B/s），等于把每个音高都压低 20 倍、全部落到 MIDI 0 号音以下——而那不是音符，于是**所有曲子、所有声部都退化成 C4 触发**。音量是双曲的 `data*256/(data+10)`。**测量**：采样没有 loop，而且这些采样是**滑音**——Stage 3 的贝斯在一个音里从 175 字节周期滑到 251 字节，用整段一个大窗平均之后什么都测不到；所以解码器从采样开头以 512 字节窗、半窗步进扫描，取**第一个**测出周期的窗（那是驱动键开时的音高），判据仍是「先有 ≤ -0.5 的凹陷、再有 ≥ 0.5 的峰」，C8 以上视为测量伪影。窗只有 512 字节，也因此把可测周期上限压到 256 字节——对这套字节率够用（163 字节已经是 105 Hz）。另一个坑：定位周期必须从**凹陷之后**开始扫，否则光滑波形在 lag 2/3 的连续相关会被当成 2 字节周期（这个 bug 让 `0x63320` 读出 2 字节、`0x6D3C0` 读出 36 字节，音符全错）。**打击乐**：这套板子的鼓和旋律共用 4 路声部，判定按每次键开所锁存的采样做，所以同一通道可以既有鼓又有旋律。**真实曲目**：`test_file/GA20/11 Redtailed Cat (Stage 3).vgz`（Undercover Cops Stage 3，120 秒、8,333 次键开、7 个采样、GA20 时钟 3,579,545）。修好字节钟之后，同一批寄存器给出的是真音高：`0x63320` 测 163 字节 → rate 204 下 **105.6 Hz（G#2, 44）**，`0x6D3C0` 测 146 字节 → 44–51 的贝斯线，`0x733D0` 9 字节 → 795 Hz 附近的 G5(79)；三个噪声采样（`0x63B70`/`0x657F0`/`0x6EB40`）测不出周期，仍按 C4 触发。这些数字与**实测音频**吻合：把 GA20 单独 solo 出来做频谱，1.0–2.0 s 段的主峰就在 96–107 Hz（正是那个滑音贝斯），0.1 s 段在 866/436 Hz（正是高音那几路）。`ga20.vgm` 用合成方波钉住旋律通路（18 字节周期 @ rate 131 = G4）与鼓触发（C4），`engine.ga20` 通过 |
| 39 | K053260 | K053260 | 旋律 ×4（鼓为触发） | 采样播放器，有音高，M4 第三颗。频率是 12 位 pitch，按 `pitch * clock / 262144` 出字节率（核心输出率 `clock / 64`，12 位累加器溢出一次取一个字节），音符同样由采样自身周期决定（`chips::estimateCycleLength()`）。三处与 Namco 两款不同：①**键开是共享寄存器 `0x28` 的上升沿**（bit *i* 起、清零停，高 4 位是 reverse），所以重复写同一个 bit 不是新音符，写声部寄存器也不是键开；②**长度是寄存器**：不循环的声部从 `start + 1` 播 `length` 个单元后自己结束（核心的先自增特性，注释点名 TMNT2 与 Vendetta），循环位（`0x2A` bit *i*）让它绕回、只有键关能停；③**两种采样格式**：PCM 是有符号 8 位，KADPCM 是 4 位增量流（低半字节在前，积分之后才测周期）。另外 `0x2F` bit 1 是整片输出使能，核心没有它就完全静音，解码器照此实现；音量 7 位、pan 3 位且 pan 0 为静音。**打击乐**：这些板子把鼓放在同四条声部上，而鼓采样没有可测周期（底鼓扫频、军鼓是噪声），退回根音会让每个鼓点变成一个旋律音；所以测不出周期、或测出的音高高于 C8（108——这个码率下 8 字节周期就是 6 kHz，只可能是伪相关）的声部一律报 C4 触发（`isDrum`、`Family::adpcmDrum`），C8 以下照常报真实音高（有音高的 tom 仍然是音符）。ROM 来自 `0x67` 的 `0x8E` 块（21 位地址、无 bank 寄存器），命令是 `0xBA`（8 位偏移 + 8 位数据）。`k053260.vgm` 用一次掩码写同时键开两个声部（voice 0 周期波形 = A4、voice 1 噪声 = C4 触发，各 4 个音符），`[13]` 实测两轨 rms 0.06。**真实曲目**：`test_file/K053260/01 The Declaration of War (Title Demo).vgz`（Vendetta，同一文件还带 YM2151）12 条轨全部有声且音符一致，其中 K053260 四条轨 9/3/7/2 个音符全是 C4 触发（鼓），YM2151 的 8 条轨不受影响 |
| 40 | K054539 | K054539 | 旋律 ×8（鼓为触发） | 采样播放器，有音高 + reverb，M4 第四颗。频率是 24 位步进：每个输出样本把 `pitch` 加进 16 位小数、溢出一次取一个单元，输出率 `clock / 384`，所以单元率 = `pitch * clock / 25165824`；单元是字节（8 位 PCM）、字（16 位 PCM）或半字节（4 位 DPCM）。音符同样由采样自身周期决定（`chips::estimateCycleLength()`）。三处与前三款都不同：①**采样靠标记结束**——核心取到 `0x80`（8 位）/`0x8000`（16 位）/字节 `0x88`（4 位）就键关，置 loop 位则跳到 loop 地址（`0x08..0x0A`）；ROM 在手时到标记的距离可以直接扫出来，所以不循环的音符长度是**精确**的，不需要任何猜测；②**音量是衰减**（`0x03`：0 = 满幅，0x40 = -36 dB），按电平读会把强弱完全弄反；③**start 寄存器同时是实时位置**——核心每个输出样本把当前位置写回 `0x0C..0x0E`，写进不同的值会重置播放小数，所以流里的值就是采样起点，解码器在键开时锁存。其余：键开是 `0x214` 掩码位、键关是 `0x215` 掩码位；格式与 loop 在 `0x200 + 2n`（bit2-3 选格式、bit5 reverse、`0x201` bit0 循环）；pan 1..0x0F 右 / 0x10 中 / 0x11..0x1F 左（pan 0 不是静音，核心把异常值当地中）；`0x22F` bit0 是整片 PCM 输出使能。打击乐与 K053260 同策：测不出周期或音高高于 C8（108）报 C4 触发（`isDrum`、`Family::adpcmDrum`）。ROM 来自 `0x67` 的 `0x8C` 块（24 位地址）；命令是 `0xD3`（16 位大端偏移 + 8 位数据，高字节走 `port`）。`k054539.vgm` 两轨各 4 个音符（A4 与 C4 触发，各自由自己采样里的 `0x80` 结束），`[13]` 实测两轨 rms 0.06。**真实曲目**：`test_file/K054539/13 Please.. Rescue The Professor (Midway Demo 2).vgz`（X-Men，同时带 YM2151，头部把 K054539 写成 48000 = 采样率，libvgm 按 ×384 折成 18.432 MHz）里 K054539 的 Ch 1–3 是带循环的低音/人声（44/44/39，velocity 17），**Ch 4–6 是鼓组**：它们暴露了共用测算的一个真问题——鼓的起音平滑，自相关在每个小延迟上都接近 1，于是最短延迟（8 样本）靠「平滑」而不是「周期」胜出，每一击都被报成 2.6 kHz 的旋律音（100/107/112）。修法写进 `chips::estimateCycleLength()`：**选中的峰之前必须出现相关性的低谷**（< 0.5），没有低谷就说明那是平滑而不是周期，返回 0。修完之后 Ch 4–6 全部是 C4 触发（16/11/11 个音符），并且鼓的 gate 改用 80 ms 触发窗（与 OPN ADPCM-A / YMZ280B / YMF271 同约定），否则长鼓采样会让相邻两击重叠成一个长音符。C352/K053260 的真实曲目回归后音符不变 |
| 41 | DMG | GB_DMG | 旋律 1–3 + 噪声触发 | |
| 42 | SAA1099 | SAA1099 | 旋律 1–6 + 噪声触发 | 与 AY 同型，但寄存器独立 |
| 43 | X1_010 | X1_010 | 旋律 ×16（波形）；PCM 模式为触发 | 采样/波形混合，有音高。波形模式按 `clock * pitch / 2^26` 出音高、状态位 7 对半；PCM 模式是 80 ms 门限的 one-shot。VGM 命令是 `0xC8`（16 位大端偏移），读取器把高字节放在 `port` 里传下来；`x1.vgm` 用同一命令上传波表/包络并带 `0x91` ROM 块（核心没有 ROM 会直接静音），`[13]` 实测 Ch1 有声且 4 个音符 |
| 44 | PPZ8 | ✗ | — | 软件 PCM 驱动，不是芯片；支持它等于实现一个驱动 |
| 45 | PPSDRV | ✗ | — | 同上 |
| 46 | SID | ✗ | — | libvgm 无核心 |
| 47 | P86 | ✗ | — | PC-98 声卡（YM2203 + ADPCM），可由现有核心组合 |
| 48 | POKEY | POKEY | 旋律 1–4 | 16 位成对时只报低通道 |
| 49 | WSwan | WSWAN | 旋律 1–4 + 噪声/语音触发 | Ch4 可切噪声，Ch2 可播声音 DMA |
| 50 | YM2609 | ✗ | — | MDSound 有核心（ZGM 驱动用），libvgm 没有 |
| 51 | ES5503 | ES5503 | 旋律 ×32（鼓为触发） | 波表芯片（Ensoniq DOC，Apple IIgs / Mirage），M4 第八颗。32 个振荡器各自走一张表，表在芯片自己的 128 KB RAM 里。音高由三件事相乘：步进 `freq * fs / 2^(9+resolution-wavetableSize)`（`es5503.c` 的 `resshifts`、`accmasks`），芯片自己的采样率 `fs = (clock/8)/(oscillatorsEnabled+2)`（核心按使能振荡器数分频，**`0xE1` 一改全片音高都变**），以及**表本身的周期**。三处与前面几颗不同、各有单测：①**没有键开位**，控制寄存器 bit0 是停振位，音符起在它「从 1 到 0」的下降沿、止于上升沿（复位后所有振荡器都按「已停振」处理——核心把 RAM 清零，首次更新读到 `0x00` 就自己停振，否则驱动第一次写 `0x00` 会被漏掉）；②**表可以是多周期的**——Undercover Cops 上传的是 1024 字节里装 10 个周期的波形，按「一表一周期」算会低三个八度，所以周期从 RAM 里测（512 字节窗、半窗步进、取第一个有周期的窗，判据是「先凹陷后峰」且峰必须是**局部极大**，否则方波的斜坡会被当成偏高的周期）；③**单周期表测不出周期**（窗自身长度内没有重复），此时若模式是循环（FREE/SYNCAM）就回退成表长——那正是芯片重复这张表的速率；不循环的 one-shot 表才判为鼓。命令 `0xD5 <port> <register> <value>`，RAM 走 `0x67` 的 **`0xE1`** 块（32 位地址在前，libvgm `_VGM_RAM_CHIPS`）。**真实曲目**：`test_file/ES5503/01 Title Screen.vgz`（Undercover Cops，59.1 秒、1426 次键开、32 个振荡器全开、时钟 7,159,090 → fs = 26,320）32 条轨里 18 条有声且全部有音符；与 solo 音频逐条核对：振荡器 0 的表周期 100 字节 → 频率寄存器 1041 下 **522 Hz（C5, 72）**，振荡器 4 的 520 → **263.8 Hz（C4, 60）**，与频谱一致。另两首（`02 Danger Nearby`、`03 Game Over`）同样 14/18 条有声轨全部有音符。`es5503.vgm`（G4 + C4）+ `engine.es5503` |
| 52 | PCM8 | ✗ | — | 软件 PCM 驱动 |
| 53 | MPCMX68k | ✗ | — | X68000 板载 PCM |
| 54 | CS4231 | ✗ | — | PC 声卡 |

**三批工作**

- **A. 有核心、缺解码器（5 项）**：只动 `plugin/Source/chips/` + `midiCapabilityFor` + 测试，不需要改 libvgm，也不需要新格式。其中 4 项真缺解码器：1 项可旋律化（OKIM6295），PWM 与 uPD7759 天生只能 one-shot，OKIM6258 只有采样率档位；另 1 项 YMZ280B 的解码器已写但**搁置**（memo 见 §11.2 行 36），不计入覆盖。C140（表行 6）、C352（表行 24）、K053260（表行 39）、K054539（表行 40）、QSound（表行 37）、GA20（表行 38）、SEGAPCM（表行 9）与 ES5503（表行 51）已从这一批划掉。
- **B. 无音频核心（15 项）**：9 个真芯片（FDS、MMC5、VRC7、N163、VRC6、FME7、SID、MultiPCM、YM2609）+ 6 个驱动/板卡（PPZ8、PPSDRV、PCM8、MPCMX68k、CS4231、P86）。它们主要出现在本插件目前不读的格式（NSF/NSFE、SPC、HES、KSS…）或驱动级回放里，所以 B 批实际是「新格式 + 新核心」的工程，不是 MIDI 输出的延伸。有核心之后 MIDI 侧很便宜：FME7 复用 AY、VRC7 复用 YM2413、VRC6 脉冲复用 NES。
- **C. 已超出（13 项）**：SCSP、ES5506、C219、MIKEY、K007232、K005289、MSM5205、MSM5232、BSMT2000、ICS2115、VBOY_VSU、YMW258、YM2414。不计入对齐，但每项同样要一个解码器才能出 MIDI，机制与 A 批相同。其中 **C219 已实现（2026-10-22，M6）**：它复用 `C140Decoder`——219 ASIC 就是十六声道的 C140，寄存器布局与 `0xD4` 命令都相同，八首 NA-1 曲目（`test_file/C219`）全部通过 `[13]`。

  **这十三项的真实前置条件**（2026-10-22 逐项核对核心文件）：

  - **ES5506**：`libs/libvgm/emu/cores/es5506.c` 只有 619 字节，`DeviceName` 返回 `NULL`、`DeviceChannels` 返回 `0`——是**占位桩，没有核心**，连发声都做不到，MIDI 无从谈起；
  - **YM2414（OPZ）**：`emu/cores` 里根本没有对应文件，同样无法播放；
  - **MSM5205**：核心存在（13.7 KB），但音高只有四档采样率选择器，按 M5 的先例不做；
  - 其余九颗（K005289、K007232、VSU、BSMT2000、MSM5232、MultiPCM、MIKEY、ICS2115、SCSP）核心属实，机制与 A 批相同。

  因此 C 批**实际可做 10 项**，不是 13；其中**已完成 7 项**（C219、K005289、K007232、VSU、MSM5232、YMW258、MIKEY）；**ICS2115 搁置**（2026-10-22）——寄存器层面已经查清（命令 `0x44`、三端口 `1`=选寄存器/`2`=低字节/`3`=高字节、ROM 块 `0x96`、`0x4F` 选振荡器、`0x10` 高字节 0 = key on、音高 `hz = fc × 33075 / 1024`），但按此序列手工构造的合成 VGM 在 libvgm 里**渲染为静音**（三种变体都试过：设置 Active Voices `0x0e`、给出循环终点、写满 32 个声道）。`keyon()` 在核心中是空函数，真正的发声条件还没找到；没有真实 rips 可对照，继续只能靠猜，因此按优先级搁置；**BSMT2000 搁置**（2026-10-22）——它的写口是“latch 高低字节 + 提交地址”协议（`bsmt2000.c:338`），而音高不在寄存器里，来自 ROM 中每个样本的头部（与 C140 同型）；没有真实 rips 可校准，自造资产只能验证自己猜的格式，因此按“优先级不高”暂不实现；VSU 的音高是 `clock / (32 × (2048 − freq))`，`0xC7` 的偏移是**大端**且 bit 15 选片（`ReadBE16`），30 首曲目里 24 首通过——其余 6 首失败在**引擎级检查**，与音符提取无关：03/05/18 是 `master meter` 偏差（这些曲目峰值到 1.0，疑似削波让 rms 比失真），04/06/07 是 `plain player` 在开头 0.5 秒内静音。两处都暂搁置，记录在此以免重复调查。原始清单为已完成 3 项：C219（复用 `C140Decoder`，八首 NA-1 曲目通过）、K005289（新增 `K005289Decoder`，音高 `clock / (32 × (freq + 1))`，`0x42` 命令，十五首曲目通过）与 K007232（新增 `K007232Decoder`，音高 `clock / (4 × (4096 − step))`，`0x41` 命令，六首曲目里每条有声轨都有音符）。

  **一条测试教训**：`vgm_mixer_test` 的“第二遍循环仍发音符”检查要求录制窗口 ≥ 两倍第一遍长度。K007232 的 `04` 第一遍 82.5 秒，用 6 秒窗口去跑必然失败——那不是缺陷，是窗口不足。长曲目要按 `loop at A of B` 那一行的 B 值放大窗口，否则会误判。

**优先级**

1. 方波/波形 PSG：X1_010——都是「周期→音高」，`chips/Pitch.h` 的曲线直接可用，单个解码器约 100–200 行，测试全在 `vgm_chip_test` 里。（HuC6280、K051649、SAA1099、POKEY、WSwan 已按此路径完成。）
2. FM：YMF271——已完成（见 §11.2 表行 35）。（YMF278B 的 FM 半已按此路径完成，见 §11.2 表行 22。YMZ280B 原属这一批，已**搁置**：vgmrips 的成套录音是“一条长样本、一路发声”，验收条件③“真实曲目的 note-on 数非零”只能得到 1 个音符，见 `docs/memo/2026-10-20-ymz280b-shelved.md`。）
3. 带音高的采样播放器：C140（**已完成**，见 §11.2 表行 6）、C352（**已完成**，见 §11.2 表行 24）、K053260（**已完成**，见 §11.2 表行 39）、K054539（**已完成**，见 §11.2 表行 40）、QSound（**已完成**，见 §11.2 表行 37）、GA20（**已完成**，见 §11.2 表行 38）、ES5503（**已完成**，见 §11.2 表行 51）——音高来自步进/采样率，需要「步进→音分 + 根音」的换算，与 ADPCM-B 的根音问题同型（§11.1）。C140 与 C352 已经把两件共用件放进 `chips/Pitch.h`：`chips::quantisePlaybackRate (rate, rootRate, finePitch)` 把根音做成参数而不是常量，`chips::estimateCycleLength (waveform, loopOffset, loopLength)` 从 ROM 里测出一个周期的长度（自相关取最强局部峰，再按 loop 是整数个周期吸附，识别不出的返回 0）——M4 余下的七项只提供自己的 rate、根音与采样波形即可，不要再各写一份换算或测周期。
4. 无音高的：PWM、uPD7759、OKIM6258——收益最小，最后做。

**验收**：新增一个芯片要一并给出 ①`ChipDecoderTest.cpp` 的周期→音高、静音判定与包络/长度用例；②`midiCapabilityFor` 的通道数；③至少一首真实曲目的 `vgm_mixer_test` 输出（note-on 数非零）；④本节与 §7.1 表格同步。

---

## 12. 评审结论

基础版范围已经确认，没有阻塞问题：

- **MIDI-BUS 第一版不做**：VST3 只有一个输出事件总线，由 REAPER 路由到端口/轨道；standalone 的 MIDI 设备选择放到后续 Settings → MIDI 输出子面板；
- 能力徽标（旋律 = 键盘，触发 = 采样波形）+ tooltip；
- 允许共用 MIDI 通道；
- 混音器总开关关闭时不输出任何 MIDI；
- 设置子面板放到后续阶段。

---

## 13. 决策记录

| 日期 | 结论 |
|---|---|
| 2026-10-22 | **ADPCM-B 的速率常数错了 288 倍（表行 12 的真实 bug，由用户「Neo Geo 的 ADPCM-B 一直是 one-shot」发现）**。核心的 DeltaT 每输出样本前进 `delta * freqbase`（`ymdeltat.c`），而 `freqbase = 1`（原生率），所以每秒 nibble 数是 `deltaN × (clock/144) / 65536`——`clock/144` 就是参考项目里的 50 kHz（7.2 MHz 器件）。我们写成 `delta × clock / 32768`，恰好大 288 倍：`channelState` 里算出的门长只有一个样本量级（音符被立刻关闭），音符表里算出的音高越过 MIDI 上限（`noteFor` 返回 -1，整条轨被丢弃）。两者同时表现为「有声、MIDI 全空」，而合成用例因为 DELTA-N 只写了两位数（29）刚好落在正确量级内，没能暴露。修正后两首 Neo Geo 真实曲目的 ADPCM-B 分别输出 16 / 24 个音符，`[12]` 恢复「有声轨必须有音符」。合成资产与单测的 DELTA-N 一并改成真实量级（8341 / 38696），并新增 `testOpnAdpcmBRealDeltaScale` 锁住「大 DELTA-N 必须出音符」 |
| 2026-10-22 | **采样芯片的音高只从寄存器来，音频只作事后核对（§14）**。原本的做法是“寄存器给出的播放率 ÷ 从 ROM 量出的采样周期”，这一步把「芯片说了什么」换成「我们以为素材是什么」：周期测错不会让某个音错，而是整首恒定偏移，外观上与“素材本来就这样”无法区分。参考项目 `reference/MDPlayer-stable` 给的是另一条路并且已被验证——先算 Hz（`deltaN × 50kHz / 2^16`、`fnum × clock / (24·div·2^21)`），再用唯一的最近频率查找变成音符，采样内容完全不参与（`common.cs:453`、`PianoRoll/YM2610.cs:162`、`form/KB/OPL/frmY8950.cs:265`）。定为本项目的标准：**先频率、后音符、永不读波形** |
| 2026-10-22 | 音高拆成两层随音符一起带出：**L1 移调量** = `1200·log2(rate / refRate)`（寄存器 + 文档化参照率，永远可算）与 **L2 根音** = `1200·log2(refRate / cycle / middleC)`（只能从采样数据测，是推断）。默认只输出 L1；L2 由设置项 `pitch.useSampleRoot` 打开。两者相加恰好等于旧映射，这是重构能逐芯片落地、且能用测试钉住的等式（`testC140TranspositionAndRoot`、`absoluteNote()`） |
| 2026-10-22 | 参照率 `refRate` 是**每芯片的文档常量**，不是测量：C140 用 8 kHz、SegaPCM 用 pitch 128（驱动的无移调档）、K053260/K054539/QSound/C352 用「一字节每输出样本」、GA20 与 ADPCM-B 用 8 kHz（后者折算成 16,000 nibble/s，与参考项目一致）。因此**默认视图的八度取决于参照率**——同一条采样内部的音程永远正确，绝对八度交给 L2，这正是开关存在的理由 |
| 2026-10-22 | ES5503 是唯一没有 L1 的芯片：它的寄存器是**表内步进**（`freq = 512` 才一样本一条目，一条目 25 Hz），任何“参照率”都是关于表的数字而非关于录音。它的音高整体来自实测周期，标 `sampleInferred` |
| 2026-10-22 | 「有没有音高」仍由内容判断，「是哪个音高」交给寄存器：测不出周期（鼓、噪声、多峰歧义）或频率为 0 的声部，一律输出 **C4 触发**，两种模式都一样。根音移调只是把八度放对，不会把鼓变成音符 |
| 2026-10-22 | 根音覆盖（按采样手调）不做，只留接口与备忘录：`NoteEvent::rootCents` 就是那个接口，将来由每曲覆盖表替换，见 `docs/memo/2026-10-22-sample-root-override.md` |
| 2026-10-19 | 优先 VST3，首选实测宿主 REAPER；使用插件原生 MIDI 输出事件总线，`NEEDS_MIDI_OUTPUT` 已经存在，不新增 CMake 开关 |
| 2026-10-19 | VST3 不提供插件侧的“MIDI Bus”选择；宿主负责把唯一输出总线路由到端口/轨道；第一版不加每轨 MIDI-BUS 下拉框；standalone 设备选择以后再做 |
| 2026-10-19 | 第一版不改 libvgm；在 `LibvgmSongInstance` 用 VGM/VGZ 解析构建只读 `MidiSequence` |
| 2026-10-19 | 每轨一个 `Off` + 1–16 下拉框，默认全部 Off；Master 也有一个，默认 Off |
| 2026-10-19 | MIDI 跟随音频可听状态：mute 或“其它轨 solo、自己未 solo”时停止并 all-notes-off；要单独禁 MIDI 用 `Off` |
| 2026-10-19 | Master 输出所有音频可听且 MIDI 已启用的轨道合并；直出和 Master 可同时存在，允许重复，默认 Off，用户自行选择 |
| 2026-10-19 | 不做 GM 鼓映射；能确定音高的通道输出原始音高，one-shot 统一 C4（60）作为 trigger，不声称真实音高；用户可在 DAW 里自行加 MIDI 映射；ADPCM-B v1 按 one-shot |
| 2026-10-19 | 两个轨选择同一 MIDI 通道时允许合并 |
| 2026-10-19 | 混音器总开关关闭时不输出任何 MIDI（直出 + Master 都停），下拉框灰显但保留数值 |
| 2026-10-19 | linked SSG 独立轨第一版不做，方案记入附录 A；S98/DRO/GYM、MIDI 文件、虚拟端口也不做 |
| 2026-10-19 | 后续增加 Settings → MIDI 输出子面板：one-shot note 映射、ADPCM-B 根音/melodic、standalone 设备、默认通道等 |
| 2026-10-19 | MIDI-BUS 第一版不做，先实现“每轨 MIDI 通道 + Master 合并 + VST3 输出”的基础版 |
| 2026-10-20 | WonderSwan 的声音 DMA（`0x89`）按“一串字节 = 一次触发”处理：报一个 C4 one-shot，和 ADPCM 的 one-shot 同型，不按字节重触发 |
| 2026-10-20 | `singleChannelMuteOptions` 的两个 `chnMute` 半字都填同一个通道掩码。libvgm 把 `chnMute[1]` 给 linked 子设备（`VGMPlayer::RefreshMuting`），而子设备与父设备的通道编号一一对应（OPL4 的 FM 半就是一个 OPL3），原来把第二个半字填成“其余全静音”正好静掉了本条轨道要的那个通道——OPL4 在混音器里整轨无声、纯播放器却有声，就是这个原因 |
| 2026-10-20 | `0x40-0x4F` 的通道位与算子位写反了：低两位是端口内的通道、bit 2-3 才是算子。反过来之后每个算子的音量都记到别的通道上，真正的载波一直停在静音默认值 `0x7F`——FM5 有音频却一条 MIDI 事件都没有（`06 Star Light Zone.vgz` 的 FM5，FM2 同样中招），就是这里。`vgm_chip_test` 的 `startFmNote` 也按错位写过算子 4（`0x43`），修好解码器后这些用例才暴露出来，已改成真实的 `0x4C`（端口 1 的 FM4/FM6 分别是 `0x4C`/`0x4E`） |
| 2026-10-20 | 新增 `vgm_opn_test`（§10.2）：期望值不再由我们写死，而由 libvgm 的 YM2612 核测出来。旧 bug 在这个测试下复现为 `0x43` 幽灵音 + 384 次比较里每个“算子 4 是载波”的用例都红，修好后全绿——单测与解码器同源的问题从这里开始不再靠自觉 |
| 2026-10-20 | `VgmCommandReader` 开始保留 `0x67` 数据块（type `0x00` 的 YM2612 PCM bank），`0x8n` 从中按序取字节写成一条 `0x52`/`0x2A` 写、再跟它自带的等待（`0xE0` 的 bank 寻址一并支持）。此前 `0x8n` 只推进时钟，DAC 轨因此永远没有 MIDI。`06 Star Light Zone.vgz` 的 DAC 轨从 0 个音符变成 3 个，`[13]` 的“DAC 轨豁免”随之删除——那条豁免就是 M1.2 的占位 |
| 2026-10-20 | 混音器 MIDI 行改版：原来的 one-shot 单脉冲徽标换成**能力徽标**（旋律 = 钢琴键盘，触发 = 采样波形，都不可点击、只给 tooltip）；下拉条目改成 `Ch.1`–`Ch.16`，弹窗每条通道前加 MIDI 标志。三枚图标都是 `juce::Path` 矢量（`Icons.cpp`），在真实 15 px 下比对过：键盘三白键两黑键、分隔线贯穿整个高度并被黑键盖住上半段，波形要加大振幅才不糊。`ComboBox` 存不了图标，所以 `MidiChannelBox` 覆写 `showPopup()`、用 `MidiMenu.h` 的 `makeMidiChannelMenu()` 建菜单；`vgm_ui_test` 新增 `[5]`（三枚图标互不相同 / 不越界 / 键盘上重下轻 / 波形左重右轻 / MIDI 是环不是实心块）与 `[6]`（弹窗 17 条、通道条目带图标、选中项打勾） |

| 2026-10-20 | 新增 C140 解码器（表行 6，M4 第一项）：它的频率寄存器是播放率而不是音符，所以先有共用件 `chips::quantisePlaybackRate (rate, rootRate, finePitch)`（`chips/Pitch.h`，8 kHz 根音记作 C4，根音是参数，§11.1 的覆盖接在这里），M4 余下八项不再各写一份换算。长度由结束地址给出并在键开时与 loop 一起锁存（核心就是那时复制 start/end），loop 位让指针回卷、只有写 mode 去掉 bit 7 才算 key-off；零音量与零频率按核心的静音处理，不报音符。`0xD4` 的 16 位偏移把高字节留在 `port` 里，所以 16 号以上的声部要靠两半拼回——`c140.vgm` 特意用 voice 0/16 差八度，漏掉高字节就只剩一轨 |
| 2026-10-20 | 新增 `loop.vgm`（`make_loop_song.ps1`）：VGM 1.61 的 0x100 字节头部，`0x1C` 写循环偏移、`0x20` 写循环长度、`0x7E` 的 loop base 显式为 0，循环体是 20 个音符 × 2 s。此前仓库里没有任何资产声明循环点，`[12]` 每次都在 `loop check skipped` 上空转；现在它真的跑起来（`20 / 19`），把 `PlaybackEngine` 里 `songTimeWithinPass()` 的折回去掉会立刻变成 `20 / 0` 并报红。`[12]` 另外对 `loop.vgm` 强制要求循环点存在——拿不到就报错而不是跳过，免得改坏头部又静默失效 |

| 2026-10-20 | X1-010 解码器（M2 第一颗）：波形模式的音高是 `clock * pitch / 2^26`（core 的 `step = base_clock/524288*freq` 走过 128 字节表），状态位 7 把 pitch 右移一位即降八度；PCM 模式没有音高、也没有能读出长度的结束寄存器，所以用 80 ms 门限做 one-shot。VGM 用 `0xC8`（16 位大端偏移），读取器把高字节塞进 `port`、低字节塞进 `addr` 传下去，`0x80` 以上的偏移（包络/波形数据）解码器直接忽略——否则上传波形会被当成按键。`vgm_chip_test` 新增四条，X1-010 在能力表里是 16 个旋律通道。端到端资产 `x1.vgm` 也补上了：波表和包络可以用同一条 `0xC8` 写上传（波表 `n` 在 `0x1000 + 128n`、包络 `n` 在 `128n`），但核心在 ROM 未分配时直接返回静音，所以还要一个 `0x67` type `0x91` 的 ROM 块（libvgm `_VGM_ROM_CHIPS`）；`[13]` 实测 Ch1 rms 0.0306、4 个 A4 音符，`ctest -R engine.x1` 全绿 |

| 2026-10-20 | YMF271 解码器（M3）：12 个 group × 4 个 bank，4 算子。算法 0–7 的载体是 bank 3，8–15 才把 bank 0 拉进来，所以「哪个算子出声」必须按算法表选，不能按 bank 顺序取。**三处踩坑**：① 地址低四位是 group 而 3/7/11/15 是空洞，第一版资产把「group 3」写成 `0x33`，核心照 fm_tab 直接丢掉——要用 group 1（`0x31`）才真有第二路；② 核心在 `mem_base == NULL` 时对 FM 组也直接返回静音，所以只测 FM 的资产也得带 `0x67` type `0x85` 的 ROM 块；③ AR=0 时包络永远停在 -60 dB，核心收下了 key-on 但没声音，AR=31 + RR=15 才既有声又能在 key-off 后收尾。`vgm_mixer_test --notes` 实测 group 0 = 4 个 A4（69）、group 1 = 4 个 A5（81），`[13]` 两轨 rms 0.318 且 `every track that makes sound also produces MIDI notes` 通过；`ctest -R engine.opx` 全绿 |

| 2026-10-20 | `psg` / `opl4` / `ym2610` / `fm_alg0` 四个资产从“手工跑”变成 ctest 用例（`engine.*`），并为它们写**逐音符断言**：组名 + 轨名 + 第几个音符 + 时刻 + 音高，容差 20 ms。此前它们只是把 `[13]` 跑一遍，而 `[13]` 只问“有声的轨有没有音符”——音高错一个八度、音符落错轨、时间偏半秒都照样绿。为拿到音高与时刻，`TrackProbe` 额外记录 note-on 序列（`probes[i].ons`）。变异验证：把 AY 分频从 16 改成 32 → 只有 `psg.vgm: AY-3-8910 / Ch 1 is 57, expected 69` 变红；把 OPL4 的 bank 压成 0 → 只有 `opl4.vgm: Ch 10` 变红。同一轮修了 `make_ym2610_song.ps1` 的音高公式（`2^(21-block)` 应为 `2^(20-block)`）：三个 FM 音符一直比注释高一个八度（A5/C6/E6 冒充 A4/C5/E5），重新生成后是 69/72/76 |
| 2026-10-21 | **C140 的音符一直是错的，这次修好**：此前只把播放率按固定 8 kHz 根音量化，而采样实际录制速率是驱动自己定的，于是「Submerged City」的贝斯报 59、和弦报 62/76/79/84，而芯片实际发的是 32（G#1）与 56/60/63（G# 大三和弦）——寄存器读对了，换算错了。现在 `VgmCommandReader::dataBlocks()` 保留 `0x67` 块（含 ROM 头的 `romSize`/`romOffset`），`MidiSequenceBuilder::assembleRom()` 按各块自己的偏移拼出 `0x8D` 的 C140 ROM，`C140Decoder::setRom()` 在键开时于 loop 区做自相关测出采样周期（按「loop 是整数个周期」吸附），音符 = 播放率 ÷ 周期；测不出来的（鼓/噪声、μ-law、无 ROM）仍退回根音。**踩坑**：第一版没做 banking 折叠，System 2 的 `((adrs&0x200000)>>2)|(adrs&0x7ffff)` 漏掉后 bank 0x14 + 0x95C0 变成 0x1495C0，1 MB ROM 里没有这个地址，`samplePeriodFor()` 返回 0——所有音又回到根音，输出和修复前**一模一样**，看起来像“ROM 没接上”。ROM 与 banking（头字节 `0x96`，`c140Banking()`）必须一起传，`setRom()` 把两件事合成一个参数就是为此。`vgm_chip_test` 新增四条：16 字节周期 → C5（72）、无 ROM 回退 C#4（61）、噪声采样回退（61）、bank 0x14/0x95C0 折成 0x495C0 → C5 且 linear 寻址越界回退 61 |
| 2026-10-21 | C352 解码器（表行 24，M4 第二项）：频率寄存器同样是播放率（`frequency * clock / 18874368`，与 C140 同一个除数），音符同样由采样自身周期决定，测量因此从 `C140Decoder.cpp` 提炼成 `chips::estimateCycleLength()`（`chips/Pitch.h`），C140 改为调用它、行为不变。四处与 C140 不同、也各有用例：①键开不是 flag 写，而是 offset `0x202` 的执行写（`0x4000` 只登记请求，执行后清位；`0x2000` 加执行写才是键关），`0x0202` 的高偏移字节不能被当成 voice 0 的寄存器；②寄存器是 16 位整字，`0xE1` 一次带 16 位大端 offset（bit 15 选第二片）与 16 位大端值，故 `RegisterWrite` 新增 `value2` 承接低字节；③采样地址 `(bank << 16) \| start` 按 ROM 容量取 2 的幂掩码，bank 1 是同一偏移再向里 64 KB；④结束地址结束不循环声部，loop 位（`0x0002`）后只由键关结束。μ-law 先按 `c352.c` 的表解码再测周期；测不出周期的退回 `quantisePlaybackRate()`，根音取芯片自身码率 `clock / 576`（0x8000 = 中央 C）。ROM 来自 `0x67` 的 `0x92` 块，头部时钟按 libvgm 的读法 `hdr[0xDC] * 72 / hdr[0xD6]`。`c352.vgm` 用一次执行写同时键开两个声部（voice 0 靠结束地址、voice 1 靠键关且在 bank 1），`[13]` 实测两轨各 4 个音符、A4/A5、rms 0.21。**真实曲目验证（2026-10-21 补）**：`test_file/c352` 的两首 Galaga 曲子发现并修掉了「单周期波表」缺口——整首曲子用一个 16 字节波形（`wave_end - wave_start + 1` = 16、loop 回 `wave_start`、ROM 块只有 16 字节载荷），比自相关窗口短，此前一律退回根音，整曲低 40 个半音。现在 `cycleFor()` 在测不出周期且 loop ≤ `kSingleCycleBytes`（256）时把 loop 长度当作周期；并按 `C352_fetch_sample` 的次序（先取样、后判断结束）把 `wave_end` 当闭区间，区域与 loop 都 +1 字节——16 与 15 字节差一个半音。修复后 Ch1/Ch2 是同一段下行三度旋律的 C 大调与 A 大调版本，`vgm_chip_test` 加了两条用例 |
| 2026-10-21 | K053260 解码器（表行 39，M4 第三项）：频率是 12 位 pitch 除以 4096 的字节步进（输出率 `clock / 64`），所以 rate = `pitch * clock / 262144`，音符仍由 `chips::estimateCycleLength()` 从 ROM 周期算出。与 C140/C352 不同、各有用例的三处：①**键开是共享寄存器 `0x28` 的上升沿**——重复写同一个 bit 不是新音符，写声部寄存器也不是键开；②**长度是寄存器**（`length`），不循环的声部从 `start + 1` 播完就停（核心的先自增特性，注释点名 TMNT2/Vendetta），循环位让指针绕回、只有键关能停；③**PCM 与 KADPCM 两种格式**，KADPCM 是 4 位增量、要积分后再测周期。另按核心实现 `0x2F` bit 1 的整片输出使能、pan 0 静音、音量 7 位。**打击乐**：Vendetta 的标题曲把鼓放在同四条声部上（实测四条轨全是 C4 触发，共 9/3/7/2 个音符），鼓采样测不出周期；若像 C140/C352 那样退回根音，每个鼓点会变成旋律音。所以「测不出周期」或「测出的音高高于 C8」都报 C4 触发 + `Family::adpcmDrum`——后者是真实曲目抓到的：噪声在 8 字节周期上伪相关，按 28–53 kB/s 算就是 6 kHz，报成了 116。ROM 走 `0x67` 的 `0x8E` 块，命令是 `0xBA`。`k053260.vgm` 两轨各 4 个音符（A4 与 C4 触发），`engine.k053260` 与 Vendetta 真实曲目（12 轨全部有声且音符一致，YM2151 不受影响）都通过 |
| 2026-10-21 | K054539 解码器（表行 40，M4 第四项）：24 位 pitch 是步进（每个输出样本把 `pitch` 加进 16 位小数，溢出一次取一个单元；输出率 `clock / 384`），所以单元率 = `pitch * clock / 25165824`，音符仍由 `chips::estimateCycleLength()` 从 ROM 周期算出——单元长度随格式变（8 位 = 字节、16 位 = 字、4 位 DPCM = 半字节，且 DPCM 要先积分再测）。三处与前三颗不同、各有用例：①**采样靠标记结束**——核心取到 `0x80`/`0x8000`/字节 `0x88` 就键关，所以到标记的距离就是音符长度，直接扫 ROM 得到，不需要任何猜测；loop 位让它跳回 `0x08..0x0A`，只有键关能停；②**音量是衰减**（`0x03`：0 = 满幅，0x40 = -36 dB），按电平读会把强弱完全弄反（用例断言 -18 dB 力度 17、-36 dB 静音）；③**start 寄存器同时是实时位置**（核心每个样本写回），所以在键开时锁存。另：键开 `0x214`/键关 `0x215` 位掩码（16 位偏移由 `0xD3` 一次带来，高字节走 `port`）、`0x22F` bit0 整片 PCM 使能、pan 0 不是静音。打击乐沿用 K053260 的判据（测不出周期或高于 C8 → C4 触发 + `Family::adpcmDrum`）。ROM 走 `0x67` 的 `0x8C` 块（24 位地址）。`k054539.vgm` 两轨各 4 个音符（A4 与 C4 触发），`engine.k054539` 与全量 ctest 27/27 通过。**真实曲目**（X-Men 的 Midway Demo，同时带 YM2151）把鼓组放在 Ch 4–6，暴露了共用测算的一个真问题：鼓的起音平滑，自相关在每个小延迟上都接近 1，最短延迟（8 样本）靠平滑胜出，每击被报成 2.6 kHz 的旋律音。修法是给 `chips::estimateCycleLength()` 加**低谷要求**（选中的峰之前相关性必须掉到 0.5 以下，否则返回 0），修完 Ch 4–6 全是 C4 触发（16/11/11 个音符），鼓的 gate 同时改为 80 ms 触发窗（否则长鼓采样让相邻两击重叠成一个长音符）；C140/C352/K053260 的单测与真实曲目回归后音符不变（K053260 Vendetta 的两个音效音从 116 变成 67——那是这个采样真实的准周期，116 本来是伪相关被 C8 上限拦下的结果） |
| 2026-10-21 | ES5503 解码器（表行 51，M4 第八颗）：Ensoniq DOC，32 路波表。音高 = `freq * fs / 2^(9+res-size) / 表周期`，其中 `fs = (clock/8)/(oscillatorsEnabled+2)`——`0xE1` 一改全片变调，所以音符表读实时值而不是锁存。三处坑各有单测：①**没有键开位**，控制寄存器 bit0 是停振位，音符起在它 1→0 的下降沿；复位后所有振荡器必须按「已停振」处理（核心清零 RAM，首次更新读到 `0x00` 就自停），否则驱动第一次写 `0x00` 的那些声部整轨没有音符——Undercover Cops 正是如此，`[13]` 报 AUDIBLE BUT SILENT IN MIDI。②**表可以是多周期的**（这首曲子上传的是 1024 字节装 10 个周期），所以周期从 RAM 测；单周期表在窗内没有重复，循环模式回退成表长、one-shot 才判鼓。③方波的自相关是斜坡，「取 ≥0.9×峰的第一个 lag」会读成偏高几个百分点，改成取**第一个局部极大**，并允许 lag 等于 maxLag（256 字节周期正好落在 512 字节窗的一半处，否则测不到）。命令是 `0xD5 <port> <reg> <value>`，RAM 走 `0x67` 的 **`0xE1`** 块（32 位地址在前）；顺带修了读取器把 `0xC0-0xFF` 也当 ROM 块的 bug——判断 ROM 要用 `type & 0xC0`（libvgm 的 `_VGM_RAM_CHIPS` 语义），`type & 0x80` 对所有 RAM 类型都成立。真实曲目三首全过：01 Title Screen 18/32 条有声轨全部有音符，振荡器 0（表周期 100 字节、频率 1041）算出 522 Hz = C5，与 solo 渲染的频谱逐条吻合。`es5503.vgm` + `engine.es5503`，全量 ctest 31/31 |
| 2026-10-21 | **GA20 音高修正（表行 38 的 bug，被用户听感抓出来）**：字节钟一直用输出率 44,100，而核心真正跑在 `clock / 4`（`iremga20.c` 的 `INIT_DEVINF(..., cfg->clock / 4, ...)`，libvgm 之后再降采样）。每个测出的音高因此低约 20 倍、全部落到 MIDI 0 号音以下，`melodic` 判 false，**所有 GA20 曲子的所有声部都退化成 C4 触发**——不是"素材只能分析到这种程度"，是代码错了。三处修改：①`reset()` 用 `clock / 4`（`flushDecoded` 本来就把设备时钟（头部 0xE0）传给解码器，之前被丢掉了）；②测量改成 512 字节窗、半窗步进扫描、取第一个测出周期的窗（GA20 采样是滑音，整段大窗会把周期平均掉）；③`bestPeriod()` 定位周期时原来从 lag 2 重扫，光滑波形会被判成 2 字节周期，改成从凹陷之后扫。修完 `0x63320` = 163 字节 → G#2(44)，`0x6D3C0` = 146 字节 → 44–51 的贝斯线，与 GA20 solo 音频的频谱（1.0–2.0 s 段 96–107 Hz、0.1 s 段 866/436 Hz）逐一对上；噪声采样仍是 C4。`ga20.vgm` 因此重新生成（16 字节 @ rate 249 → 18 字节 @ rate 131，仍是 G4），`vgm_chip_test` 的四条 GA20 用例与 `engine.ga20` 同步更新；全量 ctest 30/30 |
| 2026-10-21 | QSound 解码器（表行 37，M4 第五颗）：16 路 8 位 PCM + 3 路 ADPCM。频率是 4.12 定点步进（输出率 `clock / 2 / 1248`，字节率 `rate * clock / 10223616`），VGM 头部的老 4 MHz 时钟由解码器按 libvgm 的规则 ×15。三处与前面几颗不同、各有用例：①**没有键开位**——rate 与 volume 同时非零才发声，驱动最后写 rate 起音、写 rate=0 停音，解码器按这个上升沿起音符；②**bank 寄存器属于下一个声部**（`init_register_map()` 的 `(n<<3)+0`）；③**loop 是长度**（`new_phase -= loop_len<<12`），循环区 = `end-loop_len..end`。真实曲目（SF2 Guile Stage，79.5 秒、15 个 `0x8F` 块、26,609 条写入、ADPCM 未用）抓出两个 bug：`end` 按 int16 比较导致跨 64 KB bank 的采样（`end < start`）测不出周期；循环声部的 gate 被设成「立刻结束」，整首曲子一个音符都不出。修完之后 19 条轨里 11 条有声且全部与音频一致：PCM 2 贝斯、PCM 4/5/6 同一采样三音和弦（64/61/56，比例与 rate 完全吻合）、PCM 7–14 鼓（C4 触发）。`qsound.vgm` + `engine.qsound`，全量 ctest 28/28 |
| 2026-10-21 | SEGAPCM 解码器（表行 9，M4 第七颗）：16 路 8-bit PCM，寄存器即播放状态。命令 `0xC0 <offset lo> <offset hi> <value>` 逐字节写寄存器 RAM；`8n+2/3` 音量、`8n+4/5` 循环、`8n+6` 结束（256 字节单位，`(reg+1)*256`）、`8n+7` 步进、`0x80+8n+4/5` 当前地址（24 位位置，低 8 位是小数）、`0x80+8n+6` 控制（bit0 清零发声）。速率 `pitch * clock / 32768` 字节/秒。**没有独立键开位**：解码器只在控制字节「从关到开」的跳变起音符。**测量从采样四分之一处开始**——Slap Bass 的开头是 slap 本身的噪声，直接测周期会得 0；这个改动让同一首曲子的 104 次键开里 39 次测出音高。真实曲目（Galaxy Force II Scene Select，19.2 秒、时钟 4,026,987）验证：Ch 3 = **E1(28)** 的 slap bass（start 0、pitch 128、周期 388 字节），同一采样 Ch 5 pitch 160 = **G#1(32)**（+4 半音与 160/128 精确吻合），Ch 4 另一采样 = G#2；鼓通道仍 C4。`segapcm.vgm`（A3 + C4）+ `engine.segapcm`，全量 ctest 30/30 |
| 2026-10-21 | GA20 解码器（表行 38，M4 第六颗）：4 路 8-bit PCM。寄存器 = `通道*8+n`，0/1 起始（**单位 16 字节**）、2/3 结束（核心从不读）、4 速率、5 音量（双曲）、6 控制（bit1 键控）。速率是**分频**：一个字节每 `256-rate` 个样本，字节率 `44100/(256-rate)`——**这两句是错的，已由上方的音高修正条目推翻**（分母是 `clock/4`，芯片时钟必须参与）。采样在 ROM 的 **`0x00` 字节**处结束（没有 loop，end 寄存器形同虚设），gate 由它和键控位共同决定。测量不复用 `estimateCycleLength()`：GA20 字节率低，音乐音高是 2–10 字节的短周期（rail 见 8 字节最小延迟），且没有 loop 区可测，所以自己按「前半窗/后半窗各测一次、相差 ≤1 才算」测周期，并取「凹陷后的峰」而不是凹陷本身（否则方波会被报高一个八度）。**真实曲目**（Undercover Cops Stage 3，120 秒、8,333 次键开、7 个采样）实测全是打击乐：tom 的自相关凹陷在一个窗口读 87、另一个读 103（扫频），底鼓是 4 字节 ≈ 88 Hz，因此整首输出 C4 触发；旋律由同一首的 YM2151 承担。**这个结论已被上方的音高修正推翻**：字节钟错了 20 倍，修好之后同一批采样给出 G#2 的贝斯与 44–51 的旋律线。合成资产 `ga20.vgm`（方波 16 字节 → G4、噪声 → C4）+ `engine.ga20`，全量 ctest 29/29 |
| 2026-10-21 | QSound Logo 曲目补掉两处：①**ROM 上限**从 4 MB 改成 **8 MB**——芯片用 15 位 bank（`bank & 0x7FFF`）寻址 64 KB 页，最多 8 MB；这首曲子的采样在 `0x520000`/`0x640000`，被截断后每个声部都读到 0、测不出周期，整首曲子只剩 C4 触发。②**rate 写入的边沿**：驱动在一个持续音里反复改写 rate（颤音/滑音），旧代码「非零 rate 即键开」把 1 个音符炸成 5–36 个；现在只有「从静音到发声」的上升沿起音，播放中改 rate 视为滑音。两条各有单测（`a rate written while sounding slides the note`、`a sample at 0x480000 is reachable`）。修完同一采样的音符差与 rate 比例精确吻合（4576/4864/5440 → 74/75/77），SF2 曲目音符完全不变 |
| 2026-10-21 | 音符在**重触发**处切开：X-Men 的 Stage 6（`test_file/K054539`，82 秒、405 KB、1.1 万条 K054539 写入）把同一个采样按不同音高反复键开，若两次键开的音高相同、gate 又没落（循环或长采样），抽取器只会看到「gate 一直为真、音高没变」，于是整首曲子每轨只剩一个音符。`ChannelState` 因此新增 `keyOns`（键开次数，单调），C140/C352/K053260/K054539 各自递增；抽取器发现计数变化就关掉上一个音符、从这次键开重新起一个——这比「音高变了才换音符」更接近事实，对所有采样播放器通用（不填这个字段的芯片行为不变）。单测 `a retrigger at the same pitch is a second note` 钉住 |
| 2026-10-21 | **ADPCM-B 根音**（M4 遗留项）：YM2610/YM2608 的 ADPCM-B 从 one-shot 升级为可旋律。`DELTA-N` 是真正的音高寄存器，码率 = `delta * clock / 65536` 字节/秒（核心 ymdeltat 的 `step = delta * freqbase`，一字节两个 nibble）；`0x82` 块到手后按核心的解码表（B1/B2 步长）把 4-bit delta 积分成波形，再用 `estimateCycleLength()` 测周期，音符 = `delta * clock / 32768 / cycle`。键开在 `0x10`（OPNB）/`0x00`（OPNA port 1）的 bit7，地址按变体移位（OPNB 256 字节页、OPNA 32 字节页），`0x19/0x1A` = DELTA-N，`0x1B` = 电平——**电平写 0 就是收音符**（Neo Geo 的驱动每击前都这么写），所以 gate 由电平驱动，不再猜一个 80 ms 窗口。寄存器语义按 `fmopn.c` 与 `ymdeltat.c` 逐条核对。单测 5 条：A4（16 nibble 周期 + `DELTA-N` 29）、`DELTA-N` 加倍 = 八度、level 0 收音符、噪声仍是 C4 触发、OPNA/OPNB 地址移位。`test_file/neo_geo` 两首的 ADPCM-B 采样都测不出周期（打击乐/音效），因此仍按 C4 触发输出；有音高的曲目会自动变成真实音高。**Sax 段分析**（《KOF'98 Stage 1》1:39–2:09）：这一段是 ADPCM-B 的**流式播放**——30 秒内 1,898 次键开（约 63 次/秒），每次指向 `0x6A8D00`/`0x6B9000`/`0x6C9300`/`0x6D9600` 之一、长度恒为 66,303 字节，而 `DELTA-N` 每次不同（34,796…52,074，比值约 1–2 个半音）——**音高在 `DELTA-N` 里，采样只是被切片重放**。这解释了为什么「从 ROM 测周期」在这里失效：该速率下一个周期约两万个 nibble，而每次只有 16 ms 的片段，相关器永远看不到完整周期（它找到的是 54 kHz 的共振峰）。另外发现一个真 bug：`0x82` 块的地址掩码必须按**实际装入的数据量**（本案 2.8 MB → 4 MB）而不是头部声明的 8 MB 取 2 的幂，否则 `0x6B9000` 会读到 0（libvgm 的 `pow2_mask(memory_size)` 行为）；修好之后 Sax 采样才第一次测出周期。要真正给这种流式声部报音高，需要 §11.1 的「根音」模式：以某个参考 `DELTA-N` 为根、按 `12*log2(delta/deltaRef)` 出音，而不是逐次测周期 |
| 2026-10-22 | **M5 的三颗无音高芯片按设计不支持**（用户决定）：PWM、OKIM6258、uPD7759 不输出 MIDI。前两者是流式 DAC——VGM 记录的“寄存器写”就是音频本身，不存在音符事件；uPD7759 虽有 start 边沿与样本号，但整颗芯片没有音高，一个 C4 只会把语音当乐器。代码在 `LibvgmSongInstance` 里显式列出三者并返回 `unavailable`，使其在 `[13]` 中计为“已知缺口”而不是“有声却无声部”；覆盖维持 35 / 54 |
| 2026-10-22 | **C219 不写解码器，复用 C140**：`0xD4` 在 libvgm 里同时服务 C140 与 C219（chip type 同为 `0x1C`，`vgmplayer.cpp:1177`），而 219 的歌曲里**没有 C140 设备**——只找 `DEVID_C140` 会让每次写入落到空指针，音频照响、MIDI 全空。`MidiSequenceBuilder` 的 `case 0xD4` 因此先取 C140、再回退 C219；能力表把 `DEVID_C219` 记为 `fmChannel (16)`。这个三元表达式是 C219 能出音符的唯一原因，不要当成冗余清理掉 |

背景：libvgm 把 OPN 的 SSG 作为 linked AY8910（`opnintf.c:48-52`）；`LibvgmSongInstance::isTrackDevice()` 只保留 `parentIdx == (UINT32)-1` 的设备（`LibvgmSongInstance.cpp:31-36`），所以 YM2610/YM2608/YM2203 的 3 个 SSG 当前没有独立 mixer track。

后续如果要做，有三条路：

- **A. 维持现状**：SSG 不提供独立 MIDI 通道；这是第一版的选择。
- **B. MIDI-only lane**：在 mixer 中为 linked SSG 增加 3 条只有 MIDI、没有音频推子的轨道；它们不参与音频输出路由，只负责事件和通道选择。UI 需要明确区分“仅 MIDI”，避免用户误以为推子会改变声音。
- **C. 提升为真正可隔离的 track**：修改 `isTrackDevice`/mute 隔离/轨道枚举，把 linked SSG 变成和主设备一样的音频 track；需要验证不会重复出声、不会把主设备的 SSG 静音，风险最高，但它能同时解决音频和 MIDI 的通道级控制。

建议：如果将来确实需要 SSG 的 MIDI，先做 B（不改音频架构），C 只在需要 SSG 独立音频轨时再做。

## 14. 音高模型：移调量（L1）与根音（L2）

### 14.1 判定标准：先频率，后音符，永不看波形

采样声部的音高过去由「寄存器给出的播放率 ÷ 从 ROM 量出的采样周期」得出，这一步把**芯片说了什么**换成了**我们以为素材是什么**：周期测错的后果不是某个音错，而是整首歌恒定偏移，而且外观上与「素材本来就这样」无法区分。参考项目 MDPlayer 给的是另一条路，且已被验证可行：

- 音高一律先在**物理单位**里算出（Hz），参与运算的只有寄存器与有文档的硬件常量；
- 再由**唯一的**最近频率查找变成音符（`common.cs:453` 的 `searchYM2608Adpcm` / `searchSegaPCMNote`）；
- 采样内容**完全缺席**：ADPCM-B 的 `fSample = deltaN × 50kHz / 2^16` 里没有 ROM，它的 ADPCM 音高可视化因此永远可算。

本项目的口径对齐它，并把它拆成两层，因为「可算」与「正确」在采样芯片上不是同一件事。

### 14.2 两层定义

| 层 | 公式 | 来源 | 何时可用 |
|---|---|---|---|
| **L1 移调量** | `cents = 1200 · log2(rate / refRate)` | 寄存器 + 该芯片的**标称率** `refRate`（硬件常量：8 kHz DAC、`clock/pres` 的 delta-N 基准、采样率档位） | **永远**（只要 rate ≠ 0） |
| **L2 根音** | `rootCents` = 该采样在 `refRate` 下实际发声相对 C4 的偏离 | A 类：0（假设「标称率播放 = C4」）；B 类：只能从 ROM/RAM 实测，标 `sampleInferred` | 仅在能测出稳定单一周期时 |

- **默认输出 = L1**：`note = 60 + round(cents/100)`，余数进 pitch bend。等价于「所有采样都录在 C4」，与 MDPlayer 一致，永远不会因为测不出来而错音程。
- **打开「按根音移调」= L1 + L2**：`note` 由两层之和决定。设置项默认关。
- 两层都随 `NoteEvent` 一起带出（`transposeCents` / `rootCents` / `pitchSource`），所以切换开关是**渲染时**的决定，不需要重新解析歌曲。

`ChannelState` 与 `NoteEvent` 因此各加 `transposeCents`、`rootCents`、`pitchSource`，`ChannelState` 另加 `hz` 供诊断输出；`chips/Pitch.h` 提供 `transpositionCents()` 与 `quantiseTransposition()`，`quantisePlaybackRate()` 就是 L1 的旧名字（`rootRate` 即 `refRate`），不再是任何芯片的主路径候选。

### 14.3 每芯片归类与标称率

| 芯片 | 寄存器 → 频率 | `refRate`（L1 的参照率） | 类 | 状态 |
|---|---|---|---|---|
| C140 | `rate × clock / 18874368` | 8 kHz（标称采样率） | A | **P1 已切** |
| C219 | 与 C140 同一公式（同一核心的 `delta`） | 8 kHz（同上） | A | **M6 已切，复用 `C140Decoder`** |
| C352 | `rate × clock / 576` | `clock / 576`（一个字节每输出样本） | A | **P1 已切** |
| K053260 | `pitch × clock / 262144` | `clock / 64` | A | **P1 已切** |
| K054539 | `rate × clock / 缩放` | `clock / kOutputDivisor` | A | **P1 已切** |
| QSound | `rate × clock / 10223616` | `clock / kOutputDivisor` | A | **P1 已切** |
| SegaPCM | `pitch × clock / 32768` | `clock / 256`（pitch 128 的速率，驱动的「无移调」档） | A | **P1 已切** |
| ADPCM-B | `deltaN × (clock/144) / 65536`（nibble 率） | 8 kHz（8000/s，与参考项目的 `fSample/8000` 同口径） | B | **P2 已切，P2.1 修速率** |
| GA20 | `clock/4 / (256-rate)`（字节率） | 8 kHz（`kSampleReferenceRate`） | B | **P2 已切** |
| ES5503（多周期表） | `freq × fs / 2^(9+res-size)`（表步进） | **无**——寄存器走的是表，不是采样率 | B |`r`n| OKIM6295 | `clock / 165`（固定） | **无**——没有音高寄存器，音高整体来自采样周期 | B | **P2：不设 L1**，整首按实测周期出音，标 `sampleInferred` |

B 类的两种情形要分开看：GA20 与 ADPCM-B 的寄存器给出的是**每秒字节（nibble）数**，8 kHz 是这类 8-bit PCM 的通行录制率（参考项目也用它，`fSample / 8000`），所以 L1 有意义、`L1 + L2` 仍等于旧音高；ES5503 的寄存器给出的是**表内步进**（`freq = 512` 才是一样本一条目），任何「参考率」都是任意的（一条目每样本只有 25 Hz），所以它没有 L1，音高整体来自实测周期。

**参考率决定默认视图的八度**，这是有意的：C140 用 8 kHz（文档化的录制率）、SegaPCM 用 pitch 128（驱动的无移调档）、K053260 用「一字节每输出样本」——三者对同一个采样给出不同八度，但**同一采样内部的音程完全一致**，绝对八度由根音层负责。也因此，「关掉根音」时默认视图的八度是可预期的、跨曲稳定的，而不是随测量漂移。

A 类的 `refRate` 是文档常量，L1 即为绝对音高；B 类在默认模式下也输出 L1，但 `refRate` 只能取一个约定值（当前取「寄存器最慢档」的等效率），因此**八度不可信**，`pitchSource` 标 `sampleInferred`，UI 需要提示。

### 14.4 什么时候会退化成 one-shot（C4 触发）

三条，全部写进设置面板的说明文字：

1. 频率 / rate 寄存器为 0，或门控关闭；
2. 打开根音移调时，该采样的根音**测不出**，或测量出现**强度接近的多个竞争周期**（和弦/多复音采样）——此时不发明音高；
3. 算出的音高落在 MIDI 范围外，或高于 C8（108）——那是噪声或测量伪影。

### 14.5 设置项（P3 落地）

- 键：`pitch.useSampleRoot`（bool，默认 **false**），存 `SettingsStore`。
- 位置：Settings → MIDI 输出子面板（§11.1）。
- 文案要点（中英各一份，i18n）：
  - 标题：「按根音移调（可能不准确）」
  - 说明：「默认只按寄存器算出的移调量排布音高，同一采样内的音程永远准确。打开后会再加上采样自身的根音，用来对齐绝对八度——根音从采样数据里推断，可能偏差。」
  - 计算方式：「移调量 = 1200 × log2(播放率 / 标称率)；根音 = 采样在标称率下实际音高相对 C4 的偏离。」
  - 降级说明：「根音测不出、存在多个竞争周期、或音高超出范围时，该声部会输出为 C4 触发的一次性音（one-shot）。」

### 14.6 根音覆盖（暂不做，留接口）

用户可能要手动指定某个采样/某首曲子的根音。预留在 `NoteEvent.rootCents` 这一个字段上：将来由 `SessionState` 的每曲覆盖表给出，覆盖 `rootCents` 即可，不需要动解码器。见 `docs/memo/2026-10-22-sample-root-override.md`。

### 14.7 迁移阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | `Pitch.h` 加 L1 函数、`ChannelState`/`NoteEvent` 加字段、数据流贯通（**行为不变**） | `ctest` 全绿且 golden 输出逐字节不变 |
| P1 | 7 颗 A 类芯片的音高改走 L1，移除音高路径上的周期测量 | **已完成 6 颗**（C140、C352、K053260、K054539、QSound、SegaPCM；ADPCM-B 判为 B 类，随 P2）。每颗的 `noteFor` 只剩 `quantisePlaybackRate(rate, refRate)`，`channelState` 用 `describeSamplePitch()` 填两层；单测改断言「L1 音符 + rootCents + 两者之和等于旧音高」（`absoluteNote()`），合成资产的期望值随之改成 L1 音符 |
| P2 | GA20 / ES5503 / ADPCM-B 归位：GA20 与 ADPCM-B 用 8 kHz 折算的参照率走 L1（周期进 L2），ES5503 无 L1、整体来自实测周期 | **已完成**：`Ga20Decoder` 的 `noteFor` 只剩 `quantisePlaybackRate(rate, kSampleReferenceRate)`，ADPCM-B 同型（16,000 nibble/s），ES5503 保留 `quantiseFrequency(rate/cycle)` 并把来源标 `sampleInferred`；`ga20.vgm` 的期望由 67 改为 L1 的 58（`absoluteNote` 仍是 67）；ctest 31/31 |
| P3 | 设置项 + UI 文案 + 持久化 | **已完成**：`SettingsStore` 的 `pitch.useSampleRoot`（默认 false）→ `buildMidiSequence(..., transposeBySampleRoot)` → `emitDecodedNotes()` 在开时把 `note` 移到 `note + round(rootCents/100)`，余下的音分并进该音符自带的那个 bend（之后颤音不受影响）；UI 在 Settings → MIDI 页加开关与说明（计算方式、可能不准、三条 one-shot 规则、重新加载生效）。实测 `k053260.vgm`：关 48、开 69，差值正是该采样的根音 |
| P4 | `vgm_mixer_test --pitch-report`、跨 rate 一致性进 ctest | **已完成**：报告按轨列出 notes / 寄存器精确 / 带根音 / one-shot / 根音种类数（数据来自 `MidiEvent` 新带的 `transposeCents`、`rootCents`、`pitchSource`，由 `PlaybackEngine::getPitchSummary()` 聚合）；`pitch.k053260` / `pitch.es5503` / `pitch.ga20` 进 ctest。跨 rate 一致性由 `testC140RootIsStableAcrossRates` 钉住：同一采样两倍速率 → 根音完全相同、移调正好 +1200。报告里「一条轨出现多个根音」只提示不判失败——那通常是同一路播了多个采样 |
| P5 | 文档与决策记录 | **已完成**：§13 记下六条（寄存器优先、两层模型、参照率是每芯片常量、ES5503 无 L1、鼓仍 C4、根音覆盖留接口），§14 记录模型、每芯片表、三条 one-shot 规则与设置文案，CODEBUDDY.md 记下 `--pitch-report` 的用法 |

### 14.8 已知限制

1. **跨半音边界的颤音会把一个音拆成两个**。提取器按「音符号变化」切分（`NoteExtractor` 的 `current.note == note`），所以一条持续音在振动 ±50 音分以上穿过半音线时会拆开；L1 的整体偏移让某些曲子恰好落到边界上（`qsound.vgm` 的滑音用例因此按“不重新键开”断言）。改进方向是把 ±1 半音的连续变化并进 pitch bend，属于提取器的后续工作。
2. **B 类的默认八度是约定**。GA20/ADPCM-B 的 8 kHz、ES5503 的「实测周期即全体」都不是测量出来的录制率，跨曲稳定但不保证与听感同八度；要绝对八度就开根音。
3. **根音只和测量窗一样好**。GA20 的滑音采样取第一个测出周期的 512 字节窗，测量误差直接变成整首的常量偏移——这是开关默认关闭的原因。
4. **一个键开一个音**。采样内部的和弦/多复音不会被拆开；测不出稳定单一周期时判为 unpitched（C4），不发明音高。

## 15. 参考位置

- MIDI 能力已有：`plugin/CMakeLists.txt:166-171`、`PluginProcessor.h:33-37`
- `processBlock` 清空输入：`PluginProcessor.cpp:110-117`
- 通道条布局/高度：`ChannelStrip.h:29`、`ChannelStrip.cpp:16-25`、`:263-313`
- 混音器重建/刷新：`MixerPanel.cpp:113-141`、`:209-275`
- 轨道信息/后端接口：`playback/SongBackend.h:67-77`、`:152-176`
- 轨道状态/持久化：`PlaybackEngine.h:213-223`、`SessionState.h:15-23`、`SessionState.cpp:76-85`
- linked SSG：`libs/libvgm/emu/cores/opnintf.c:48-52`、`LibvgmSongInstance.cpp:31-36`
- YM2610 通道/写寄存器：`libs/libvgm/emu/cores/opnintf.c:235-251`、`fmopn.c:4104-4205`
- YM2610 非 B FM 通道：`fmopn.c:3621-3624`；B：`:3798-3803`
- JUCE VST3 MIDI 输出：`juce_audio_plugin_client_VST3.cpp:3733-3735`
- JUCE AU MIDI 输出：`juce_audio_plugin_client_AU_1.mm:729-744`、`AUv3.mm:1647`
- 参考项目钢琴/FM 算法：`reference/imgui-vgmplayer-v0.44/examples/example_vgm_player/main.cpp:258-397`、`:483-637`
- MDPlayer 的音高做法（本次标准来源）：`reference/MDPlayer-stable/MDPlayer/MDPlayerx64/common.cs:453-469`（频率→音符取最近值）、`PianoRoll/YM2610.cs:162-166`（FM：`fnum × clock / (24·div·2^21) × 2^(oct+2)`）、`form/KB/OPL/frmY8950.cs:265-272`（ADPCM：`deltaN × 50kHz / 2^16`，再除以 8 kHz 取音符）

## 附录 B：后续里程碑

设计文档写完并不等于目标记得住——§11 的 P1–P5 只是“一次交付”的阶段划分，真正耗时的是 §11.2 那 54 项芯片覆盖。这里把它排成可验收的里程碑：**每做完一项，先回来改这里的「状态」和 §11.2 的现状表，再提交**。状态只有三种：完成 / 进行中 / 未开始。

**当前基线（2026-10-22）**：P1–P4 完成，P5 进行中（M2、M3 完成，M4 **完成**——C140、C352、K053260、K054539、QSound、GA20、SEGAPCM、ES5503、OKIM6295 全部落地；M5 **取消**（PWM、OKIM6258、uPD7759 无音高，按设计不输出 MIDI）；MIDI 设备类型 33、覆盖 35 / 54，YMZ280B 已搁置不计，口径见 §11.2）；`vgm_core_test`、`vgm_chip_test`、`vgm_opn_test` 通过，`ctest` **34/34** 通过（含 `pitch.k053260`/`pitch.es5503`/`pitch.ga20` 三个音高审计用例），`vgm_mixer_test` 的 psg / opl4 / minimal / wswan / ym2610 / fm_alg0 / dac / loop / x1 / opx / c140 / c352 / k053260 / k054539 / qsound / ga20 / segapcm / es5503 **十八个资产全部进了 ctest**（`engine`、`engine.wswan`、`engine.dac`、`engine.loop`、`engine.x1`、`engine.opx`、`engine.c140`、`engine.c352`、`engine.k053260`、`engine.k054539`、`engine.qsound`、`engine.ga20`、`engine.segapcm`、`engine.es5503`、`engine.psg`、`engine.opl4`、`engine.ym2610`、`engine.fm_alg0`）。`ymz280b.vgm` 不在其中：它的解码器能出声也能报 4 个音符，但真实 rips 只有一路，进 ctest 会让“一套资产各自证明什么”变得含糊（见 §11.2 行 36）。另有两首真实 C352 曲目（`test_file/c352` 的 Galaga 两首）手动验证过：各 3 轨有声、21/20/12 个音符，音高是同一段下行三度旋律的 C 大调与 A 大调版本——它们不进 ctest，因为 `test_file/` 不入库。另有一首 K054539 真实曲目（`test_file/K054539`，X-Men 的 Midway Demo，同时带 YM2151）验证过：K054539 六条有声轨里 Ch 1–3 是带循环的低音、Ch 4–6 是鼓组（C4 触发），并因此修掉了「平滑起音被当成 8 样本周期」的共用测算问题（`estimateCycleLength()` 现在要求峰前有相关性低谷）。

| 里程碑 | 内容 | 验收 | 状态 |
|---|---|---|---|
| M1 | 基础版：`MidiCapability`/事件序列/每轨 MIDI 通道/Master 合并/VST3 输出/通道条 UI（P1–P4） | `PlaybackEngineTest` `[10]`–`[12]` 全绿；`vgm_mixer_test --notes` 覆盖 YM2610/YM2612/YM2151/AY/SN | 完成 |
| M1.1 | 收尾既有红灯：`ym2610.vgm` 的 4 条混音器检查已修（`[8]` 改成先用 MIDI 侧找出真正有声的轨，RPN 检查改从该轨开始，不再把唯一一次 RPN 花在静音轨上）；`[12]` 缺的循环资产补上了（`loop.vgm`：50 ms 前导 + 20 个音符 × 2 s 循环体，header 0x1C/0x20 声明循环点，0x7E 的 loop base 显式为 0） | `ctest -R engine.loop` 全绿并打印 `loop at 0.05 s of 2.05 s: 20 note-on in the first pass, 19 after it`；把 `songTimeWithinPass()` 的折回去掉后 `[12]` 立刻变红（`20 / 0`）；`loop.vgm` 的 loop 点被清零时 `[12]` 报 `DID NOT RUN` 而不是跳过 | 完成 |
| M1.2 | YM2612 DAC 流的 MIDI：`0x8n` 只带等待值（1 字节），样本字节在 type `0x00` 的 `0x67` 数据块里。`VgmCommandReader` 现在保留该 bank（含 `0xE0` 寻址），`0x8n` 取下一字节写成 `0x2A` 寄存器写 + 自带等待 | 真实曲目 `06 Star Light Zone.vgz` 的 DAC 轨从 0 → 3 个音符；`dac.vgm`（爆发/静音交替）在 `[13]` 下 ≥10 个音符（实测 19）；`[11]` 逐字节比对 bank 内容与时刻；`ctest -R engine.dac` 全绿 | 完成 |
| M1.3 | 期望值交给芯片（§10.2）：`vgm_opn_test` 用 libvgm 的 YM2612 核测每个通道是否出声，解码器的 `gate` 必须一致；`PlaybackEngineTest` `[13]` 再加一条“有声的轨必须有音符”的端到端不变量 | `ctest -R "chip\|opn"` 全绿；把 `totalLevel` 位序改回旧写法后 `vgm_opn_test` 必须变红；`vgm_mixer_test` 五个资产 + `fm_alg0.vgm` 的 `[13]` 全绿 | 完成 |
| M2 | A 批波形 PSG：X1_010（周期→音高，复用 `chips/Pitch.h`） | 按 §11.2「验收」四条；24 → 25 / 54。`vgm_chip_test` 四条（A4 的 6.10 值、分频位、PCM one-shot、0x80 以上偏移不按键）；`x1.vgm` 在 `[13]` 下 Ch1 有声（rms 0.0306）且报 4 个 A4 音符，`ctest -R engine.x1` 全绿 | 完成 |
| M3 | A 批 FM：YMF271；**YMZ280B 搁置** | 同上；25 → 26 / 54（不含 YMZ280B）。`vgm_chip_test` 的七条 OPX 用例覆盖音高、算法决定载体、组电平静音、block→八度、sync 1 成对复制、PCM one-shot 与 PCM 四路独立；`opx.vgm` 在 `[13]` 下 group 0/1 各报 4 个音符（A4/A5）、rms 0.318，`ctest -R engine.opx` 全绿。YMZ280B 的解码器/单测已写但**不计入覆盖、不进 ctest**：vgmrips 上这颗芯片的成套录音实际是“一条长样本、一路发声”，没有可断言的每声部音符。详见 `docs/memo/2026-10-20-ymz280b-shelved.md`（含重启条件） | 完成 |
| M4 | 采样播放器音高体系：C140、C352、K053260、K054539、QSound、GA20、ES5503、OKIM6295、SEGAPCM，并把 ADPCM-B 根音（§11.1）一起纳入 | 「步进/采样率→音分 + 根音」是一个共用工具而非九份私有换算；**进行中**：共用工具 `chips::quantisePlaybackRate()` 与 `chips::estimateCycleLength()` 已随 C140/C352/K053260 落地（根音是参数；采样周期从 ROM 测），C140、C352、K053260、K054539、QSound、GA20、SEGAPCM、ES5503 完成、其余一项（OKIM6295）未开始；ADPCM-B 根音已完成（见决策记录）；34 → 35 / 54 | 进行中 |
| M5 | ~~PWM、uPD7759、OKIM6258~~ **已取消** | 2026-10-22 决定：三者无音高，不输出 MIDI；覆盖保持 35 / 54，A 批只剩搁置的 YMZ280B | 取消 |
| M6 | C 批 13 项（SCSP、ES5506、C219、MIKEY、K007232、K005289、MSM5205、MSM5232、BSMT2000、ICS2115、VBOY_VSU、YMW258、YM2414）——不在 54 内，但机制与 A 批相同 | 每项一个解码器 + `vgm_chip_test` 用例 | 未开始 |
| M7 | B 批：新格式（NSF/NSFE、SPC、HES、KSS…）+ 9 个真芯片，FME7 复用 AY、VRC7 复用 YM2413、VRC6 脉冲复用 NES | 54 / 54 达成 §11.2 目标 | 未开始 |
| M8（并行，不阻塞 M2–M7） | Settings → MIDI 输出子面板（§11.1）：one-shot note 映射、ADPCM-B 根音/melodic、standalone 设备、默认通道 | 设置项持久化 + `vgm_ui_test` 的布局断言 | 未开始 |
| M9（按需） | linked SSG（附录 A 的 B 方案：只 MIDI、不带音频推子的 lane） | YM2610/2608/2203 的 3 个 SSG 有独立 MIDI 通道且不影响音频 | 未开始 |

两条排期规则，避免重演“写完文档就忘了目标”：M2–M5 是纯加法，每项不超过一天的量，按 §11.2 优先级顺序推进，不要跳着做；M7 一旦开始就意味着要碰 `VgmCommandReader` 之外的新格式解析，先把格式读取与解码器分开落地（参照 `backends/libvgm` 的现状），否则 MIDI 侧会被格式工作拖住。

## 16. 装配层的重构计划（2026-10-22）

**问题**：MidiSequenceBuilder.cpp 1167 行，25 个 `ChipBuffer<>` 成员、22 次 `flushDecoded`、10 处 `assembleRom`、52 个 `case 0xNN`。加一颗芯片要改五处：解码器、builder 接线、`LibvgmSongInstance` 能力表、`CMakeLists`、文档表行。M6 还要加 11 颗，照这个写法文件会超过 2000 行。

**目标结构**：

- `chips/ChipProfile.h`：每芯片一行——VGM 命令、默认时钟、ROM 块与大小、MIDI 能力、解码器工厂；
- `chips/ChipSession.h`：类型擦除的缓冲区（`queue` / `flush` / `channelCount`），builder 不再为每颗芯片写一个具名成员；
- builder 退化为“命令 → profile → 投递”，预计 ~400 行；`LibvgmSongInstance` 的能力 switch 并入 profile。

**三个提交**：

1. 引入 `ChipSession` / `ChipProfile` 与新表，先迁移 C140（A 类）、OKIM6295（无音高）、ES5503（无 L1）各一颗，要求 `--notes` 输出逐字节不变；
2. 迁移其余 22 颗，删除具名缓冲区；
3. 能力表并入 profile，删掉 `LibvgmSongInstance` 里的 switch。

**验证**：`plugin/Tests/assets/*.vgm` 与 `test_file/` 样本的 `--notes` 前后 diff 必须为空；`ctest` 34/34。

**注释约定**（新增解码器必须遵守）：

- 每个 `case 0xNN` 上方写明该寄存器的硬件名称与位含义，禁止“寄存器 0”这类同义反复；
- 同组寄存器共享一张表时（如 C140 的每声道 16 字节），在 switch 前给出整表，case 内只写差异；
- 魔法常量必须带出处（`libvgm` 核心文件与行号，或数据手册页码）。
