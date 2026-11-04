# OKIM6295：四道坑，和一个不断变长短的采样

> 第一颗真正做完的采样芯片——相对搁置的 RF5C164 而言（见
> `docs/memo/2026-11-04-the-sample-that-was-never-the-one-playing.md`）。
> 两次交付都"看起来对"，两次都被耳朵否掉：先是一半长度，再是 1.6 倍速度。
> 两次的根因都不是芯片知识不足，而是**同一段算术在两个地方各写了一遍**。

## 1. 现象

`test_file/OKIM6295/02 Repeated Tragedy.vgz`，四声部 ADPCM。

| 轮次 | 用户反馈 | 当时的读数 |
|---|---|---|
| 一 | 菜单导出的底鼓被拆成两半，拖拽是完整的 | 菜单 `okim_001_1778` / `okim_002_1858`；行 3714 点 |
| 二 | 音高比渲染快约 1.6 倍 | 菜单 12396.7 Hz；设备自报 7747.0 Hz |

两次都不是"差一点"，而是**成比例的系统性偏差**——这正是"两处各写一份算术"的典型症状。

## 2. 四个坑

### 坑一：地址的单位（字节 还是 nibble）

ROM 采样表每条 8 字节，前六个是 start 和 stop（各三字节，大端）。

```c
// okim6295.c:537
voice->count = 2 * (stop - start + 1);   // 单位是 nibble
voice->base_offset = start;              // 而 start 是字节偏移
```

**start/stop 是字节地址，样本是它的两倍个 nibble。** 我一开始按 nibble 解释，于是每个样本
都被导出一半——`okim_001_1778` 正是 3554 的一半。

**怎么定案的**：不是读文档，是**行**。行解码的是 `2*(stop-start+1)` 个 nibble，那才是用户听到
并且拖出去的录音。**导出可以有自己的读法，但不能和行不同。**

### 坑二：×2 是我自己加的

我曾把"一字节两个电平"理解成"速率要翻倍"。错的：

```c
INIT_DEVINF(retDevInf, ..., info->master_clock / divisor, ...)   // 设备速率
nibble = read_byte(base_offset + sample / 2)                     // 每样本推进一个 nibble
```

**一个输出样本就是一个 nibble**，速率本身已经按 nibble 计。再乘 2 就是把内容单位（电平/字节）
和速率单位（样本/秒）混在一起。

### 坑三：分频比是板上的跳线，文件里没有

`divisor` 是 132（pin 7 高）或 165（低）。`02 Repeated Tragedy` 从不写寄存器 `0x0C`，
所以文件里没有任何线索；设备启动时按跳线选了 **132**，而代码默认 **165**。

```
12396.7 / 7747.0 = 1.6002 = 2 × (165/132)
```

**坑二给 2，坑三给 1.25，乘起来正好是用户听到的 1.6。**

### 坑四：拖拽和菜单是两条独立生产线

- **拖拽**：`writeChannelShape` → `writeSampleWav` → 行的 `state.wave`（解码器）
- **菜单**：`exportSamples` → `writeSamples` → 收集器（`collectOkim6295Samples`）

两边各自把"这一段 ROM 怎么变成一个采样"算了一遍。**只要算两遍，就一定会分叉**，而且
两边单独看都像对：一个有波形、一个有文件，没有任何断言会失败。

## 3. 实现步骤（下一颗芯片照这个顺序）

| 步 | 做什么 | 判断标准 |
|---|---|---|
| 1 | 确认素材真的会播 | 探针 `-4`：行有 `active` 且 `wave` 非空。**没写启动寄存器的曲子直接换**（K053260 的 KADPCM 就是白查一轮） |
| 2 | 找到 ROM 块类型 | `kRequirements.memoryType`，与解码器同一处来源 |
| 3 | 摸清样本表 | 条目位置、字段顺序、**单位**。单位拿不准就用行来定，不要看文档猜 |
| 4 | 解码器填 `wave` + `sampleRate` | 缓存键按样本（不是按通道），两个声部共用一个鼓时只解码一次 |
| 5 | **速率取设备自报** | `PLR_DEV_INFO::smplRate` → `ChipConfig::deviceRateHz`。自己算分频一定会在跳线上翻车 |
| 6 | 收集器走寄存器流 | 找 key-on 边沿，按表取字节，键按样本号；与解码器**同一套算术** |
| 7 | 压缩格式进 `SampleFormat` | 编解码器放进 `chips/<Chip>Codec.h`，解码器与导出层共用一份 |
| 8 | 断言，不靠眼睛 | 电平数、范围、非平线、速率非零 |
| 9 | **一致性验收** | 探针 `-4` 的行 vs 菜单导出：点数与速率必须逐项相等 |

第 9 步是这一颗能收敛的原因：它把"两条生产线"变成一个可比较的量。

## 4. 验收数据（最终）

```
行  (Ch 1)   wave = 3714 points @ 7747.0 Hz
菜单         okim_002_3714      @ 7747.0 Hz      ← 逐点、逐赫兹相同
```

7747.0 = 设备自报 = 渲染实际播放的速率，不是我们推算的。

三首曲子：`01 Credit` 7 个样本、`02 Select` 13 个、`02 Repeated Tragedy` 5 个。
其他芯片未动：RF5C164 6、QSound 2、C352 1。

## 5. 复用清单（给下一颗）

写新芯片前先问五个问题，每个都有对应的坑：

1. **素材里的驱动真的会启动它吗？** —— 否，换曲子
2. **样本表在哪、单位是什么？** —— 拿不准就问行
3. **速率从哪来？** —— 设备自报优先，绝不自己算分频
4. **两条路径的算术是同一份吗？** —— 否则必然分叉
5. **验收怎么比？** —— `-4` 行 vs 菜单，点数 + 速率

## 6. 涉及的文件

| 文件 | 角色 |
|---|---|
| `plugin/Source/chips/Okim6295Codec.h` | 步长表与 nibble 解码，解码器与导出层共用 |
| `plugin/Source/chips/Okim6295Decoder.cpp` | 行：`state.wave`、`state.sampleRate`、按样本缓存 |
| `plugin/Source/backends/libvgm/Okim6295Samples.cpp` | 菜单导出：命令端口握手 + 采样表 |
| `plugin/Source/backends/libvgm/SampleCollector.cpp` | `SampleFormat::msm6295` 分支 |
| `plugin/Source/chips/ChipTypes.h` | `ChipConfig::deviceRateHz`（设备速率进解码器的通道） |
| `plugin/Tests/RenderProbe.cpp` | `-4` 模式：播放中捕获每行的波形与速率 |
