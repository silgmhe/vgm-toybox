# 可测性架构与实施计划

> **状态**：**方案（未执行）** —— 本文只定义"做成什么样、按什么顺序、如何验收"，**不含任何代码改动**。
> **基准 commit**：`3a1108e974bd64446b84c93ff7c2f9d9b2ca8623`（短号 `3a1108e`，2026-10-16）
> **来源**：`docs/review/2026-10-16-architecture-and-code-review.md`（债务登记册 §6）
> **目的**：让"这个改动对不对"可以**用文字验证**，而不是靠截图、鼠标与前台窗口；并据此按顺序偿还 review 里的债。

---

## 0. 目标与验收标准

**目标 1（可验证）**：新增或修改的 UI 行为，能用**一段文本命令**复现并断言，不需要截图、不需要鼠标、不需要前台窗口。

**目标 2（可回归）**：四类回归门中的前三类由 CI 自动执行：
| 门 | 现状 | 目标 |
|---|---|---|
| 样例精确（`vgm_mixer_test` 的 −1000.00 dB） | 有 ✓ 但 CI 不跑 ✗ | CI 跑 ✓ |
| 结构与行为断言（新） | 无 ✗ | `vgm_app_test` 在 CI 跑 ✓ |
| 像素不变量（`vgm_ui_test`） | 有 ✓ 但 CI 不跑 ✗ | CI 跑 ✓ |
| 端到端（真窗口/宿主） | 人工冒烟 ✓ | **保持人工**（见 §8） |

**目标 3（可维护）**：review 里的 P0/P1 债务清零，且**结构上不再产生同类债**。

**反目标（明确不做）**：不做 OS 级 UI 自动化（WinAppDriver 等）、不做 DAW 宿主内自动化、不追求覆盖率数字、不引入重量级测试框架。

**备选方案（保留，不作废）**：截图、鼠标点击、图像识别**继续作为备选测试手段** ✓ —— 本文不是"禁止用它们" ✗，而是把它们从**默认验证方式**降级为**兜底**：文本手段覆盖不到时照旧使用 ✓（审美判断、拖放、文件关联/打开方式、多显示器 DPI、宿主内行为）。两者都会留下可复核的产物（脚本文本 / 截图与窗口枚举记录 ✓），所以"换手段"不会丢掉结论 ✓。

**做完怎么算成功（一句话）**：我能只用 `vgm_app_test` 的脚本文本 + 三个既有测试的输出，回答"加载这首歌之后界面进入了什么状态、哪一项打勾、哪个卡片可见、按下空格后发生了什么"，并且这些断言在 CI 上每次提交都跑。

---

## 1. 问题陈述（为什么现在做）

**证据一：三个真实缺陷都逃过了所有自动检查**
| 缺陷 | 提交 | 现有测试能否发现 |
|---|---|---|
| 带空格的路径打不开歌 | `67af914` | ✗ 无（逻辑在 `PluginProcessor::getStartupFile`，无测试） |
| View 菜单勾选不跟随 | `99aa57a` | ✗ 无（逻辑在 `MainMenu`，无测试） |
| About 卡死（模态阻塞） | `8df3832` | ✗ 无（只能靠**窗口枚举**才看出来） |

**证据二：现有验证手段的代价**
每一次验证都是：启动 → 截图（还撞过 DPI 偏移 ✗）→ 读图 → 有时还要模拟鼠标；且**无法进 CI** ✓。为此我们临时加过若干诊断开关（`--about`、`--visualiser`、`--tick`），每次都要改代码 → 构建 → 截图 → 再删掉 ✗。

**证据三：我们已经有正确的先例，只是没覆盖到 UI**
三个 `vgm_*_test` 都是"文本进、文本出"的控制台程序 ✓；`vgm_ui_test` 证明了**离屏渲染 + 数值断言**可行 ✓（甚至记录了"采样必须在 `Graphics` 析构之后"这种细节 ✓）。缺的不是能力，而是**把 UI 层接进同一个模式**。

**结论**：不是"GUI 不可测"，而是**"点击之后应该发生什么"这份知识没有一处可读的表述**。把它变成可读状态，是后续所有优化的前提。

---

## 2. 架构：五层可测性

```
                        ┌──────────────────────────────────────────┐
   脚本（文本）  ──────▶ │  AppController（意图层，无 Component 依赖）│
                        └───────────────┬──────────────────────────┘
                                        │ 调用
                        ┌───────────────▼──────────────────────────┐
                        │  PlaybackEngine / BackendRegistry（模型） │
                        └───────┬──────────────────────┬───────────┘
                                │ 状态                  │ 事件（消息线程）
                  ┌─────────────▼──────────┐   ┌───────▼─────────────────┐
                  │ UiSnapshot（组件树文本）│   │ EventLog（结构化事件）  │
                  │ MenuModel（菜单即数据） │   └─────────────────────────┘
                  └─────────────┬──────────┘
                                │ 断言（文本）
                  ┌─────────────▼──────────────────────────────────────┐
                  │ vgm_app_test：读脚本 → 执行 → 打印 ok/FAIL → 退出码 │
                  └────────────────────────────────────────────────────┘
   像素层（保留）：vgm_ui_test —— 离屏渲染 + 不变量断言，只回答"看起来对不对"
```

| 层 | 输入 | 输出（可断言） | 需要窗口？ | 需要渲染？ |
|---|---|---|---|---|
| 1 纯逻辑 | 参数/数据 | 返回值 | 否 | 否 |
| 2 控制器（意图） | 命令 | 模型状态 | 否 | 否 |
| 3 组件树 | 命令 | 结构/几何/文本/勾选 | **否**（`ScopedJuceInitialiser_GUI` 即可） | 否 |
| 4 像素 | 命令 | 图像像素 | 否（离屏 ✓） | 是（离屏 ✓） |
| 5 端到端 | 真实操作 | 日志/人工判断 | **是** | 是 |

**设计原则**：**断言可以宽，路径必须真**。测试驱动的是**真实组件与真实控制器**，不是平行实现；一旦出现"测试专用路径"，这层架构立刻失去意义（见 §7 风险 4）。

---

## 3. 组件设计（API 草案，仅定义形状）

### 3.1 控件身份（`setComponentID`）

给每个可交互/可断言的控件一个稳定 ID，命名按"区域.控件[.序号]"：

| 区域 | ID 示例 |
|---|---|
| 窗口外壳 | `editor.open`、`editor.status`、`editor.menubar`、`editor.viewstack` |
| 走带 | `transport.play`、`transport.pause`、`transport.stop`、`transport.cursor`、`transport.loop`、`transport.seek`、`transport.loopCount`、`transport.fade`、`transport.volume` |
| 歌曲信息 | `card.songInfo`、`card.songInfo.title`、`card.songInfo.time` |
| 芯片 | `card.chips`、`chip.<index>` |
| 混音 | `card.mixer`、`mixer.enable`、`mixer.spread`、`mixer.merge`、`strip.<index>.gain`、`strip.<index>.pan`、`strip.<index>.mute`、`strip.<index>.solo`、`strip.<index>.output`、`strip.master.*` |
| 页面 | `view.play`、`view.visualiser` |

> 依据：JUCE 已提供 `setComponentID` / `getComponentID` / `findChildWithID`（`juce_Component.h:105,712`）✓，无需自造查询机制。

### 3.2 `ui/UiSnapshot.h` —— 组件树 → 文本

```
juce::String dumpComponentTree (juce::Component& root, int maxDepth = -1);
juce::String dumpComponent (juce::Component& component);       // 单节点
juce::String dumpComponentById (juce::Component& root, const juce::String& id);
```
每行一个节点（JSON 对象，便于 grep 与人工阅读）：
```json
{"id":"transport.loop","type":"IconButton","bounds":[12,180,40,40],"visible":true,"enabled":true,"checked":true,"text":""}
```
字段固定为：`id / type / bounds / visible / enabled / checked / text`（不存在适用字段时为 `null`，不要省略字段 —— 稳定 schema 比省字节重要）。

**树来源**：优先走 JUCE 无障碍树（`Component::createAccessibilityHandler()`、`AccessibilityHandler::getChildren()` —— `juce_AccessibilityHandler.h:217` ✓），缺失时回退到 `getNumChildComponents()/getChildComponent()` 遍历（`juce_Component.h:680` ✓）。
**双收益**：同一份投资让**屏幕阅读器**可用 ✓ —— 这是本项目一直没有的能力。

### 3.3 `ui/MenuModel.h` —— 菜单即数据（单一真源）

```
struct MenuEntry { juce::String group, id, label, shortcut; bool checked = false, enabled = true; std::function<void()> action; };
class MenuModel { std::vector<MenuEntry> entries; /* 构建 PopupMenu 与查询都读它 */ };
```
- `MainMenu::getMenuForIndex()` 由模型生成 ✓；
- 测试直接读模型 ✓（`menu View` → `View=[Play(id=view.play, checked), Visualiser(id=view.visualiser)]` ✓）。

**为什么这是"结构性修复"**：勾选类 bug（`99aa57a`）的根因是**当前页的状态存了两份**；模型化之后"菜单内容"与"当前状态"只有一处真源，测试读到的就是菜单要画的 ✓。

### 3.4 `app/AppController.h` —— 意图层（Humble Object）

```
class AppController {                        // 不依赖任何 juce::Component
public:
    // 意图（UI 与测试都调这些；窗口只负责"画出来"）
    void loadSong (const juce::File&);  void closeSong (void);
    void togglePlayStop (void);  void togglePlayPause (void);  void seekTo (double seconds);  void seekBy (double delta);
    void setLoopCount (int);  void setFadeSeconds (double);  void setMasterVolume (double);
    void setTrackGain (int, double);  void setTrackPan (int, double);  void setTrackMuted (int, bool);  void setTrackSoloed (int, bool);
    void setTrackOutput (int, int bus);  void spreadOutputs (void);  void mergeOutputs (void);  void setMixerEnabled (bool);
    void setChipMuted (int, bool);  void selectView (int);
    // 可观察状态（只读；供 UiSnapshot/断言使用）
    struct State { bool loaded, playing, finished, mixerEnabled; double position, duration; int tracks, chips, currentView; juce::String status; };
    State getState (void) const;
};
```
- 现在这些动作分散在 `PluginEditor`（private ✗，只能由活窗口驱动 ✗）；
- 抽出后：**装配层可测**（正是 review 的 H-2 ✓），启动参数解析也可变成纯函数进 `vgm_core_test` ✓；
- **模板敲定前的约束**：控制器只编排（调用引擎/面板设定），**不做**任何绘制或布局判断 ✓。

### 3.5 `app/EventLog.h` —— 结构化事件（"发生过什么"）

```
toybox::EventLog::add ("load.start", {{"file", path}});
toybox::EventLog::dump();     // 文本
```
- 有界环形（例如最近 256 条 ✓），可选转储到 `%TEMP%` ✓；
- **只在消息线程记录**（不进音频线程 ✗，不分配、不加锁 ✓）；
- 现阶段 `%TEMP%\vgmtoybox-startup.log` 的"面包屑"思路 ✓ 升级为结构化 ✓（同一机制，字段化之后机器可读 ✓）。

### 3.6 测试入口 `vgm_app_test`（关键件）

控制台程序（`EXCLUDE_FROM_ALL` ✓，与既有三个测试同规格）：`ScopedJuceInitialiser_GUI` ✓ → 构造 `ToyboxAudioProcessor` + `ToyboxAudioProcessorEditor`（**不 addToDesktop** ✓）→ 读脚本 → 执行 → 打印 → **失败则非零退出码** ✓。

**脚本语法（草案，需在 Phase 0 定稿）**：
```
# 注释
load <path>              close
press space|enter|home|left|right|ctrl+o|ctrl+1..9
seek <seconds>
click <componentId>            # Button::triggerClick()（juce_Button.h:236 ✓）
toggle <componentId>           # 仅 toggle 控件
set <componentId> <value>      # slider / combo
menu File|View|Help
view <name>
state                          # AppController::getState() 一行文本
dump [componentId]             # 组件子树
assert <componentId> <field> <op> <value>   # field: visible/enabled/text/checked/bounds
events                         # EventLog 转储
```
**输出**：`ok …` / `FAIL …`（含期望与实测 ✓）；退出码 0/1 ✓。

**时钟推进**：用 `MessageManager::runDispatchLoopUntil()`（`juce_MessageManager.h:105` ✓）推进刷新与异步回调 —— **不要 sleep** ✗（Phase 0 确认它在控制台下的实际行为，见 §7 风险 2）。

**交互注入规则（纪律）**：只通过组件自身 API（`triggerClick()` ✓ / `setValue()` / `editor.keyPressed()` ✓）或控制器 ✓；**禁止**直连引擎内部实现来"造状态" ✗。

### 3.7 验收样例（写成断言的样子）

```
$ vgm_app_test --script load-and-switch.txt
ok   load "C:/music/18 Ms_X Ending -X-.vgz"      playing=true chips=5 tracks=12 status="Playing 0:00 / 1:23"
ok   press space                                  playing=false
ok   click transport.loop                         loop=true loopCount=2
ok   menu View                                    items=[Play(id=view.play, checked), Visualiser(id=view.visualiser)]
ok   view Visualiser                              currentView=1
ok   assert view.visualiser visible == true
ok   assert view.play       visible == false
ok   menu View                                    items=[Play(id=view.play), Visualiser(id=view.visualiser, checked)]
ok   press ctrl+1                                 currentView=0
Result: all checks passed
```
最后两行就是 `99aa57a` 那个 bug 的**永久回归断言** ✓。

---

## 4. 分期计划

| 期 | 交付物 | 验收标准 | 对应债务 | 成本 | 风险 |
|---|---|---|---|---|---|
| **P0 探针** | 无窗口构造 `editor` + 打印组件树（一次性 spike，代码可丢） | 能在控制台打印出真实的组件树（≥20 个节点） | — | **1 小时** | 高（决定后续可行性 ✓，见 §7） |
| **P1 骨架** | 控件 ID 命名 + `ui/UiSnapshot.h` + `vgm_app_test` 六命令 + `Tests/TestSupport.h` | §3.7 的会话前 5 行能跑通 | L-3 | 0.5–1 天 | 中 |
| **P2 装配层** | `MenuModel` + `AppController` + 启动参数纯函数化 + 相应断言 | "勾选跟随""参数挑选"进入自动测试 | **H-2**、L-1 的顺带整理 | 1 天 | 中 |
| **P3 门禁** | 自绘控件的无障碍 handler（双收益）+ CI 跑三个测试 + `vgm_app_test` | CI 上四次绿灯；PR 上能看见 UI 编译失败 | **H-3** | 1–2 天 | 中（CI 的环境折腾） |
| **P4 铺路** | `M-2` 刷新分层 + `M-3` 卡片头上提到 `CardComponent` | 新增一个可视化 View **不复制**卡片头代码 | M-2、M-3 | 1 天 | 低 |
| **P5 热路径** | `H-1` 加载事务化（准备-换入）+ `M-5` 锁契约显式化 | 加载大文件时不再出现"连续静音块"（用 §3.5 的事件时间戳可证） | **H-1**、M-5、M-1（部分） | 1–2 天 | 高（动引擎，需独立窗口） |

**顺序的理由**：先"能发现问题"（P0–P1 ✓）→ 再"让装配层可测并进 CI"（P2–P3 ✓）→ 再"改结构为功能铺路"（P4 ✓）→ 最后"动热路径"（P5 ✓，因为它最需要回归保护 ✓）。

**与功能的关系**：P4 完成后才开始"波形 / 频谱"面板 ✓（它们要复用卡片头与刷新分层 ✓，且需要 §3.2 的树断言来验证"面板出现了" ✓）。

---

## 5. 与 review 债务的映射

| 债务（review §6） | 解决于 | 说明 |
|---|---|---|
| H-1 加载全程持锁 | P5 | 方案：锁外构建会话 → 锁内只换指针 |
| H-2 装配层 0 覆盖 | **P2** | `AppController` + 纯函数参数解析 |
| H-3 CI 不编译插件、不跑测试 | **P3** | 新增 Windows job + 四个测试 |
| M-1 `PlaybackEngine` 四职责 | P4/P5 渐进 | 触碰即拆（`render` 三分、`RoutingPlan`） |
| M-2 面板每帧全量刷新 | **P4** | `syncStructure()` + `refreshValues()` |
| M-3 卡片头套路 ×3 | **P4** | 上提到 `CardComponent`，常量进 `Theme.h` |
| M-4 格式文案派生 5 处 | 随手 | 与 P2 一起做（控制器/注册表出文案） |
| M-5 可重入锁隐式契约 | P5 | `*Locked()` 私有方法 |
| M-6 两套前端并存 | 决策文档 | 不在本计划内（只需一句话结论） |
| M-7 libvgm 更新策略 | 文档 | 不在本计划内 |
| L-1 失效引用 / L-2 配置 / L-3 测试内重复 / L-4 风格判据 / L-5 债务标记 | P1（L-3）、随手（其余） | L-1 已单独处理（见 §8） |

---

## 6. 本计划**不**解决的问题（保持诚实）

- **审美判断**仍需人眼 ✓（"发光好不好看" ✓）；但"有没有发光、尺寸对不对"由 P4/`vgm_ui_test` 覆盖 ✓；
- **拖放、文件关联/打开方式、多显示器 DPI、宿主内插件加载**仍需真窗口/真 OS ✓（保留人工冒烟 + 结构化事件 ✓）；
- **宿主（DAW）兼容性**不在自动门禁内 ✓（成本远高于收益 ✓）；
- 上面这些场合**继续走截图 / 点击 / 识别那条路** ✓ —— 它们是**备选方案**，不是被取消的方案 ✗；本文只是不再让它们承担"日常回归"的职责 ✓。

---

## 7. 风险与替代方案

| # | 风险 | 触发信号 | 替代方案 |
|---|---|---|---|
| 1 | 无窗口构造 `editor` 失败（某些包装要求 peer） | P0 探针报错/断言 | 退到**面板级**（只构造 + 布局各面板 ✓），仍覆盖大部分行为；editor 级留给"隐藏窗口 + 消息循环" |
| 2 | `runDispatchLoopUntil()` 在控制台下的行为不符合预期（异步/定时器） | P0/P1 中刷新不推进 | 手动推进：把"每帧要做的事"抽到控制器的一个 `tick()` ✓（**这正是 P2 的顺带收益** ✓），测试直接调它 |
| 3 | 无障碍树对自绘控件覆盖不足 | `dump` 缺节点 | 双轨：以 ID + 子组件遍历为主 ✓，无障碍为辅 ✓（别绑死在无障碍上） |
| 4 | UI 测试逐渐变成"第二套实现"（与真实路径分叉） | 测试里出现"直接改引擎/绕过按钮" | 纪律：断言可以宽、**路径必须真** ✓；评审时把这条当硬性要求 ✓ |
| 5 | 断言过密导致每次调整布局都要改测试 | 改 padding 就红一片 | 断"关系"而非"像素"：可见性/顺序/勾选/文本 ✓，具体坐标只在关键处（例如"卡片不重叠"）✓ |

---

## 8. 现状与边界

- **不做的事**：把 OS 级 UI 自动化做成**门禁** ✗（**不是不用它** —— 它是备选方案，只是不当作每次提交都要跑的关卡 ✓，见 §0 与 §6）、宿主内自动化 ✗、覆盖率指标 ✗、gtest/Catch2 等重框架 ✗（沿用现有 `check()` 模式 + `Tests/TestSupport.h` ✓）。
- **已有资产**：三个控制台测试（`EXCLUDE_FROM_ALL` ✓ —— **CI 需要显式构建它们** ✓，这点要写进 P3 的 job ✓）；`vgm_ui_test` 的离屏渲染经验 ✓；`%TEMP%` 面包屑日志 ✓（P1 起升级为结构化事件 ✓）。
- **`L-1`（11 处失效引用）** 不属于本计划，已单独说明（见 review 文档 §8 的替换命令 ✓）。
- **本计划的产出物**：P0 的结论（可行/不可行 + 形式）+ P1..P5 各自的提交与断言；每一期结束都在本文追加"实施记录"一节（与 `ui-button-design.md` 的做法一致 ✓）。

---

## 9. 下一步（等你点头）

1. **批准本方案** → 我做 P0 探针（1 小时，结果只有两种：可行 → 继续 P1；不可行 → 按 §7 风险 1 退到面板级 ✓）；
2. 或者**先调整方案**（期次顺序、脚本语法、ID 命名、是否引入事件日志——都还没冻结 ✓）；
3. 或者**先做 review 的 P0-B（CI）**：它与 P3 高度重合，先落地能让后面每一期都在门禁保护下进行 ✓。

---

## 10. 实施记录

| 期 | 状态 | 说明 |
|---|---|---|
| **P0 探针** | ✅ **GO**（2026-10-16，`3c7c36a`） | `plugin/Tests/AppHarnessTest.cpp` + 目标 `vgm_app_test`：控制台里构造**真实**的 `ToyboxAudioProcessor` + `ToyboxAudioProcessorEditor`，`setSize (820, 650)` 完成布局，打印出 **53 个组件**的树（bounds / visible / enabled / text ✓）。两个关键事实：`ScopedJuceInitialiser_GUI` 让 GUI 对象无需桌面 ✓；`AudioProcessor::setTypeOfNextNewPlugin()`（`juce_AudioProcessor.h:1370`）是 JUCE **自己给包装器用的**钩子 ✓（`wrapperType` 是 const ✓，这不是测试后门 ✓，§7 风险 1 **未发生** ✓）。目标链接插件的 **STATIC 共享代码**（`juce_add_plugin` 创建的那个 ✓），所以驱动的是**同一个 editor** ✓。转储器 `ui/UiSnapshot.h` 报告组件**自己的 visible 标志**而非 `isShowing()`（无窗口时后者恒为 false，什么也说明不了 ✓）。探针同时报告"**有 ID 的组件：0 / 53**" → P1 的第一件事 ✓。 |
| **P1 组件 ID + 命令语言** | ⏳ 进行中 | `vgm_app_test` 目前只做 P0 的事（报表 + 退出码 ✓）；控件 ID 与命令语言（load / click / menu / view / assert ✓）是下一步 ✓。 |
| **P2 装配层（H-2）** | ✅ **完成**（2026-10-16，`d9587f8`） | "参数 → 歌曲"抽成纯函数 `playback/StartupSong.h` ✓（arguments 进、`File` 出 ✓、"能否播放"由调用方以谓词注入 ✓）；`vgm_core_test` 新增 `[5] startup arguments` **7 项断言**：曾失败的**路径含空格** ✓、前置选项 ✓、首个可用者胜出 ✓、存在但后端打不开 ✓、路径不存在 ✓、无参数 ✓。面包屑行为不变 ✓（无参数仍不写日志 ✓）。 |
| **P3 CI 门禁（H-3）** | ✅ **完成**（2026-10-16） | `build.ps1` 增加 `-Target a,b,c` / `-Test` / `-TestSong` ✓（**默认调用不变** ✓；`-Test` 运行已构建的测试，失败即让脚本失败 ✓）→ **CI 与本地跑同一条命令** ✓。新增 job `plugin-tests`（`windows-latest`、`shell: powershell` ✓，用 `actions/checkout@v4` ✓）：`.\build.ps1 -Target vgm_core_test,vgm_ui_test,vgm_app_test -Test` ✓ = **编译插件 + 三个测试并运行** ✓。`vgm_mixer_test` 因需要真实歌曲而**留在本地门禁** ✓（原因写在 job 注释里 ✓）。触发分支同时扩到 `feature/**` ✓ —— 否则这个 job 在合并前形同虚设 ✓。**本地已按同一条命令验证**：`3 test target(s) passed` ✓；带 `-TestSong` 时 `4 test target(s) passed`（含 −1000.00 dB ✓）。**CI 上的首次真实运行需推送后观察** ✓（GitHub Actions 无法在本地执行 ✗）。 |
| P4 铺路（M-2 / M-3） | ⏳ 待做 | 刷新分层 + 卡片头上提 ✓，然后才开可视化面板 ✓。 |
| P5 热路径（H-1 / M-5） | ⏳ 待做 | 需要独立的安静窗口 ✓。 |

**P0 的额外收获**：探针输出已经能直接回答过去需要截图才能回答的问题 ✓ —— "未加载时 seek 滑杆 `enabled:false`" ✓、"`ChipPanel` / `MixerPanel` 在无歌曲时 `visible:false`" ✓、"走带按钮的文本是 Play / Pause / Stop / Loop / Playback start" ✓、"菜单栏有三个 48/57/56 px 的项（File / View / Help）" ✓。这正是 §3.7 那类断言的雏形 ✓。
