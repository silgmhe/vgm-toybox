---
description: 改完界面之后，用无头 harness 截图自查（vgm_app_test 的 shot 命令），不要只凭代码判断
globs: plugin/Source/ui/**, plugin/Source/PluginEditor.cpp, plugin/Source/PluginEditor.h
alwaysApply: false
---

# 界面改动必须截图自查

## 规则

**任何可见的界面改动，完成后都要跑一次无头截图，并真的看一眼图片。** 测试全绿不代表界面是对的。

这个项目里连续出现过的界面缺陷，没有一个被测试抓住，全是在截图里一眼看见的：

- meter 高度算成 0（16 px 的矩形上下各内缩 8 px），整条不可见；
- 键盘比行卡片高 4 px，从卡片里"长"出来；
- 白键与卡片同色（9% 墨色），看起来只有一排黑块；
- 琴床铺满整列而键到 B8 就停，右侧留一条空底；
- 主题切换后按钮仍是旧配色（构造时写死了颜色）。

## 工具

`plugin/Tests/AppHarnessTest.cpp` —— 构建目标 **`vgm_app_test`**，不建窗口，用脚本驱动真实的 `PluginEditor`，支持 `shot <path.png>`（`createComponentSnapshot` 直接写 PNG）。

```powershell
# 构建（EXCLUDE_FROM_ALL，平时不会自动构建）
cmake --build build --target vgm_app_test

# 运行一个脚本
build\plugin\vgm_app_test.exe plugin\Tests\scripts\keyboard-shot.txt
```

输出默认写到 `build\shots\`（`build*/` 已在 `.gitignore` 里）。

## 写一个截图脚本

参照 `plugin/Tests/scripts/keyboard-shot.txt`：

```
size 1500 900                     # 先定窗口尺寸，布局按它算
load test_file/nsf/xxx.nsf
tick                              # 让异步加载落地
menu invoke view.keyboard         # 切到要看的视图（view.mixer / view.keyboard / view.scope）
tick
sleep 400                         # 等 30 Hz 面板请求快照（见下面的坑）
engine play
host 0.1 playing                  # 每次驱动一个处理块；重复若干次推进播放
sleep 300
shot build/shots/keyboard.png
```

可用命令见 `printHelp()`（`load / press / click / toggle / set / menu invoke / tick / shot / size / host / engine / select / dump / assert / sleep`），现成脚本在 `plugin/Tests/scripts/`。

## 三个坑（都踩过）

1. **没有 peer**：harness 不建窗口，`Component::isShowing()` **恒为 false**。任何用 `isShowing()` 当"是否在屏幕上"守卫的面板，在截图里永远是空的。用 `isVisible()` 代替。
2. **音频不会自己跑**：芯片面板的数据来自音频线程，必须 `engine play` + 若干次 `host <秒> playing`；只有截图没有块，就只会看到"该格式暂未提供逐通道状态"。
3. **顺序**：`sleep` 要在第一次 `host` **之前**，让面板先把 `stateCaptureWanted` 打开，否则所有块都白跑。

## 何时必须做

- 改了 `plugin/Source/ui/` 下任何 `paint()`、布局、颜色、间距；
- 改了主题调色板（`Theme.h/.cpp`）——深浅两套都要看；
- 新增/删除界面元素。

## 何时可以跳过

纯逻辑改动、不改变任何像素的改动（例如给已有函数改名）。

## 提交前：没截就说明没截

界面的改动如果**没有**跑截图自检，提交说明里必须写「未截图」以及原因（例如只改了逻辑而画法未变、或 harness 照不到这一处）。**沉默地跳过不算遵守这条规则**——和没写测试一样，区别只在于连洞都看不见。

这条不是形式主义：本项目连续三轮 UI 改动（发光缺失、发光截断、图例过时）都能在 `shot` 里一眼看见，但当时一次都没跑。

## 验证不了就交给用户，不要死磕

harness 有它照不到的地方：真的鼠标悬停、只有 standalone 才有的页面（例如设置里的音频页依赖 standalone::available()）、需要真实音频设备的界面、依赖窗口尺寸或 DPI 的细节。

遇到这类情况：

1. **最多试两次**。两次之内找到可复现的截图方式就继续；找不到就停手。
2. **明确告诉用户**：哪一处没能验证、为什么、请他看什么（一句话描述期望的现象）。
3. **不要**为了让截图好看而改产品代码（例如伪造 hover 坐标、给组件塞测试钩子）；那是把测试的成本转嫁给运行时。
4. 如果这个盲区以后会反复挡住工作，再单独提出来讨论要不要给 harness 加能力——而不是每次临时绕过。

反面例子：给键盘 M/S 加悬停反馈时，为了拍到那 4% 的变暗，连续试了六七种坐标推算，全部落空；正确的做法是第二次失败后就说明“悬停效果无法截图验证，请在真窗口确认”。
