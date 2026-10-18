# 界面语言（i18n）设计

状态：**已实现**（2026-10-18）　分支：`feature/i18n`

相关代码：

- `plugin/Source/i18n/LanguagePack.{h,cpp}`：解析一份 `.lang` 文件；
- `plugin/Source/i18n/LanguageManager.{h,cpp}`：加载内置包、扫描外部目录、选择与回退；
- `plugin/assets/i18n/{en,zh-Hans}.lang`：编译进二进制的内置语言包；
- `plugin/Source/ui/SettingsOverlay.{h,cpp}`：`LanguageSettingsPage`；
- `plugin/Source/settings/SettingsStore.{h,cpp}`：保存 `ui.language` / `ui.languageTag`；
- `plugin/Source/PluginEditor.cpp`：收到设置广播后刷新整个窗口；
- `plugin/Tests/scripts/language.txt`、`plugin/Tests/assets/i18n/my-chinese.lang`：窗口测试。

本文只描述“界面语言”。设置窗口的页面容器、主题、持久化位置见 `settings-and-theme-design.md`；Standalone 的托盘菜单结构见 `standalone-last-song.md` 相关部分。

---

## 1. 结论

界面字符串**不接管 JUCE 的全局 `LocalisedStrings`**，而是由项目自己的 `LanguageManager` 查找：

```text
选中的外部文件 -> 同语言标记的内置包 -> 英文内置包 -> 键名本身
```

内置英文与简体中文随二进制发布；用户可以把 `.lang` 文件放进设置目录下的 `i18n` 文件夹，不重新编译就能增加一种语言，或覆盖某种内置语言的少数条目。语言选择写入 `settings.xml`，插件与 Standalone 共用同一份选择。

先定下来的几件事：

| 决定 | 原因 | 落点 |
|---|---|---|
| 自己解析 `.lang`，不用 `juce::LocalisedStrings` | 需要文件的 `id:`（回退用的语言标记），以及“第几行出错”的诊断；`LocalisedStrings` 两者都不暴露 | `LanguagePack` |
| 内置语言编进 BinaryData | 插件 bundle 不能依赖旁边的资源目录；VST3 与 Standalone 必须看到同一份列表 | `plugin/CMakeLists.txt` 的 `VGMToyboxI18n` |
| 外部文件放在 `<设置目录>/i18n` | 与 `settings.xml` 同目录；便携安装和测试的临时目录不需要第二套路径约定 | `LanguageManager::externalDirectory()` |
| 保存 `id` 与 `languageTag` 两个值 | `id` 精确指向某个文件；文件被删后，`tag` 还能回退到同语言的内置包 | `SettingsStore` |
| 切换通过 `SettingsStore` 同步广播 | 插件和 Standalone 共用同一个 store；每个打开的编辑器都能在同一帧重新取词 | `SettingsStore::broadcastChange()` |
| 语言页手动“重新扫描” | 不做文件监听；编辑器、DAW、网盘同步目录里改文件的行为不可控，显式按钮更容易解释 | `LanguageSettingsPage` |
| 语言名用文件自己的写法 | `English`、`简体中文` 不应被当前界面语言翻译，否则用户找不到自己的语言 | `language:` 元数据 |

**非目标**：

- 不做复数、性别、上下文（`ngettext` / ICU MessageFormat）；
- 不做 RTL 布局镜像，也不做按语言切换字体；
- 不做日期、数字、文件大小的区域格式；
- 不翻译歌曲元数据、文件名、芯片名、插件/产品名；
- 不自动翻译，不联网下载语言包；
- 不监听文件变化；新增或修改文件后由用户点 **Rescan**；
- 不把语言选择做成宿主参数或工程状态。

---

## 2. 语言包格式

文件是 UTF-8 文本（可带 BOM），扩展名 `.lang`。格式刻意接近 JUCE 的 `LocalisedStrings`，但解析器是项目自己的。

### 2.1 元数据

```text
id: zh-Hans
language: 简体中文
```

- `id:` 必填，是语言标记（`en`、`zh-Hans`、`ja`……），也是回退链匹配内置包的依据；
- `language:` 必填，是语言列表里显示的名字，使用该语言自己的写法，**不翻译**；
- `countries:`、`format:` 接受但当前不参与解析，只为兼容 JUCE 格式和未来格式变化；
- 元数据名大小写不敏感；
- 元数据行可以出现在文件任意位置，不要求真的在前两行；README 与用户文档建议放在开头，是为了可读性。

### 2.2 键值

```text
"menu.file.open" = "打开歌曲...";
"transport.play"  = "播放";
```

- 键大小写敏感，必须非空；
- 值可以用引号，也可以写成裸值；裸值会去掉首尾空白和末尾一个 `;`，适合 `id: zh-Hans` 这种简单值；
- 引号值内部支持 `\n`（当前平台的换行）、`\r`、`\t`、`\"`、`\\`；其他 `\x` 会变成 `x`（反斜杠被丢掉）；
- 值不能跨行；需要换行时写 `\n`；
- 引号闭合后只允许空白和一个可选的 `;`，后面再有字符视为错误；
- 注释只支持整行：去掉行首空白后以 `//` 或 `#` 开头；**不支持行尾注释**，所以值里的 `#` 是普通字符；
- 同一个文件里重复键是错误，不会“后者覆盖前者”；
- 缺少 `id:` 或 `language:` 是错误；
- 解析错误会带上文件名和行号，例如 `my-file.lang line 12: duplicate key "transport.play"`。

### 2.3 示例

```text
id: zh-Hans
language: 简体中文

# 只翻译两行也可以；其余键会回退到内置简体中文。
"menu.file.open" = "打开歌曲...";
"transport.play" = "播放";
```

英文包是**参考包**：应用会查找的每个键都应先出现在 `plugin/assets/i18n/en.lang`。当前（2026-10-18）有 153 个键。其他包可以只翻译一部分；空字符串按“没有这个键”处理，缺少的数量显示在语言页。

外部文件可以额外包含英文包没有的键，解析器会接受，但应用不会查找它们，也不会为此报警。

### 2.4 外部文件的限制

`LanguageManager::scanExternal()` 的行为：

- 只看 `<设置目录>/i18n` 这一层，不递归；
- 只收 `*.lang`（平台文件系统的大小写语义）；
- 跳过文件名以 `.` 或 `_` 开头的文件（隐藏文件、模板）；
- 按文件名大小写不敏感排序；
- 最多加载 128 个文件；超出的文件不加载，语言页给一行警告；
- 单个文件最大 1 MiB；超限跳过并警告；
- 任何一个文件解析失败只跳过该文件，其它文件照常加载。

---

## 3. 加载、选择和回退

### 3.1 启动

`LanguageManager::initialise()` 是幂等的，第一次调用时：

1. 从 BinaryData 载入 `en.lang` 和 `zh-Hans.lang`，放在列表最前面；
2. 创建并扫描外部 `i18n` 目录；
3. 读取保存的选择并应用。

内置资源名不是文件名：`juce_add_binary_data` 会把 `zh-Hans.lang` 变成 `zhHans_lang`（连字符被丢掉，不是换成下划线）。`loadBuiltIn()` 里必须写生成后的名字。

内置包缺失或解析失败不会让程序崩溃：会记录警告，`current` 可能为空，此时 `tr()` 退回键名，语言页显示内置错误。正常构建不应出现这种情况。

### 3.2 第一次启动

没有保存的选择时，`chooseDefault()` 读 `juce::SystemStats::getUserLanguage()`：

- 只有返回值等于 `zh`（大小写不敏感）时选内置 `zh-Hans`；
- 其他返回值一律选 `en`。

这里刻意不做 `zh-CN` / `zh-TW` / `zh-Hant` 的启发式匹配。当前只有两个内置包；如果系统返回的不是精确的 `zh`，落到英文比猜错繁体/简体更可预测。增加第三个内置语言时，应同时修改这段映射，而不是让它跟着系统字符串自动变化。

### 3.3 保存的选择

`SettingsStore` 保存两个键：

```text
ui.language      = builtin:zh-Hans | external:my-chinese.lang
ui.languageTag   = zh-Hans
```

- `builtin:<id>` 的 `<id>` 是内置包 `id:` 的值；
- `external:<filename>` 的 `<filename>` 是外部文件的文件名（含 `.lang`）；
- `ui.language` 决定精确选择；
- `ui.languageTag` 是同一份文件的 `id:`，用于文件消失后的同语言回退。

`applyStoredSelection()` 的规则：

| 保存的状态 | 结果 |
|---|---|
| `ui.language` 指向当前列表里的条目 | 直接使用该包 |
| `ui.language` 找不到，但 `ui.languageTag` 能匹配内置包 | 使用该内置包，并在语言页显示“保存的语言未找到”的警告 |
| 两者都找不到 | 按系统语言选默认包，同样显示警告 |
| 没有保存值 | 按系统语言选默认包，不写回设置 |

找不到文件时**故意不删除** `ui.language`：用户可能只是临时重命名、换机器同步中，或把文件放回去。文件回来后再点 Rescan，保存的选择会重新生效。这也是为什么 `getCurrentId()` 返回的是“当前真正生效的包”，而 `getRequestedId()` 保留“用户最初要求的 id”。

### 3.4 回退顺序

`translate(key)` 的查找顺序：

1. 当前包（外部文件或内置包）；
2. 如果当前包不是英文，找**同语言标记的内置包**；
3. 英文内置包；
4. 键名本身。

因此：

- 一份 `id: zh-Hans` 的用户文件只翻译两行，其余中文来自内置简体中文，而不是英文；
- 一份 `id: ja` 的用户文件没有内置日文包，缺失项落到英文；
- 外部文件把某个键的值写成空字符串，等同于没翻译，会继续回退；
- `translate("")` 返回空字符串；只要键名非空，`translate()` 不会返回空字符串。

### 3.5 语言列表

`getAvailable()` 的顺序是：

1. 内置 `en`；
2. 内置 `zh-Hans`；
3. 外部文件，按文件名排序。

列表项显示为：

```text
English (built-in)
简体中文 (built-in)
简体中文 (my-chinese)
```

第一段来自文件的 `language:`，括号里是内置标记或去掉 `.lang` 的文件名。外部文件和内置包可以拥有相同的 `id:`，它们会作为两个独立选项出现；测试正是利用这一点验证“外部文件覆盖部分键、其余回退内置包”。

---

## 4. 切换与界面刷新

### 4.1 选择流程

`LanguageSettingsPage` 的下拉框把 `shownLanguages` 的下标 + 1 作为 JUCE item id。用户选择后：

1. `LanguageManager::selectById(id)` 在**当前列表**里查找；找不到就返回 `false`，页面重新刷新回原值；
2. 找到后设置 `current`、清掉“文件不存在”警告；
3. `SettingsStore::setLanguageSelection(id, tag)` 写入两个键并保存；
4. `SettingsStore` 同步广播（`sendSynchronousChangeMessage()`）。

如果用户选的正是当前项，store 认为没有变化，不写文件也不广播；页面本来就是正确的，无需刷新。

### 4.2 广播后的刷新

每个 `ToyboxAudioProcessorEditor` 都监听同一个 `SettingsStore`。收到广播后：

- 调 `LanguageManager::applyStoredSelection()` 重新读设置（主题变化时这一步是 no-op）；
- 重设打开按钮的文本和工具提示；
- 重设菜单的页面名称，并让菜单重建；
- 调 `viewStack.refreshLanguage()`、`dropHint.refreshLanguage()`、`aboutOverlay.refreshLanguage()`、`settingsOverlay.refreshLanguage()`；
- `SettingsOverlay::refreshLanguage()` 再重建侧栏标签，并让每个页面重新取词；
- 最后 `sendLookAndFeelChange()`、更新状态行、`repaint()`。

`LanguagePage` 自己的“重新扫描”也会调 `SettingsStore::notifyChanged()`：即使新增文件没有改变当前包，也要让已经打开的窗口重新读列表/警告。

### 4.3 为什么不用全局 `LocalisedStrings`

`juce::LocalisedStrings` 是进程级安装的。一个窗口正在画 tooltip 时另一个窗口切换语言，容易出现“半套语言”的中间状态，也不方便让每个编辑器独立刷新。现在的做法是：

- `LanguageManager` 用 `shared_ptr<const LanguagePack>` 保存当前包；
- 切换只交换指针，包本身不可变；
- 所有取词都走 `i18n::tr(key)`，不读全局 locale；
- 刷新由 store 广播驱动，而不是由某个控件直接改另一个控件。

### 4.4 Standalone 托盘

托盘菜单在**每次弹出时**由 `TrayMenu::build()` 重新构造，里面的 `i18n::tr(...)` 因此总是当前语言。没有长期存在的、缓存了旧语言的托盘菜单项，也就不需要额外的 `refreshLanguage()`。

---

## 5. 语言设置页

设置窗口的页面顺序是 `Theme`、`General`、`Language`、`Audio`（`Audio` 只有 Standalone 有）。`Language` 页包含：

| 控件 | 行为 |
|---|---|
| 下拉框 | 内置包 + 外部文件；显示名来自文件自己的 `language:` |
| 来源标签 | 内置包显示“内置翻译：简体中文”；外部文件显示完整路径 |
| 详情标签 | `Language tag: zh-Hans - missing keys: 2`，缺键数相对英文包 |
| 警告区 | “保存的文件不存在”以及每个被跳过文件的一行错误 |
| **Open Folder** | 创建 `<设置目录>/i18n` 并在系统文件管理器中显示 |
| **Rescan** | 重建外部列表、清掉旧的外部警告、重新应用保存的选择，然后广播刷新 |

页面不提供“删除语言”或“导入文件”：用户直接管理文件夹，路径由按钮打开，避免再造一套文件生命周期。

---

## 6. 新增文案与新增语言

### 6.1 新增一条界面文案

1. 在 `plugin/assets/i18n/en.lang` 加键，键名用稳定的语义标识（`transport.play`，不要用英文原文当键）；
2. 在 `plugin/assets/i18n/zh-Hans.lang` 加同一键；
3. 在代码里用 `i18n::tr("...")`，不要在控件构造时把结果缓存到不会被刷新的 `static` / 成员里；
4. 如果控件在语言切换时必须改文字，把它纳入所属组件的 `refreshLanguage()`；
5. 需要参数时用 `{0}`、`{1}` 和 `tr(key, arg0, arg1)`，不要手工拼接半句话；
6. 在语言页确认缺键数为 0（或明确接受缺失）。

占位符是字面替换，不支持重排参数、复数或转义。中文和英文语序差异大时，应该让整句成为一条翻译，而不是把单词拼起来。

### 6.2 增加一种内置语言

1. 把 `.lang` 放进 `plugin/assets/i18n/`；
2. 在 `plugin/CMakeLists.txt` 的 `juce_add_binary_data(VGMToyboxI18n ...)` 的 `SOURCES` 里加入文件；
3. 在 `LanguageManager::initialise()` 里用生成后的资源名调用 `loadBuiltIn("...", "tag", builtIns)`；
4. 如果它应参与首次启动的默认选择，修改 `chooseDefault()` 的映射——不要假设它会自动被选中；
5. 在 README 的语言说明里补上名称；
6. 用 `language.txt` 同样的方式加一个窗口脚本，至少验证“选中后打开按钮、菜单、设置页都换了文字”。

内置文件的 `id:` 必须与 `loadBuiltIn()` 传入的 tag 一致；内置 ID 是 `builtin:<id>`，改动它会让用户已保存的选择失效。

### 6.3 增加一种外部语言

不需要改代码或重新构建：

1. 复制 `en.lang` 作为键清单；
2. 修改 `id:` 和 `language:`；
3. 放进 `<设置目录>/i18n`；
4. 打开语言页点 **Rescan**，选中它；
5. 没翻译的键会按第 3.4 节的顺序回退。

文件名不要以 `.` 或 `_` 开头，否则会被当成隐藏文件/模板跳过。

---

## 7. 测试

自动化入口是 `plugin/Tests/scripts/language.txt`，由 `plugin/CMakeLists.txt` 自动发现为 `app.language`：

```powershell
.\build.ps1 -Test
# 或只跑这一个：
ctest --test-dir build -R app.language --output-on-failure
```

脚本的第一行指令：

```text
# settings languageDir plugin/Tests/assets/i18n
```

harness 会在窗口创建前把该目录的 `*.lang` 复制到本次运行的临时 `<设置目录>/i18n`，所以测试不会读写真实用户目录。`my-chinese.lang` 故意只翻译两个键，并声明 `id: zh-Hans`。

脚本覆盖：

- 选英文内置包：打开按钮、文件菜单显示英文，来源显示 `Built-in`；
- 选简体中文内置包：上述文字变中文，`Rescan` 按钮也变中文；
- 选外部 `my-chinese`：`editor.open` / `menu.file.open` 使用文件里的 `EXT...` 文本，来源显示文件名；
- 外部文件没有的键（如 `settings.language.rescan`）仍显示内置中文，而不是英文；
- 详情标签显示缺键数；
- 再切回英文：窗口、菜单、语言页一起恢复英文。

这验证了“外部覆盖 + 同语言内置回退 + 运行时刷新”这条主路径。没有自动化覆盖的路径：文件过大、解析失败、文件被删除后的警告、保存选择跨进程恢复；这些目前靠手工验证，解析器的错误信息由 `LanguagePack` 保证带行号。

---

## 8. 已知限制

- **没有复数/性别/上下文**。英文的 `1 file` / `2 files` 这类文案只能写成不区分数量的句子，或由代码分支后使用不同键。
- **没有 RTL 镜像**。阿拉伯语、希伯来语等语言即使翻译了文字，布局方向也不会自动改变。
- **没有区域格式**。日期、数字、字节大小仍是代码里的格式，不跟随语言。
- **没有文件监听**。外部文件改动后必须点 Rescan 或重开窗口。
- **首次启动只识别精确的 `zh`**。系统返回 `zh-CN`、`zh-TW`、`zh-Hant` 时会落到英文；这是当前两个内置包下的保守选择，不是完整的 BCP-47 匹配。
- **英文包的键集是唯一基准**。没有构建期检查 `en.lang` 与 `zh-Hans.lang` 键集完全一致；漏翻会在语言页以缺键数暴露，而不是让构建失败。
- **翻译文件只支持 UTF-8**，不支持 UTF-16/UTF-32。
- **语言名不翻译**。这是有意的；如果某种语言没有通用名称，文件名后缀会作为区分。
- **语言列表最多 128 个文件、单个 1 MiB**。这是为了避免用户误放大量文件或超大文件拖慢启动；超出部分只警告。

---

## 9. 文件索引

| 文件 | 作用 |
|---|---|
| `plugin/Source/i18n/LanguagePack.h/.cpp` | 不可变语言包；解析、查找、缺失计数 |
| `plugin/Source/i18n/LanguageManager.h/.cpp` | 单例；内置加载、外部扫描、选择、回退、警告 |
| `plugin/assets/i18n/en.lang` | 参考包，应用查找的全部键 |
| `plugin/assets/i18n/zh-Hans.lang` | 内置简体中文 |
| `plugin/Source/settings/SettingsStore.h/.cpp` | `ui.language`、`ui.languageTag` 的读写与广播 |
| `plugin/Source/ui/SettingsOverlay.h/.cpp` | `LanguageSettingsPage` 与设置页 `refreshLanguage()` |
| `plugin/Source/PluginEditor.cpp` | 收到广播后刷新窗口、菜单、浮层 |
| `plugin/Source/Standalone/TrayMenu.cpp` | 每次构建托盘菜单时取当前语言 |
| `plugin/Tests/scripts/language.txt` | `app.language` 窗口测试 |
| `plugin/Tests/assets/i18n/my-chinese.lang` | 测试用部分翻译文件 |
| `plugin/CMakeLists.txt` | `VGMToyboxI18n` BinaryData 与脚本自动发现 |
