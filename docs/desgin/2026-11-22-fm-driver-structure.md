# `OpnVoiceDriver` 该不该按芯片拆分

> 2026-11-22。回答"是不是该按芯片拆成 `OpnVoiceDriver` / `OpmVoiceDriver`"。
>
> 结论：**不要拆成两个完整驱动文件**，但**必须改名，并把五处差异收进策略**。

## 一、现在的混入程度

`OpnVoiceDriver.cpp` 404 行，`isOpm` 出现 6 次（1 处定义 + 5 处使用）：

| 位置 | 内容 | 性质 |
|---|---|---|
| L266 | `channelInPort` 掩码（`& 0x07` vs `& 0x03`） | **数据**，1 行 |
| L273-306 | `load` 的初始化序列（写 `0x20` vs 写 `0x22`/`0x27`/`0x2B`/`0x29`） | **代码**，约 34 行 |
| L313-319 | `load` 末尾补写 pan | **代码**，约 7 行 |
| L340-370 | `noteOn` 的 KC/KF 反算 | **代码**，约 31 行 |
| L432-437 | `noteOff` 的 `0x08` vs `0x28` | **代码**，约 6 行 |

**合计约 79 行，占 404 行的 20%。**

**关键一点：五处里只有一处是数据，其余四处是代码块。** 这决定了方案选择（见第四节）。

## 二、为什么"拆成两个文件"行不通

### 2.1 驱动实例是**一个**，不是每芯片一个

```240:246:plugin/Source/backends/libvgm/LibvgmTrackPlayer.h
std::unique_ptr<playback::IVoiceDriver> voiceDriver;
...
mutable std::unique_ptr<playback::IVoiceDriver> exportDriver;
```

**一个 `LibvgmTrackPlayer` 只有一个 `voiceDriver`，服务整首歌的所有芯片**——而一首 VGM 可以同时有 YM2612 和 YM2151（那个 "a backend with an OPN and an OPM rebuilds" 的注释就是这么来的）。

**所以拆成两个驱动后，必须再加一个门面按 `model` 分派。** 而那个门面要做的事，和现在 `load` 里的 `if` 是同一件事——只是把 `if` 换成了 `unique_ptr` 的两次 `new`。

### 2.2 `Core` 与渲染完全型号无关

| 成员 | 是否按型号变化 |
|---|---|
| `Core` 的构造（`SndEmu_Start` + `devFunc`） | **只参数化**：`clockFor(model)`、`deviceIdFor(model)`、`smplRate` |
| `render`（`devDef->Update` + `(L+R)/2`） | **完全无关** |
| `rate` | **完全无关** |
| `startCore` 的速率选择 | **只参数化** |

**拆开的话，这四样要么复制两份，要么再提取一个 `FmVoiceCore`。** 而提取出来之后，两个驱动各剩不到 100 行，**"两个文件"就只是为了那 79 行差异**。

## 三、`Vrc7VoiceDriver` 不能当作先例

它确实是独立文件（186 行），但理由不同：

**`Vrc7VoiceDriver` 不走 libvgm**——它用自己的 OPLL 实现，没有 `DEV_INFO`、没有 `devFunc`、没有可共享的 `Core`。

**所以那条先例的判据是"不同基础设施才拆文件"，不是"每颗芯片一个文件"。** 把 OPM 和 OPN 拿它类比，前提就不成立。

## 四、三个方案

| | 文件 | 改动 | 加 OPL 家族时 | 名字 |
|---|---|---|---|---|
| **A** 只改名 | 1 | 改 6 处名字 | 加第 3 个 `if` | `FmVoiceDriver` ✓ |
| **B** 拆两驱动 + 门面 | 4–5 | 提取 `Core`，加门面 | 加第 3 个派生类 | 各自诚实 ✓ |
| **C** 改名 + 策略 | 2 | 4 段代码搬进策略 | 加 1 个策略实例 | `FmVoiceDriver` + `OpnDialect`/`OpmDialect` ✓ |

### 推荐 C

```cpp
// 匿名命名空间，与 Core 同文件
struct Dialect
{
    int channelMask;                       // 0x03 / 0x07
    void ( *init ) ( Core&, int channel ); // 0x22/0x27/0x2B/0x29  vs  0x20 = pan
    void ( *pitch ) ( Core&, int channel, double hz );
    void ( *keyOn ) ( Core&, int channel );
    void ( *keyOff ) ( Core&, int channel );
};

constexpr Dialect kOpnDialect { 0x03, &opnInit, &opnPitch, &opnKeyOn, &opnKeyOff };
constexpr Dialect kOpmDialect { 0x07, &opmInit, &opmPitch, &opmKeyOn, &opmKeyOff };
```

`load` / `noteOn` / `noteOff` 变成编排：

```cpp
dialect->init (core, channelInPort);
for (const auto& reg : patch.regs) { core->write (port, reg.first & 0xFF, reg.second); }
dialect->afterPatch (core, channelInPort);   // OPM 的 pan 补写，OPN 空
```

**加 OPL 家族时加一个 `kOplDialect`，不动 `load`/`noteOn`/`noteOff`。**

### 为什么不用 B

`LibvgmTrackPlayer` 的一个实例必须能服务多颗芯片——**门面是绕不过去的**。而门面 + 两个派生类，与门面 + 两个策略结构，是同一件事的两种写法；**后者少两个文件、少一层继承、`Core` 不用提出 `.cpp`**。

### 为什么不用 A（只改名）

**改名解决的是"名字说谎"，不解决"加一颗芯片要改四个地方"。** 而 `code-quality.md` §4 的判据是后者。

## 五、风险与代价

| 项 | 评估 |
|---|---|
| 策略结构必须与 `Core` 同文件 | `Core` 含 `DEV_INFO`，只能在 `.cpp`；策略放匿名命名空间即可 |
| 函数指针的可读性 | 4 个方法都是"写几个寄存器"，比 34 行的 `if` 分支短 |
| `noteOn` 的 OPM 数学（31 行）要搬 | 整段搬进 `opmPitch`，没有逻辑变化 |
| 行为风险 | **纯结构改动**，但 `code-quality.md` §9 要求"重构不得夹带行为变更"——**先落一条断言（`peak`），再改结构** |

## 六、建议的顺序

**这次不做。** 理由：

1. **OPM 刚跑通，`peak = 1.0` 是手验的，还没有断言守着。** 在结构改动之前先补断言，否则"改了之后不对"分不清是重构错还是本来就没验证过。
2. 分支 `feature/fm-export-fidelity` 已经载着 15 个文件的未提交改动（导出费率 + 后台化）。**两件不相干的事挤在一个分支里，评审时读不出哪一处属于哪件。**

**之后的顺序：**

| 步 | 内容 |
|---|---|
| 1 | 合并 `feature/fm-export-fidelity`（导出费率 + 后台化，先验过） |
| 2 | 补 OPM 的 `peak` 断言 |
| 3 | 改名 + 策略（独立分支 `refactor/fm-voice-dialect`），**纯结构，一条断言都不改** |

**第 3 步的验收标准是"所有断言不变地通过"**——这是 §9 对 refactor 唯一的验证方式。
