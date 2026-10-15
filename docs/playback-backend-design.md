> 设计文档：播放后端解耦（中间层标准）
> 里程碑：**M2.5（插入里程碑，位于 M2 之后、下一功能"多轨 Audio Output"之前）**
> 分支：`feature/multitrack-output`（或新开 `feature/playback-backend`）
> 日期：2026-10-15
> 状态：**待评审**（尚未实现）

---

## 1. 需求与约束

### 1.1 功能性需求

| # | 需求 |
|---|---|
| F1 | 播放引擎核心逻辑（混音、轨道模型、传输控制、时间线）**只依赖一个中间层标准**，不依赖 libvgm |
| F2 | libvgm 成为该标准的**第一个适配器**，行为与现在**逐样本一致** |
| F3 | 未来可加入其它格式后端（首个目标：SNES `.spc`，因硬件机制无法转成 VGM/VGZ） |
| F4 | 面向前端（UI）的 API、混音器行为、既有功能**不变**；新增格式只是"多一个后端" |
| F5 | 后端的能力差异（能否 seek / 有无循环点 / 能否按通道隔离 / 原生采样率）必须**可表达、可降级** |
| F6 | 为下一个功能"多轨 Audio Output"预留：轨道级渲染与隔离是标准的**一等公民** |

### 1.2 非功能性需求

- **实时安全**：音频线程 `render()` 路径不得加锁、分配、日志（现有约定，见 §2.3）。
- **行为零回归**：现有验收测试（`vgm_mixer_test`，含"求和与单实例逐样本一致 −1000 dB"）必须保持通过且数值不变。
- **性能**：每块一次虚函数分派，开销可忽略；不得引入每帧/每块的额外拷贝。
- **可增量交付**：重构过程任何时候分支都可用（每步都能编译、能跑、能听）。
- **可验证的解耦**：能机械地证明"核心不含 libvgm"（源码级守卫），而不是靠约定。

### 1.3 约束与假设

- 团队 1 人（作者本人），C++17 + JUCE 8，libvgm 为子模块；构建脚本 `build.ps1` + 离线测试 `vgm_mixer_test`。
- **交付形态是 VST3 插件（+ 独立运行）**：所有后端随插件二进制一起编译发布，用户拿到的就是"装进 DAW 的插件"，因此**不做运行期后端加载/插件化**（见 ADR-008）。这同时意味着后端数量与二进制体积、许可合规都在**发布时**一次性决定。
- 后端可**编译期可选**：SPC 后端可能受许可影响（SNESAPU 闭源 / snes9x GPL），因此注册表必须支持"这个后端这次没编进来"（条件编译开关，而不是运行期发现）。
- 本里程碑**不实现任何新格式**，也不做多轨输出；只做解耦与标准落地。

---

## 2. 现状耦合分析

### 2.1 直接耦合点（include 级，只有 6 个文件）

| 文件 | 引入的 libvgm 头 |
|---|---|
| `vgm/MixerEngine.h` | `stdtype.h`（**公共头，泄漏最广**） |
| `vgm/MixerEngine.cpp` | `emu/SoundDevs.h`、`emu/SoundEmu.h`、`player/vgmplayer.hpp`、`utils/MemoryLoader.h` |
| `vgm/TrackPlayer.h` | `stdtype.h`、`player/playera.hpp`、`utils/DataLoader.h` |
| `vgm/TrackPlayer.cpp` | `droplayer/gymplayer/s98player/vgmplayer.hpp`、`utils/MemoryLoader.h` |
| `vgm/TrackLayout.h` | `stdtype.h`、`player/playerbase.hpp` |
| `vgm/TrackLayout.cpp` | `emu/SoundEmu.h` |

UI / `PluginProcessor` **不直接** include libvgm，但通过 `vgm/MixerEngine.h` **间接**看到 `stdtype.h` 与 libvgm 类型；`ui/MixerPanel.cpp` 甚至直接声明了 `UINT32 deviceID`。

### 2.2 最深的耦合点（按"改动量 × 语义不可替代性"排序）

1. **实例模型**：`TrackPlayer` = `PlayerA` + `DATA_LOADER` + `WAVE_32BS`（32 位整数、`Render()` 返回字节数、`kInt32ToFloat = 1/2³¹`）。"每通道一个播放器、共享一份内存镜像"的整体架构建立在此之上。
2. **隔离机制**：`PLR_MUTE_OPTS`（`disable` 挂起省 CPU + `chnMute[2]` 位序）同时承担"通道隔离 / 芯片静音 / 省电"三件事，且只能探测核心是否支持 `SetMuteMask`。
3. **时间模型**：`PlayerA::Config{loopCount, fadeSmpls, endSilenceSmpls, pbSpeed, masterVol}`、`PLAYTIME_LOOP_INCL|PLAYTIME_TIME_PBK`（负值=无限）、`GetLoopTime()` 其实是**循环长度**、VGM 专属 `GetModifiedLoopCount`、`PLAYPOS_SAMPLE` seek。
4. **枚举模型**：`PLR_DEV_INFO` + `DEV_DECL` + `DEV_DEF` + `SndEmu_GetDevName` + `DEVID_*` 特例（如 T6W28 后半的处理）。
5. **类型泄漏**：`ChipEntry::devID`（`UINT32`）、`TrackLayoutEntry::deviceID`、`ui/MixerPanel.cpp` 的 `UINT32`、公共头里的 `stdtype.h`。

### 2.3 可复用的好基础（决定重构风险高低）

- **测试只走公共 API**：`Tests/MixerEngineTest.cpp` 不引用任何 libvgm 类型 → 天然的回归网。
- **实时安全约定已成型**：轨道参数/表值/位置全是原子量；音频线程 try-lock 失败即静音；libvgm 日志回调已按线程白名单静音（注释里写明"构造 String 会分配"）。
- **UI 已经是纯消费者**：UI 只调 `MixerEngine` 的公共方法，不碰播放器对象。
- **解锁点存在**：`player().GetPlayer()` 已经是"核心只通过一个指针点到 libvgm"的形状，说明边界可以就地在 `TrackPlayer` 处切开。

---

## 3. 架构风格选择

| 方案 | 说明 | 评价 |
|---|---|---|
| A. 全量重写为"通用播放器" | 自己抽象所有芯片/时间模型 | ❌ 丢掉 libvgm 的格式覆盖与正确性，工期不可控，违背"不重造轮子" |
| B. 只在文件级包装（解码器插件） | 新格式解码成 VGM 再喂 libvgm | ❌ SPC 的音频**无法**转成 VGM（用户已指出机制问题），等于没解决问题 |
| C. **端口-适配器（Hexagonal）+ 注册表** | 定义后端标准，libvgm 与未来 SPC 各做一个适配器；核心只依赖标准 | ✅ **采用** |
| D. 动态插件（.dll 运行期加载后端） | 运行期可插拔 | ⏸ 暂不做（§7 ADR-008）：2–3 个后端不值得引入 ABI/加载/部署复杂度 |

**结论：模块化单体 + 端口-适配器。** 核心是"应用内核"（混音、轨道、传输、时间线），外部世界（具体格式引擎）只能通过**端口**接入。

```
┌────────────────────────── 应用层（不依赖任何格式引擎） ──────────────────────────┐
│  PluginEditor / PluginProcessor / ui/*            （不变）                       │
│        │                                                                        │
│        ▼                                                                        │
│  MixerEngine  ── Facade：加载/传输/混音器/音量两级/无锁查询（公共 API 基本不变）   │
│        │                                                                        │
│        ├── SongTimeline      循环/淡出/时长数学（秒↔采样、含/不含循环语义）        │
│        ├── MixerSum          增益/声像/静音/独奏求和 + 峰值 + 两级音量             │
│        └── Resampler         宿主采样率与后端原生率不一致时的重采样（预留）        │
└───────────────┬────────────────────────────────────────────────────────────────┘
                │  端口：ISongBackend / ISongInstance / ITrackPlayer / SongInfo / TrackInfo / Capabilities
    ┌───────────┴────────────┬───────────────────────┬──────────────────────────┐
    ▼                        ▼                       ▼                          ▼
LibvgmBackend            SpcBackend(未来)        FakeBackend(测试)         …其他格式
(opera: PlayerA/… )      (SNES 模拟器)
```

---

## 4. 中间层标准（端口）设计

### 4.1 边界类型（全部自有，无 libvgm 类型）

| 类型 | 说明 |
|---|---|
| `DeviceId` / `TrackId` | **不透明** 32 位标识（后端自定含义；旧实现是 libvgm 的 `info.id` / 通道号） |
| `SongMemory` | 共享的歌曲镜像视图 `{const void* data, size_t size, const juce::File& file}`（一次读盘、多实例共享） |
| `SongInfo` | 时长（<0=未知）、循环点（<0=无）、是否无限循环、标签表、芯片/设备清单、轨道数、`nativeSampleRate`、格式名 |
| `ChipInfo` | `{DeviceId id, juce::String name, juce::String core, bool mutedByUser}`（UI 芯片卡片用，字段与今天一致） |
| `TrackInfo` | `{TrackId id, DeviceId deviceId, int deviceIndex, int channelIndex, bool isolatable, juce::String name, juce::String groupName}`（**与今天 `TrackLayoutEntry` 字段一一对应，UI 无需改**） |
| `BackendCapabilities` | 见 §4.4 |

### 4.2 接口草图

```cpp
namespace vgmplay::playback
{
/** 一个隔离的渲染实例（今 TrackPlayer 的角色）。生命周期只在消息线程。 */
class ITrackPlayer
{
public:
    virtual ~ITrackPlayer() = default;

    virtual void prepare (double hostSampleRate, int maxBlockSize) = 0;   // 消息线程，渲染前一次
    virtual bool load (const SongMemory& image, juce::String& error) = 0; // 消息线程
    virtual void start (void) = 0;
    virtual void pause (void) = 0;
    virtual bool seekSeconds (double seconds) = 0;

    /** 时间线：loopCount 0 = 无限循环，fadeSeconds 0 = 不淡出。 */
    virtual void setTimeline (int loopCount, double fadeSeconds) = 0;

    /** 让本实例只输出 track 的声音（隔离）；实现方式见 §4.3。 */
    virtual void setTrackAudible (TrackId track, bool audible) = 0;
    /** 静音某个设备（芯片卡片）；不支持时返回 false。 */
    virtual bool setChipAudible (DeviceId device, bool audible) = 0;

    /** 音频线程：写 numSamples 个浮点立体声样本，返回实际写入数。不得分配/加锁/日志。 */
    virtual int render (float* left, float* right, int numSamples) = 0;

    virtual double positionSeconds (void) const = 0;
    virtual bool isFinished (void) const = 0;
};

/** 一首已打开的歌曲（后端实现；核心持有它，只在消息线程建立/销毁）。 */
class ISongInstance
{
public:
    virtual ~ISongInstance() = default;
    virtual const SongInfo& getInfo (void) const = 0;
    virtual const std::vector<TrackInfo>& getTracks (void) const = 0;
    virtual BackendCapabilities getCapabilities (void) const = 0;
    /** 建一个播放器实例（多实例隔离时会被调用多次）。 */
    virtual std::unique_ptr<ITrackPlayer> createPlayer (void) = 0;
};

/** 格式后端：识别 + 打开。 */
class ISongBackend
{
public:
    virtual ~ISongBackend() = default;
    virtual juce::String getName (void) const = 0;
    virtual juce::StringArray getFileExtensions (void) const = 0;       // 供文件对话框/拖放过滤
    virtual bool canOpen (const SongMemory& image) const = 0;           // 魔数/扩展名判定
    virtual std::unique_ptr<ISongInstance> open (const SongMemory& image, juce::String& error) = 0;
};

/** 注册表：UI 的扩展名过滤、加载时的后端选择都从这里来。 */
class BackendRegistry
{
public:
    static BackendRegistry& getInstance (void);
    void add (std::unique_ptr<ISongBackend> backend);                   // 编译期注册（可条件编译）
    ISongBackend* findFor (const SongMemory& image) const;
    juce::StringArray getAllExtensions (void) const;                    // ".vgm;.vgz;.spc"
};
}
```

### 4.3 轨道隔离：后端能力，而非核心假设

核心只需要"**一个实例能被要求只输出某一条轨道**"。三种实现方式由后端声明：

| `IsolationMode` | 含义 | 谁用 | 成本 |
|---|---|---|---|
| `multiInstance` | 每个轨道一个实例，靠设备静音 + 通道静音位隔离 | libvgm（今天的方式） | 实例数 × 渲染成本；核心可选"只对发声轨渲染" |
| `inPlaceChannelMix` | 单实例内部按通道增益掩码隔离（如 SPC 的 8 个 voice 增益） | 未来的 SPC | 单实例成本，隔离精度依赖后端的通道粒度 |
| `none` | 无法隔离 → 该设备退化为**单轨**（今天 `isolatable == false` 的兜底语义） | 部分格式 | 无 |

核心据此决定：`multiInstance` 才建多实例；否则建 1 个实例并把"轨道"降级为一轨。**混音器 UI 不需要知道是哪种模式**（`TrackInfo::isolatable` 已表达结果）。

### 4.4 能力协商（避免"最小公分母"接口）

```cpp
struct BackendCapabilities
{
    bool canSeek        = false;   // 能否按秒/采样定位
    bool canLoop        = false;   // 能否循环 N 次
    bool canFade        = false;   // 能否淡出
    bool hasLoopPoint   = false;   // 能否给出循环点（UI 的循环标记）
    bool canMuteChips   = false;   // 能否按设备静音（芯片卡片）
    IsolationMode isolation = IsolationMode::none;
    bool rendersAtHostRate  = false;
    double nativeSampleRate = 0.0; // 0 = 任意宿主采样率
};
```

**降级规则（核心统一处理，UI 只读能力位）**

| 能力缺失 | 表现 |
|---|---|
| `canSeek == false` | 进度条只读、`setPositionSeconds` 变为"重新加载并快进"或忽略；UI 置灰 |
| `hasLoopPoint == false` | 不画循环标记（今天 `getLoopPointSeconds() < 0` 已是这个语义） |
| `canFade == false` | 淡出下拉置灰，用 `canLoop` 的整数循环收尾 |
| `canMuteChips == false` | 芯片卡片不显示（今天 `hasChips()` 已是这个语义） |
| `isolation == none` | 该设备一条轨（今天是 `isolatable == false`） |
| `rendersAtHostRate == false` | 核心插入重采样（见 §4.5） |

> 结论：**能力位是"把今天的隐式兜底变成显式契约"**，不是新功能。

### 4.5 时间线、采样率与重采样的归属

- **时间线数学归核心**（`SongTimeline`）：秒↔采样、时长（含/不含循环）、淡出采样数、无限循环判定。后端只提供"原始素材"（`GetTotalTime`/`GetLoopTime` 之类的语义由适配器**翻译成** `SongInfo` 的字段），核心不再出现 `PLAYTIME_*`。
- **采样率**：后端声明 `rendersAtHostRate` / `nativeSampleRate`。能跟宿主的（libvgm，通过重建播放器）直接跟；不能的（典型 SPC 输出 32 kHz）由核心的重采样器统一处理 —— **只实现一次、只测试一次**。
- 本里程碑只**定义字段与接口**，重采样器实现放到 SPC 里程碑（见 §9 的 P4）。

### 4.6 实时契约（写进端口注释，测试守卫）

1. `render()` 只在音频线程调用；**不得**加锁、分配、写日志、抛异常。
2. 所有开关（轨道增益/声像/静音/独奏、两级音量、芯片静音）走**原子量**，音频线程直接读，永不抢引擎锁。
3. 生命周期（创建/销毁/`load`/`seek`/`setTimeline`/`prepare`）只在消息线程，且在引擎锁内。
4. `render()` 返回"实际写入样本数"，短块时其余样本由调用方清零（今天的语义）。
5. 后端不得在音频线程回调进核心（除已有的"结束"标志外，用 `std::atomic` 发布）。

---

## 5. 目标代码结构

```
Source/
├── playback/                      ← 核心：零 libvgm 依赖（源码守卫强制）
│   ├── SongBackend.h              端口：ISongBackend / ISongInstance / ITrackPlayer + 边界类型
│   ├── BackendRegistry.{h,cpp}    注册表 + 扩展名查询（UI 从这里取过滤字符串）
│   ├── SongTimeline.{h,cpp}       循环/淡出/时长数学
│   ├── MixerEngine.{h,cpp}        门面（今天的类，改用端口；公共 API 不变）
│   └── Resampler.{h,cpp}          预留（P4）
├── backends/
│   ├── libvgm/                    ← 唯一允许 include libvgm 的地方
│   │   ├── LibvgmBackend.{h,cpp}          识别 .vgm/.vgz/.s98/.dro/.gym + 打开
│   │   ├── LibvgmSongInstance.{h,cpp}     SongInfo/TrackInfo 枚举 + createPlayer
│   │   ├── LibvgmTrackPlayer.{h,cpp}      今 TrackPlayer（PlayerA/DataLoader/WAVE_32BS/int32→float）
│   │   ├── LibvgmMuteIsolation.{h,cpp}    PLR_MUTE_OPTS 映射、chnMute 位序、disable 挂起
│   │   └── LibvgmVgz.{h,cpp}              vgz 解压（今天在 MixerEngine 里）
│   └── fake/                      ← 测试后端（振荡器，3 轨；不含任何格式引擎）
│       └── FakeBackend.{h,cpp}
└── ui/  Source/*.{h,cpp}          ← 不变（仅去掉间接看到的 stdtype/UINT32）
```

---

## 6. 采用 / 不采用的设计模式

| 模式 | 用在哪 | 为什么 |
|---|---|---|
| **Ports & Adapters（六边形）** | 整体结构 | 让"格式引擎"成为可替换的外部细节，核心可独立测试 |
| **Adapter** | `LibvgmBackend` | 把 libvgm 的怪癖（时间模型、mute 位序、字节渲染）全部吸收在适配器内 |
| **Abstract Factory + Registry** | `BackendRegistry` | 加载时按"谁能打开"选择后端；扩展名列表数据驱动 UI；支持编译期可选后端 |
| **Facade** | `MixerEngine` | UI/宿主只认一个稳定面，重构对上层零感知 |
| **Strategy** | `IsolationMode`、重采样策略 | 能力差异用策略表达，而不是让核心写 `if (format == SPC)` |
| **Capability Object** | `BackendCapabilities` | 避免"最小公分母接口"把 VGM 的丰富能力砍掉 |
| **Template Method（核心侧）** | `SongTimeline` + 门面 | 时间线流程固定，后端只填原始素材 |

**明确不采用**

- ❌ **运行期 .dll 插件 / 后端运行期加载**：本产品的交付形态是 **VST3 插件**，插件必须是一个自包含、可签名、可合规分发的二进制；在宿主进程里加载第三方动态库会带来 ABI、崩溃归因、签名与许可合规风险。后端一律**编译期注册**（可选后端用条件编译开关）。
- ❌ **最小公分母接口**（会阉割 VGM 的循环/淡出/芯片静音，反而要写特例）。
- ❌ **核心直接持有 `PlayerA`/`DATA_LOADER` 之类的"薄包装"**（就是今天的耦合，等于没解耦）。
- ❌ **把重采样塞进每个后端**（重复实现、重复踩坑）。

---

## 7. 架构决策记录（ADR）

### ADR-001：在"歌曲/轨道"层切分，而不是"文件解码"层
**上下文**：解耦目标是让新格式接入，但 SPC 无法转成 VGM，必须在**播放**层接入。
**决策**：端口定义在"一首歌 → 若干轨道 → 可隔离渲染实例"这一层。
**备选**：① 解码到 VGM 再喂 libvgm（SPC 原理上不可行）；② 只在文件读取处抽象（不解决播放）。
**后果**：后端要承担"轨道枚举 + 隔离渲染"两份责任，但这两个正是混音器与多轨输出所需的全部能力。

### ADR-002：`MixerEngine` 保留为门面，UI/宿主 API 不变
**上下文**：UI 与宿主已依赖 `MixerEngine` 的公共方法；测试也只走公共 API。
**决策**：类名、文件、方法签名尽量冻结；只做类型净化（ADR-005）。
**后果**：重构对上层零影响、可随时回滚；代价是名字里仍带 "Mixer"，未来若要更名（`PlaybackEngine`）是独立的一次机械重命名（**待定，见 §10**）。

### ADR-003：能力位 + 显式降级，而非最小公分母
**决策**：`BackendCapabilities` 声明能力；核心统一降级；UI 只读结果字段。
**备选**：所有后端都必须支持全部功能（不可能：SPC 无 VGM 的循环点语义）。
**后果**：能力位需要维护（新增能力时要同步降级规则与 UI），但今天已有的隐式兜底被显式化，**净收益**。

### ADR-004：轨道隔离是后端能力（`IsolationMode`）
**决策**：`multiInstance`（libvgm）/ `inPlaceChannelMix`（SPC 类）/ `none`（退化单轨）。
**备选**：核心强制"每轨一实例"（对 SPC 可能是 8 个模拟器实例，代价与正确性都不可控）。
**后果**：多轨输出功能天然可用（有隔离即可按轨渲染到独立总线）；未实现隔离的后端只损失"分轨"能力，不阻塞播放。

### ADR-005：公共头不出现 libvgm 类型（含 `stdtype.h`）
**决策**：`DeviceId`/`TrackId` 为自有不透明类型；边界只用 `float/double/int/juce::String/std::vector`。
**备选**：沿用 `UINT32`（现状）——已被 UI 直接引用，是正在扩大的技术债。
**后果**：需要一次机械改造（`ChipEntry::devID`、`TrackLayoutEntry::deviceID`、`ui/MixerPanel.cpp`），收益是 UI 与 libvgm 彻底无关。

### ADR-006：端口上的实时契约用"测试 + 守卫"保证，而非评论
**决策**：`render()` 无锁/无分配/无日志写入接口注释，并由"FakeBackend 全链路测试 + 现有 −1000 dB 逐样本回归"守护。
**备选**：只在文档里写约定（无法防止回归）。
**后果**：新后端接入时有明确 checklist；测试是唯一可信的守卫。

### ADR-007：采样率协商 + 核心持有重采样器
**决策**：后端声明 `rendersAtHostRate`/`nativeSampleRate`；不一致时核心重采样。
**备选**：要求后端必须输出宿主采样率（SPC 模拟器做不到精确可变率时会产生音高/音色偏差）。
**后果**：多一个核心组件（本里程碑只定接口，P4 实现）。

### ADR-008：编译期后端注册 + 源码级守卫（**不做运行期加载**）
**上下文**：本产品的交付形态是 **VST3 插件（+ 独立运行）**——插件必须是一个自包含、可签名、可合规分发的二进制，运行在宿主进程里；而可替换的"格式引擎"在后端侧（libvgm 静态链接、未来 SPC 可能受许可限制）。在宿主进程内加载第三方动态库会引入 ABI 兼容、崩溃归因、代码签名与许可合规四类风险，且收益仅是"少一次重编译"。
**决策**：① 后端**编译期**注册（`BackendRegistry` 在插件启动时填充，可选后端用条件编译开关，如 `VGMPLAY_ENABLE_SPC_BACKEND`）；② 构建脚本加一条守卫 —— `Source/` 下除 `backends/` 外不得出现 libvgm 头 `#include`。
**备选**：运行期动态库加载（已否决，理由见上）；把后端做成独立进程（否决：实时音频路径不可接受）。
**后果**：解耦可机械验证、发布物是单一插件二进制、许可合规在发布时一次性决定；代价是新增格式必须重新编译（对当前规模无影响）。

---

## 8. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 重构破坏实时安全（引入锁/分配） | 高（爆音、掉块） | 端口只传裸指针/原子量；`FakeBackend` + 现有逐样本回归；每步用 `vgm_mixer_test` 卡门 |
| 行为漂移（VGM 播放与今天不一致） | 高 | 保留"求和 vs 单实例 −1000 dB"与全部电平检查作为**硬门禁**；适配器层不做任何数学改写 |
| 接口设计过度（为想象中的格式做抽象） | 中 | 只为**已知的两个**后端（libvgm、SPC）设计；新增能力走"能力位 + 降级"，不改接口 |
| 隔离语义误解导致音频重复/丢失 | 中 | 隔离只在适配器内实现；核心只做"单轨输出"假设；补一条"多轨求和 vs 单实例"测试 |
| SPC 后端许可（SNESAPU 闭源 / snes9x GPL） | 中（**未来**） | 后端编译期可选 + 注册表条件注册；许可决策放到 SPC 里程碑单独评审 |
| 范围蔓延成"重写引擎" | 高 | 本里程碑非目标清单（§9.4）明确；适配器只搬不改 |

---

## 9. 里程碑 M2.5：任务分解与验收

### 9.1 任务（每步提交、每步可跑）

| 步 | 内容 | 验收 |
|---|---|---|
| **P0** | 本文档 + ADR 评审通过 | 用户确认（含 §10 的待定项） |
| **P1** | 落 `playback/SongBackend.h` 端口 + `BackendRegistry` + `FakeBackend`；把 libvgm 代码整体搬进 `backends/libvgm/`（**只搬家不改逻辑**），`MixerEngine` 改为通过端口驱动 | `vgm_mixer_test` 全绿且 `−1000 dB` 数值不变；`FakeBackend` 单测通过；app 冒烟 |
| **P2** | 类型净化：`DeviceId`/`TrackId`、去掉公共头的 `stdtype.h`、修 `ui/MixerPanel.cpp` 的 `UINT32`、`devID`→`deviceId` | 编译通过；`read_lints` 干净；UI 无 libvgm 相关标识符 |
| **P3** | 数据驱动扩展名：`FileChooser`/拖放过滤/提示文案全部来自 `BackendRegistry::getAllExtensions()` | 新增一个后端"只注册、不改 UI"即可出现在过滤与拖放里（用 FakeBackend 演示） |
| **P4（延后）** | `Resampler` 实现 + 采样率协商落地 | 留到 SPC 里程碑 |
| **P5** | 源码守卫（构建脚本检查 `backends/` 之外的 libvgm include）+ 文档更新 | 守卫失败会让构建报错（可演示） |

### 9.2 里程碑验收（Definition of Done）

1. `Source/playback/`（或核心部分）**零** libvgm include，且由 P5 守卫机械证明。
2. 现有 `vgm_mixer_test` 全绿，`plain vs mixer` 仍为 `−1000 dB`（逐样本一致）。
3. `FakeBackend` 全链路测试：核心的增益/声像/静音/独奏/两级音量/时间线在**没有 libvgm** 的情况下被验证。
4. UI 与 `PluginProcessor` 的调用点**不变**（除 P2 的类型净化）。
5. 独立运行与 VST3 构建、冒烟测试通过；`dist` 可听可用。
6. 文档：`CODEBUDDY.md` 新增"播放后端端口"章节；`docs/mixer-milestones.md` 记录 M2.5。

### 9.3 回滚策略

每步一个提交；P1 是唯一的大步，其"只搬家不改逻辑"约束保证失败时 `git revert` 即可回到今天的状态。

### 9.4 非目标（本里程碑明确不做）

- ❌ 不实现任何新格式（SPC 是独立里程碑）
- ❌ 不改混音器行为/UI 布局/音量语义
- ❌ 不做多轨 Audio Output（下一个功能，建立在 ADR-004 的隔离能力上）
- ❌ 不做运行期插件加载、不做 M3/M4（仍搁置）

---

## 10. 待用户决策

| # | 问题 | 建议 |
|---|---|---|
| D1 | 分支：在本分支做，还是新开 `feature/playback-backend` 并从当前分支拉出？ | **新开分支**（与多轨输出分开，便于独立验收与回滚） |
| D2 | `MixerEngine` 是否顺带更名 `PlaybackEngine`（文件移到 `Source/playback/`）？ | P1 里保留原名，P2 做机械重命名（一次提交，diff 干净） |
| D3 | SPC 后端路线（SNESAPU / snes9x / 自研 DSP 模拟）与许可 | 留到 SPC 里程碑单独评审；本里程碑只保证"可编译期可选" |
| D4 | 隔离降级策略：`isolation == none` 时是"单轨"还是"不给混音卡片" | 沿用今天的"单轨"（不丢芯片卡片与整轨推子） |

---

## 11. 与后续功能的衔接

- **多轨 Audio Output**：核心已有的"每轨隔离渲染"直接映射为"每轨一条输出总线"；`IsolationMode::multiInstance` 的后端天然支持，`inPlaceChannelMix` 需后端提供通道渲染，`none` 则只有主输出（UI 上置灰）——**这条能力位现在就要写对**。
- **M3 示波器/频谱**：端口的 `render()` 与轨道峰值是唯一数据源，后端无关，M3 恢复时不需要再改后端。
- **M4 持久化**：宿主状态里只存"文件路径 + 轨道参数 + 音量 + 循环/淡出"，与格式无关（今天已如此），新格式接入不需要改状态模型。
