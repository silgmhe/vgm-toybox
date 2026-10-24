# MDPlayer 架构研究：格式驱动、芯片核心与可视化

- **日期**：2026-10-24
- **对象**：`reference/MDPlayer-stable`（C# / WinForms，MDSound 外部 DLL）
- **目的**：判断它的"多格式 → 统一引擎"是否值得借鉴，以及波形/音高数据能否为我们将来的 DAW 导出所用
- **相关**：`2026-10-22-mdplayer-visualisation-survey.md`（已有一份键盘窗/钢琴窗的音符判定调研，本文不重复其公式细节）

## 1. 结论

三句话：

1. **不存在统一中间格式。** 统一的是**一层驱动契约**（`baseDriver` 的四个抽象方法 + 一组共享字段），不是数据表示。每个格式驱动各自直接操作芯片，落地方式还分成四种互不兼容的形态。
2. **芯片核心不在这个仓库里。** 真正发声的是外部二进制 `MDSound.dll`（`MDPlayerx64.csproj:57-73`）；仓库内的 `Driver/` 只是"格式解析 + CPU/时序"宿主。
3. **可视化不是从引擎"推"出来的，而是 UI 侧按帧"拉"寄存器镜像。** 音频线程与 UI 线程之间**没有任何锁、队列或原子量**，靠的是"写影子寄存器 + 后台线程定时读 + 脏值比对"。

如果只看目录结构（`Driver/NSF`、`Driver/vgm.cs`、`form/` 186 个文件），确实很像"多格式 + 通用引擎"；但打开 `baseDriver` 就会发现它只有四个抽象成员，而且**没有 `Render`**。

## 2. 分层

```
L0  芯片核心（本仓库外，MDSound.dll）
    MDSound.MDSound（混音/调度） + MDSound.MDSound.Chip（描述结构）
    实现：MDSound.np.chip.{nes_apu, nes_dmc, nes_fds, nes_n106, nes_vrc6, nes_vrc7, nes_mmc5, nes_fme7}
          MDSound.iremga20 / MDSound.K054539 / ...
    ≤ 契约：Update(sender, chipID, int[2][len], len) / Start / Stop / Reset
            + Clock / SamplingRate / Volume / Instrument / Option / type / ID

L1  格式驱动  Driver/{NSF,GBS,HES,AY,SID,vgm,xgm,ZGM,MID,…}
    ≤ 职责：解析文件 → 建 CPU/内存/bank → 每帧把寄存器写进去

L2  寄存器门面  ChipRegister.cs（273 KB）
    ≤ 写入口、影子寄存器镜像、硬件状态直读、MIDI 导出、LED

L3  渲染门面   Audio.cs（549 KB）
    ≤ mds.Update(块渲染)、主音量、淡出、16bit 限幅、静态状态访问器

L4  UI 状态快照  MDChipParams.cs / ChipLEDs.cs / VisVolume.cs / Tone.cs

L5  显示层  form/KB/{OPN,OPL,PSG,NES,PCM,WF,OPX}/*.cs
    ≤ screenChangeParams(): 读 L3 → 自行算 Hz/音名 → 写 MDChipParams
    ≤ screenDrawParams():   脏检查 → DrawBuff.*
```

## 3. 驱动契约：统一在"写法"，不在"数据"

`Driver/baseDriver.cs` 的全部抽象成员（`baseDriver.cs:33-36`）：

```csharp
public abstract bool init(byte[] vgmBuf, ChipRegister chipRegister, EnmModel model,
                          EnmChip[] useChip, uint latency, uint waitTime);
public abstract bool init(byte[] vgmBuf, int fileType, ChipRegister chipRegister, EnmModel model,
                          EnmChip[] useChip, uint latency, uint waitTime);
public abstract void oneFrameProc();
public abstract GD3 getGD3Info(byte[] buf, uint vgmGd3);
```

共享的是**字段**（`baseDriver.cs:5-31`）：`vgmBuf`、`chipRegister`、`model`、`useChip`、`vgmSpeed`、
`vgmFrameCounter`、`Counter / TotalCounter / LoopCounter / vgmCurLoop`、`Stopped`、`GD3`。

**没有 `Render()`，也没有任何事件/命令/队列类型。** 全仓检索 `class *Event/Command/Queue` 只命中格式私有物
（`HES/kmevent.cs:3`、`RCP/MIDIEvent.cs:4`、`SID/libsidplayfp/Event.cs:28`）。

### 四种落地形态（互不兼容）

| 形态 | 代表 | 做法 |
|---|---|---|
| **A. 写寄存器** | `vgm.cs:585/666/783`、`S98.cs:567-613`、`xgm.cs:315`、`ZGM`、`AY/port.cs:80`、`HES` | 解析命令 → `chipRegister.setXxxRegister(...)` |
| **B. 自渲染 PCM** | `NSF/nsf.cs:478-665`、`SID/sid.cs:107-129`、`MXDRV` | 自己 new 芯片、自己 `Tick/Render`，**绕过 MDSound 混音** |
| **C. 走 MIDI** | `MID/MID.cs:359/500/525` | `chipRegister.sendMIDIout(...)`，根本不进寄存器模型 |
| **D. 空壳** | `mp3.cs`、`Wma.cs`、`Aac.cs`、`aiff.cs` | `init` 一律 `NotImplementedException`，实际由 NAudio 旁路解码（`Audio.cs:11372-11391`） |

主循环必须用 `is` 判断绕开抽象（`Audio.cs:11441-11485`）：

```csharp
if (DriverVirtual is nsf nsf1)      { cnt = nsf1.Render(buffer, sampleCount/2, offset) * 2; }
else if (DriverVirtual is sid sid1) { sid1.Render(buffer, sampleCount); }
else if (DriverVirtual is MXDRV mX) { ... }
else                                { cnt = mds.Update(buffer, offset, sampleCount, oneFrameProc); }
```

> "统一"在这里的含义是：**任何格式都必须实现这三个方法**，仅此而已。

### VGM 与 NSF 的对照

| 维度 | `vgm.cs` | `NSF/nsf.cs` |
|---|---|---|
| 头解析 | `getInformationHeader()`（`:153`） | `getGD3Info()` 兼做头解析（`:28-103`） |
| 推进 | `oneFrameProc` → 命令表 `Action[0x100]`（`:116`、`:195-218`） | `oneFrameProc` **空转**（`:165` 被注释），真正推进在 `Render()` |
| 芯片来源 | MDSound 内部持有 | 驱动 `new` 并登记到 `chipRegister.nes_*`（`:268-307`） |
| 产物 | 寄存器写 + wait 计数 | `Int16` 立体声 PCM |
| 时钟 | 文件内 wait 命令（1/44100 s） | 头里的 `speed_ntsc/pal` → `nes_cpu.Start(...)`（`:441/446`） |

## 4. ChipRegister：唯一的门面（也是最值得借鉴的一层）

`ChipRegister.cs` 承担五件事：

1. **写入口**：`setYM2612Register`（`:4645`）、`setNESRegister`（`:1860`）、`setAY8910Register`、…
   按 `EnmModel` 分叉：`VirtualModel → mds.WriteXXX`（`:4836/1870`）、`RealModel → 真实芯片`、`PianoRollModel → pianoRollMng.SetRegister`。
2. **影子寄存器镜像**：`fmRegisterYM2612[chipID][port][addr] = data`（`:4661`），派生量 `fmVolYM2612`、`fmKeyOnYM2612`、`maskFMChYM2612`。
3. **硬件状态直读**：`mds.ReadNESapu`（`:1892`）、`ReadNESdmc`、`ReadFDS`、`ReadGA20Status`、`ReadK054539Status`。
4. **每通道轨迹**：`getN106Register` / `getVRC6Register`（`:6968-6990`）→ `ITrackInfo[]`，**只有这两颗芯片有**。
5. **副作用**：`chipLED.PriOPN = 2`、`midiExport.OutMIDIData(...)`（`:4662`）。

`ITrackInfo` 的成员由调用点反证（`frmN106.cs:173-182`）：`GetVolume()`、`GetFreq()`、`GetFreqHz()`、
`GetNote(hz)`、`GetKeyStatus()`、`GetHalt()`、`GetFreqShift()`、`wavelen`。

> 这一层是关键设计：**芯片核心只管发声，状态通过一个门面统一暴露**，UI 与 MIDI 都只认这个门面。

## 5. 音高是怎么算出来的：两套体系并存

**总原则：芯片核心不报音名；音名由 UI 层从寄存器反算。唯一例外是 NES 扩展芯片（`ITrackInfo.GetNote`，实现不在本仓库）。**

### 5.1 NES 系：log2 经验式

`form/KB/NES/frmNESDMC.cs:104`：

```csharp
freq = (reg[3 + i*4] & 0x07) * 0x100 + reg[2 + i*4];          // 11bit period
note = 104 - (int)((12 * (Math.Log(freq)/LOG_2 - LOG2_440) + NOTE_440HZ + 0.5));
```

三角同一形状，常数是 `92`（`:129`）。**没有经过真正的 Hz**，时钟被折进 `104`/`92` 两个魔数里。

### 5.2 FM 系：FNum/Block → Hz → 查表

`form/KB/OPN/frmYM2612.cs:185-190`：

```csharp
float fmDiv = 6;
float ff = freq / ((2 << 20) / (masterClock / (24 * fmDiv))) * (2 << (octav + 2));
ff /= 1038f;
n = Common.searchYM2608Adpcm(ff);        // Tables.pcmMulTbl 上最近邻搜索，返回 0..95
```

### 5.3 NES 扩展芯片：交给核心的包装类

```csharp
// frmN106.cs:177-179
v = info[ch].GetNote(info[ch].GetFreqHz()) - 4 * 12;   // 再降 4 个八度
```

`GetFreqHz()` 在 `MDSound.np.chip.nes_n106` 内（仓库外），但**同一套代码在我们 `libs/nsfplay` 里是可读的**
（`nes_n106.cpp:60`）：

```cpp
t->freq = (double(t->_freq) * clock) / double(15 * 65536 * channels * t->wavelen);
t->key  = (t->volume > 0) && (t->_freq > 0);
```

> 两套体系的存在说明：**这件事没有通用解**，每个芯片族都得单独写。MDPlayer 的选择是"各写一份"，而不是"抽象一个统一公式"。

## 6. 可视化：无锁 pull 模型

### 6.1 三个线程

| 线程 | 位置 | 职责 |
|---|---|---|
| `trdVgmReal`（音频） | `Audio.cs:11171` | 驱动渲染、写寄存器、写影子镜像 |
| `screenMainLoop`（后台数据线程） | `frmMain.cs:1148` / `:5233` | 按 `1000/ScreenFrameRate` 抓快照 |
| WinForms 主线程 | — | 真正 GDI 绘制 |

**`ChipRegister.cs` 里 `lock (` 出现 0 次，也没有 `Interlocked` / `Monitor` / `ConcurrentQueue`。**
共享的是普通 `static` 字段与数组。

### 6.2 数据传递

```
音频线程: setXxxRegister → ①mds.WriteXXX（发声） ②影子数组（UI 用） ③midiExport ④chipLED
UI 后台:  screenChangeParamsForms() → 每个 frmXXX.screenChangeParams()
            → Audio.GetFMRegister()/GetN106Register()/… 读同一块内存
            → 写入 frmMain.newParam（与 Form 共享引用）
UI 主线程: BeginInvoke → screenDrawParamsForms() → 脏检查 if (old == new) return; → DrawBuff.*
```

只有一处有"拷贝语义"：波形缓冲 `Audio.CopyWaveBuffer`（`Audio.cs:260`）→ `nsf.visWaveBuffer.Copy`。

### 6.3 逐芯片差异怎么分

**不是一个大 switch，是"每芯片一个 Form 类 + 一个 MDChipParams 数据类 + 一个 PianoRoll 分析类"。**

- `form/KB/` 下 60+ 窗体，按族分目录：`OPN/`、`OPL/`、`PSG/`、`NES/`、`PCM/`、`WF/`、`OPX/`
- `MDChipParams.cs` 里每芯片一个类，形状都不同：
  `NESDMC{ sqrChannels[2], triChannel, noiseChannel, dmcChannel }`（`:514`）、
  `FDS{ channel, wave[32], mod[32] }`（`:527`）、`YM2612{ channels[13] }`（`:112`）、
  `YMF278B{ channels[47] }`（`:496`）
- 画法也各一套：`KeyBoard` / `KeyBoardOPN` / `KeyBoardOPNA` / `KeyBoardDCSG` / `KeyBoardDMG`（`drawBuff.cs:1315-1566`）
- PianoRoll 侧 10 个 `Analyze` 实现（`PianoRoll/{AY8910,DMG,HuC6280,K051649,YM2150,YM2413,YM2608,YM2610,YM2612,SN76489}.cs`）

## 7. N163 波表可视化（与"导出给 DAW"最相关）

### 7.1 数据直达核心 RAM，不是寄存器重建

```
frmN106.cs:161  TrackInfoN106[] info = (TrackInfoN106[])Audio.GetN106Register(0);
  → Audio.cs:12015 → ChipRegister.cs:6984 → nes_n106.GetTracksInfo()
  → nes_n106.cpp:53
        t->wavelen = get_len(channel);
        t->tone    = get_off(channel);
        for (int i = 0; i < t->wavelen; ++i)
            t->wave[i] = get_sample((i + t->tone) & 0xFF);   // 带相位偏移
```

波表本体：`nes_n106.h:35` `UINT32 reg[0x80]`（128×32bit = 256 个 4bit 采样），
`get_sample` 按半字节取（`nes_n106.cpp:344`）：

```cpp
return (index & 1) ? ((reg[index>>1] >> 4) & 0x0F) : (reg[index>>1] & 0x0F);
```

**导出波表需要的三个参数**：`wavelen`（长度，`256 - (reg[0x44+ch*8] & 0xFC)`）、
`tone`（起始相位，`reg[0x46+ch*8]`）、`channels`（`((reg[0x7F] >> 4) & 0x07) + 1`）——
只拿 `wave[]` 会丢掉相位与长度，音色还原不出来。

### 7.2 三重反转（如果照搬会踩）

1. 核心内部：`nes_n106.cpp:36` `int channel = 7 - trk; // invert the track display`
2. Form 侧：`frmN106.cs:17` `private bool rev = true;` + `:212` `int vch = rev ? (7-ch) : ch;`
3. 点击屏蔽也走 `vch`（`:129-139`）

### 7.3 UI 侧整形

```csharp
// frmN106.cs:186
for (int i = 0; i < 280; i++)
    if (i < nyc.bank) nyc.aryWave16bit[i] = info[ch].wave[i];
    else { /* 左移一位 */ nyc.aryWave16bit[i] = nyc.aryWave16bit[i+1]; }   // 滚动尾巴
```

**`aryWave16bit` 的后半段不是波表**，是"上一帧左移 + 最新输出"的示波器效果。真实波表只有 `[0, bank)`。
导出时必须只用这一段。

## 8. 现成的取数入口（若要复用）

| # | 入口 | 位置 | 拿到什么 |
|---|---|---|---|
| 1 | `Audio.GetN106Register(chipID)` | `Audio.cs:12015` | `TrackInfoN106[]`：`wave[256]`、`wavelen`、`tone`、`output`、`freq`、`volume`、`key` |
| 2 | `ChipRegister.getFDS/VRC6/VRC7/MMC5Register` | `:1881-2028`、`:6968-6990` | 核心对象引用或整块 `byte[] reg` |
| 3 | `Audio.GetFMRegister/GetFMKeyOn/GetFMVolume` | `Audio.cs:11980/12302/12990` | 影子寄存器 `int[2][0x100]` + 派生量 |
| 4 | `Audio.GetYM2609UserWave(chipID,p,n)` | `Audio.cs:13079` | `byte[]`，**唯一已具"波表导出 API"形态的方法** |
| 5 | `Audio.CopyWaveBuffer(short[][])` | `Audio.cs:260` | 成品立体声 PCM 快照（NSF/SID 走环形队列） |
| 6 | `Audio.pianoRollMng.lstPrNote` | `PianoRollMng.cs:16` | `PrNote{ch,startTick,endTick,key,freq}`，可直接转 MIDI |
| 7 | `MDChipParams.Channel.aryWave16bit / PSGWave` | `MDChipParams.cs:73-74` | UI 整形后的波形（含滚动尾巴，见 §7.3） |

**三个坑**（客观事实，非建议）：

1. 全部跨线程裸共享，无锁；`GetTracksInfo()` 返回的是核心内部数组的地址，C++ 侧每帧原地覆写。
2. `getN106/getVRC6/getVRC7Register` 在 `chipID != 0` 时**直接返回 null**。
3. `aryWave16bit` 后半段是滚动尾巴，不是波表。

## 9. PianoRoll：第二遍回放

它不是从仿真核心取数，而是**把同一份文件用 `PianoRollModel` 再跑一遍驱动**：

```csharp
// Audio.cs:3086
DriverPianoRoll = new Vgm(setting);
((Vgm)DriverPianoRoll).dacControl.model = EnmModel.PianoRollModel;
// Audio.cs:4166 重新 init
ChipRegister 里共 113 处 EnmModel.PianoRollModel 分支，全部转向 pianoRollMng.SetRegister(...)
// PianoRollMng.cs:40 → 每芯片一个 Analyze
```

代价是**两倍驱动开销**；收益是音符判定与音频渲染彻底解耦，且能离线（seek/导出）重放。

## 10. 对我们项目的启示

### 10.1 我们与它的根本差异

| 维度 | MDPlayer | 我们 |
|---|---|---|
| 统一的是什么 | **输入侧写法**（驱动契约） | **输出侧形态**（`ISongInstance`：render / renderTracks / seek / 轨道表） |
| 芯片核心位置 | 外部 DLL，不可改 | 进程内源码（`libs/nsfplay`、libvgm） |
| 多轨隔离 | 掩码 + 重建驱动 | 一轨一实例（`multiInstance`）+ `SetMask` |
| 状态暴露 | 影子寄存器（无锁共享） | 目前**没有**统一的状态暴露层 |

**结论：架构上我们其实比它更"统一"**——它统一的是写法，我们统一的是产物。但它有一层我们没有的东西：**ChipRegister 式的状态门面**。

### 10.2 值得借鉴的三点

1. **状态门面独立于渲染**：我们目前 `NsfPlayer` 既渲染又持有状态，没有"给 UI/MIDI 用的只读状态视图"。若要可视化和 MIDI，这一层得单独建。
2. **波表直读核心 RAM**（§7.1）：这正好是我们能做得比它更好的地方——NSFPlay 源码在手，`nes_n106::reg[]`、`nes_vrc6` 的波形寄存器都可以直接暴露，不必绕道 UI 整形。
3. **音高换算按芯片族各写一份**（§5）：它用两套（log2 经验式 / FNum→查表），说明通用公式不存在；我们的 `NsF` / `libvgm` 若要出音名，也应该按族分派，而不是找一个万能公式。

### 10.3 不建议照搬的四点

| 项 | 原因 |
|---|---|
| 无锁共享 + 定时 pull | MDPlayer 是独立进程的玩具播放器；我们是 VST，音频线程不能被 UI 无锁读（我们的 `getTrackPositionSeconds` 已经是反例：它取锁，seek 期间会阻塞） |
| log2 魔数（104 / 92） | 时钟常数被折进字面量，换采样率/换芯片就失配 |
| 第二遍回放做 PianoRoll | 我们已经是 13 实例，再翻倍不可接受；应改为"播放时按帧快照" |
| 固定 8 行 UI | N163 实际通道数 1–8 可变，MDPlayer 用"填静默值"绕开，导出时会多出空通道 |

### 10.4 关于"把波表提取给 DAW 的 VST"

我们的条件比 MDPlayer 好：

- **数据在进程内**：`nes_n106::reg[0x80]` 可直接读，无需经过外部 DLL 的结构体封送；
- **但我们没有现成的状态视图**：需要新增一层只读快照（区别于渲染路径），并解决与音频线程的同步（**不能**学 MDPlayer 的无锁裸读）。

最小可行的形态（供后续设计参考，本文不展开）：

```
音频线程（每个 render 块结束）
   → 快照：每通道 {key, note, freqHz, volume, wave[], wavelen, tone}
   → 写入一个双缓冲/SPSC 队列
UI / 导出线程读快照（无阻塞、无锁竞争）
```

波表导出还需注意：4bit 半字节、长度可变（`256 - ...`）、`tone` 表示相位偏移、
`channels` 影响 `/channels` 的频率折算——四者缺一，音色即不成立。

## 11. 一句话总结

MDPlayer 不是"多格式 → 一种中间格式 → 通用引擎"，而是**"多格式 → 同一个驱动契约 → 各自操作芯片"**；
它的芯片核心是外部 DLL，音高与波形全靠 UI 层从寄存器反算。
真正值得学的不是它的分层，而是**状态门面与渲染解耦**这个念头——而这恰好是我们做 MIDI 与波形导出时缺的那一层。
