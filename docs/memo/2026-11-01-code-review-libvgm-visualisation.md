# Code review：`feature/libvgm-visualisation`

> 按 `.codebuddy/rules/code-quality.md` 逐条体检。范围：`master..HEAD`，38 个提交，46 文件，
> +4998 / −1095。结论先说：**分层是干净的，问题集中在"填了一半的字段"和"每颗芯片各写一遍"。**

## 一、按规则的体检

| 条款 | 判定 | 依据 |
|---|---|---|
| §1 名字说明意图 | **通过** | `hitCount` / `cycleBytes` / `pendingHits` / `sampleOffset` 都说明用途；无 `k48` 式命名 |
| §2 注释写为什么 | **通过** | 每处绕路都有理由：`$4012` 八位、`$4013` 十六加一、`hitCount` 不复用 `keyOns` |
| §3 高内聚低耦合 | **一处违规** | 见问题 3：C140 的寻址算法有两份 |
| §3 跨模块只传值 | **通过** | `playback::ChannelState` 是纯值；`wave` 用 `shared_ptr` 而非传 panel |
| §4 加东西只改一处 | **未达标** | 见问题 4：每颗采样芯片要复制一整套"扫描→解码→去重" |
| §5 不留技术债 | **三处** | 见问题 1、2、5 |
| §6 可测试 | **通过** | 新增 34 条断言；`channelFromChip` / `collectC140Samples` / `decimateForDisplay` 都能直接调用 |
| §7 颜色走主题 | **三处既有违规**，非本支引入 | 见问题 6 |
| §7.5 断言 vs 截图 | **通过** | 核心逻辑有断言，绘制靠手工验；本轮两次 bug 分别是这两条腿发现的 |
| §8 提交即最小单元 | **通过** | 38 个提交各自一件事；诊断代码单独进出 |
| §9 重构不夹带行为 | **通过** | `5897160`（提路由）、`407c0b1`（搬装配）都是纯搬移，`46/46` 前后一致 |
| §10 掩码改动跨型号验证 | **部分** | `$4012` 的 8 位只在 NES 上验过；C140/C219 已对齐，C352 未做 |
| §11 所有权 / 谁填字段 | **两次踩中** | 见问题 7 |
| §12 位宽去实现里读 | **两次踩中** | `$4012` 凭记忆掩 6 位；C352 的 `0xE1` 猜了命令字节 |

## 二、问题清单

### 1. `cycleBytes` 是一个没人读的字段（§5）

```
ChipTypes.h:190        字段声明
ChipState.h:60         字段声明
C140Decoder.cpp:484    填
ChannelFromChip.cpp:105 转交
KeyboardRowPaint.cpp   ← 不读
```

显示在 `f10171f` 改回"整段填充"之后，不再需要"一个周期多长"。**字段留了下来，注释还写着"显示用它裁一个周期"，是错的。**

**建议**：删三处声明 + 一处填写 + 一处转交，并在 `C352Decoder` 里同样检查（它的 `cycleBytes` 是另一个同名的局部变量，需区分）。

### 2. `C140Samples::assembleRom` 与 `memory::rom` 重复（§3 §5）

```cpp
// C140Samples.cpp:48 —— 私有副本
std::vector<uint8_t> assembleRom (const midi::SongIr& ir)

// ChipMemoryAssembly.cpp —— 共用版本
std::vector<uint8_t> memory::rom (const midi::SongIr&, uint8_t type, std::size_t maxBytes, bool clip)
```

两份都在"按 type 拼 ROM 块"，都是 20 行。**`memory::rom` 是 `407c0b1` 专门搬出来给两边共用的，`b38f14e` 又写了一份。**

**建议**：`collectC140Samples` 直接调 `memory::rom(ir, 0x8D, 16u << 20, false)`。

### 3. C140 的寻址算法两份（§3）

```cpp
// C140Decoder.cpp —— 私有
uint32_t sampleOffset (uint32_t address, uint8_t bank) const

// C140Samples.cpp:29 —— 复制
uint32_t romOffset (uint32_t address, uint8_t bank, uint8_t banking)
```

注释里写了"故意复制，因为解码器把它保持私有"，**但那是说明，不是理由**：两份算法漂移的后果是"导出的字节和行里唱的字节不同"，而这正是 `06d4eed` 那一类 bug 的形状。

**建议**：提到 `chips/C140Address.h`（或 `ChipMemoryAssembly`）里，两边都用。**代价小，收益是消除一个静默分叉点。**

### 4. 每颗采样芯片都要复制一套收集流程（§4）

`NesDpcmBank.cpp` 和 `C140Samples.cpp` 的结构几乎相同：

```
遍历 writes → 按寄存器分组 → 记录地址/长度 → key-on 时切字节 → 去重 → 解码 → 命名
```

**差的是"地址怎么算"和"字节在哪类块里"。** 剩下 9 颗采样芯片、8 颗波表芯片会各写一遍第 6 份、第 7 份……

**建议**：抽一个 `SampleCollector`（或 `collectSamples(ir, ChipKind)` + 每芯片一个 `SampleLocator` 回调），把"遍历/去重/命名"共用，每颗芯片只提供"从寄存器算地址"和"块类型"。**这是本支最大的一笔结构性债**，越晚还越贵。

### 5. `sampleRate` / `wave` 只有两颗芯片填（§11 后半）

```
填 state.wave：  VsuDecoder（局部用途）、C140Decoder、NesApuDecoder（经 LibvgmTrackPlayer）
填 state.sampleRate：NesApuDecoder、C140Decoder
其余 19 颗：空
```

**不是缺陷**（逐步接），但**要有清单**，否则"拖不出来"会被当成 bug 反复排查。

**建议**：在 `docs/desgin/libvgm-visualisation.md` 的 §9 维护一张表，列出每颗芯片的 `wave` / `sampleRate` 状态。

### 6. 三处遮罩用 `juce::Colours::black`（§7）

```
AboutOverlay.h:96          g.fillAll (juce::Colours::black.withAlpha (0.35f));
ExportOptionsOverlay.h:134 同上
ExportReportOverlay.h:73   同上
```

**不是本支引入**，但规则明确点名这一类。遮罩黑属于"没有设计决定"的颜色，应进 `Palette` 或至少具名成 `theme::scrim()`。

**建议**：单独一个提交处理，不夹带在功能里。

### 7. 两次踩中 §11 的"顺序依赖"

- `71c46ec`：`songIr` 在样本收集之后才赋值，收集器读到 null，导出报"没有采样"
- 同一个提交里：`hasC140` 问的是 `deviceInfo`（`DEVID_*`），而收集器遍历的是 `SongIr::chips`（`ChipKind`）——**两个编号系统，问错了一个**

**这两条已修，但形状值得记下来**：`空列表` 对一首歌是**合理答案**，所以守卫写错时不会报错，只会安静地返回"没有"。

## 三、已解决的坑（索引）

细节在各自的 memo 里，这里只列一行：

| 坑 | 记录 |
|---|---|
| 采样的"速率"不是"频率"（七颗芯片共用的 `hz` 少一次除法） | `2026-11-01-the-rate-that-was-not-a-frequency.md` |
| 静默丢弃、两条读取路径、位宽凭记忆、`period==0`、`released` 误用、`keyOns` 不可复用、采样 vs 事件 | `2026-11-01-visualisation-traps.md` |
| 本支的样本导出与显示不一致 | 本文问题 1-4、7 |

## 四、架构评价

**干净的部分**：

```
chips/<Chip>Decoder        每颗芯片一个解码器，只吐 ChannelState
        ↓
playback::channelFromChip  唯一的转换层：芯片状态 → 可绘制状态
        ↓
ui/KeyboardRowPaint        只画收到的值，不知道芯片名
```

**三条约束都守住了**：UI 里没有芯片名、解码器不碰 JUCE、转换层是唯一映射点。

**在变坏的部分**：

`LibvgmTrackPlayer` 现在持有 5 个 map：

```
dpcmNotes      sampleId → 音符
dpcmSamples    sampleId → 波形缓存
pendingHits    通道 → 未展示的命中
lastHits       通道 → 上次的计数
（还有 replayIr / replayDevices / replayer）
```

**它是一个"回放 + 样本表 + 命中累积 + 波形缓存"的合并类。** 目前还能读，但它已经同时承担四件事——**下一次再加功能时，应该先拆**（至少把"样本表 + 缓存"移出去成 `DpcmLookup`）。

## 五、建议的处理顺序

| 优先级 | 事项 | 代价 |
|---|---|---|
| **立刻** | 删 `cycleBytes`（问题 1） | 10 分钟 |
| **立刻** | `assembleRom` 改用 `memory::rom`（问题 2） | 10 分钟 |
| **近期** | 寻址算法提到共用处（问题 3） | 半小时 |
| **近期** | §9 的芯片状态表（问题 5） | 半小时 |
| **下一个芯片之前** | 抽 `SampleCollector`（问题 4） | 半天，越晚越贵 |
| **独立提交** | 遮罩黑进主题（问题 6） | 一刻钟 |
| **计划中** | 拆 `LibvgmTrackPlayer`（问题 8） | 半天 |

**问题 4 是唯一一件"现在不做、以后会变成六份重复"的事。** 其余要么十分钟，要么可以排。

## 六、`46/46` 覆盖了什么、没覆盖什么

**覆盖**：通道类型映射、FM 打包、占空比四档、DPCM 的速率/波形/样本身份、C140 的寻址/波形/样本收集、噪声零索引、命中的跨帧保留、列抽稀的 min/max。

**没覆盖**：

- **任何绘制**（`KeyboardRowPaint` 的两个 bug 都是靠眼睛发现的）
- **`LibvgmSongInstance::prepare` 的编排**（两次顺序 bug 都在这里，没有任何断言）
- **多芯片同曲**（所有验证都是单芯片）

**第二条最值得补**：`prepare` 是"解析 → 收集 → 装配 → 连播放器"的编排，**它有顺序依赖，而顺序依赖最容易被下一次改动打破**。一个 harness 断言（"给一个 NES+C140 的合成 IR，`samples()` 非空"）就能守住。
