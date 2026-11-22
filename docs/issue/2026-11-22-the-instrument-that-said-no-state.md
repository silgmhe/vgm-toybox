# 仪器说没有状态，芯片有状态

> 2026-11-22。YM2151 调研期间。
>
> 一句话：**harness 的键盘页一直画着加载时的空快照，而 `captureState` 每次调用都返回八行正确数据。** 我拿这台仪器量了一整轮，得出的结论是错的。

## 一、症状：一句提示语和一张截图

`05 Intimidating Spaces (Boss).vgz`，harness 截图为空白，显示：

```
This format does not report per-channel state yet.
```

同一份文件在**真实窗口**里是八行 `Ch 1`–`Ch 8`，音高 `G#3/E3/G#4/E3/B3/G#3/E1/A0`，每行一个单周期形状。

**两者相差整整八行。** 这不是"画得不一样"，是"一个有、一个没有"。

## 二、我的三次错误判断

### 第一次：怪曲子

`host 1 playing` … `host 180 playing`，位置停在 `00:00`。

**我以为 `N` 是块号。** 它不是——`commandHost` 的第一个参数是**位置（秒）**，我把 66 秒的曲子推到第 180 秒，越界后复位到 0。**于是我一直在看第 0 秒**，一个还没有音符的时刻。

### 第二次：怪 `shot` 的时机

改用 `host 0 playing` × 600，位置推进到 6.4 秒，`[cap]` 打印确认：

```
[cap] call=501 channels=8 chips=1 pos=5.344
```

**八行，位置正确。但界面还是空的。**

到这一步，观测内部已经出现**矛盾**：同一进程里，采集说八行，面板说零行。**矛盾的观测意味着有一个环节在说谎，而不是数据在说话。** 我没有停在这里，而是继续往下解释——这是这次真正的失误。

### 第三次：怪"没接线"

我 grep 到 `VgmReplayer` 只有四个写回调（`onOpnWrite`/`onNesWrite`/`onGbWrite`/`onSsgWrite`），没有 OPM 的，于是写下结论：

> 「OPM 不是'画得不好'，而是状态根本没被采集。」

**这个结论是错的**，而且它有证据支持——只是证据不完整：`note=55/52/64/...` 全部正确，说明寄存器解析**在跑**。**如果状态没被采集，音高不可能对。**

**一条证据被当成全部证据，是这一轮最贵的错误。**

## 三、根因：两个时钟，只有一个属于 harness

面板的快照只有一个来源：

```1699:1757:plugin/Source/ui/ContentSlot.cpp
void ContentSlot::timerCallback (void)
{
    ...
    engine.setStateCaptureWanted (true);
    const bool captured = engine.captureChipState (snapshot);
    if (! captured && snapshot.valid) { return; }
    keyboardPanel->setSnapshot (std::move (snapshot));
}
```

**`Timer` 只在消息循环里跑。harness 没有消息循环**，所以这段代码在它那里**从未执行过一次**。

而 `tick` 走的是另一条路：

```664:670:plugin/Tests/AppHarnessTest.cpp
void commandTick (void)
{
    // one frame of the window's own refresh, without a message loop
    editor.refreshUi();
}
```

```
tick → refreshUi (PluginEditor.cpp:1045)
     → ViewStack::refresh (ViewStack.h:100)
     → ContentSlot::refresh (ContentSlot.cpp:1628)
     → mixer->refresh()          ← 只有这一句
```

**`ContentSlot::refresh()` 只刷 Mixer，不采快照。**

于是：`shot` 画的是**加载时留下的那份空快照**，一次都没被替换过。**面板是诚实的，它只是从来没被问过。**

## 四、修复：把那一帧提取出来，两个入口都调

```1628:1639:plugin/Source/ui/ContentSlot.cpp
void ContentSlot::refresh (void)
{
    if (mixer != nullptr)
    {
        mixer->refresh ();
    }

    // **键盘这一帧也从这里来**，不只是从定时器：headless harness 没有消息循环去跑 `Timer`，
    // 而一个看不见通道的 harness 不能用来检查通道。幂等——同一帧第二次采集替换同一份快照。
    refreshChipState ();
}
```

`timerCallback` 里采快照那段整体搬进 `refreshChipState()`，`timerCallback` 转发。刷新率的设置（`startTimerHz`）留下，它只对定时器有意义。

**两个时钟可能同一帧都触发**，第二次采集替换同一份快照：一次锁、一次遍历八到十六个通道——比再引入一个"谁拥有这一帧"的机制便宜。

**验证**：同一首曲子，加载后三步 `host`：

| | 结果 |
|---|---|
| 修前 | `This format does not report per-channel state yet.` |
| 修后 | `Ch 1`–`Ch 8`，音高与窗口一致，各自单周期 |

## 五、这不是第一次

**用错误的基准测出来的"通过"，比没有测试更贵**——它会让你停止怀疑。

这个项目里已经有过至少四次同一形状：

| 事件 | 基准错在哪 | 后果 |
|---|---|---|
| `vgm_chip_test` 编译不了 | 多余的 `OpnVoiceDriver.cpp` | `ctest` 一直跑旧二进制 |
| `vgm_keyboard_test` 编译不了 | **同一个文件** | 同上，且有一条断言过期了没人知道 |
| 这次的 harness | 快照只从 `Timer` 来 | 一整轮调研结论错误 |
| 断言写的是观测值（`length == 17`、`buttonSize == 30`） | 把现状钉成契约 | 错误被锁进测试，一直是绿的 |

**四种的错误都在"测量工具"这一侧，而不在被测代码。** 而四种的共同点是：**工具绿了，所以代码被推定为对的。**

## 六、判据：三条，按顺序问

### 6.1 工具说"没有"，而代码说"有"——先验工具

**只要同一个进程里出现"内部数据有、界面没有"，第一步不是解释它，而是证明哪一侧在说谎。**

具体做法：在**数据产生点**和**数据消费点**各打一行。这次的 `[cap] channels=8` 和面板的空白，两行日志放在一起，问题就已经定位到"中间那一段"——而不是"上游没数据"。

**我当时做了前半步（`[cap]`），却没做后半步（面板拿到的是什么）。**

### 6.2 与真实应用的差异不能超过一次测量

**harness 存在的唯一理由是"和窗口一致"。** 一旦两者在某首曲子上不一致，**在解决这个不一致之前，harness 的所有输出都不可采信**。

这次我拿它量了 OPM 的现状、量了"没有行"、写下了错误的结论——**在第一次看到提示语的时候就该停下**，而不是把它当成"OPM 的特性"记进文档。

### 6.3 拿不准就让真实应用验一次

**用户提出的这条要写进流程**：

> 当 harness 的输出与预期相反、或与窗口不符时，**先请用户在真实应用里跑一次**，再继续分析。

理由很简单：**真机是基准，harness 是被测对象。** 用被测对象去证明自己的结论，就是循环论证——这次我正是这么做的。

## 七、给 harness 自己的检查

**harness 缺一条自检**：它现在能截出"空面板"而不报警。可以加的是——

1. **加载后必须至少有一次非空快照**：对任何有通道规划的格式，`snapshot.channels.size() == ChipPlans 的通道数`，否则 `shot` 报失败而不是成功；
2. **`tick` 的语义要写在帮助里**：它跑的是"窗口的一帧刷新"，**不含定时器**；
3. **`host` 的参数要写成 `host <秒> [playing|stopped]`**，当前的 `report (false, "host needs a position in seconds")` 在出错时才说，正常路径没人读。

第 1 条最有价值：**它把"仪器坏了"变成一个失败的断言，而不是一张看起来正常的截图。**

## 八、留下的

**这次的代价是一个完整的调研轮次**：`OpmDecoder` 是 153 行读了一遍、核心源码翻了两处、寄存器分布打了三轮、结论写错两次。**而真正的原因在一行 `mixer->refresh()` 旁边。**

**一句话留给下一次**：

> **工具沉默的时候，先怀疑工具；工具说话的时候，先怀疑自己听错了。**

---

**相关**：`docs/issue/2026-11-21-why-half-a-chip-went-silent.md`（第 6.6 节的"四件套"是同一条精神：`peak=0` 有两种解释，必须先看 `gate`）。**那一次的教训是"数据要带上下文"，这一次是"上下文要先确认来源可信"。**
