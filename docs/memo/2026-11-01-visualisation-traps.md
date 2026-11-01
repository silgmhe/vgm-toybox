# libvgm 可视化：十二个坑

> 这一支（`feature/libvgm-visualisation`）从"键盘页有行"做到"DPCM 和 Noise 的音符都对"，其间
> 修掉的每个问题都有一个共同点：**它不报错，只是画得不对**。这份记的是那些坑的形状，不是功能说明。
>
> 功能说明在 `docs/desgin/libvgm-visualisation.md`；这里只写"哪里会骗人"。

## 0. 一句话

**这一层没有编译错误、没有断言失败、没有异常。** 所有错都以"某个通道看起来正常"的形式出现，
所以判据只能是"和另一个正确的东西对比"——导出对而显示不对、音频对而画面不对、NSF 对而 libvgm 不对。

## 1. 静默丢弃：`sendToSeparateBuffer` 遇到空回调直接 return

**症状**：整个 NES 曲子一行都没有。日志 `inst=0`。

```cpp
// ChipWriteRouter::route
case midi::ChipKind::nesApu:
    sendToSeparateBuffer (onNesWrite, DEVID_NES_APU);
//  sendToSeparateBuffer 里：if (sink == nullptr) { return; }   ← 不报错
```

**根因**：NES、GB、SSG 的写交给回调而不是注册表。我照搬 `VgmReplayer` 的结构，**漏了同一段接线**。

**教训**：`if (nullptr) return;` 在热路径上是"丢弃"，不是"跳过"。**当路由表有三条分支时，
数一遍谁接了几条。**

## 2. 两条读取同一份数据的路径，只有一个是对的

**症状**：DPCM 波形是一条平线，而**同一个文件的导出是正确的**。

导出走 `collectNesDpcmSamples`（直接读命令流），显示走解码器的内存镜像。
**两条路读同样的字节，得出不同的答案。**

**修法**：显示改用导出的那份数据（`DecodedSample` 加 `sampleId`），镜像路径整条删掉。

**教训**：**同一份数据有两个读取者时，先问它们是否一定一致。** 不一致的症状是"一个对一个错"，
而错的那个通常还在工作，只是内容不对。

## 3. `period == 0` 不等于"没有音高"

**症状**：Noise 行的音符号是空的。

```cpp
else if (chipState.period != 0 || chipState.block != 0)   // ← 挡住了一切
```

2A03 的 noise `period` 是 **`periodIndex`（0–15 的索引）**，**索引 0 是最慢档，不是静音**。

**修法**：加 `hasPitch` 标志，只有 noise 填。**不改 `period == 0` 的含义**——其余 29 个解码器靠它表达"没有音高"。

**教训**：一个字段的取值域被两个含义共用时（0 = 值 / 0 = 缺省），**加一个布尔比改语义便宜**。

## 4. 位宽凭记忆猜：`$4012` 是 8 位，不是 6 位

**症状**：DPCM 波形变成一条**固定不变**的台阶。

```
miss id=0x000081      ← 解码器：page=0x00
ids:   0x800081 ...   ← 收集器：page=0x80
```

`0x80 & 0x3F = 0`。而 `$4012` 的语义是 **`$C000 + value * 64`**，value 覆盖整个 `$C000..$FFFF`。

**后果链**：page 错了 → `sampleId` 不匹配 → 查表落空 → `wave` 为空 → **退回画 `cycleFor(delta)` 的合成台阶**
→ 看起来是波形，永远不变。

**教训**：这是这一支**第三次**凭记忆猜位宽（前两次：`$4013`、OPL 的 slot 表）。
**凡是有"低 N 位"字样的寄存器，去被模拟的核心里读一行。** 猜错的症状不是崩溃，是"看起来对"。

## 5. 注释说必须除以周期，代码没除

**症状**：C352 每个通道都是 C9；C140 / ES5503 也偏高，只是幅度小到像正常音高。

```cpp
// Pitch.h 的注释
// `transpositionCents (rate / cycle, middleC)` equals
// `transpositionCents (rate, refRate) + rootCentsFromCycle (cycle, refRate)`

// Pitch.h 的代码
pitch.hz = rate;        // ← 少了 / cycle
```

`rate` 是**字节/秒**。旁边 `rootCentsFromCycle` 老老实实做了 `referenceRate / cycleLength`。

**教训**：**同一个函数里两处做换算、一处做了一处没做，那就是证据。** 不用先知道正确答案。

## 6. `$4013` 的长度带填充

**症状**：波形里有尾巴。

`$4013` 是 `16n + 1` 字节，驱动启动短样本时仍请求一整块，**尾部是填充**。
NSF 那边一直用 `dpcm::finalLength` 裁掉，libvgm 侧没有。

**教训**：长度寄存器的"值"和"实际数据长度"是两件事，**导出侧如果做了裁剪，显示侧也要做**。

## 7. `released` 被当成"保持显示"用

**症状**：DPCM 行整首歌都是亮的，音符停在最后一次命中，**把 note-off delay 调到最大也没用**。

我让 `released` 永远为真，想解决"鼓点之间闪断"。结果：

```cpp
// ContentSlot::afterSnapshot —— 早有这套
if (channel.active) held[i] = { note, volume, now };
else if (holdMs > 0 && (now - held[i].at) <= holdMs) { ... channel.active = true; }
```

`active` 永真 → `held` 每帧刷新 → **绕过设置里的 `holdMs`**。

**教训**：**解码器只该说"现况"，不该说"显示多久"。** 后者是显示层的决策，而且已经有地方实现了。

## 8. 琴键只看 `note`，不看 `active`

**症状**：波形暗了，琴键还亮着。

```cpp
const bool audible = channel.active && channel.volume > kSilenceLevel;   // 波形用它
paintKeys (g, row.keys, channel.note);                                  // 琴键只看 note
```

芯片的音高寄存器**会一直保持最后的值**，所以"没在响"和"有音高"可以同时成立。

**教训**：**同一个文件里两个判据，我只查了一个。** 改颜色时该顺便问"还有谁在用这个状态"。

## 9. `keyOns` 不能拿来做显示计数

**症状**：`testNoiseIsADrum` 立刻失败，MIDI 输出会多出音符。

`NoteExtractor` 用 `keyOns` 的变化**切分音符**（重触发）。我第一版把显示用的计数加在它上面。

**修法**：加独立的 `hitCount`，`keyOns` 一个字没动。

**教训**：**给已有计数器换语义之前，先 grep 谁在读它。** 那次是测试救的，不是我想到的。

## 10. 捕获是"采样"，命中是"事件"

**症状**：加速的鼓点丢音符，音频有、画面没有，**delay 调到最大也没用**。

```cpp
// PlaybackEngine.cpp:1488
// losing one frame of display is free
std::unique_lock<std::mutex> guard (stateMutex, std::try_to_lock);
```

**对持续音是免费的，对 5–20 ms 的鼓不是**：一次跳过的 block 可以装下整个命中。

**修法**：解码器数 `hitCount`（"自你上次问以来发生过"），播放器把命中保留 16 个 block 再补回快照。

**教训**：**采样和事件不能混用同一个管道。** 一个可以丢，一个不能。

## 11. pulse 的 gate 也会闪

**症状**：Square 2 有音符号、音量条满格，**但波形是淡的、键不亮**。

`pulseActive` 要求 `length.value > 0`，而**驱动每帧重写 `$4003` 维持一个音**，两次写之间 length 归零。

**修法**：装载 length counter 是这颗芯片唯一的 key-on → `++pulse.hits`。

**教训**：我上一轮判断"pulse 的音够长不会丢"，**推的是音符时长，实际该看 gate 时长**。
同一个驱动行为（每帧重写寄存器）把长音变成了一串极短的脉冲。

## 12. 截图和 harness 会给出"看起来是证据"的东西

| 现象 | 真相 |
|---|---|
| 键盘页空 | `tick` 只重绘，**不跑 render**，捕获分支从未进入 |
| 侧边栏只有 3 行 | `createComponentSnapshot` **画不全 `TreeView`**，真实窗口是 8 行 |
| `pump()` 后 `inst=0` | 解码器建了，但播放位置没动，`advanceTo` 喂不到写 |

**教训**：**截图只能证明"它画了什么"，不能证明"状态是什么"。** 要看状态就去打日志，
要看画面就开真实窗口。

## 三条可复用的判断

**① 部分成功比全失败信息量大。**
5 个样本 2 个测出周期，说明机制是好的、问题在"结果怎么用"；全 0 会把人送回去查 ROM。

**② 两个消费者读同一份数据时，先确认它们一致。**
`hz` 的两条路、DPCM 的两条路，两次都是"一个对一个错"。

**③ 症状落在"看起来合理"的区域时，去找那个不合理的配对。**
"有音符 + 波形淡" 矛盾 → `active` 假。"注释说除周期 + 代码没除" 矛盾 → 代码错。

## 注释写在哪（防止重犯）

| 位置 | 写了什么 |
|---|---|
| `chips/Pitch.h` | 为什么除以周期，以及指向 `2026-11-01-the-rate-that-was-not-a-frequency.md` |
| `chips/C352Decoder.cpp` `cycleFor` | `loopLength > 256` 不回退的取舍与后果 |
| `chips/ChipTypes.h` `hitCount` | 为什么不复用 `keyOns`（会改 MIDI） |
| `chips/ChipTypes.h` `hasPitch` | 为什么零周期不等于没有音高 |
| `chips/ChipTypes.h` `wave` / `sampleRate` | 为什么共享、为什么不是 `hz` |
| `chips/NesApuDecoder.cpp` `$4012` | 八位，掩到六位会把 0x80 变成 0 |
| `chips/NesApuDecoder.cpp` `$4013` | 十六加一，尾部是填充 |
| `backends/libvgm/ChipWriteRouter.h` | 三个回调不接就是静默丢弃 |
| `backends/libvgm/LibvgmTrackPlayer.h` `pendingHits` | 命中是事件、快照是采样 |
| `backends/libvgm/LibvgmTrackPlayer.cpp` `lookupDpcmSample` | 一条来源，不做第二次读取 |
| `ui/KeyboardRowPaint.cpp` | 键亮表示"在响"，与波形同一个判据 |
| `ui/ContentSlot.cpp` `afterSnapshot` | note-off delay 是唯一的保持来源 |
