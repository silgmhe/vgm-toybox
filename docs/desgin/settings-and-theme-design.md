# 设置窗口与主题（Settings & Theme）设计

状态：**已决策**（2026-10-17，见 §11 决策记录）；实施中：**M1–M4 已完成**（§13），**字体设置已增补**（2026-10-18，§13 M7），M5 系统实时跟随、M6 暗色像素组与版本待做　分支：`feature/theme`　版本位：**MINOR**（`0.0.2` → `0.1.0`，依据 `docs/desgin/maintenance-policy.md:85`：新 UI 页面 = MINOR）

凡本文档给出的结论都带 `文件:行`；未验证的写"待实现时确认"。

---

## 1. 需求冻结

| # | 需求 | 落点 |
|---|---|---|
| R1 | 新增 `Option` 菜单，内含 `Settings` 打开设置窗口 | `ui/MainMenu.h` |
| R2 | 窗口形态类似关于窗口 | 复制 `ui/AboutOverlay` 的**窗口内浮层**模式（无 peer、无 modal） |
| R3 | 以后所有设置都放这里 | `ui/SettingsOverlay`：左 `TreeView` 选类别，右参数区 |
| R4 | 持久化到用户 AppData（注意权限） | `settings/SettingsStore`，见 §4 |
| R5 | 路径必须用 JUCE 函数获取 | `File::getSpecialLocation (userApplicationDataDirectory)` ✓ |
| R6 | 不在根目录直接放文件，建 `VGMToybox` 子目录 | `…/VGMToybox/settings.xml` |
| R7 | 拒绝注册表 | §2.3：JUCE 8 属性文件写的就是文件 ✓ |
| R8 | 插件与 Standalone 通用同一份设置 | 文件名写死，**不用** `getApplicationName()`，见 ADR-2 |
| R9 | 首版两个类别：`Theme`、`General` | 两个页面组件 |
| R10 | Theme 页：选主题 + "跟随系统"开关 | §5、§8 |
| R11 | 跟随时：系统深色→Dark，浅色→Light | `Desktop::isDarkModeActive()`，§2.4 |
| R12 | `Help` **前**加 `Theme` 菜单，选项互斥 | 复用现有 `MenuEntry.checked` ✓ |
| R13 | 现主题定名 **Light**，新增 **Dark**（One Dark 风格） | `ui/Theme.h` 调色板化，§5 |

**非目标**：不引入宿主参数 / 工程状态 ✗；不做 UI 缩放 ✗；**字体**原先列在非目标里，已于 2026-10-18 增补（§13 M7），语言仍由 `i18n-design.md` 单独设计（`Language` 已作为独立页面加入）；不做跨进程实时同步 ✗；**指标（`Theme.h:36-49` 的 `margin`/`cardRadius` 等 `constexpr`）全部不动** ✓。

---

## 2. 需求合理性分析

### 2.1 成立（有证据）

| 判断 | 证据 |
|---|---|
| JUCE 能读系统深浅色 | `Desktop::isDarkModeActive()`　`juce_Desktop.h:209` |
| 还能**监听**系统切换（不只启动时读一次） | `DarkModeSettingListener` / `addDarkModeSettingListener()`　`juce_Desktop.h:68-76, 188-209` |
| 三平台都实现了检测 | §2.4（不是只有 macOS 能用） |
| AppData 有跨平台 API | §2.5 |
| 不写注册表也能做 | §2.3 |
| 插件与 Standalone 共享天然成立 | 同一用户同一路径；但应用名必须写死（ADR-2） |
| 互斥菜单有现成机制 | `MenuEntry.checked` → `item.isTicked`　`MainMenu.h:17, 247` |
| 切主题代价很小 | 16 文件约 170 处 `theme::`；**唯一**缓存颜色的地方是 `ui/ButtonPaint.h:31` → 兼容层保持函数名不变，170 处一行不改 |

### 2.2 必须处理（否则会以某种方式坏掉）

| 风险 | 处理 |
|---|---|
| **沙盒宿主写盘失败** | 保存失败不抛、不崩、不阻塞；原因记入 `SettingsStore::lastError()`，General 页显示"设置无法保存"，内存值继续生效 |
| **音频线程绝不碰文件** | `SettingsStore` 只在 message thread 读写；`setThemeMode()` 只改变量 + 发广播 |
| **快速连点导致写盘风暴** | 用 `PropertiesFile` 的延时保存（`millisecondsBeforeSaving`，默认 3000 ms） |
| **目录可能不存在**（尤其 Linux） | JUCE 自己提醒过要先建（`juce_File.h:909-911`）→ 显式 `createDirectory()` |
| **主题是进程级，LookAndFeel 是每实例** | `ui/ToyboxLookAndFeel.h:11-13` 规定每编辑器一个实例、绝不用全局默认 → 广播后**每个编辑器自己**刷新 LookAndFeel 与 `setColour` |
| **同宿主开多个实例** | 广播同步一致 |
| **Linux 跟随时可能读不到**（Wayland／无 gsettings） | 判定为**浅色**，不报错（§8） |
| **Windows 有前提** | 需 Win10 build ≥ 17763 且非高对比度（`juce_Windowing_windows.cpp:776-788`） |

### 2.3 证据：不需要注册表

属性文件类在 `juce_data_structures/app_properties/juce_PropertiesFile.{h,cpp}`；该模块全树搜索 `Registry|registry` → **0 命中**；Windows 上它写的就是 `userApplicationDataDirectory` 下的文件（`juce_PropertiesFile.h:160-161` 原文示例）。R7 满足 ✓。

### 2.4 证据：系统主题检测

| 平台 | 检测方式 | 文件:行 | 前提 | 实时通知 |
|---|---|---|---|---|
| Windows | `uxtheme.dll!shouldAppsUseDarkMode()` | `juce_Windowing_windows.cpp:769-789` | Win10 build ≥ 17763，非高对比度 | ✓ `WM_SETTINGCHANGE`+`ImmersiveColorSet`（:814-824） |
| macOS | `AppleInterfaceStyle == "Dark"` | `juce_Windowing_mac.mm:304-308` | 10.14+ | ✓ `AppleInterfaceThemeChangedNotification`（:311-324） |
| Linux | XSettings 主题名；否则 `gsettings get org.gnome.desktop.interface gtk-theme` | `juce_XWindowSystem_linux.cpp:2163-2192` | X11；名字含 `dark`/`black` | 仅 XSettings 变化；Wayland／无 gsettings → 读不到 |

### 2.5 证据：AppData 是什么

| 平台 | `userApplicationDataDirectory` 返回 | 证据 | 本设计使用 |
|---|---|---|---|
| Windows | `C:\Users\<用户>\AppData\Roaming` | `juce_Files_windows.cpp:721` | `%APPDATA%\VGMToybox\settings.xml` |
| macOS | `~/Library` | `juce_Files_mac.mm:209` | `~/Library/Application Support/VGMToybox/settings.xml` |
| Linux | `$XDG_CONFIG_HOME` 或 `~/.config` | `juce_Files_linux.cpp:135` | `~/.config/VGMToybox/settings.xml` |

macOS 的 `Application Support` 由我们补：JUCE 只给到 `~/Library`，Apple 的约定是设置进 `Application Support` —— JUCE 自己在 `PropertiesFile` 里也这么做（`juce_PropertiesFile.h:95-113` 要求 `osxLibrarySubFolder` 必须是 `"Application Support"`，否则运行时断言）。

**为什么路径自己算而不用 `Options::getDefaultFile()`**：它在 Linux 上落在 `~/[folderName]/`（`juce_PropertiesFile.h:163-164`），不是 XDG 的 `~/.config` ✗。但 `PropertiesFile` 有显式 File 的构造函数（:185-186）→ 路径自己算、读写交给它 ✓，白拿延时合并写入 / 成败返回值 / 析构自动 flush（:188-191）/ `isValidFile()` 损坏检测（:198）/ 可选的进程间锁（:152）✓。

---

## 3. 现状（改动落点）

| 位置 | 事实 |
|---|---|
| `ui/Theme.h:11-102` | `toybox::ui::theme`：16 个颜色 + 指标 + 阴影，全是 header-only `inline`（**编译期**，所以必须先变成运行时状态） |
| `theme::` 调用点 | **16 文件、约 170 处** |
| 颜色缓存 | 仅 `ui/ButtonPaint.h:31` |
| `ui/ToyboxLookAndFeel.h:17-53` | `LookAndFeel_V4` 子类，重写 8 个绘制函数（M2 由 `LightLookAndFeel` 改名而来） |
| `ui/MainMenu.h` | `MenuEntry{id,label,shortcut,checked,enabled,separatorBefore,action}`（:12-21）；`buildGroups()` 是**唯一**菜单定义（:68）；`getMenuBarNames()` 返回 `{File,View,Help}`（:225）；`item.isTicked = entry.checked`（:247）；`invoke(id)` 专供控制台测试（:210） |
| `ui/AboutOverlay.h` | 窗口内浮层（"no peer, no modal state"，:15-16）；关闭按钮 `componentID = "about.close"`（:23） |
| `PluginEditor.cpp` | `addChildComponent(aboutOverlay)`（:89）；`resized` 里铺满（:164-167）；ESC 关闭（:199-203）；`showAbout()`（:299-305）；`menu.standalone` 判据（:32） |
| 持久化现状 | `PropertiesFile|getSpecialLocation|XmlElement` 在 `plugin/Source` **0 命中** → 这是第一处落盘 |
| 单例惯例 | `playback::BackendRegistry::getInstance()`（`PluginProcessor.cpp:29`） |
| 测试 | `plugin/Tests/{AppHarnessTest,PlaybackCoreTest,PlaybackEngineTest,UiButtonTest}.cpp` + `scripts/{load-and-play,window-basics}.txt`；脚本命令 `load/press/click/toggle/set/dump/assert/sleep`（`AppHarnessTest.cpp:13-25`） |
| 版本 | `CMakeLists.txt:22`：`project(vgmtoybox VERSION 0.0.2 …)`，全项目唯一来源 |

## 4. 存储设计：`SettingsStore`

新增 `plugin/Source/settings/SettingsStore.{h,cpp}`（并把两个 `.cpp` 登记进 `plugin/CMakeLists.txt` 和需要的测试目标；实现时按现有写法登记）。

```cpp
namespace toybox::settings
{
/** 用户偏好，进程内唯一一份，落到 %APPDATA%\VGMToybox\settings.xml（§2.5）。

    只在 message thread 读写（音频线程永远不碰文件）；保存失败不抛，原因留在
    `lastError()` —— 沙盒宿主里写盘失败是正常情况，不是崩溃的理由。
*/
class SettingsStore : public juce::ChangeBroadcaster,
                      private juce::DeletedAtShutdown
{
public:
	enum class ThemeMode { light, dark, followSystem };

	static SettingsStore& getInstance (void);		// 同 BackendRegistry 的惯例

	ThemeMode getThemeMode (void) const;
	void setThemeMode (ThemeMode mode);				// 改变量 + 广播 + 延时落盘

	/** 把 followSystem 解析成实际该用的调色板（§8 的回退规则也在这里）。 */
	bool shouldUseDarkPalette (void) const;

	static juce::File settingsDirectory (void);		// …/VGMToybox
	static juce::File settingsFile (void);			// …/VGMToybox/settings.xml

	/** 空表示一切正常；非空是要显示在 General 页的一行说明。 */
	juce::String lastError (void) const;

	/** 删除设置文件并回到出厂值（General 页的"恢复默认"）。 */
	bool resetToDefaults (void);
};
}
```

要点：

- 常量**全部写死**，不用 `getApplicationName()`（ADR-2）：`folderName = "VGMToybox"`、`applicationName = "settings"`、`filenameSuffix = ".xml"` —— 后两个凑起来正好是 `Options::getDefaultFile()` 会给出的同名文件（`juce_PropertiesFile.cpp:117-119`），所以哪天改用那个 API 路径也不会变；
- 读：`PropertiesFile::Options{ folderName, osxLibrarySubFolder = "Application Support", commonToAllUsers = false, storageFormat = storeAsXML }` + 显式 File 构造（§2.5）。macOS 上 `osxLibrarySubFolder` **必须**设，否则断言；
- 写：先 `settingsDirectory().createDirectory()`，失败/保存失败只记 `lastError()`；键名用扁平 `theme.mode`（`light|dark|followSystem`），未识别值一律回退 `light`；字体偏好用 `appearance.font.mode`（`systemUI|custom`）+ `appearance.font.family`，见 §13 M7；
- 未知键**保留**：以后加设置项时旧文件不会被清掉（`PropertiesFile` 是键值表，天然满足）；
- 进程间锁留 `nullptr` → 两个 DAW 同时开着时互相覆盖是**已知限制**（§11 决策 8）；
- **读取语义（决策 8）**：启动读一次进内存、读完不持有文件句柄，只在改设置的那一刻写一次 —— `PropertiesFile` 的默认行为正好如此（读走 `parseXML`，写完即关）；
- `reload()`：给"读一次"留一个**再次读取**的入口（否则"进程外把文件改坏了"在运行中的进程里无法观察），测试与手工修复用，应用自身不调用；
- **测试覆盖目录**：环境变量 `VGMTOYBOX_SETTINGS_DIR` 非空时覆盖目录（只给测试用；未设置一律走平台默认路径，应用从不设置它）。

**补充（2026-10-18）：Standalone 状态文件**

JUCE 的 `StandalonePluginHolder` 还需要一个自己的 `PropertySet` 来保存音频设备、窗口位置和最后一次插件状态。这些东西不是用户偏好，而且 `filterState` 可能很大，所以没有并进 `settings.xml`：

- 新路径：`…/VGMToybox/standalone-state.xml`（三平台与 `settings.xml` 同目录）；
- 旧文件：JUCE 原来用 `ApplicationProperties` 生成 `VGMToybox.settings`，Windows 在 `%APPDATA%\VGMToybox\` 内，macOS / Linux 在 `VGMToybox` 文件夹的上一级；
- 首次启动时把旧文件移到新路径；`VGMTOYBOX_SETTINGS_DIR` 覆盖生效时跳过迁移，避免测试把开发机的真实状态搬进临时目录；
- 两个文件职责固定：`SettingsStore` 只写偏好，`StandaloneApp.cpp` 只写 Standalone 状态；这样语言/主题的即时保存不会覆盖大块插件状态。

## 5. 主题模型：`theme::` 从函数变成调色板

`ui/Theme.h` 改形（**函数名、签名、返回值类型全不变**）：

```cpp
namespace toybox::ui::theme
{
enum class Id { light, dark };

struct Palette
{
	juce::Colour window, windowTop, windowBottom, card, accent, primaryText, secondaryText,
	             border, cardBorder, track, error, mute, solo,
	             meterLow, meterMid, meterHigh,
	             cardAmbientShadow, cardKeyShadow, accentGlow, muteGlow, soloGlow,
	             controlShadow, buttonMaterial, buttonHairline, buttonShadow;
};

const Palette& palette (void);		// 当前生效的一份
void setTheme (Id id);				// 只换指针，不广播（广播由 SettingsStore 负责）

// 兼容层：170 处调用点无需改动
inline juce::Colour window (void)			{ return palette().window; }
inline juce::Colour accent (void)			{ return palette().accent; }
// …其余同名函数照此重写
}
```

- **指标不动**（`margin`/`cardGap`/`cardPadding`/`cardRadius`/`buttonRadius`/`sliderThumbRadius`/各 blur 与 offset 全是 `constexpr`，与主题无关）；
- 唯一必须改的调用点：`ui/ButtonPaint.h:31`（缓存了默认面色）→ 改成每次取值；
- `muteGlow()`/`soloGlow()` 现在是"语义色 + alpha"的组合函数（`Theme.h:68-69`）→ 保留为函数，内部读调色板；
- 暗色下的**层级方向**要换：浅色是"白卡片浮在浅灰窗上"，暗色必须是"**卡片比窗口亮**"才读得出浮起（One Dark 的 `#21252B` 底 + `#2C313A` 面板正是这个关系）。

### Dark 调色板（提案，One Dark 血统）

| 色键 | Light（`Theme.h:15-34` 现值） | Dark（提案） |
|---|---|---|
| `window` | `ff f2f2f7` | `ff 21252b` |
| `windowTop` | `ff f9f9fc` | `ff 2c313a` |
| `windowBottom` | `ff ecedf2` | `ff 1b1f26` |
| `card` | `ff ffffff` | `ff 2c313a` |
| `accent` | `ff 0a84ff` | `ff 61afef` |
| `primaryText` | `ff 1d1d1f` | `ff abb2bf` |
| `secondaryText` | `ff 74747a` | `ff 7f848e` |
| `border` | `ff d7d7dd` | `ff 181a1f` |
| `cardBorder` | `ff e6e6ec` | `ff 3e4451` |
| `track` | `ff dcdce2` | `ff 3e4451` |
| `error` | `ff d70015` | `ff e06c75` |
| `mute` | `ff e5484d` | `ff e06c75` |
| `solo` | `ff f5c518` | `ff e5c07b` |
| `meterLow` | `ff 34c759` | `ff 98c379` |
| `meterMid` | `ff ffcc00` | `ff e5c07b` |
| `meterHigh` | `ff ff3b30` | `ff e06c75` |
| `cardAmbientShadow` | `14 000000` | `33 000000` |
| `cardKeyShadow` | `11 000000` | `26 000000` |
| `accentGlow` | `4d 0a84ff` | `4d 61afef` |
| `controlShadow` | `38 000000` | `4d 000000` |
| `buttonMaterial` | `ff ffffff` | `ff 353b45` |
| `buttonHairline` | `ff d0d0d8` | `ff 4b5263` |
| `buttonShadow` | `26 000000` | `40 000000` |

M/S 与表头**保持 DAW 约定**（红/琥珀/绿黄红），只按 One Dark 的调子降饱和 ✓（决策见 §11）。

## 6. 设置窗口：`ui/SettingsOverlay`

复制 `AboutOverlay` 的成熟形态（`AboutOverlay.h:15-16`：窗口内浮层，无 peer、无 modal、不会卡住宿主）：

- 全屏铺满编辑器（`PluginEditor.cpp:164-167` 的写法），半透明遮罩 + 居中面板；
- 左 `TreeView`（根不可选，两个子项 `Theme` / `General`，之后加设置就加子项），右内容区 `ViewStack` 复用现成的页面容器；
- `componentID` 全部命名，供控制台脚本点击/断言：`settings.tree`、`settings.page.theme`、`settings.theme.light`、`settings.theme.dark`、`settings.theme.followSystem`、`settings.close`、`settings.general.path`、`settings.general.openFolder`、`settings.general.reset`；
- **即时生效**：改动立刻写入 `SettingsStore` 并广播（主题当场变），底部只有 `Close`；ESC 关闭（`PluginEditor.cpp:199-203` 的同一分支里加一条）；
- Theme 页：`Light` / `Dark` 单选 + `Follow system` 开关，**开关打开时单选仍可点**（点了就等于关掉开关，与菜单一致 ✓），并显示**当前实际生效**的颜色（"跟随系统（当前：Dark）"，§8）。**菜单与页面是同一份状态的两个视图**：菜单里选 Dark 会关掉开关 ✓ 反之亦然 ✓（都经由 `SettingsStore` 广播回来重画）；
- General 页（首版就放这些，决策 4）：设置文件完整路径（只读文本）+ `Open folder` 按钮 + `Restore defaults` 按钮 + `lastError()` 非空时的一行警示。

> 实现演进：Theme 页已扩为 **Appearance**（主题 + 字体，见 §13 M7），`settings.page.theme` 相应改为 `settings.page.appearance`；上面"首版两个类别"的描述保留为当时的决策记录。

## 7. 菜单：`Options` 与 `Theme`

`MainMenu` 只是数据（`buildGroups()` 是唯一出处，`MainMenu.h:68`），所以两处改动都在这一个函数里，外加 `getMenuBarNames()`（:225）改成 `{ "File", "View", "Options", "Theme", "Help" }`：

```
Options │ Settings…            (Ctrl+,  描述性快捷键，窗口在 keyPressed 里自己绑定)
Theme   │ Light               (互斥勾选)
        │ Dark
        │ Follow system       (与设置页的开关同一个状态)
```

- 互斥用**现有的 tick** 机制（`MenuEntry.checked` → `item.isTicked`，`MainMenu.h:247`）而不是 `addRadioItem`：View 菜单已经是 tick，两套标记在同一根菜单栏里会看起来像两种控件（ADR-4）；
- `checked` 必须**每次构建时现问** `SettingsStore`，不缓存 —— `MainMenu.h:38-39` 记着这个教训（"that bug is why"）；勾选反映**存下来的设置**（`light`/`dark`/`followSystem` 恰有一项被勾上，互斥因此没有歧义），跟随系统时**实际生效**的颜色由设置页那一行文字说明（§8），不需要把 `Light`/`Dark` 置灰；
- 新增回调 `std::function<void()> onSettings;` + 主题项直接调 `SettingsStore`（菜单不需要经过编辑器），编辑器只负责 `onSettings = [this] { showSettings(); }`；
- `describe()`/`invoke(id)` 自动获益：脚本可以 `invoke menu.theme.dark` 并断言 `menu.theme.dark checked == true`。

## 8. 跟随系统

- 每个编辑器实现 `juce::Desktop::DarkModeSettingListener`（或由 `SettingsStore` 统一持有监听并广播）→ 系统切换时重算 `shouldUseDarkPalette()`；
- `shouldUseDarkPalette()`：`light`/`dark` 直接返回；`followSystem` 返回 `Desktop::isDarkModeActive()`；
- **回退规则**（必须明写，否则用户会看到"开关没反应"）：
  - Linux 上 Wayland／无 gsettings → `isDarkModeActive()` 恒 `false` → 跟随系统 = 浅色（不报错）；
  - Windows 未达 build 17763 或处于高对比度模式 → 同上；
  - 设置页在"跟随系统"打开时显示**当前实际生效的颜色**（例："跟随系统（当前：Dark）"），这样拿不到系统值时用户能立刻看出原因；
- 无系统值时**不**把 `followSystem` 改掉（用户的偏好要留着，换个平台/换台机器该继续跟随）。

## 9. 测试方案

1. **存储（控制台）**：**已实现** —— `vgm_core_test` 的 `[7] the settings store` 段把目录指到 `%TEMP%\vgmtoybox-settings-test`（`VGMTOYBOX_SETTINGS_DIR` 覆盖，§4），断言：目录与文件名、首次使用建目录、**无文件时读默认且 `lastError()` 为空**（全新安装不该报错）、写入后磁盘上确有该值、大小写不敏感与未知值回退 `light`、**损坏文件 `reload()` 为 false 且 `lastError()` 非空且不崩**、`resetToDefaults()` 复位。
2. **窗口（脚本）**：新增 `plugin/Tests/scripts/settings-and-theme.txt`，用现成命令（`AppHarnessTest.cpp:13-25`）驱动：`click settings.close` 之类；要点是 `assert settings.theme.dark checked == true`、切主题后 `dump` 里编辑器的调色板确实变了。
3. **像素（`vgm_ui_test`）**：现有按钮测试只覆盖浅色（"白面浮在白卡上"）。**暗色必须各加一组**同款断言（阴影/发丝线/圆角在暗底上是否仍可读）—— 这正是截图之外唯一的机械化证据（ADR-5）。
4. **CI**：无需改矩阵（三个平台已在跑三个测试目标），但 Windows 行会顺带验证 `%APPDATA%` 路径、Linux 行验证 `~/.config`。

## 10. ADR

| # | 决策 | 理由 |
|---|---|---|
| ADR-1 | 用 `PropertiesFile` + **自己算的 File**，而不用 `Options::getDefaultFile()` | Linux 默认落 `~/VGMToybox`，不是 XDG 的 `~/.config`（§2.5）；显式 File 构造保留延时写入、成败返回、析构 flush |
| ADR-2 | 目录名与文件名写死常量，不用 `getApplicationName()` | 插件里没有 `JUCEApplication` 实例，该 API 在插件上下文不可靠；写死才能保证插件与 Standalone 命中**同一份**文件（R8） |
| ADR-3 | 设置窗口做成**窗口内浮层**，不是独立窗口 | `AboutOverlay.h:15-16` 已有一段血泪：独立窗口在宿主里会挡住整个应用；且浮层能跟随编辑器缩放、能被控制台脚本驱动 |
| ADR-4 | 主题互斥用 `isTicked`，不用 `addRadioItem` | 与 View 菜单同一套视觉与代码路径（`MainMenu.h:247`） |
| ADR-5 | 暗色配色**必须**进像素测试 | 浅色主题的"按钮是否浮起"只存在于像素里（`vgm_ui_test` 的存在理由）；换一套底色的全部风险也只在像素里 |
| ADR-6 | 主题状态放在 `theme::` 的进程级变量，而不是每个编辑器各存一份 | 设置本来就是用户级全局；LookAndFeel 仍保持**每编辑器一个实例**（`ui/ToyboxLookAndFeel.h:11-13` 的约束不破），两者靠广播衔接 |
| ADR-7 | 即时生效，无 OK/Cancel/Apply | 主题只有"当场看到"才好选；设置项少且都可回退（General 页有 Restore defaults） |

## 11. 决策记录（2026-10-17 已定）

| # | 问题 | 决定 |
|---|---|---|
| 1 | 菜单名与位置 | `Options`，`View` 之后、`Theme` 之前 |
| 2 | `Theme` 菜单列不列 `Follow system` | 列：`Light` / `Dark` / `Follow system` 三项互斥（选了跟随就等于关掉设置页的开关） |
| 3 | 设置窗口是否即时生效 | 即时生效，无 OK/Cancel/Apply（ADR-7） |
| 4 | General 页首版内容 | 设置文件路径（**只读**文本，作用是"出问题时能找到它、备份/删掉它"，不是让用户改路径）+ `Open folder` + `Restore defaults` + 保存失败时的警示行 |
| 5 | 暗色下 M/S 与电平表配色 | 保持 DAW 约定（红 / 琥珀 / 绿黄红），只按 One Dark 降低饱和（§5 表） |
| 6 | `LightLookAndFeel` 改名 | 改成 `ToyboxLookAndFeel`（M2 一起做，连带更新旧文档引用 `docs/review/2026-10-16-….md:205` 的 L-1）。**M2 已做**，文档引用已同步 |
| 7 | 版本号 | 功能完成、合并前改 `CMakeLists.txt:22` → `0.1.0`（新 UI 页面 = MINOR，`maintenance-policy.md:85`）；中途不改，否则中途构建都会自称 0.1.0 |
| 8 | 跨进程同步 | 接受不实时：**启动读一次 → 读完释放文件 → 只在改设置时写入**（这正是 `PropertiesFile` 的语义）。两个进程互相覆盖时后写者胜，重启后一致 |

## 12. 里程碑（建议实施顺序）

1. **M1 存储**：**已完成**（见 §13）—— `SettingsStore` + 路径三平台可打印 + 读写/降级/损坏测试（不碰 UI，风险最低、后续全靠它）；
2. **M2 调色板化**：`Theme.h` 改造 + Dark 配色 + `ButtonPaint.h:31` 修正 + 现有 170 处零改动验证（跑一遍现有测试应全绿）；
3. **M3 设置窗口**：`SettingsOverlay` + Tree + Theme/General 页 + ESC + 组件 ID；
4. **M4 菜单**：`Options`/`Theme` 两组 + `getMenuBarNames` + `describe()` 可见；
5. **M5 跟随系统**：监听 + 回退规则 + 页面显示当前实际生效值；
6. **M6 测试与收尾**：脚本、暗色像素组、版本号 `0.1.0`、`docs`/README 相应更新（按 `maintenance-policy.md` §6 流程走 tag）。

## 13. 实施记录

### M1 存储（2026-10-17）

- 新增 `plugin/Source/settings/SettingsStore.{h,cpp}`；`SettingsStore.cpp` 登记进 `plugin/CMakeLists.txt` 的插件目标与 `vgm_core_test`，并给该测试目标加 `juce::juce_data_structures`（`PropertiesFile` 所在模块；插件目标本来就经 `juce_gui_basics` 依赖它，所以只有测试目标需要显式加）；
- 与设计一致，只有三处补充，都不改变上面的结论：
  1. `isValidFile()` 只在构造后查一次 —— `juce_PropertiesFile.h:194-198` 写明"valid **or non-existent**"，所以全新安装不会误报"读不了设置"；
  2. 增加 `reload()`（§4）：测试与手工修复用，应用自身不调用；
  3. 测试用 `VGMTOYBOX_SETTINGS_DIR` 覆盖目录，不碰开发机真实 `%APPDATA%`；
- 跑过：`build.ps1 -Target vgm_core_test -Test` → **5 个测试目标全绿**（三个控制台二进制 + 两个窗口脚本 `load-and-play.txt`、`window-basics.txt`；`vgm_mixer_test` 按既定门槛跳过，它需要 `-TestSong`）；
- 未碰 UI：`theme::`、菜单、编辑器一行未改（M2 才动）；
- 提交：`7a196cc`（"add the settings store, the file a theme choice will live in"，与本文档同批）。

### M2 调色板化（2026-10-17）

**改了什么**

- 新增 `plugin/Source/ui/Theme.cpp`（两套调色板 + 当前主题 + `systemTheme()`）；`ui/Theme.h` 改成调色板模型，**getter 名、签名、返回类型全部不变** → 调用点一行未改（这是 §3 定下的验收条件）。`Theme.cpp` 登记进 `plugin/CMakeLists.txt` 的插件目标与 `vgm_ui_test`；
- `ui/LightLookAndFeel.{h,cpp}` → `ui/ToyboxLookAndFeel.{h,cpp}`（决策 6）：构造函数拆出无参 `refresh()`，"读调色板"只在这两个地方发生，此后该文件里再无硬编码颜色（`Colours::white` 与几个 `0xff…` 字面量，如提示框的 `0xf7ffffff`、"未勾选方框"的 `#c5c5cb`、手柄渐变的 `#e6e6ec`，全部换成色键）；
- `ui/ButtonPaint.h` 去掉 `ButtonFace::fill` 的默认初始化器（§5 点出的那处；理由同 §5：成员初始化器只在构造时跑一次，而 Face 常在构造函数里就建好了）；
- "自己 `setColour` 出来的颜色"收进各自的 `lookAndFeelChanged()`：编辑器（Open 按钮、状态行）、`TransportPanel`（音量文本框 4 个色键）、`ChannelStrip`（M/S）、`SongInfoPanel`（3 个标签）、`AboutOverlay`（关闭键 + 两条链接）、`DropHintOverlay`（提示文字）；`ChipPanel` 的那行**删掉**即可，LookAndFeel 的 `ToggleButton` 颜色本来就是 `primaryText`。

**机制**（§5 只写了"广播由 `SettingsStore` 负责"，具体链条在这里定死）

`SettingsStore::setThemeMode()` → `ChangeBroadcaster` → 编辑器 `changeListenerCallback()` → `applyPalette()`（存储模式 → `paletteFor()` → `theme::setTheme()` + `lookAndFeel.refresh()`）→ `sendLookAndFeelChange()` + `updateStatusLine()` + `repaint()`。

两条路缺一不可，因为颜色有两种存放方式：`setColour()` 存的是**副本**（`sendLookAndFeelChange()` 会递归走整棵树，`juce_Component.cpp:1929-1951`，每个控件在自己的重载里重取），而画时**查**的色不在任何控件里（只有 `repaint()` 能救）。`sendLookAndFeelChange()` 先自己后子，所以面板重取色一定早于子控件。`Slider` 更特殊：`colourChanged()` → `lookAndFeelChanged()` 会 `valueBox.reset(new)` **销毁重建**文本框（`juce_Slider.cpp:593-607`），重建时读的正是滑块自己刚被重设的那几个 `textBox*` 色，所以音量数字换主题后立刻跟着变（写 `TransportPanel` 时验证过这一点）。

**与设计的三处偏离**

1. **`shouldUseDarkPalette()` 没有放进 `SettingsStore`**（§6 的草案）：`vgm_core_test` 只链 `juce_core`/`juce_audio_basics`/`juce_data_structures`，而 `juce::Desktop` 属于 `juce_gui_basics` —— 放进去那个测试**链接不过**，而"核心测试不链格式引擎与 gui"正是这个仓库刻意维持的机械证明。解析因此留在 UI 层：`theme::systemTheme()`（`Theme.cpp`）+ 编辑器里的 `paletteFor()`；§8 的回退规则不变（拿不到系统值就是浅色），设置页要显示"当前：Dark"时复用 `theme::systemTheme()`（M3）。
2. **`Palette` 比 §5 草案多 6 个色键**：`onAccent`、`tooltipBackground`、`controlFaceTop`、`controlFaceBottom`、`controlEdge`、`controlGroove`。这些角色原本是 `ToyboxLookAndFeel.cpp` 里的字面量，不进调色板就切不了主题。
3. **两个颜色值偏离 §5 表**：`windowTop` `2c313a` → `262a32`、`windowBottom` `1b1f26` → `1a1e24`。§5 自己那条规则是"卡片必须比窗口亮"，而 §5 给的 `windowTop` **等于** `card`（都是 `2c313a`）—— 卡片在窗口上半部会化进背景，于是顶端往下压、底端跟着重算。其余色键与 §5 表逐项相同。

**另修一个只有暗色才暴露的 bug**

`drawButtonBackground()` 原来用"感知亮度 < 0.9"判断"中性面还是填充面"：浅色下成立（白面亮、强调色暗），但暗色的中性面 `#2c313a` 比它的强调色 `#61afef` **更暗** —— 每个中性按钮都会被判成填充型、涂成卡片色，等于隐形。改成与主题自己的中性色比较（`backgroundColour != theme::card() && != theme::buttonMaterial()`）。

**测试**

- `build.ps1`（全量）→ 通过；
- `build.ps1 -Target vgm_core_test,vgm_ui_test,vgm_app_test -Test` → **5 个测试目标全绿**（三个控制台二进制 + 两个窗口脚本）；
- **`vgm_ui_test` 必须补 `Source/ui/Theme.cpp`**：调色板以前是头文件里的 `inline` 常量，现在定义在翻译单元里，而那个测试只编 painter 相关源码（`UiButtonTest.cpp` + `IconButton.cpp` + `Icons.cpp`）→ 先**链接失败**（`unresolved external symbol theme::palette`），补上源码即绿。这条改动本身就是"调色板真的离开头文件了"的证据；
- 遗留：§8 的 `Desktop::DarkModeSettingListener`（系统实时切换）**未接**，它只需调同一条 `applyPalette()` + 广播路径；暗色像素组按 ADR-5 归 M6。

### M3 设置窗（2026-10-17）

**改了什么**

- 新增 `plugin/Source/ui/SettingsOverlay.{h,cpp}`：`SettingsOverlay`（遮罩 + 居中面板 + 侧栏 `TreeView` + `ViewStack` 两页 + Close）、`ThemeSettingsPage`、`GeneralSettingsPage`；两个页面都是 `ui::View` 子类，所以进 `ViewStack` 与其它页面同路。文件登记进 `plugin/CMakeLists.txt`；（2026-10-18：standalone 里多出第三页 Audio，见 `native-window-and-audio-settings.md` §2.4；下面的两页描述是当时的状态）
- 组件 ID 与 §6 表逐项一致（`settings`、`settings.tree`、`settings.page.theme|general`、`settings.theme.light|dark|followSystem`、`settings.theme.current`、`settings.general.path|pathCaption|openFolder|reset|error`、`settings.close`），脚本可直接断言；
- 编辑器接线（`PluginEditor.{h,cpp}`）：成员 `settingsOverlay` + `addChildComponent` + `onClose`、`showSettings()`、`resized()` 同步尺寸、`keyPressed()` 里 `Ctrl+,` 打开、ESC 先关它再关 About；两个浮层互斥（各自的 `show*()` 把对方 `setVisible(false)`）。

**两处细化**（不改结论）

1. 侧栏树三项设置：`setRootItemVisible(false)`（不显示 root 那一行）、每个 `CategoryItem::canBeSelected() == false`（§6 要的"根不可选"）、`setOpenCloseButtonsVisible(false)`。第三项让 `getIndentX()` 变成 −1，而行宽按 `viewport.getViewWidth() - indentX` 算（`juce_TreeView.cpp:1854-1880`、`:1938-1960`），正好把那 1 像素补回来 —— 行仍从树的左边缘起；
2. 每次打开都 `refreshFromStore()`：页面只显示存储里的值（§6"菜单和页面是同一状态的两个视图"），所以打开时重读一次即可；页面自身不监听存储，关闭期间的菜单改动在下次打开时显示出来。

**一处偏离：广播改成同步**（`SettingsStore::broadcastChange()`）

`juce::ChangeBroadcaster::sendChangeMessage()` 是**异步**的 —— 要等下一次消息循环回调才通知，而 §9.2 的测试计划正是"`menu invoke theme.dark` 之后看窗口（乃至像素）"。没有消息循环时那条链永远走不到。改成 `sendSynchronousChangeMessage()`：写入返回前监听器已跑完，主题在**当次调用内**生效（宿主里也更好推理：这次点击处理完，调色板就已经换了）。所有监听器只读存储，不会重入写入。

### M4 菜单（2026-10-17）

- `ui/MainMenu.h` 多两组：`Options > Settings...`（`Ctrl+,`，转发到新的 `onSettings`）与 `Theme > Light/Dark/Follow system`；`getMenuBarNames()` 变成 `File, View, Options, Theme, Help` —— 顺序即 `buildGroups()` 的顺序，`getMenuForIndex()` 用它索引，注释里写明两者不能各自排序；
- Theme 三项**每次构建都现问 `SettingsStore`** 拿勾选（沿用 View 菜单的教训），动作只写存储、不通知任何人：广播由存储负责，菜单不做第二个真相来源。

**测试与验证（M3 + M4）**

- `build.ps1 -Target vgm_core_test,vgm_ui_test,vgm_app_test -Test` → **5 个测试目标全绿**；
- 另用一个临时窗口脚本（`build.ps1 -Target vgm_app_test` 构建后手工跑；环境变量 `VGMTOYBOX_SETTINGS_DIR` 指向临时目录，不碰真实配置）走 44 条断言：菜单勾选、窗口内容与真实设置文件路径、ESC 关闭、`Ctrl+,` 打开、菜单改主题后窗口显示新值、`Follow system` 的 "now: Light"，并留三张截图（浅 / 深 / 跟随）。深色那张同时证明"写入→广播→生效"整条链走通（不是只有存储里的值对）；
- 浅色与深色截图逐像素看过：深色下整个面板（侧栏、选中行、按钮、标签、遮罩）都换了色，无残留白块。

**harness 的三处补强**（都为跑上面那个脚本，都在 `plugin/Tests/AppHarnessTest.cpp`）

1. `press escape`：`keyFromName()` 补 `escape`（浮层的 ESC 关闭此前无法脚本化）；
2. `sleep <ms>` 现在**真的跑消息循环**（`commandSleep()` 调 `MessageManager::runDispatchLoopUntil()`）。此前它只是 `Sleep()`，而帮助文字写的是 "let the message loop run" —— 文字与实现不符；`settle()` 的注释也据此重写（默认只等 ✓ 需要下一个回调的东西用 `sleep` ✓）；
3. 前提：给 `vgm_app_test` 单开 `JUCE_MODAL_LOOPS_PERMITTED=1`（`plugin/CMakeLists.txt`）。它只给 `MessageManager` 多加一个成员函数**声明**（无数据成员、无虚函数），所以不会与插件那份头文件产生布局分歧。

**第 2 条是必需的，值得单独记**：`TreeView` 的行宽由 `AsyncUpdater` 在下一个回调里算（`juce_TreeView.cpp:794-802` 给内容组件 `setSize()`，`:400` 再按内容宽度给每行设 `setBounds`）。没有消息循环时，脚本里打开的设置窗侧栏行**永远是窄的**（截图上是 "Th…"/"Ge…"，只有 40 来像素宽），而真实程序一帧内自愈 —— 这是"脚本看不见异步布局"，不是产品缺陷。`click` 的语义**没有动**：`load-and-play.txt` 依赖它现在的行为（只翻状态不跑处理器），需要处理器真跑的脚本在 `click` 后面跟一条 `sleep` 即可。

**遗留**

- `settings.theme.*` 按钮与 `settings.general.reset/openFolder` 的**点击路径**要靠 `sleep` 才能跑到（`click` 只投递消息）→ 归 M6 的脚本（§9.2）；
- `Follow system` 的系统实时切换（§8）仍归 M5；
- 未提交：按计划随 M6 收尾一起提交。

### M7 字体设置（2026-10-18 增补）

**需求变化**：用户要求 Appearance 页提供字体设置，点击后打开字体选择器。原 §1 R9/R10 只规划 Theme + General；实现把 Theme 页扩为 **Appearance**（Theme 段 + Font 段），`settings.page.theme` → `settings.page.appearance`，其余页面不动。

- **存储**（`Source/settings/SettingsStore.*`）：`appearance.font.mode`（`systemUI` / `custom`）+ `appearance.font.family`。`getFontMode()` 在 custom 但 family 为空/纯空白时按 systemUI 处理，避免坏文件把窗口锁在没有字体的状态。读写/重置由 `vgm_core_test` 的字体验证覆盖。
- **解析**（`Source/ui/UiFont.*`）：`systemUiFamily()` 取 `Typeface::findSystemTypeface()->getName()`；`resolvedFamily()` 在 custom 字体未安装时回退到它。**关键点**：JUCE 的默认 `Font` 通过 `LookAndFeel::getDefaultLookAndFeel()` 解析 typeface，而不是组件继承到的 `ToyboxLookAndFeel`。只在 `ToyboxLookAndFeel` 上 `setDefaultSansSerifTypefaceName()` 时，截图确认窗口仍用默认字体；`applyStoredSelection()` 改为写**默认 LNF** 后才真正生效。`Font` 在首次使用时缓存 typeface，所以已存在的标签不能原地换字体，页面明确提示重开窗口；没有用"重建所有控件"的假实时。
- **选择器**（`Source/ui/FontChooserDialog.*`）：JUCE 8.0.9 没有原生字体对话框，因此做了组件式选择器：搜索框 + `Font::findAllTypefaceNames()` 列表（第一行 System UI，排除 `<...>` 占位名）+ 大/小两行预览（含中文）+ OK/Cancel；双击和 Enter 接受，Esc 取消。custom 字体缺失时，设置页显示 `settings.appearance.font.missing` 并说明回退到 System UI。
- **测试**：`plugin/Tests/scripts/settings-pages.txt` 断言 `settings.appearance.font.value` 的 `typeface == @systemUI`。harness 新增 `typeface` 断言字段和 `@systemUI` 运行时解析（`plugin/Tests/AppHarnessTest.cpp`），这条断言在只设置 `ToyboxLookAndFeel` 的旧实现下会失败；`# settings font <family>|systemUI` 指令可在编辑器和窗口存在之前写入字体，便于脚本观察。`FontChooserDialog` 自身仍通过手工打开验证（harness 不驱动独立 `DialogWindow`）。

