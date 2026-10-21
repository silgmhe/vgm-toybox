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
| YM2610 / YM2610B | FM：key on/off + FNUM/BLOCK + TL/pan；ADPCM-A/B：one-shot trigger |
| YM2612 | 6 FM；DAC 为 one-shot |
| YM2608 | 6 FM；ADPCM-A/B one-shot（SSG 见 §6.3） |
| YM2203 | 3 FM（SSG 见 §6.3） |
| YM2151 | 8 FM |
| YM2413/YM3526/YM3812/YMF262 | FM；rhythm 通道 one-shot |
| AY8910 / SN76489 | tone 为 melodic；noise 为 one-shot |
| NES APU / GB DMG | pulse/triangle/wave melodic；noise/DMC one-shot |
| C6280 / K051649 / SAA1099 / POKEY / WSWAN | 波形/方波 PSG：周期→音高；噪声按速率取鼓键；声音 DMA 与 16 位成对同样由解码器自己处理 |
| 其他 | 暂不产生事件，`midiCapability = unavailable` |

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
| ADPCM-B | 60 |
| YM2612 DAC | 60 |
| noise | 60 |
| rhythm（OPLL/OPL） | 60 |

- 这些 note **只是触发标识**，不代表采样本身的真实音高；不要把 60 当成鼓映射，也不要声称它还原了采样音高；
- 用户如果需要在 DAW 里做鼓映射，可以自行加 MIDI 效果器；插件不内置 drum map；
- ADPCM-B v1 按 one-shot 处理；后续在 Settings → MIDI 输出里提供根音/采样率覆盖或 melodic 模式；根音换算的共用件 `chips::quantisePlaybackRate (rate, rootRate, finePitch)` 已随 C140 落在 `chips/Pitch.h`，根音是参数，ADPCM-B 接 melodic 时只需提供自己的 rate 与 rootRate；
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
| YM2610 | 0–5（非 B 只有 1/2/4/5） | 6–12 | 6–11 ADPCM-A、12 ADPCM-B |
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

后续在设置面板增加一个 **MIDI 输出** 子页，集中放可配置项，不把这些控件塞进通道条：

- one-shot 通道的 trigger note 映射（ADPCM-A/B、DAC、noise；默认 C4，允许改 note）；
- ADPCM-B 的根音/采样率覆盖，或切换为 melodic；
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
| MIDI 设备类型（`midiCapabilityFor` 的 `case` 数，含已搁置的 YMZ280B） | 28 |
| 上表对应的 MDPlayer 主表条目 | 30 / 54 |
| 有音频核心、缺解码器 | 8（另有 YMZ280B：解码器已写但搁置） |
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
| 5 | PWM | 32X_PWM | — | 只是 DAC 数据流，没有音高寄存器，天生只能 one-shot |
| 6 | C140 | C140 | 旋律 ×24 | 采样播放器，有音高。频率寄存器是播放率：`frequency * clock / 288 / 65536` 字节每秒。**音符由采样自身周期决定**：键开时在 loop 区（无 loop 则前 2 KB）做自相关，取「值 ≥ 最强局部峰 −0.02 的最短 lag」再按「loop 是整数个周期」吸附，得到该采样一个周期的字节数，音符 = `播放率 / 周期`（`chips/C140Decoder.h`）。周期测不出来的（鼓/噪声、μ-law、无 ROM）才退回 `chips::quantisePlaybackRate()` 的 8 kHz 根音（根音是参数，§11.1；时钟翻倍即八度）。ROM 来自 `0x67` 的 `0x8D` 块（`MidiSequenceBuilder::assembleRom()` 按各块自带的 ROM 偏移拼），寻址按头字节 `0x96` 的 banking 折叠——System 2 是 `((adrs&0x200000)>>2)|(adrs&0x7ffff)`，**不折叠的话 1 MB ROM 里根本没有 bank 0x14，所有音符会静默退回根音**。结束地址（`0x18/19`）决定音符长度，键开时与 `0x1A/1B` 一起锁存——核心就是这么复制 start/end 的，所以中途改结束地址只影响下一个键。loop 位（`0x15` 的 bit 4）让指针回卷，越过结束地址也不停，只有写 mode 去掉 bit 7（驱动的 key-off）能结束。`0xD4` 是 16 位大端偏移，高字节落在 `port` 里，所以 `0x100` 以上的声部要靠两半拼回；`c140.vgm` 用 voice 0/16 差八度、两种结束方式各 4 个音符，`[13]` 实测 2 轨各 4 音符、rms 0.17 |
| 7 | OKIM6258 | MSM6258 | — | ADPCM，音高只有采样率档位，无连续音高 |
| 8 | OKIM6295 | MSM6295 | — | 采样播放器，有音高 |
| 9 | SEGAPCM | SEGAPCM | — | 采样播放器，有音高 |
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
| 34 | uPD7759 | uPD7759 | — | 语音合成器，无音高，只能 one-shot |
| 35 | YMF271 | YMF271 | 旋律 ×12 | 4 算子 FM，12 个 group × 4 个 bank，4 种 sync 模式（0 = 四算子一组，写 bank 0 即复制到其余三个；1 = 两两成对；3 = 四路独立 PCM）。VGM 命令是 `0xD1`（端口字节 bit 7 是芯片实例、低 7 位是 bank），读取器把 bank 留在 `port` 里传下来——忽略它就会漏掉「只有 bank 0 的写会传播」这条。地址低四位是 group，3/7/11/15 是空洞。核心在 ROM 未分配时连 FM 组也不发声，所以资产要带 `0x67` type `0x85` 的 ROM 块；`opx.vgm` 用 group 0/1 各出 A4/A5，`[13]` 实测 Ch1/Ch2 各 4 个音符、rms 0.318 |
| 36 | YMZ280B | YMZ280B | 触发 ×8（**搁置**） | 采样播放器。解码器已写但**不计入覆盖、不进 ctest**：vgmrips 上这颗芯片的成套录音实际是“一条长样本、一路发声”，没有可断言的每声部音符。见 `docs/memo/2026-10-20-ymz280b-shelved.md` |
| 37 | QSound | QSOUND | — | 采样播放器，有音高 |
| 38 | GA20 | GA20 | — | 采样播放器，有音高 |
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
| 51 | ES5503 | ES5503 | — | 采样播放器，有音高 |
| 52 | PCM8 | ✗ | — | 软件 PCM 驱动 |
| 53 | MPCMX68k | ✗ | — | X68000 板载 PCM |
| 54 | CS4231 | ✗ | — | PC 声卡 |

**三批工作**

- **A. 有核心、缺解码器（9 项）**：只动 `plugin/Source/chips/` + `midiCapabilityFor` + 测试，不需要改 libvgm，也不需要新格式。其中 8 项真缺解码器：5 项可旋律化，PWM 与 uPD7759 天生只能 one-shot，OKIM6258 只有采样率档位；另 1 项 YMZ280B 的解码器已写但**搁置**（memo 见 §11.2 行 36），不计入覆盖。C140（表行 6）、C352（表行 24）、K053260（表行 39）与 K054539（表行 40）已从这一批划掉。
- **B. 无音频核心（15 项）**：9 个真芯片（FDS、MMC5、VRC7、N163、VRC6、FME7、SID、MultiPCM、YM2609）+ 6 个驱动/板卡（PPZ8、PPSDRV、PCM8、MPCMX68k、CS4231、P86）。它们主要出现在本插件目前不读的格式（NSF/NSFE、SPC、HES、KSS…）或驱动级回放里，所以 B 批实际是「新格式 + 新核心」的工程，不是 MIDI 输出的延伸。有核心之后 MIDI 侧很便宜：FME7 复用 AY、VRC7 复用 YM2413、VRC6 脉冲复用 NES。
- **C. 已超出（13 项）**：SCSP、ES5506、C219、MIKEY、K007232、K005289、MSM5205、MSM5232、BSMT2000、ICS2115、VBOY_VSU、YMW258、YM2414。不计入对齐，但每项同样要一个解码器才能出 MIDI，机制与 A 批相同。

**优先级**

1. 方波/波形 PSG：X1_010——都是「周期→音高」，`chips/Pitch.h` 的曲线直接可用，单个解码器约 100–200 行，测试全在 `vgm_chip_test` 里。（HuC6280、K051649、SAA1099、POKEY、WSwan 已按此路径完成。）
2. FM：YMF271——已完成（见 §11.2 表行 35）。（YMF278B 的 FM 半已按此路径完成，见 §11.2 表行 22。YMZ280B 原属这一批，已**搁置**：vgmrips 的成套录音是“一条长样本、一路发声”，验收条件③“真实曲目的 note-on 数非零”只能得到 1 个音符，见 `docs/memo/2026-10-20-ymz280b-shelved.md`。）
3. 带音高的采样播放器：C140（**已完成**，见 §11.2 表行 6）、C352（**已完成**，见 §11.2 表行 24）、K053260（**已完成**，见 §11.2 表行 39）、K054539（**已完成**，见 §11.2 表行 40）、QSound、GA20、ES5503——音高来自步进/采样率，需要「步进→音分 + 根音」的换算，与 ADPCM-B 的根音问题同型（§11.1）。C140 与 C352 已经把两件共用件放进 `chips/Pitch.h`：`chips::quantisePlaybackRate (rate, rootRate, finePitch)` 把根音做成参数而不是常量，`chips::estimateCycleLength (waveform, loopOffset, loopLength)` 从 ROM 里测出一个周期的长度（自相关取最强局部峰，再按 loop 是整数个周期吸附，识别不出的返回 0）——M4 余下的七项只提供自己的 rate、根音与采样波形即可，不要再各写一份换算或测周期。
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

背景：libvgm 把 OPN 的 SSG 作为 linked AY8910（`opnintf.c:48-52`）；`LibvgmSongInstance::isTrackDevice()` 只保留 `parentIdx == (UINT32)-1` 的设备（`LibvgmSongInstance.cpp:31-36`），所以 YM2610/YM2608/YM2203 的 3 个 SSG 当前没有独立 mixer track。

后续如果要做，有三条路：

- **A. 维持现状**：SSG 不提供独立 MIDI 通道；这是第一版的选择。
- **B. MIDI-only lane**：在 mixer 中为 linked SSG 增加 3 条只有 MIDI、没有音频推子的轨道；它们不参与音频输出路由，只负责事件和通道选择。UI 需要明确区分“仅 MIDI”，避免用户误以为推子会改变声音。
- **C. 提升为真正可隔离的 track**：修改 `isTrackDevice`/mute 隔离/轨道枚举，把 linked SSG 变成和主设备一样的音频 track；需要验证不会重复出声、不会把主设备的 SSG 静音，风险最高，但它能同时解决音频和 MIDI 的通道级控制。

建议：如果将来确实需要 SSG 的 MIDI，先做 B（不改音频架构），C 只在需要 SSG 独立音频轨时再做。

## 附录 B：后续里程碑

设计文档写完并不等于目标记得住——§11 的 P1–P5 只是“一次交付”的阶段划分，真正耗时的是 §11.2 那 54 项芯片覆盖。这里把它排成可验收的里程碑：**每做完一项，先回来改这里的「状态」和 §11.2 的现状表，再提交**。状态只有三种：完成 / 进行中 / 未开始。

**当前基线（2026-10-21）**：P1–P4 完成，P5 进行中（M2、M3 完成，M4 进行中——C140、C352、K053260、K054539 已落地，其余五项未开始；MIDI 设备类型 28、覆盖 30 / 54，YMZ280B 已搁置不计，口径见 §11.2）；`vgm_core_test`、`vgm_chip_test`、`vgm_opn_test` 通过，`ctest` 27/27 通过，`vgm_mixer_test` 的 psg / opl4 / minimal / wswan / ym2610 / fm_alg0 / dac / loop / x1 / opx / c140 / c352 / k053260 / k054539 **十四个资产全部进了 ctest**（`engine`、`engine.wswan`、`engine.dac`、`engine.loop`、`engine.x1`、`engine.opx`、`engine.c140`、`engine.c352`、`engine.k053260`、`engine.k054539`、`engine.psg`、`engine.opl4`、`engine.ym2610`、`engine.fm_alg0`）。`ymz280b.vgm` 不在其中：它的解码器能出声也能报 4 个音符，但真实 rips 只有一路，进 ctest 会让“一套资产各自证明什么”变得含糊（见 §11.2 行 36）。另有两首真实 C352 曲目（`test_file/c352` 的 Galaga 两首）手动验证过：各 3 轨有声、21/20/12 个音符，音高是同一段下行三度旋律的 C 大调与 A 大调版本——它们不进 ctest，因为 `test_file/` 不入库。另有一首 K054539 真实曲目（`test_file/K054539`，X-Men 的 Midway Demo，同时带 YM2151）验证过：K054539 六条有声轨里 Ch 1–3 是带循环的低音、Ch 4–6 是鼓组（C4 触发），并因此修掉了「平滑起音被当成 8 样本周期」的共用测算问题（`estimateCycleLength()` 现在要求峰前有相关性低谷）。

| 里程碑 | 内容 | 验收 | 状态 |
|---|---|---|---|
| M1 | 基础版：`MidiCapability`/事件序列/每轨 MIDI 通道/Master 合并/VST3 输出/通道条 UI（P1–P4） | `PlaybackEngineTest` `[10]`–`[12]` 全绿；`vgm_mixer_test --notes` 覆盖 YM2610/YM2612/YM2151/AY/SN | 完成 |
| M1.1 | 收尾既有红灯：`ym2610.vgm` 的 4 条混音器检查已修（`[8]` 改成先用 MIDI 侧找出真正有声的轨，RPN 检查改从该轨开始，不再把唯一一次 RPN 花在静音轨上）；`[12]` 缺的循环资产补上了（`loop.vgm`：50 ms 前导 + 20 个音符 × 2 s 循环体，header 0x1C/0x20 声明循环点，0x7E 的 loop base 显式为 0） | `ctest -R engine.loop` 全绿并打印 `loop at 0.05 s of 2.05 s: 20 note-on in the first pass, 19 after it`；把 `songTimeWithinPass()` 的折回去掉后 `[12]` 立刻变红（`20 / 0`）；`loop.vgm` 的 loop 点被清零时 `[12]` 报 `DID NOT RUN` 而不是跳过 | 完成 |
| M1.2 | YM2612 DAC 流的 MIDI：`0x8n` 只带等待值（1 字节），样本字节在 type `0x00` 的 `0x67` 数据块里。`VgmCommandReader` 现在保留该 bank（含 `0xE0` 寻址），`0x8n` 取下一字节写成 `0x2A` 寄存器写 + 自带等待 | 真实曲目 `06 Star Light Zone.vgz` 的 DAC 轨从 0 → 3 个音符；`dac.vgm`（爆发/静音交替）在 `[13]` 下 ≥10 个音符（实测 19）；`[11]` 逐字节比对 bank 内容与时刻；`ctest -R engine.dac` 全绿 | 完成 |
| M1.3 | 期望值交给芯片（§10.2）：`vgm_opn_test` 用 libvgm 的 YM2612 核测每个通道是否出声，解码器的 `gate` 必须一致；`PlaybackEngineTest` `[13]` 再加一条“有声的轨必须有音符”的端到端不变量 | `ctest -R "chip\|opn"` 全绿；把 `totalLevel` 位序改回旧写法后 `vgm_opn_test` 必须变红；`vgm_mixer_test` 五个资产 + `fm_alg0.vgm` 的 `[13]` 全绿 | 完成 |
| M2 | A 批波形 PSG：X1_010（周期→音高，复用 `chips/Pitch.h`） | 按 §11.2「验收」四条；24 → 25 / 54。`vgm_chip_test` 四条（A4 的 6.10 值、分频位、PCM one-shot、0x80 以上偏移不按键）；`x1.vgm` 在 `[13]` 下 Ch1 有声（rms 0.0306）且报 4 个 A4 音符，`ctest -R engine.x1` 全绿 | 完成 |
| M3 | A 批 FM：YMF271；**YMZ280B 搁置** | 同上；25 → 26 / 54（不含 YMZ280B）。`vgm_chip_test` 的七条 OPX 用例覆盖音高、算法决定载体、组电平静音、block→八度、sync 1 成对复制、PCM one-shot 与 PCM 四路独立；`opx.vgm` 在 `[13]` 下 group 0/1 各报 4 个音符（A4/A5）、rms 0.318，`ctest -R engine.opx` 全绿。YMZ280B 的解码器/单测已写但**不计入覆盖、不进 ctest**：vgmrips 上这颗芯片的成套录音实际是“一条长样本、一路发声”，没有可断言的每声部音符。详见 `docs/memo/2026-10-20-ymz280b-shelved.md`（含重启条件） | 完成 |
| M4 | 采样播放器音高体系：C140、C352、K053260、K054539、QSound、GA20、ES5503、OKIM6295、SEGAPCM，并把 ADPCM-B 根音（§11.1）一起纳入 | 「步进/采样率→音分 + 根音」是一个共用工具而非九份私有换算；**进行中**：共用工具 `chips::quantisePlaybackRate()` 与 `chips::estimateCycleLength()` 已随 C140/C352/K053260 落地（根音是参数；采样周期从 ROM 测），C140、C352、K053260、K054539 完成、其余五项未开始；30 → 35 / 54 | 进行中 |
| M5 | 无音高 one-shot：PWM、uPD7759、OKIM6258 | A 批只剩搁置的 YMZ280B；35 → 38 / 54 | 未开始 |
| M6 | C 批 13 项（SCSP、ES5506、C219、MIKEY、K007232、K005289、MSM5205、MSM5232、BSMT2000、ICS2115、VBOY_VSU、YMW258、YM2414）——不在 54 内，但机制与 A 批相同 | 每项一个解码器 + `vgm_chip_test` 用例 | 未开始 |
| M7 | B 批：新格式（NSF/NSFE、SPC、HES、KSS…）+ 9 个真芯片，FME7 复用 AY、VRC7 复用 YM2413、VRC6 脉冲复用 NES | 54 / 54 达成 §11.2 目标 | 未开始 |
| M8（并行，不阻塞 M2–M7） | Settings → MIDI 输出子面板（§11.1）：one-shot note 映射、ADPCM-B 根音/melodic、standalone 设备、默认通道 | 设置项持久化 + `vgm_ui_test` 的布局断言 | 未开始 |
| M9（按需） | linked SSG（附录 A 的 B 方案：只 MIDI、不带音频推子的 lane） | YM2610/2608/2203 的 3 个 SSG 有独立 MIDI 通道且不影响音频 | 未开始 |

两条排期规则，避免重演“写完文档就忘了目标”：M2–M5 是纯加法，每项不超过一天的量，按 §11.2 优先级顺序推进，不要跳着做；M7 一旦开始就意味着要碰 `VgmCommandReader` 之外的新格式解析，先把格式读取与解码器分开落地（参照 `backends/libvgm` 的现状），否则 MIDI 侧会被格式工作拖住。

## 14. 参考位置

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
