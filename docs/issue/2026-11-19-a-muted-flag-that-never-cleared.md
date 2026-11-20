# 一个只置不清的标志，让一条轨"有可视化但没声音"

::: 引用块：

`01 Good Morning (Opening).vgz` 的 NES APU `Square 1` 没有声音，但混音台和键盘页都正常显示它。
把它当"混音器静音错了"查了三轮，最后发现是一条**解码器内部的标志从来不清零**——
和混音器毫无关系，甚至和音频路径也无关。

:::

---

## 一、现象

| 观察 | 说明 |
|---|---|
| `Square 1` 听不到 | 用户报告 |
| `Square 1` 有可视化 | 波形、音符都在 |
| 这条分支之前正常 | 明确是本次改动引入 |

**"有可视化但没有声音"** 这个组合本身就是最有价值的线索：可视化来自**聚合 player**（`trackPlayers[0]` 的
`captureState`），声音来自**这条轨自己的 player**。两者不一致，说明**这条轨的 player 不存在**。

---

## 二、排查路径

### 第一轮：怀疑错了对象

用户最初怀疑"非线性混音开关影响了 vgz"。实测否定了：

```
01 Good Morning   non-linear ON  : corr(DPCM, Noise) = 0.5816
                  non-linear OFF : corr(DPCM, Noise) = 0.5914
```

开关对 vgz 没有影响。**用户自己纠正了这一点**，省掉一整轮弯路。

### 第二轮：确认是"被跳过"

本分支为省内存加了一条规则——**全曲没有事件的轨不建模拟器**：

```cpp
if (trackIndex > 0 && ! trackHasEvents[(size_t) trackIndex])
    return std::make_unique<playback::SilentTrackPlayer> ();
```

打印每轨的判定，问题立刻可见：

```
[ev]  3 Square 1   midi=1 hasEvents=0     ← 被判为静音
[ev]  4 Square 2   midi=1 hasEvents=1
```

**`Square 1` 明明在响，事件流里却一条事件都没有。**

### 第三轮：改判据——对了，但代价太大

先把判据从"每轨事件数"改成"每设备事件数"，bug 修好了：

```
01 Good Morning            25.5 MB   ✓
07 Challenging Stage Clear 550.4 MB  （原本 24.7 MB）
```

`07` 的三个设备都在发声，所以 88 条轨全部建了回来。**内存优化几乎被抵消**。
这说明改判据只是绕开症状，**没有回答"为什么没有事件"**。

### 第四轮：问对了问题

不再改判据，而是问：**这条轨为什么产生不了事件？**

在 `pulseActive` 里打印四个条件：

```
[pulse1] muted=1 length=254 vol=4 period=285
```

**长度、音量、周期全部正常，只有 `muted` 为 1。**

---

## 三、根因

```cpp
void NesApuDecoder::clockSweep (Pulse& pulse)
{
    ...
    if (target > 0x7FF)
        pulse.muted = true;
    ...
    if (pulse.timerPeriod < 8)
        pulse.muted = true;
}
```

**整个文件只有这两处写 `muted`，都是 `= true`，没有任何 `= false`。**

而硬件对静音的判定是**每帧一次**：sweep 溢出只静音那一帧，下一帧周期回到范围内就恢复。

这个实现把"当帧的结论"当成了"永久的结论"。**曲子开头一次 sweep 溢出，`Square 1` 就在事件流里永久消失。**

### 完整因果链

```
sweep 溢出一次
  → pulse.muted 永久为 true
    → pulseActive() 从此恒为 false
      → 解码器不再为 Square 1 产生 note
        → trackHasEvents[3] = false
          → createPlayer(3) 返回 SilentTrackPlayer
            → 无声
```

**可视化不经过这条链**——它读 `GetKeyStatus()`，不看 `muted`。所以两边分叉。

---

## 四、修法

```cpp
void NesApuDecoder::clockSweep (Pulse& pulse)
{
    Sweep& sweep = pulse.sweep;

    // Cleared every frame, because the hardware decides every frame.
    pulse.muted = false;
    ...
}
```

一行。`Square 1` 立刻 `hasEvents=1`，判据也随之改回按轨：

```
07 Challenging Stage Clear   550 MB → 20.7 MB
01 Good Morning               27.3 MB
```

---

## 五、教训

### 1. 判据的可靠性，等于它依赖的那段实现

"这条轨有没有事件"读起来像个客观事实，实际是**别人算出来的结论**。
当那个"别人"有 bug 时，判据就是错的——而错误会伪装成"这条轨真的静音"。

### 2. 三条路径各自独立，不一致时找共同底层

| 路径 | 判据 | 结果 |
|---|---|---|
| 可视化 | `GetKeyStatus()` | 有声 |
| 事件流 | `pulseActive()` 四条件 | 无声 |
| 音频 | 芯片模拟 | 有声 |

三者不一致时，**不要去争论哪个对**，而是找**它们本来该共享的底层事实**是什么。
这次答案是"这个通道这一帧是否可听"，而 `muted` 把它变成了"曾经是否可听"。

### 3. 粘滞状态是最常见的 bug 形状之一

任何**每帧/每块重算**的结论，都不该用**累积**的变量存。
判据很简单：**这个量描述的是"现在"还是"曾经"？** 如果语义是"现在"，每个周期都要重置。

### 4. 一次事件影响全程，最难从现象反推

sweep 溢出发生在第几百毫秒，症状是"整首都没声音"。
**时间和因果不对齐**，所以从现象推理永远推不到——只能靠打印中间量。

### 5. "最可能"不能当结论

这一轮我犯过一次：说扫描要"几十秒"，实测 `fod_hopeless` 只 **2.67 秒**。
用户一句"扫描并不会卡住啊"就把它戳破了。

**推断出来的数字，要么测，要么标明是推断。**

### 6. 用户的观察比我的假设更可靠

`Square 1` 有可视化、`Square 2` 在同一设备上、`07` 只有三个通道有声——
**每一条都是用户先说、我后验证的**。这些外部事实是缩小范围的唯一依据。

---

## 六、检查清单

1. **"有可视化但没声音"** → 先查这条轨的 player 是否存在，不要查混音
2. 看到 `muted` / `active` / `enabled` 这类布尔，**先找它的复位点**
3. 判据依赖解码器时，**先在解码器里打印中间量**，再决定判据怎么改
4. 改判据能修 bug 但会损失收益时，**说明还没找到根因**
5. 每个"推断的数字"都标注来源

---

## 七、留下的东西

`vgm_nsf_probe --scan` 是这一轮的副产物：它同步跑一次 `scanSong(300)` 并计时。

```bash
vgm_nsf_probe test_file/nsf/fod_hopeless.nsf --scan
#   scanSong(300 s) took 2.67 s
#   dpcm samples 16  midi events 80471
```

写它是因为我给出过一个没测过的"几十秒"。**这类工具存在的意义，是让下一次不必再猜。**

---

## 八、仍然开放的一项

`07 Challenging Stage Clear` 现在 20.7 MB。但它的问题是**88 条轨里有 85 条不发声**，
靠的是"事件为零就不建"这条规则。

**这条规则仍然只和解码器一样可靠。** 如果哪天又出现"该响却空着"的轨，
第一个该看的地方是解码器，不是混音器——这一点写进了 `LibvgmSongInstance` 的注释里。
