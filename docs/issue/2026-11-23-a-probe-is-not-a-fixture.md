# 探针不是夹具，oracle 不是校准

> 2026-11-23。用户问了一句：**"为什么你的 OPL 探针测试要放到 `OpnCoreOracleTest.cpp` 里？"**
>
> 那个文件的第一行写着 "The YM2612 decoder against the chip itself"，而里面躺着三颗别的芯片的满度测量。

## 一、我怎么做的

`testOpmReachesFullScale` / `testOpllReachesFullScale` / `testOpl2ReachesFullScale` 全写进了
`OpnCoreOracleTest.cpp`。

**理由很实际**：那是**唯一已经链接 libvgm**（`SndEmu_Start`、`DEVID_*`）并且已经链了
`FmVoiceDriver` 和 `NoteWaveform` 的测试二进制。**放进去不需要新 target、不需要改 CMake、不需要新的
运行入口。**

## 二、为什么这个理由是错的

### 1. 两类测试的失败含义不同

| | 它问的问题 | 失败意味着 |
|---|---|---|
| **oracle**（该文件本来的内容） | 同一段寄存器流，解码器和芯片是否给出**相同答案** | 解码器错了，或者芯片模型的理解错了 |
| **calibration**（我塞进去的） | 这颗芯片在某个 patch 下**能到多高** | 表里那个数字错了 |

**同一个红色，两种完全不同的含义，被放进同一个文件。**

### 2. 名字与内容漂移——而这正是我另外两份文档在批评的事

`the-variant-nobody-passed.md` 讲"实现存在但没接上"，`one-chip-is-not-a-class.md` 讲"名字和适用范围
不一致"。**这个文件是同一类问题的第三种形态**：一份文档承诺回答 A，实际在回答 A 和 B。

**而它不会报错**——这正是这类问题能活下来的原因。

### 3. 它给临时探针提供了落脚点

我在这两轮里反复往这个文件里塞**临时**探针（打印 patch、打印鼓的写入、扫 trim），验证完再删。

**临时代码之所以能反复进提交，是因为这个文件已经被当成万用工具夹了。**
一个有明确职责的文件会让人本能地不往里塞无关东西；一个已经在漂移的文件不会。

## 三、规矩（写给下一次的我）

1. **临时探针不进提交。** 它存在的理由就是"看一眼就删"；一旦需要保留，它就该变成断言。
2. **需要长期保留的测量 = 断言 + 独立文件 + 语义正确的 target。**
   一个数只要被写进代码（`fmOutputBits`），就必须有一条断言守着它，且那条断言放在
   "问这个问题"的文件里。
3. **一个测试文件的头注释描述什么，就只能放什么。** 头注释是契约，不是装饰。
4. **"没有别的 target 能链接这个库"不是理由，是信号。** 出现这个念头时，正确动作是加一个 target
   （半小时），而不是把内容塞进最近的邻居。

## 四、结果

**拆开**（`vgm_fm_level_test`）：

| 文件 | 内容 | target |
|---|---|---|
| `Tests/OpnCoreOracleTest.cpp` | 6 个用例：YM2612 解码器 vs 芯片、OPM 出声、音高、抽取滤波 | `vgm_opn_test` |
| **`Tests/FmLevelTest.cpp`** | OPM / OPLL / OPL2 的满度与混音电平（未来 OPL3、OPX 也在这里） | **`vgm_fm_level_test`** |

`FmLevelTest.cpp` 的头注释写明了它**不做**什么：

> These are not decoder oracles. An oracle asks two implementations the same question and compares
> their answers; these ask the chip one question - with its loudest patch, how far up the scale does a
> single voice get? - and record the answer as `chips::fmOutputBits`.

**两边的用例数与断言数都没有减少，只是各回各位。**

## 五、一句话

> **"放这里最省事"和"放这里对"的区别，通常只值半小时。**
> 而省下的那半小时，会以"下次又放进来"的形式反复收利息。
