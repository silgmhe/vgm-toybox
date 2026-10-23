# FTM 对标：现状与剩余差异

日期：2026-10-24　分支：`feature/multiformat-core`

## 1. 这一轮建立了什么

对标的意思不是"我们的测试通过"，而是**拿 FamiTracker 自己的解析结果比**。为此搭了一条四段链路：

```
Dn-FamiTracker.exe <module.ftm> -export <out.txt>     ← 权威，FT 自己的文档转储
        ↓
tools/ftm_from_text.py         读文本 → 音符事件流
tools/ftm_reference.py         直接读二进制 → 同一格式（独立实现）
        ↓
build/plugin/vgm_chip_test.exe --ftm-dump <module.ftm>  ← 我们的 loader
        ↓
tools/ftm_regression.ps1       三方对照，逐音符 diff
```

**判定规则**（很重要，避免误改产品）：

| 现象 | 结论 |
|---|---|
| 文本 ≈ loader | 通过 |
| 文本 ≈ 二进制 ≠ loader | **产品 bug**，可以改 |
| 文本 ≠ 二进制 | **工具 bug**，先别改产品 |

## 2. 构建参考实现

`tools/build_dnft.ps1` 记录了三处非默认事实：

| 关卡 | 解法 |
|---|---|
| 工具集要 v143 | 本机是 14.51 → `PlatformToolset=v145` |
| MFC | 需安装 VS 的 C++ MFC 组件 |
| Windows SDK | 不在默认目录，`KitsRoot10` 指向 `D:\Windows Kits\10` |
| 驱动头 `drv_*.h` | 需 `ca65`/`ld65`（cc65）：在 WSL 里 `make -j4 ca65 ld65`，再跑 `build_engine.py` 生成 |
| 警告即错误 | 现代编译器对 `gsl::span` 有新警告，需关闭（`.vcxproj` 已改，附 `.orig`） |

导出命令行（我最初误判为"只能 GUI"）：

```
Dn-FamiTracker.exe <module>.ftm -export <out.txt>
```

## 3. 当前结果

```
PASS  fod_hopeless                    2786
PASS  fod_pancake                     3826
PASS  snowburnb                       2385
PASS  Sunrise_to_Sunset               3276
FAIL  15_coffeezoneb                  文本 1530 / loader 759
FAIL  TMNT_Tournament_Fighters        1356 / 1356，内容 1263 处不同
FAIL  VietnameseMuskrats               969 / 970，差 1 个事件
```

`ctest` 34/34。

### 已修的真实缺陷（都由对标发现）

| 缺陷 | 影响 |
|---|---|
| 音符字节 13/14（release/cut）被当成音高 | 7 首共 429 个假音符 |
| `PATTERNS` 遇到 `track != 0` 直接 `break` | 多轨道模块丢掉后续图案 |
| 行号超出图案长度时越界索引 | 静默读越界（由独立读取器崩溃暴露） |

### 工具的缺陷（同样影响结论，已修）

| 缺陷 | 影响 |
|---|---|
| 转换器 Fxx 未跨帧延续 | 6,624/6,637 全错 |
| 转换器音符字母→半音映射错（E 应为 4） | 整片偏移 |
| 转换器只在被比较的通道上扫描结构效果 | 漏掉循环跳转，计数翻倍 |
| 二进制读取器把 release/cut 当音符 | 与产品同源的错误 |

## 4. 剩余三个差异（定位到单点，未修）

### 4.1 `VietnameseMuskrats` — 1 个重复事件

```
只在 loader：(pitch=52, start=1012, end=1032) 多 1 次
只在文本：  无
其余 968 个音符完全一致
```

已排除：重复行号、重复图案记录、release/cut、通道错位。
现场：`frame=5, pattern=0x06, row=0`，该行带 `F04`（变速）。

**首选怀疑**：`trackOf` 通道映射。该模块 8 个通道，而 loader 只为"顺序表非空"的通道建轨；
若某通道顺序表为空但图案里有音符，`trackOf` 与实际发射的 track 可能不一致，导致同一事件计两次。

### 4.2 `15_coffeezoneb` — 两个工具互相不一致

```
文本 1530 / 二进制 759
```

两者共 150 条图案记录、57 帧、11 通道，均无重复。差异接近 2 倍，形状与"循环截断规则不同"一致：

- 产品与二进制读取器都在**第一条向后跳**处停止（等价于 VGM 的 `loopCount=1`：前奏 + 一圈）；
- 文本导出是**整份文档**（`TextExport::ExportFile` 直接转储），不含播放语义。

已知该文件里 `B`（跳转）只有 1 个、`D`（跳过）15 个。
**下一步**：给两个工具加帧级 trace，确认是谁没有在跳转处停下。

### 4.3 `TMNT` — 计数一致、内容不同

```
1363 vs 1356（相等），但 1263 处事件不同
```

疑似扩展芯片通道（VRC6/N163）的文本单元格与我们的读法不同，尚未定位。

## 5. 怎么跑

```powershell
# 只有本地才跑全量；CI 跑 ctest 即可
tools\ftm_regression.ps1              # 需要 build\ftm_exports\*.txt
tools\ftm_regression.ps1 -Export      # 需要时用 FT 重新生成导出
```

基准导出（`build/ftm_exports`，4.7 MB）不入库；FT 只在重新生成基准时使用。

## 6. 结论

- **音符层的链路已经打通**，7 首里 4 首逐音符一致；
- 剩下 3 首各是一个单点问题，其中 `Vietnamese` 的"1 个重复事件"最适合先修（现场已缩到 `frame=5, pattern=06, row=0`）；
- 所有差异都有可复现的比对脚本，**不依赖人工判断**。
