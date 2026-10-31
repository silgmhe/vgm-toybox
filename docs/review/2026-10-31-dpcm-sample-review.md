# DPCM 采样分支评审

**评审对象**：`feature/keyboard-wavetable-export`，35 个提交，40 文件 +3875 / −703
**对照基线**：`master`，评审点 `d080d5e`
**依据**：`.codebuddy/rules/code-quality.md`（12 条）
**方法**：读 diff 与关键函数，逐条对照规则；只看代码与文档一致性，不评价功能方向

---

## 结论

**功能是正确的，但注释和文档停在半路上。**

这一支上改了三次判断（`$4013` 四位→八位、长度改边界法→又改回寄存器、缓冲 193→241→4096），**每一次代码都改对了，注释却没有跟着改**。结果是同一个结构体里的两条字段注释，说的都是已经不存在的实现。

问题分布：

| 类型 | 条数 | 性质 |
|---|---|---|
| **注释与实现矛盾** | 3 | 直接误导，且都是这几次改动自己造成的 |
| **核心算术无单测** | 1 | 过去三次出错都在这三个函数里 |
| **过时设施未清理** | 4 | 边界法删了，围绕它的字段、缓存键、注释还留着 |
| **重复与死代码** | 3 | 规则第 4、5 条点名的形状 |
| **文档过时** | 1 份 | `research/2026-10-29` 仍是四位时代 |

**没有发现正确性缺陷。** 八位长度、尾部填充裁剪、padding 字节、`$4015` 边沿、bank 快照，这五处逻辑与 `nes_dmc.cpp` 和 NotSoFatso 的行为一致，`Entry080` 的 176/688 也与 FamiStudio 的 `.dmc` 逐字节相同。

---

## 一、必须修

这三条不是风格问题：它们让读代码的人按一个不存在的实现去理解系统。

### H-1 `NsfBus.h` 的两条字段注释停在四位时代（规则 2、12）

```74:95:plugin/Source/backends/nsf/NsfBus.h
struct DpcmSelection
{
    uint16_t start = 0;		// absolute CPU address: $C000 + page * 64
    uint16_t length = 0;	// bytes: (register & 0x0F) * 16 + 1
```

而实际写入是：

```200:209:plugin/Source/backends/nsf/NsfBus.cpp
pendingDpcmLength = (uint8_t) (val & 0xFF);
```

`& 0xFF` 之后长度上限是 **4081**，不是注释里那条四位公式的 241。同一结构体的下一条注释也在说旧事：

> That register names one playback and **holds four bits, so it tops out at 241 bytes**

**规则 12 是这一支自己立下的**——"一个寄存器的宽度，去实现里读"。注释违反的正是这条。

顺带一处：`bytes` 的注释把"320 and 512"写成这个格式的通则，而那是 `Entry080` 的两个采样。规则 12 第 3 条刚说过不要拿观测值当上限，注释里又用了一次。

### H-2 `dpcmSampleLength` 一族没有任何断言（规则 6、7.5）

`trimFiller`、`decodeDpcm`、`NsfPlayer::dpcmSampleLength` 三个函数，测试里零命中。

而这三轮所有错误都出在这里：

| 错 | 表现 | 发现方式 |
|---|---|---|
| 长度取 `$4013` 的低四位 | 689 → 177 | 用户听出军鼓不对 |
| 尾部填充没裁 | 321 而非 320 | 和 `.dmc` 逐字节比对 |
| `16k+1` 的 padding 字节没去 | 321 而非 320（另一种路径） | 同上 |
| 长度取"一次播放"而非"采样" | 320 → 65 | 用户提供 `.dmc` 才暴露 |

**四次都是靠外部文件比对发现的，没有一次是测试抓住的。** 规则 7.5 要求核心算术必须有覆盖边界与历史缺陷的用例，这四个错正是现成的用例。

建议：把三个函数移到可引用的位置（`playback/` 或 `backends/nsf/` 的公开函数），用 `Entry080` 的 `$D200`/`$D380` 两段真数据做固定基准，断言 320 / 512、以及"长度落在 16k+1 时少一字节"。

### H-3 `decodeDpcm` 的注释挂在了 `trimFiller` 上（规则 1、2）

```58:67:plugin/Source/backends/nsf/NsfPlayer.cpp
/** The DPCM sample at `start`, integrated into -1..1.
    ...
*/
uint16_t trimFiller (const std::array<uint8_t, 4096>& bytes, uint16_t length)
```

这段注释描述"以 `start` 为起点积分"，**签名的两个参数里没有 `start`**；而它本该描述的 `decodeDpcm` 就在下一行，没有注释。

原因是加 `trimFiller` 时把注释插在了函数之间。读者按注释理解 `trimFiller`，会以为它做积分。

---

## 二、应修

### M-1 `pagesSeen` 的注释和它的用途已经分家（规则 2、5）

```114:123:plugin/Source/backends/nsf/NsfBus.h
/** Every distinct $4012 page seen, in order, with how many times each was written.

    Diagnostics for one question: a sample longer than 241 bytes ... has to be played in
    pieces, and the pieces are named by walking this register.
*/
```

三处不准：

1. 说"Diagnostics"，但它是 `dpcmPages()` 的数据源，且 `NsfPlayer.cpp:1031` 拿 `pageCount` 当了缓存键；
2. "longer than 241 bytes" 是四位时代的话；
3. `NsfBus.cpp:233` 还留着 **"This list is what the boundaries are read off"**——长度已经不看边界了。

### M-2 缓存键里有一个已经不参与计算的量（规则 5）

```1026:1038:plugin/Source/backends/nsf/NsfPlayer.cpp
// Keyed on the start count as well as the selection, because ...
// and keyed on the page count, because the length is "to the next sample" and
// that boundary is unknown until the driver has pointed at the next one.
if (dpcmWaveStart != bus->dpcm.start || dpcmWaveLength != length
    || dpcmWaveStarts != bus->dpcmStarts || dpcmWavePages != bus->pageCount)
```

边界法删掉之后，`pageCount` 不再影响 `length`，这条键只会带来无谓重算和一句错注释。`dpcmWavePages` 字段随之成为死键。

### M-3 `isSample` 的判据写了两遍（规则 4、7）

```356:plugin/Source/playback/WaveExport.cpp
const bool isSample = channel.sampleRate > 0.0f && channel.wave.size () >= 2;
```

```283:plugin/Source/playback/WaveExport.cpp
if (path.empty () || channel.wave.size () < 2 || channel.sampleRate <= 0.0f)
```

第二处是同一个判据的反写。规则 7 的"同一个语义只能有一个来源"，改动一处就会不一致——比如将来允许 `wave.size() == 1` 的单字节采样，只改一边就会静默跳过。

### M-4 两个只写不读的字段（规则 5）

`dacValue`（`NsfBus.cpp:185` 写，无人读）、`pageWrites`（`NsfBus.cpp:198` 写，无人读）。它们是排查"鼓的衰减在不在 DAC"时的计数器，结论已经拿到（2 次），字段留着没有消费者。

同理，`dacWriteCount()` / `dpcmStartCount()` / `dpcmPages()` 只有 `NsfProbe` 在用。这不是错，但应在头文件注明"探针专用"，否则会像 `sampleRate` 那样被误认为所有后端都填。

### M-5 每帧整份拷贝采样（规则 3）

```1040:plugin/Source/backends/nsf/NsfPlayer.cpp
state.wave = dpcmWave;
```

`dpcmWave` 最多 4081×8 = 32648 个 float（130 KB），而这段代码在 `captureState()` 里**每帧执行一次**，15 Hz 就是约 2 MB/s 的纯拷贝，还发生在音频线程上。`wave` 是值类型（`std::vector<float>`），无法避免拷贝，但可以只在长度或起点变化时赋值，其余帧复用同一个已填好的 `ChannelState`。

### M-6 `decimateForDisplay` 埋在匿名命名空间（规则 6）

它是纯算术（分桶取绝对值最大的点），却定义在 `KeyboardRowPaint.cpp` 的 `namespace { }` 里，无法被调用、无法被断言。规则 6 点名的反例就是"波形合成埋在 private 嵌套类里，只能画出来看"。

---

## 三、可延后

- **注释转义损坏两处**：`NsfBus.h:55-57` 的 `\-\ for VRC6 ... \/\ for VRC7`，`NsfBus.cpp:164-165` 的 `at - rather than` 与 `its master switch at .`——地址字符丢失，读的人得不到信息。不是这次 DPCM 引入的，但既然看到就记在这里。
- **`NsfBus::peek` 用 `const_cast` 调非 const `Read`**（规则 3）。注释解释了"只读、不改状态"，但 const 性被绕过了，属于接口形状的味道。
- **`4abaad5` 一个提交做了两件事**：改位宽 + 删边界法。两者相关，且提交说明里写了删除理由，可以接受；严格按规则 8 应当拆成两个。
- **`d080d5e` 把规则与 memo 放在同一提交**。规则 12 就是那份 memo 的提升，主题一致，可接受。

---

## 四、文档一致性

**`docs/memo/2026-10-30-the-register-width-nobody-checked.md` 与代码一致。** 第 4 节"正确的实现，最终形态"写的正是当前代码（八位、`16k+1` padding、无边界推断）。

**`docs/research/2026-10-29-dpcm-sample-display.md` 过时两处：**

```
L84  | 长度 | $4013 → (reg & 0x0F) * 16 + 1 字节 |
L94  3. 断言：... 长度等于 $4013 推出值 ...
```

两处都是四位时代。另外这份文档缺少后来才知道的两件事：**尾部填充要裁剪**，以及 **`$4013` 公式里那个 `+1` 是每采样自带的 padding 字节**。

`docs/research` 的定位是"某次调研的快照"，过时本身不算缺陷，但**它现在给出的公式是错的**，而 H-2 的修复要以它为依据写测试。所以要么改这两行，要么在文首加一行"本文写于四位时代，长度部分见 memo 2026-10-30"。

---

## 五、建议顺序

1. **H-1、H-3、文档四处**（纯文字，零风险，一次提交）
2. **H-2 的断言**（先把 `Entry080` 的 320/512 钉住，再谈重构）
3. **M-1、M-2、M-4**（边界法的遗留物，一起清）
4. **M-3、M-5、M-6**（各自独立，随下次触碰该区域处理）

前两步做完，这一支就达到了规则里"注释与实现一致、核心算术可断言"的最低线。

---

## 六、检查过、无问题的部分

- **颜色**（规则 7）：本次改动不涉及任何颜色，`theme::` 之外无字面量。
- **`sampleRate` 的空值**（规则 11）：只有 `NsfPlayer` 填它，其余后端为 0 并走原路径——注释已写明"Zero when this channel's shape is not a sample"，这是有意的常态而不是缺陷。
- **缓冲大小**（规则 12.3）：4096 来自 bank 窗口宽度，不是观测值。
- **断言内容**（规则 12.4）：`NsfFormatTest` 里新增的几条写的是"应当"（边沿、速率、循环位），没有把观测值写成应当。
