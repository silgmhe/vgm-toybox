# 维护策略：两套前端与上游依赖

> **状态**：**提案**（2026-10-17）—— 由 review 的 **M-6 / M-7** 触发（`docs/review/2026-10-16-architecture-and-code-review.md` ✓）。
> 这两条本来就只需要"一句话决策" ✓，下面是我建议的那句话 ✓；要改就直接改这份文档 ✓（它比代码里的注释更容易找到 ✓）。

## 1. `vgmplayer/` —— 冻结为遗留前端

**决定**：`vgmplayer/`（6451 行、13 文件 ✓，与 `plugin/` **不共享代码** ✗）**冻结** ✓。

- 只接受**正确性与安全性**修复 ✓（libvgm 同步带来的 API 变化 ✓、崩溃与越界修复 ✓）；
- **新功能一律只进 `plugin/`** ✓（VST3 + standalone ✓）；
- 将来若要统一 ✓：第一步共享 `playback/BackendRegistry` 与加载路径 ✓（让它走同一套后端注册与 `ISongInstance` ✓），第二步才是界面 ✓ —— 不设时间表 ✓，但**每次要动 `vgmplayer` 的播放逻辑时，先评估是否该切成共享路径** ✓。

**为什么这么定**：两套前端并存的实际成本是"同一个 libvgm 用法改两次" ✓；而把 6451 行整体迁移的收益不确定 ✗。"冻结 + 单向收敛（新功能只进一边）"是成本最低的中间态 ✓。

## 2. libvgm（以及其它 vendored 库）

**决定**：跟随上游的**重要修复与版本 tag** ✓，**每季度评估一次** ✓。

- 评估什么 ✓：上游 release notes 里是否有**崩溃 / 越界 / 格式兼容**类修复 ✓ —— 这几类直接等于"某些歌能不能打开" ✓；
- 每次同步必须过**四个门禁** ✓：`vgm_mixer_test`（**−1000.00 dB 逐样本** ✓）、`vgm_core_test` ✓、`vgm_ui_test` ✓、`vgm_app_test` 的脚本 ✓，外加 standalone 冒烟 ✓；
- **本地补丁**必须记录在 `libs/libvgm/PATCHES.md` ✓（**目前：无本地补丁** ✓ —— 一旦新增，同一提交里补上这个文件 ✓）；
- 同步**单独成一个提交** ✓（不与功能混合 ✓），提交信息写明上游版本与门禁结果 ✓；
- **本地改动记录** ✓（2026-10-17 ✓）：vendored 树**只保留 CMake** 这一套构建系统 ✓ —— 移除了上游的 `*.vcxproj`（含 `.filters` ✓）、`libVgmTest.sln` ✓、`Makefile` ✓、`Dockerfile` ✓、`Compiling.txt` ✓，以及**嵌套的 `.github/`**（workflow + action ✓ —— GitHub 只执行**仓库根目录**的 workflow ✗，子目录里的永不运行 ✓），和它们专用的预编译 zlib 库 `libs/libvgm/libs/lib/` ✓（MSVC 6 / 2010 时代的产物 ✗，`zlib.txt` 里自己写着 ✓，而 CMake **零引用** ✓ —— 我们的 zlib 由 CPM 从源码构建 ✓）。**保留的**都有引用 ✓：`libs/libvgm/libs/iconv/`（Windows 构建要用 ✓，`audio/CMakeLists.txt:167-176` 引用 ✓）、`include_mingw` / `include_vc` / `include_vc6` ✓、`cmake_modules/` ✓。**同步上游时这些文件会被带回来** ✓ → 按同样原则**再删一次** ✓，并确认 iconv 与那几个 include 目录仍在 ✓。

**为什么**：`libs/` 是 vendored 源码（无 submodule ✓）✓ —— "什么时候同步、怎么验证"不写下来，每次都要临场决定 ✓；而 libvgm 是**唯一解析外部输入**的地方 ✓，它的正确性直接就是"能不能放歌" ✓。

## 3. 有意不做的事

- **不加 `.clang-format`** ✗ —— 风格已经稳定 ✓，全量格式化会制造一次巨大的无关 diff ✓，而收益是"以后不用争格式" ✓（目前没有第二个作者在争 ✓）。要加时：先在**单独的提交**里跑一次全量格式化 ✓，不要混进功能改动 ✓。
- **不做 vendored 库的自动更新机器人** ✗ —— 同步需要跑门禁并听一遍 ✓，不适合无人介入 ✓。

## 4. 许可（Licence）

**决定**：本项目（`plugin/`，即 VST3 + standalone）以 **AGPL-3.0-or-later** 发布 ✓ —— `LICENSE` 放 GNU 原文（**逐字未改** ✓），README 写明适用范围、以及"为什么不是 MIT" ✓。

- **为什么**：**JUCE 是"AGPLv3 或商业授权"二选一** ✓（`juce/d6e0/LICENSE.md:7` ✓）→ 不买授权就等于选定 AGPLv3 ✓；其余依赖都与 AGPLv3 兼容 ✓：inih 是 BSD-3-Clause ✓、wingetopt 是 ISC 风格 ✓、**zlib 是 Zlib 许可** ✓（从源码构建并随二进制分发 ✓，README 归属表已列 ✓）、`libs/libvgm/emu/cores/` 的模拟核心逐文件声明 BSD-3-Clause ✓。
- **什么会改变它**：购买 JUCE 商业/个人授权 ✓ —— 那时才可以把本项目换成宽松协议（MIT / Apache-2.0 ✓）。**在没有 JUCE 授权的情况下不要改协议** ✗。
- **libvgm**：**不附带许可证文件** ✓；作者于 **2026-10-17 在论坛答复"保持开源即可"** ✓ —— 该答复记录在 README ✓。**每次同步上游前先确认它仍然有效** ✓，并在有机会时**请作者补一个许可证文件** ✓（文件比论坛答复更稳固 ✓）。
- **不可重新授权的部分**：`vgmplayer/` 是 Valley Bell 的 VGMPlay 的衍生 ✓ → 保持上游条款 ✓，我们不为它加或换协议 ✓。
- **纪律**：**新依赖没有许可证文件就不引入** ✓（若上游确实没有 ✓，与作者确认后把结论写进 README 表 ✓，而不是留白 ✓）；`LICENSE` 与各依赖自带的许可文件**永不修改** ✓ —— 我们只写自己的文件 ✓。
- **SPDX 头**：可选 ✓ —— 新文件可以带一行 `// SPDX-License-Identifier: AGPL-3.0-or-later` ✓；旧文件不必为它专门返工 ✓（碰到再补 ✓）。

## 5. 插件格式（导出哪些、为什么）

**当前发布**：**VST3** 与 **Standalone** ✓（`juce_add_plugin(... FORMATS VST3 Standalone)` ✓）。

| 格式 | 状态 | 依据（2026-10-17 实测 ✓） |
|---|---|---|
| VST3 / Standalone | ✅ 随构建产出 ✓ | 现有配置 ✓ |
| **VST2** | ⏸ **暂缓**（2026-10-17）—— 适配器写了一半，**布局已被证明 ✓**，卡在 12 个 ABI 常量 ✗ | JUCE 支持它 ✓，但格式名是 **`VST`** ✗（不是 `VST2` ✓），且**必须有 SDK** —— `JUCEUtils.cmake:1510` 明写：没有 `juce_set_vst2_sdk_path()` 就 `FATAL_ERROR` ✓；Steinberg 已停止分发且不允许再分发 ✗。净室重写版 [`Xaymar/vst2sdk`](https://github.com/Xaymar/vst2sdk)（**BSD-3** ✓）**结构体 / 操作码 / 类别 / 标志位齐全 ✓**，且与经典布局**逐字段一致** ✓ —— 已用 **23 条 `static_assert`**（`offsetof` + `sizeof` 对 SDK 的 `vst_effect_t` ✓）证明过 ✓；但**缺 12 项纯 ABI 常量** ✗：`VstTimeInfo` 结构 ✓、7 个 `kVst*Valid` 位值 ✓、4 个 `kVstMax*Len` 缓冲区长度 ✓、以及 `effFlagsNoSoundInStop` 是否与 `SUPPORTS_DOUBLE` **共用 bit 12** 的确认 ✗。这些是**数值** ✗，必须来自权威文本 ✓，**不许猜** ✓（作者 README 原话："structured data for which I can only guess" ✓）。**若将来重启** ✓：① 拿到权威来源（旧版 SDK 参考 / VST 2.4 接口文档 / 另一份带经典名与数值的宽松许可头文件 ✓）→ 补齐这 12 项 ✓；② `-DTOYBOX_BUILD_VST2=ON` 打开开关并先只验证能编译 ✓；③ 写**自测宿主**（`LoadLibrary` + `VSTPluginMain` + 遍历 `AEffect` + 跑 `processReplacing` ✓）作运行时门禁 ✓；④ 通过后再考虑默认开启 ✓。当时的两个文件 —— `libs/vst2sdk/` 与其中为 JUCE 写的 `pluginterfaces/vst2.x/aeffect.h`（经典名映射 + static_assert 证明 ✓）—— **已删除**（2026-10-17 ✓），从未入 git ✓。上面的记录因此是唯一留存的依据 ✓：**布局一致性已被证明 ✓**，**缺的是那 12 个数值** ✓ —— 重启时按四步重建即可 ✓。 |
| **CLAP** | ⏸ **暂缓**（2026-10-17 ✓） | JUCE **无** CLAP 格式 ✗（`FORMATS` 只认 `AAX AU AUv3 LV2 Standalone Unity VST3` + 条件性的 `VST` ✓），只能靠 [`clap-juce-extensions`](https://github.com/free-audio/clap-juce-extensions) ✓（**MIT** ✓，CPM 可引入 ✓，需连同子模块 `clap-libs/clap` + `clap-libs/clap-helpers` 一起取 ✓）。**但当前不兼容 JUCE 8.0.9** ✗：`0.26.0` 与 `main` **都**仍实现已被 JUCE 8 移除的 `AudioPlayHead::getCurrentPosition()` ✗（`error C3668` + 抽象类无法实例化 ✗）。**上游已有修复** ✓：`645ed2f` *"Compatibility with JUCE 8.0.11" (#169)* ✓，位于 `next` / `juce-develop` 分支 ✓。**要重启时**：把 `CLAP_JUCE_EXTENSIONS_TAG` 固定到**含该修复的提交**（✗ 不要钉分支名 ✓）→ 先只用 `cmake` 配置 + `--target VGMToybox_CLAP` 验证能编译 ✓ → 再把 `clap-validator` 加进 CI ✓（纯文本 + 退出码 ✓，正合我们的测试方式 ✓）。 |

**给 CPM 引入第三方格式包装器的两个坑（都踩过了 ✓）**：
1. 第三方包装器目标**不继承**插件目标的属性 ✗ → C++ 标准必须靠 **`CMAKE_CXX_STANDARD` 这类目录作用域设置**传下去 ✓，否则 JUCE 会以 `#error JUCE requires C++17 or later` 停下 ✗；
2. `build.ps1` 的 `Invoke-Native` 曾把原生命令写到 **stderr** 的进度输出当成致命错误 ✗（git 克隆时必现 ✓，与格式无关 ✓）→ **已修** ✓。

## 6. 版本号与发布

**决定**：**语义化版本 `MAJOR.MINOR.PATCH`**（semver ✓），当前从 **`0.0.1`** 起 ✓（0.x 表示接口仍在收敛 ✓ —— 对插件而言"接口"= 宿主可见的东西：插件码、参数布局、保存的状态 ✓）。

**唯一来源** ✓：顶层 `CMakeLists.txt` 的 `project(vgmtoybox VERSION …)` ✓。**其它地方一律派生，不许再写一遍** ✓：

| 派生到哪 | 怎么来的 |
|---|---|
| `Help > About` 显示的版本 ✓ | JUCE 把 `PROJECT_VERSION` 变成 `JucePlugin_VersionString` ✓（`JUCEUtils.cmake:1558` ✓，无需在 `juce_add_plugin` 里再写 `VERSION` ✓） |
| VST3 的 `moduleinfo.json` ✓ | 同上 ✓（构建产物里可直接核对 ✓） |
| Windows 文件属性的 `FileVersion` ✓ | 同上 ✓ —— **注意**：改版本后要删一次生成的 `.rc` ✗（`build/plugin/VGMToybox_artefacts/JuceLibraryCode/VGMToybox_resources.rc` ✓），否则 exe 里还是旧值 ✗ |
| 发布流程的 tag 校验 ✓ | `release.yml` 从同一行解析 ✓，**tag 不匹配就拒绝发布** ✓✓ |

**发版流程** ✓：

```bash
# 1) 在 CMakeLists.txt 里把 VERSION 改成新版本（唯一一处 ✓）
# 2) 提交并推送 master（会跑三平台构建与测试 ✓）
git tag v0.0.2          # tag 必须等于 VERSION，前缀 v ✓
git push origin v0.0.2  # → 三平台构建 + 测试 + 打包 + 自动发布 Release ✓
```

**什么时候升哪一位** ✓（对插件来说，"不兼容"主要是宿主侧的 ✓）：

| 位 | 何时升 |
|---|---|
| **MAJOR** | 宿主可见的不兼容改动 ✓：改 `PLUGIN_CODE`/`PLUGIN_MANUFACTURER_CODE` ✓、参数集合或保存状态的格式破坏性变化 ✓（会让已有工程里的实例失效或参数错位 ✓） |
| **MINOR** | 新功能 ✓、新格式 ✓、新 UI 页面 ✓（旧工程照常打开 ✓） |
| **PATCH** | 修 bug ✓、文档 ✓、不影响宿主的内部重构 ✓ |

**纪律** ✓：`0.0.1` 之前若已有对外产物，**不要**回填版本号 ✗ —— 直接从 `0.0.1` 起 ✓，并在 release 说明里注明它与之前那些临时构建无对应关系 ✓（版本号一旦对外就是承诺 ✓）。
