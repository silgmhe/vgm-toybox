# 波表拖拽导出

**日期**：2026-11-01
**分支**：`feature/wavetable-drag-export`
**状态**：设计已确认，待实现

## 1. 需求

键盘页每个通道的波表窗口，**直接拖进 Serum 一类的合成器**，省掉"点导出 → 选文件夹 → 再去导入"三步。

四条约束（用户确认）：

1. **DPCM 行也能拖**：拖出去的是当前采样，和导出波表时 DPCM 的处理一致
2. **空波表禁用拖拽**：没有形状可导的通道，光标不变、不出现提示
3. **设置里可配清理**：开关 + 小时数，默认**不清理**
4. **配置与导出按钮隔离**：拖拽固定单周期 + 芯片原生位深，不读导出窗口的设置

## 2. 现在有什么

这次需求几乎不需要新逻辑，因为需要的每一件都已经在代码里：

| 需要 | 已有 | 位置 |
|---|---|---|
| 波表矩形 | `row.wave` | `KeyboardRowLayout::Row` |
| 单通道写文件 | `writeCycleWav (channel, path, single, 2.0, bits)` | `WaveExport.cpp:214` |
| 采样写文件 | `writeSampleWav (channel, path)` | `WaveExport.cpp:288` |
| 判断是采样还是周期 | `isSampledChannel()` / `hasPeriodicShape()` | `WaveExport.h` |
| 数据源 | `drawnSnapshot()` | `ContentSlot.cpp:196` |
| 鼠标位置与 hover | `mouseMove` 已有逐矩形命中测试 | `ContentSlot.cpp:551` |
| Tooltip | `TooltipWindow { this, 650 }` | `PluginEditor.h:172` |
| 拖文件到外部 | `DragAndDropContainer::performExternalDragDropOfFiles` | JUCE 静态函数 |
| 强调色 | `theme::accent()` | `Theme.h` |

**缺的只有三样**：触发热区、临时文件、清理。

## 3. 拖拽的流程

```
mouseMove   落在 row.wave 上 → 记 hoveredWaveRow，repaint 那一行
            → 边框换成 theme::accent()，外加一圈轻微光晕
            → 光标 DraggingHandCursor
            停留 650 ms → TooltipWindow 调 getTooltip() → "拖到合成器里"

mouseDrag   超过 JUCE 的 5 px 阈值，且起点在 row.wave 内
            → 生成临时文件（同步，见 ADR-001）
            → performExternalDragDropOfFiles (files, false, this)
            → 拖拽结束，清 hoveredWaveRow
```

**顺序不能倒**：`performExternalDragDropOfFiles` 是同步调用，文件必须在调用前写好。

### 写什么

一个函数，两个分支，与 `exportWavetables` 的循环同源：

```cpp
/** 写一个通道可以拖出去的产物。

    采样写原始音频，周期写单周期波表；两者都不是就不写（返回 false，调用方不开始拖拽）。
    `bits` 只对周期有意义：采样的深度由它自己的编码决定，量化它是另一件事。
*/
bool writeDraggableShape (const ChannelState& channel, const std::string& path, int bits);
```

实现就是 `exportWavetables` 里那三行判断搬过来：

```cpp
if (isSampledChannel (channel))            return writeSampleWav (channel, path);
if (! hasPeriodicShape (channel))          return false;
return writeCycleWav (channel, path, ExportMode::single, 2.0, bits);
```

**`exportWavetables` 改成调它**，于是"什么通道能被导出"这条规则只有一个出处。现在它只在一处还好，复制到第二处之后，改一处忘另一处就是下一个 bug。

**`bits` 固定 `kNativeBits`**：`kNativeBits` 的语义就是"这颗芯片产出多少位就是多少位"，和用户要的第 4 条完全一致，不需要额外选项。

## 4. 临时文件

### 放哪

```
%TEMP%/VGMToybox/drag-export/
  └─ 2A03_Pulse-1_2026_1101_082356.wav
```

**专用子目录**，理由有三条：

1. 清理只扫这里，**不碰别的程序的 temp 文件**
2. 判断"这是我们的文件"不需要解析文件名——目录本身就是标记
3. 用户想手动清，路径是可见的

### 叫什么

`<chip>_<channel>_<timestamp>.wav`，时间戳 `2026_1101_082356`（精确到秒）。

**带芯片名**：和 `exportWavetables` 的 `<chip>_<channel>` 同构，拖进 Serum 后还能认出是哪颗芯片的。`2A03_Pulse 1` 和 `N163_Wave 8` 不会撞名。

**重名**：同一秒内拖同一个通道两次，第二份加 `-2`、`-3`。用 `juce::File::getNonexistentSibling()` 或等价的手写循环，不覆盖已有文件。

### 谁清理

**设置 → 导出**页：

| 控件 | 类型 | 默认 |
|---|---|---|
| 自动清理拖拽导出缓存 | toggle | **关** |
| 保留时长 | 数字输入，单位小时 | 3 |

**开启后**：程序启动时开一个后台线程，删掉 `drag-export/` 下**修改时间早于 `now - hours`** 的文件，然后线程结束。**一次性任务，不是常驻定时器。**

**默认关**，因为默认删用户的文件是坏默认——一个拖出去还没导入的文件被清掉，比磁盘多占几 MB 严重得多。

**多实例不冲突**：按时间判断，3 小时内不会误删另一个 Toybox 刚拖的文件。真要撞上，前提是那个文件拖出来放了 3 小时还没导入。

**目录不存在时**：不创建、不报错。第一次拖拽时创建。

## 5. 设置页

新增 `ExportSettingsPage`，侧边栏在"键盘"之后加一行"导出"。

**职责**：导出相关的设置。这次只有清理那一组，将来导出的其他偏好（比如默认位深）也该放这里，而不是继续堆进"通用"。

**代价，必须写下来**：加一页要动四处——`PageIndex` 枚举、侧边栏一行、`addSubItem`、`pages.addView`。`SettingsOverlay.cpp:133` 的注释已经承认这一点（"adding a page is one row here, one addSubItem, one pages.addView and one PageIndex entry"）。

**这次不去重构它**：把四步收成一张表是另一件事，夹在"加一个拖拽"里做，会让这个提交既改功能又改设置窗口的结构。规则 9 反对这个。

## 6. `MessageBoxOverlay`

用户要的是一个**通用**的提示窗口，不是又一个专用浮层。名字用 `MessageBoxOverlay` 而不是 `MessageBoxOverlayer`——项目里所有浮层都以 `Overlay` 结尾（`ExportReportOverlay`、`SettingsOverlay`、`AboutOverlay`、`ExportOptionsOverlay`）。

**接口**：

```cpp
class MessageBoxOverlay : public juce::Component
{
public:
    struct Message
    {
        juce::String title;
        juce::String body;
        juce::String dismiss;      // 按钮文字，空则用 "OK"
    };

    void setMessage (Message message);
    std::function<void (void)> onDismiss;
};
```

**只做一件事**：显示一段文字和一个关闭按钮。**不做**多按钮、不做返回值、不做好坏图标——`ExportReportOverlay` 已经证明了"浮层做得越像对话框，就越会被塞进它不该有的逻辑"。

**关闭方式**：按钮、Esc、点遮罩。和 `AboutOverlay` 一致。

**第一次使用**：拖拽生成临时文件失败时，显示"无法为这个波表生成临时文件"。

## 7. ADR

### ADR-001：同步生成，不用异步

**最初的想法是异步生成临时文件**，避免拖拽卡顿。**这个想法不成立**：

`performExternalDragDropOfFiles` 把路径交给系统的那一刻，文件必须已经在磁盘上。异步写的话，Serum 可能拿到一个还不存在的路径，或者读到只写了一半的文件。

**而且异步没有必要**：`writeCycleWav` 写的是 2048 点 + 44 字节头，**不到 1 毫秒**。加了线程反而引入"拖拽开始但文件没写完"的竞态。

**代价**：一次拖拽会让 UI 线程阻塞最多约 1 ms。**看不出来**。

### ADR-002：在 `mouseDrag` 里生成，不在 `mouseDown`

在 `mouseDown` 生成，用户点一下不拖就会留下一个文件；在 `mouseDrag` 超过阈值时生成，**点了不拖什么也不留**。

JUCE 的阈值是 5 px，而波表窗口是 88×26 —— 手一抖就会越过，所以这不会让拖拽变难。

### ADR-003：拖拽固定配置，不读导出窗口

用户要的第 4 条。**这也是唯一正确的做法**：导出窗口的设置存在 `SettingsStore` 里、供"批量导出"使用，拖拽是一个手势而不是一次批量操作，它没有机会让用户先确认选项。

**代价**：想用别的位深拖出去，得先导出再拖文件。**可接受**——拖拽的价值就是快。

### ADR-004：清理默认关闭

**默认删文件是坏默认。** 一个已经拖出去、但还没导入合成器的文件，可能就是用户下一步要用的东西。

**代价**：不开启的话目录会一直长。**用户自己选的**，而且路径可见、可以手动清。

## 8. 验证

| 断言 | 位置 |
|---|---|
| `writeDraggableShape` 对周期通道写出单周期文件（带 `clm ` 标记） | 单元测试 |
| 对采样通道写出原始速率音频（无标记） | 单元测试 |
| 对两者都不是的通道返回 false 且不建文件 | 单元测试 |
| 文件名含芯片名、通道名、秒级时间戳 | 单元测试（纯函数，可传时间） |
| 同一秒内重复拖拽产生 `-2` 而不是覆盖 | 单元测试 |
| 清理按修改时间删文件、且只删自己目录 | 单元测试（临时目录 + 两个时间戳） |
| 悬停波表出现提示、拖拽生成文件 | `vgm_app_test` 脚本 + 截图 |

**界面部分按 `7.5` 的规矩用眼睛看**：hover 的边框和光晕要出图确认，不做像素断言。

## 9. 已知限制

1. **拖拽时没有自定义缩略图**。JUCE 拖文件到外部用系统光标，无法替换成波形预览；Serum 自己会显示导入的波表。
2. **一次一个通道**。不支持框选多行一次拖多个文件。
3. **拖拽失败只有一句提示**，不区分原因（磁盘满、路径不可写、通道其实没有形状）。
4. **设置页要改四处**（见第 5 节），这是既有结构，不是这次引入的。
5. **清理只在启动时跑一次**。程序开着几天不关，缓存不会在这期间被清。
