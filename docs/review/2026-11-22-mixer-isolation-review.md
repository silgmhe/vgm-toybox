# 混音器隔离与命名：合入前审查

> 2026-11-22。`fix/nsf-track-isolation`，5 个代码提交（另 3 个文档），7 个文件、约 384 行。
> 依据 `.codebuddy/rules/code-quality.md`。
>
> **结论：可以合入。** 五个提交都修的是"用户能听见/看见"的真缺陷，每个都附了修复前后的测量值，
> 并且每一个都被手工或测试验证过。下面仍列出六处值得跟进的点，**没有一处阻塞合入**。

## 一、改了什么

| 提交 | 缺陷（谁发现的） | 修复 |
|---|---|---|
| `71b20f6` | NSF 一开混音器整首静音 | `applyTrackIsolationTo` 只在 `players.size() == layout.size()` 时逐个设隔离 |
| `c6ef728` | **N163 芯片卡静音无效**（用户） | `RenderChannels` 把调用方的 `mask` 并进每通道 |
| `1c3cd43` | 上一条方向反了（用户当天听到） | `saved \| ~(1 << (7-i))` |
| `39e9a66` | **长 NSF 拖进度条冻结数秒**（用户） | seek 一律投递给 worker |
| `4d6b18a` | 采样芯片声部被叫作 `FM 1` | `midi::carriesFm` + 第二半区独立编号 |

**两条最值得记住的证据**：N163 的 `all muted 1.9318 -> 1.9318` 变成 `-> 0.0000`；
隔离修复后 `solo rms 0.000000` 变成 `0.066301 vs 0.066302`（差 0.0015%）。

## 二、MEDIUM-1：seek 变成"乐观位置"

`requestPositionSeconds` 现在无条件：

```cpp
positionSeconds.store (target);		// 消息线程立刻写
seekRequested.store (true);			// 播放器稍后才动
```

**传输条会显示音频还没到达的位置。** 对一首 60 秒的 NSF，这个差可能持续一秒以上——而那正是
用"不冻结窗口"换来的东西。

**没有引入正确性问题**（`render` 会在拿到锁之前渲染静音，和之前一样），但**界面现在会说一个小谎**。
建议后续加一个 `seeking` 状态让传输条显示出来，而不是让用户看到位置在跳、声音还没到。

## 三、MEDIUM-2：所有后端都变成异步

这条修复的触发条件是"`playerCount == 1` 也可能是贵的"，但改成了**全部异步**，包括 VGM 那种
即时 seek。代价是一个音频块（约 10 ms）的延迟和上面那条乐观位置——换来的是一处统一的分支。

**这是对的取舍**（两套路径正是这次 bug 的来源），但值得记：**如果将来某个调用方需要"seek 完成后
再继续"，它必须等 `positionSeconds` 追平，而不是假设调用返回即完成。**

## 四、LOW

1. **`carriesFm` 的 `default:` 会吞掉新芯片。** 一个新增的 FM 类型若忘记加进 `switch`，会安静地
   被判为"非 FM"，症状是它的声部被叫成 `Ch n`——**正是这个提交修的那类错**。建议在该 `switch`
   上方留一行"新增 FM 类型必须加在这里"，或在 `ChipPlansTest` 里加一条覆盖检查。
2. **`getPlayLengthSeconds()` 在锁外读 `song`。** 这是既有写法（`requestPositionSeconds` 本来也
   没为它加锁），但既然这个函数现在完全不取锁了，值得一并标注。
3. **`expectNote` 的失败信息说不清缺什么。** 找不到轨道和"轨道没有音符"报的是同一句
   `... note 0 exists`。这一次它让"名字变了"看起来像"MIDI 坏了"，而这个误诊持续了很久。
   **建议拆成两句**：`no track named X` / `X has only N notes`。
4. **`opl4.vgm` 留了一个空 `else if` 分支。** 注释解释了原因，但空块容易被读成漏写；也可以把
   注释挪到链上方并删掉该分支。
5. **`NES_N106::RenderChannels` 仍为 8 个通道调用 `Render()`**，即使 `maxChannels` 更小（既有）。
6. **`applyTrackIsolationTo` 用数量相等作为"是不是 in-place"的代理。** 它可靠（`playersWanted`
   就是按这个算的），但不如把 `IsolationMode` 传进来直白；保持现状可以，注释已写明理由。

## 五、规范对照

| 条款 | 结果 |
|---|---|
| 2 注释写为什么 | ✅ 每处修复都有"修复前测到多少"，包括 `\| vs \| ~` 那对易混写法 |
| 5 不留技术债 | ⚠️ MEDIUM-1/2 是明示的取舍，不是遗漏；LOW-1 建议补一条覆盖检查 |
| 9 修复不夹带 | ✅ 五个提交各自只改一处，命名与掩码分开 |
| 12 位宽/掩码去实现里读 | ✅ 两次都去 `nes_n106.cpp` 读了置位含义；第一次读对了方向也读错过一次 |
| 13 一份数据一个来源 | ✅ `carriesFm` 与 `channelPlanFor` 同表同词汇 |

## 六、验证

```
ctest            54/54（全部二进制由当前源码重建）
vgm_opn_test     音高三档、滤波器、OPM 电平与出声
vgm_core_test    全部通过（本分支修好了它编译不了的问题）
vgm_chip_test    25 pairs / stride is eight
vgm_nsf_test     全部通过
vgm_app_test     面板截图 all checks passed
```

**用户手验**：N163 静音生效、N163 各通道互不串音、长 NSF 拖进度条跟手、采样芯片行名为 `Ch n`。
