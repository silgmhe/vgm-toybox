# 波表拖拽导出：代码质量评审

**日期**：2026-11-01
**分支**：`feature/wavetable-drag-export`
**范围**：`master..c9f580e`，21 个文件，+1195 −25
**依据**：`.codebuddy/rules/code-quality.md` 的 12 条

## 结论

| 级别 | 数量 | 一句话 |
|---|---|---|
| 必须修 | 0 | 没有会让用户看到错误结果的问题 |
| 应修 | 3 | 一个命名边界不闭合、一个失败路径会重复生成、一个理由写了三遍 |
| 可延后 | 2 | 设置页的四处改动、每次拖拽重建目录 |

**功能是对的，测试是够的**（`NsfFormatTest` 新增 15 条断言，覆盖三态写出、命名、清理边界）。**问题都在"以后会不会变坏"这一侧。**

## 修复记录

评审当天三条应修全部处理，**两条提交，行为只改在出错路径上**（`46/46`，`dragExportName` 的边界分支补了断言）。

| 编号 | 处理 | 提交 |
|---|---|---|
| R-1 命名兜底会覆盖 | 已修：循环边界处返回空串，调用方据此报失败 | `e58e25d` |
| R-2 失败后重复生成 | 已修：检查 `performExternalDragDropOfFiles` 的返回值，并用 `dragHandled` 让一次按键只产生一次拖拽 | `ef5453a` |
| R-3 理由写两遍 | 已修：`mouseDrag` 只留手势距离，理由归 `startWaveDrag` | `ef5453a` |
| L-1 设置页五处改动 | 保留：独立的重构，记在这里而非夹带 | — |
| L-2 每次拖拽 `createDirectory` | 保留：行为正确，只是多一次系统调用 | — |

**新增的断言**是 R-1 的配套：`exists` 全部为真的情形是唯一能走到循环边界的路径，没有它这一支等于没测过。

## 应修的三条

### R-1：`dragExportName` 的兜底会返回一个已存在的名字

```cpp
// WaveExport.cpp:452
for (int n = 2; n <= 999 && exists (candidate); ++n)
    candidate = base + "-" + std::to_string (n) + ".wav";
```

**循环的边界是 `n <= 999`，不是"找到一个不存在的"**。同一秒里拖第 999 次之后，`candidate` 停在 `-999.wav` 上，而它仍然存在——`startWaveDrag` 会拿这个名字去写，**覆盖掉上一个文件**。

概率极低，但这是**规则 5 说的"用补丁掩盖"**：循环的意图是"找一个空的"，它的写法是"试到 999 为止"。

**建议**：把兜底说清楚——要么在 999 之外返回空串让调用方报错，要么把上限去掉（每轮都产生一个不在磁盘上的名字，循环必然终止）。**现在这个写法靠"999 够用"这个假设工作**。

### R-2：拖拽失败后，同一个手势会继续生成文件

```cpp
// ContentSlot.cpp:710
startWaveDrag (hoveredWaveChannel);
```

`startWaveDrag` 里 `performExternalDragDropOfFiles(...)` **返回 bool，被忽略了**。

正常情况下它是阻塞的——拖拽期间跑 modal loop，`mouseDrag` 不会重入。**但当它立即返回 false**（平台不支持外部拖拽、文件被占用），`mouseDrag` 会在下一次鼠标移动时**再调一次** `startWaveDrag`，于是：

1. 生成第二个文件
2. 再失败一次
3. 用户拖了 20 个像素，磁盘上多了 20 个同样的 wav

**建议**：检查返回值，失败时用 `onDragFailed` 报一次并**记住这次手势已经处理过**（`mouseUp` 清）。

### R-3：同一个理由写了三遍

"必须先写文件再交给系统"这件事出现在：

| 位置 | 说法 |
|---|---|
| `ContentSlot.cpp:699` `mouseDrag` 的 doc | `Written to disk first and handed over second: ...` |
| `ContentSlot.cpp:1340` `startWaveDrag` 的 doc | `The write comes first and its result is checked: ...` |
| `docs/desgin/wavetable-drag-export.md` §7 ADR-001 | 完整的一段 |

**代码里两处相邻不远的函数各自解释一遍**，正是规则 2 说的"重复的解释会在两处逐渐不一致"。文档那份是对的（那是 ADR 的地方），**代码里留一处就够**。

**建议**：`mouseDrag` 只留"什么时候开始"，`startWaveDrag` 承担"为什么这个顺序"。

## 可延后的两条

### L-1：加一个设置页要动五处

`PageIndex` 枚举、侧边栏 `addSubItem`、`pages.addView`、成员指针、`new` 那一行。`SettingsOverlay.cpp:133` 的注释承认是四处（它没数成员指针）。

规则 4 说"加东西应该只改一处"。**这次没改它是对的**——把它收成一张表是独立的一笔，混进拖拽提交会让这个分支既改功能又改设置窗口结构（规则 9）。

**但它已经在同一个文件里被记了两次**（注释 + 本文档），下一次加页之前应该先做。

### L-2：每次拖拽都 `createDirectory`

```cpp
if (folder.empty () || ! juce::File (folder).createDirectory ().wasOk ())
```

目录存在时这个调用是 `mkdir` 失败 → 返回 `wasOk() == false`？**不是**：JUCE 的 `createDirectory` 对已存在的目录返回成功。所以**行为正确**，只是多做一次系统调用。

每次拖拽一次，**可以不管**。写在这里是因为它看起来像 bug，下一个读者会停下来想一下。

## 检查过、无问题

写这一节是因为评审不该只有问题单。

| 规则 | 检查结果 |
|---|---|
| **7 颜色** | `KeyboardRowPaint` 与 `MessageBoxOverlay` **零字面量**；`scrim()` 进了 `Palette`；`setColour` 在 `lookAndFeelChanged()` 里而非构造函数 |
| **11 所有权** | 拖拽读的是 `KeyboardPanel::snapshot`，和 `paint()` 同一份（`drawnSnapshot()` 那条路）；没有 moved-from |
| **6 可测试** | `hasSomethingToWrite` / `writeChannelShape` / `dragExportName` / `clearDragCache` 都是可传参调用的纯函数，各有断言 |
| **12 位宽与缓冲** | 本支没有掩码改动；`kWaveDragThreshold` 是手势阈值不是缓冲大小 |
| **9 不夹带** | 五个提交都不是 refactor；`exportWavetables` 的行为变更（先过滤再取名）写在 `feat(playback)` 里，并在说明中点名 |
| **8 提交** | 每个提交一件事；`+8/−0` 的语言文件说明这次没有行尾污染 |
| **失败处理** | `clearDragCache` 先收集后删除、只删 `.wav`、未来时间戳不当作过期；`dragExportFolder` 拿不到 temp 时返回空串而不是猜一个目录 |

**一处值得记下的好判断**：`clearDragCache` 用**文件系统的时钟**算年龄，不是墙上时钟。

```cpp
const auto now = fs::file_time_type::clock::now ();
```

用户改过系统日期时，墙上时钟会让整个缓存看起来都过期了。这不是我想到的写法，是写的时候查了 `last_write_time` 的返回类型——**规则 12 的"去实现里读"用在了这里**。

## 建议顺序

```
R-1（十分钟，改循环条件）
R-3（五分钟，删一处注释）
R-2（半小时，加返回值检查与手势标志）
```

**R-1 和 R-3 修完就可以合并**；R-2 也建议一起——它是三个里唯一会让用户看到"拖一下多出很多文件"的。

## 与上一支的对比

上一支（`pcm-sample-export`）的评审有 2 条必须修：**539 行行尾污染**和**零断言的新单元**。

这一支两条都没有：
- 语言文件 `+8/−0`
- `WaveExport` 的新函数 15 条断言

**上一支的两个教训，这一次都兑现了。**
