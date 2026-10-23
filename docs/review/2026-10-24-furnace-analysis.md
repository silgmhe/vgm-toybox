# Furnace 的实现分析：导入各式格式，导出 VGM/VGZ

> 参考代码：`reference/furnace`（**不入库**，本地参考树，见 `.gitignore`）。
> 本文只记录读到的内容与行号，供以后决定"要不要参考、参考哪一部分"。
> 所有行号基于当前参考树的版本，改动后需重新核对。

## 1. 它到底做了什么

Furnace 不是"文件到文件"的转换器。它有一个**统一的内部工程模型**，导入、导出是两半：

```
.ftm / .mod / .s3m / .dmf / .fur ...
        │  导入器（每格式一个 reader）
        ▼
   内部工程（DivSong + 乐器 + 模式 + 效果）
        │  播放引擎（逐 tick 跑 chip dispatch）
        ▼
   寄存器写流（DivRegWrite：地址 + 值，可带延时）
        │  VGM 编码器
        ▼
      .vgm  →（GUI 侧 zlib deflate）→ .vgz
```

**导出 VGM 时它会把歌自己播放一遍**，边播边记（`saveVGM` 里第一行注释就是
`// play the song ourselves`）。

这意味着：它的"转换"从来不是格式翻译，而是**重放 + 记录**。这一点与我们 `SongIr`
（寄存器写流）在结构上是同一层，只是它上面还多一个完整工程模型。

## 2. 关键文件

| 文件 | 规模 | 作用 |
|---|---|---|
| `src/engine/vgmOps.cpp` | 2,256 行 | VGM 导出全部实现 |
| `src/engine/fileOps/ftm.cpp` | 2,438 行 | FamiTracker 导入 |
| `src/engine/fileOps/*.cpp` | 21 个 | 其余格式导入（mod/s3m/it/xm/dmf/fur/midi…） |
| `src/engine/export/*.cpp` | 12 个 | 其他导出（wav/zsm/grub/…） |

注意：`engine/export/` 里**没有** VGM——VGM 是特例，它不在 `export/` 目录，而是独占
`engine/vgmOps.cpp`，因为它需要驱动整个引擎。

## 3. VGM 导出的机制

### 3.1 入口

```cpp
// vgmOps.cpp:566
SafeWriter* DivEngine::saveVGM(bool* sysToExport, bool loop, int version,
                               bool patternHints, bool directStream,
                               int trailingTicks, bool dpcm07, int correctedRate);
```

- `version < 0x150` 直接拒绝（低于 1.50 的 VGM 头不够用）；
- `sysToExport[]` 决定哪些芯片导出（多芯片工程可以只导一部分）；
- `loop` 决定是否写循环点；
- `directStream` 走另一条路（芯片直接产出数据流，而不是寄存器写）。

### 3.2 逐 tick 的主循环

```cpp
// vgmOps.cpp:2161
for (int i = 0; i < song.systemLen; i++) {
    std::vector<DivRegWrite>& writes = disCont[i].dispatch->getRegisterWrites();
    for (DivRegWrite& j : writes) {
        if (j.addr == 0xfffffffe) {          // 延时指令
            curDelay += j.val * (44100.0 / dispatch->rate);
        } else {
            sortedWrites.push_back({i, DivDelayedWrite(curDelay, writeNum++, j.addr, j.val)});
        }
    }
    writes.clear();
}
```

三个要点：

1. **芯片核心每 tick 交出一次寄存器写列表**，不是逐采样；
2. 地址 `0xfffffffe` 是伪地址，表示"延后若干芯片时钟"，被换算成 44.1 kHz 采样偏移；
3. 所有芯片的写**混在一起按时间排序**，再统一编码——否则同一时刻多芯片的写顺序会乱。

### 3.3 单位与等待

- 内部统一按 **44,100 Hz 采样**计时；
- 只在时间真正前进时写 `0x61`（等待），因此零等待指令不会白占字节；
- 有一个 `runStreams` lambda（`:2097`）专门处理"流式芯片"（`0x93`/`0x94` 命令）
  的循环点，按剩余时间插入等待。

### 3.4 每种芯片一个 case

`performVGMWrite()`（`vgmOps.cpp:27`）是一个巨型函数，**131 个 `case DIV_SYSTEM_`**。
签名里那串状态参数说明了它的复杂度：

```cpp
void DivEngine::performVGMWrite(SafeWriter* w, DivSystem sys, DivRegWrite& write,
    int streamOff, double* loopTimer, double* loopFreq, int* loopSample,
    bool* sampleDir, bool isSecond, int* pendingFreq, int* playingSample,
    int* setPos, unsigned int* sampleOff8, unsigned int* sampleLen8,
    size_t bankOffset, bool directStream, bool* sampleStoppable,
    bool dpcm07, DivDispatch** writeNES, int rateCorrection);
```

NES 那一条最直接：

```cpp
// vgmOps.cpp:264
case DIV_SYSTEM_NES:
    w->writeC(0xb4);                            // NES APU 写
    w->writeC(baseAddr2 | (write.addr & 0xff)); // baseAddr2 = isSecond ? 0x8000 : 0
    w->writeC(write.val);
    break;
```

- `baseAddr2` 是"第二颗同型号芯片"的地址位——**同一颗芯片出现两次**时靠它区分；
- FDS 也走 `0xB4`，但写之前做了地址重映射（`:269`）；
- `0xB3` 是 N163/FME7 一类（偏移+数据），`0xB9` 是 PCE，`0xC0` 是 Sega PCM……

### 3.5 DPCM 采样走数据块，不走寄存器

```cpp
// vgmOps.cpp:61（bank 切换时）
w->writeC(0x67); w->writeC(0x66); w->writeC(0xc2);
w->writeI((isSecond?0x80000000:0) | (howMuchWillBeWritten+2));
w->writeS(0xc000);                     // 装载地址 $C000
w->write(&sampleMem[write.val<<14], howMuchWillBeWritten);
```

源码注释自己写着：

> "this code looks like a mess because it is a hack. don't blame me if your VGM ends up
> being over a gigabyte!"

说明：**采样数据是把整段 bank 塞进文件**，体积换正确性。`dpcm07` 开关则改用 `0x68`
（`0x07` 类型 = NES DPCM ROM 块）。

### 3.6 头部回填（这里全是"相对偏移"）

```cpp
// vgmOps.cpp:2291
w->seek(4, SEEK_SET);      w->writeI(len);                  // 1) 总长度
w->seek(0x14, SEEK_SET);   w->writeI(gd3Off - 0x14);        // 2) GD3 标签
                           w->writeI(tickCount);            // 3) 总采样数
                           w->writeI(realLoopPos - 0x1c);   // 4) 循环偏移 ← 相对 0x1c
                           w->writeI(tickCount - realLoopTick); // 5) 循环长度
w->seek(0x34, SEEK_SET);   w->writeI(songOff - 0x34);       // 6) 数据起点 ← 相对 0x34
```

**每一个偏移都是相对自身字段的**。这正是本项目踩过的坑：把 `0x34` 写成绝对值，
读取器会回一句 `not a VGM stream`，而错误信息指向"格式不对"。

循环点靠两张表把 tick 映射回字节位置：

```cpp
int realLoopTick = tickSample[loopTickSong];   // 循环点对应的采样序号
int realLoopPos  = tickPos[loopTickSong];      // 循环点对应的字节位置
```

### 3.7 VGZ

`engine/vgmOps.cpp` 只产出**未压缩**的 VGM 缓冲（`SafeWriter`）；压缩发生在 GUI 层：

```cpp
// src/gui/gui.cpp:2818
ret = deflateInit(&zl, Z_DEFAULT_COMPRESSION);
...
deflate(&zl, Z_FINISH);
```

也就是说 **VGZ = 同一份 VGM 缓冲的 zlib deflate**，没有额外格式逻辑。

## 4. FTM 导入是怎么做的

```cpp
// fileOps/ftm.cpp:430
bool DivEngine::loadFTM(unsigned char* file, size_t len, bool dnft, bool dnft_sig, bool eft);
```

- **2,438 行**，把 `.ftm` 完整翻译成 Furnace 自己的工程（乐器、模式、效果、扩展芯片）；
- 三个布尔参数区分：原生 FamiTracker / Dn-FamiTracker（签名不同）/ EFT 变体；
- 代码里有一处自述：`// so it appears .ftm doesn't keep track of how many patterns are stored in the file....`（`:1697`）——它也在猜格式细节。

结论：**它不把 FTM 当"驱动输入"，而是当音乐数据完整建模**。这也是它 2,438 行 vs
我们 ~500 行的差别来源。

## 5. 与我们的结构对照

| 维度 | Furnace | 本项目（`feature/multiformat-core`，已搁置） |
|---|---|---|
| 中间表示 | 完整工程（模式/乐器/效果） | 寄存器写流（`SongIr`） |
| 出声路径 | 引擎播放工程 → 寄存器写 → VGM | 自己算出寄存器写 → VGM |
| 时间精度 | tick + 芯片内延时（`0xfffffffe`） | 采样（`sampleTime`） |
| 多芯片 | `isSecond` + 地址位 | `deviceId` |
| 循环点 | `tickPos`/`tickSample` 表映射 | 一遍即止，无循环点 |
| 采样数据 | 整 bank 塞进 `0x67` 块 | DPCM ROM 块（`0x08`） |
| 扩展芯片 | 131 个 case 全覆盖 | 仅 VRC6/VRC7/N163 的寄存器映射 |

**最大的结构性差异**：Furnace 用"延时伪地址"让芯片表达 **tick 内时序**；我们的
`sampleTime` 已经能做同样的事，只是我们的写是**自己算出来的**，而它是**芯片核心产出的**。

## 6. 可取与不可取

### 值得参考的部分

| 内容 | 位置 | 价值 |
|---|---|---|
| 头部字段的相对偏移规则 | `vgmOps.cpp:2291` | 直接对应我们踩过的 bug |
| 多芯片的 `isSecond` 编码 | `vgmOps.cpp:30` | 我们的 `deviceId` 只区分芯片，不区分同型号重复 |
| tick → 字节位置的两张表 | `vgmOps.cpp:2307` | 将来做循环点必用 |
| NES DPCM 块的两种写法 | `vgmOps.cpp:37`、`:61` | `0x07`（ROM 块）vs `0xC2`（bank 快照） |
| FTM 效果号 → 语义的映射 | `fileOps/ftm.cpp` | **当字典用**，不是抄代码 |

### 不该照抄的部分

- **2,256 行单函数** `performVGMWrite`：131 个 case 挤在一个函数里，是体积增长的结果，不是设计；
- **采样整 bank 塞文件**：注释自己承认会导致上 GB 的文件；
- **完整工程模型**：我们要做的只是寄存器层，引入工程模型等于重写 Furnace 的一半。

## 7. 三条可选路线（供决策）

| 方案 | 做法 | 代价 | 风险 |
|---|---|---|---|
| **A** | 参考 `ftm.cpp` 的映射表，重写我们的 FTM→`SongIr` | 中 | 之前正是死在这层细节上 |
| **B** | 照搬两层架构（工程模型 + 播放器） | 大 | 高，但与 Furnace 同样健壮 |
| **C** | 先做 NSF（驱动自带，不需要导入器） | 小 | 只解决 NSF |

**当前建议**：C（最快出声）+ 把 `ftm.cpp` 当**字典**产出我们自己的"效果号 → 语义"
对照表，再决定 A 是否值得做。

## 8. 已核实的行号索引

| 主题 | 位置 |
|---|---|
| VGM 导出入口 | `engine/vgmOps.cpp:566` |
| 单芯片写编码 | `engine/vgmOps.cpp:27`（函数头）、`:264`（NES） |
| 延时伪地址 | `engine/vgmOps.cpp:2165` |
| 写排序与等待 | `engine/vgmOps.cpp:2188` |
| 头部回填 | `engine/vgmOps.cpp:2291` |
| GD3 | `engine/vgmOps.cpp:2258` |
| DPCM 数据块 | `engine/vgmOps.cpp:37`、`:61` |
| VGZ 压缩 | `gui/gui.cpp:2818` |
| FTM 导入 | `fileOps/ftm.cpp:430` |

> 以上仅为静态阅读结论，**没有运行 Furnace**。若要用于实现，建议先用它导出一份
> `.vgm` 与我们的产物做逐命令对比，再决定采纳哪一条。
