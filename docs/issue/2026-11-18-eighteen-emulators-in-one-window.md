# 十八个模拟器挤在一个窗口里：9 KB 的曲子为什么占 1.2 GB

> 一首 **9 KB** 的 VGM，打开后工作集 **1221 MB**。
> 把混音器关掉，它立刻落回几十 MB。
>
> 所以那不是「一首歌的内存」，是「十八条轨各自一个完整模拟器的内存」。

## 一、症状

`07 Challenging Stage Clear.vgz`（Namco ND-1，2× YMW258 + C352）：

```
vgz 压缩后        1 KB
解压后 VGM        9 224 字节
峰值工作集        1221.4 MB
```

**十万倍。** 而曲子本身只有 3 个通道在响。

## 二、排查：四层，每层排除一类

### 第一层：文件大小

先排掉「数据太大」这个最自然的猜测。

`04 Stage 1.vgz`（1.8 MB）289 MB，`02 The Military System`（94 KB）312 MB —— **两者相同**。

**占用与文件大小无关**，所以不是数据本身，是结构。

### 第二层：VMMap，看是哪一类内存

```
Private Bytes    1,741,476 K  = 1.7 GB
Private Data     1,108,824 K  = 1.08 GB     ← 就是它
```

而地址列表里**几十行 `16,388 K` 的块**，`Type = Heap (Private Data)`，`Heap ID: 1`（CRT 主堆）。

`16,388 K ÷ 8 = 2,097,664 ≈ 2^21` —— **2M 样本的立体声 int32**。

**1.08 GB ÷ 16 MB ≈ 68 个。**

### 第三层：分配钩子，把 C++ 排掉

`plugin/Tests/AllocProbe.cpp`（Release 分支）重载了全局 `operator new`：

```
<1K     count=70 760    total=  4.8 MB
<64K    count=1 233     total=  9.5 MB
<1M     count=0         total=  0.0 MB
>=1M    count=10        total=160.0 MB

整个运行分配总量：174.2 MB
```

**C++ 侧只有 174 MB，而工作集是 1221 MB。**

`operator new` 是**所有** C++ 分配的必经之路 —— `std::vector`、`std::string`、`make_shared`、JUCE 的容器全走它。**174 MB 就是 C++ 的全部。**

**剩下的 1 GB 来自 `malloc`**，而这个项目里的 C 代码只有一处：**libvgm 和它的芯片核心**。

### 第四层：Debug CRT 钩子，拿到调用栈

`AllocProbe.cpp` 的 Debug 分支改用 `_CrtSetAllocHook`（能看见 `malloc`），`RtlCaptureStackBackTrace` 取栈，退出时用 `SymFromAddr` 解析：

```
>=1M    count=99     total=1574.4 MB

0x...5557   1408.0 MB   WriteChipROM+0x47
0x...FB04    160.0 MB   std::_Allocate<16,...>
```

**99 次，1.4 GB，同一个调用点。**

## 三、根因

`WriteChipROM` 本身**不分配**：

```648:661:libs/libvgm/player/vgmplayer_cmdhandler.cpp
if (memID == 0)
{
    if (cDev->romSize != NULL)
        cDev->romSize(cDev->base.defInf.dataPtr, memSize);
    if (cDev->romWrite != NULL && dataLen)
        cDev->romWrite(cDev->base.defInf.dataPtr, dataOfs, dataLen, data);
}
```

它只是把 `memSize` 和 `data` **转发**给芯片。真正分配的是 `cDev->romSize(...)` —— **芯片核心收到 `memSize` 后自己 `malloc` 一块 ROM。**

**`memSize` 来自 VGM 的 `0x67` 数据块，是芯片的寻址上限**（C352 声明 32 MB，按 16 MB 分档），**与曲子里实际有多少采样无关**。

而 **`perTrack` 为真时，每条 mixer 轨一个 `LibvgmTrackPlayer`**：

```1201:1213:plugin/Source/playback/PlaybackEngine.cpp
const bool perTrack = config.mixerEnabled
                       && caps.isolation == playback::IsolationMode::multiInstance
                       && ! pending.layout.empty ();
...
const size_t playersWanted = perTrack ? pending.layout.size() : 1;
```

**18 个 player × 每个 3-5 个设备 × 16 MB = 1.2 GB。**

**每一份都是同一个 ROM 的拷贝，因为每个芯片实例都要有自己的地址空间。**

## 四、为什么关掉混音器就好了

这就是上面那段代码的 `&&` 第一项。

`config.mixerEnabled` 为假 → `perTrack` 为假 → **`playersWanted = 1`** → 一个 player、一个芯片实例、一份 ROM。

**用户把混音器关掉，内存立刻落回去——这是对这个根因最直接的验证**，比我读十遍代码都强。

## 五、为什么没修

### 改 libvgm 加共享机制？评估过，不做

ROM 缓冲区**住在芯片实例内部**，不在 `VGMPlayer` 那一层。要共享就得改 `romSize` 的签名，然后：

| 芯片 | ROM 性质 |
|---|---|
| C352 / C140 / SegaPCM / QSound / MultiPCM … | 只读 |
| **RF5C68 / WonderSwan / VSU** | **可写 RAM** |

**可写的不能共享**，共享了就是一个 player 的写入污染另一个的声音。真做起来会变成「只读的共享、可写的各留一份」，每个芯片还要标自己是哪一种。

**这不是加一个共享机制，是给 ROM 引入所有权模型**——涉及 14 个芯片核心，而收益和下面那条相同。

### 设备级 track：可行，但没做

`enumerate()` 里 track 按 **device** 分组而不是按 channel，`layout.size()` 从 18 变成 3-5，内存到约 250 MB。

**代价**：一个 YM2612 的六个 FM 通道只能整体调音量。

**收益**：键盘页的通道级波形不受影响（它读 `trackPlayers[0]` 的 `captureState`，那个聚合 player 仍能看到全部通道）。

**这是一个产品决定，不是技术决定**，所以留在这里等它被决定。

### 已做的那一小部分

`RomCache`（`46106d7`）：`VgmReplayer` 的 `image` 从 `std::vector` 改成 `shared_ptr`，按芯片类型共享。那一笔是**另一条路径**（`memory::forRequirements`），省了 128 MB（`04 Stage 1` 289 → 125 MB），**但碰不到 `0x67` 这条**。

## 六、我低估了多少

上一轮我告诉用户"18 个 player × 6.3 MB ≈ 100 MB，只占 1.2 GB 的 9%"。

**实测是 1.4 GB。差一个数量级。**

错在**把「每个 player 的 C++ 对象」当成「每个 player 的全部成本」**。真正的成本在 C 侧：每个 player 的每个设备都要装一份 ROM，而 ROM 是按芯片寻址上限分配的。

**教训**：`operator new` 的钩子只能回答"C++ 用了多少"，**不能回答"这个对象总共占多少"**——那个对象可能在 C 代码里又分配了一大片。

## 七、下次的检查清单

1. **「关掉某个功能就好」是最快的二分。** 这次用户一句话就锁定了 `mixerEnabled`，比四层工具都快。
2. **C++ 的分配钩子看不见 C 代码的 `malloc`。** 两者都要测，否则会得出"内存不在我这边"的错误结论。
3. **`RtlCaptureStackBackTrace` 的 skip 帧数要试。** skip 3 落在 `_malloc_dbg+0x2f`，skip 6 落在 `realloc+0x2e`，skip 8 才到 `WriteChipROM+0x47`。
4. **转发函数不是分配点。** `WriteChipROM` 的栈帧在分配上方，但它自己一个字节都不申请。
5. **芯片的 ROM/缓冲按寻址上限分配，不按曲子大小。** 一个 9 KB 的 VGM 可以让芯片装 16 MB 的 ROM。
6. **估算和实测差一个数量级时，先说"我低估了"，再解释为什么。**

## 八、结论

**内存 = 混音器轨数 × 每轨设备数 × 芯片 ROM 上限。**

不是曲子的大小，不是波形的精度，不是采样分析。

**关掉混音器就恢复**，这是这个公式最简短的证明。

两条路都写在这里：改引擎（14 个核心，涉及可写 ROM 的所有权），或改 track 粒度（产品决定）。**在有人做出那个决定之前，这个 1.2 GB 是「通道级混音」的真实价格，不是 bug。**
