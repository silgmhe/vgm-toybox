# 键盘页的行：Review 与重构方案（2026-11-08）

> 状态：**方案待批准**。分支 `refactor/keyboard-single-source`（基于 `5c8a6cd`），本文件之前无任何代码改动。

## 一、问题

键盘页（Keyboard）与 Mixer 描述同一批声部，却各自枚举一次设备，于是两边的行数、顺序、名字、状态归属都可能不同。

最近一个多月在这一处的五次补丁全部失败，每次都是“猜对了某一种布局、弄坏了另一种”：

| # | 做法 | 结果 |
|---|---|---|
| 1 | 解码器把 `channelCount` 从 13 改成 11 | 主机换了名字表 → 重复 FM 行 + Solo 连体 |
| 2 | 快照里把 linked 设备排到头设备之后 | 顺序对了，状态仍错 |
| 3 | 用 `deviceId != headId` 判 linked | libvgm 给 SSG 与父芯片**同一个 id** → 恒 false |
| 4 | 用 `instance.device` 指针判 linked | 两者是**同一个 `PLR_DEV_INFO`** → 恒 false |
| 5 | 按累计声道数 / 按 tracks 分块分配 | 挪动了别的设备的声道号 → Sega PSG 直接无可视化 |

## 二、根因（有代码依据）

1. **两套枚举**
   - Mixer：`LibvgmSongInstance::describeSong`（`LibvgmSongInstance.cpp:757-954`）遍历 `deviceInfo`，head 设备每声道一条 `TrackInfo`；linked 子设备（`child.parentIdx == headIndex`）追加在 `linkedBase = 父的声道数` 之后，名字固定 `"SSG n"`。
   - 键盘：`LibvgmTrackPlayer::capture`（`LibvgmTrackPlayer.cpp:624-819`）遍历 `replayer->instances()`，即“每个 device 一个 instance”，用解码器自己的 `channel` 编号。
2. **`trackFor(deviceId, channel)` 是唯一的桥**（`LibvgmTrackPlayer.cpp:597-622`）：先按 `deviceId` 过滤，再按 `channelIndex == channel` 命中。
3. **YM2610 的形态正好击穿它**：head 是 13 声道（6 FM + 6 ADPCM-A + 1 ADPCM-B），linked SSG 是 3 声道且**复用父的 `deviceId` 与 `PLR_DEV_INFO`**。于是 SSG 的 `channel = 0..2` 命中了父的 FM 0..2 轨道；而 Mixer 里 SSG 的三条轨道（`channelIndex = 13..15`）永远拿不到 `trackIndex`。
4. **`TrackInfo::deviceIndex` 与 `PLR_DEV_INFO` 下标不是一回事**：`++deviceIndex` 只在 head 分支执行，linked 设备不占号（`LibvgmSongInstance.cpp:951`）。任何“按 device 序位推算”的写法都依赖这个易错事实。
5. **`trackIndex` 的语义是整个下游的契约**：`ChannelState::trackIndex` 注释写明是 `getTracks()` 的下标（`ChipState.h:104`），键盘页的 M/S 直接用它调 `setTrackMute/setTrackSolo`（`ContentSlot.cpp:297-300, 554-561`）。**一旦翻译错，静音作用到别的设备上**——这比“画错波形”更严重。

## 三、目标

**行的身份只有一处来源：`tracks`。**

- 键盘页的行集合 = `tracks` 中 `channelIndex >= 0` 的条目，**顺序与 Mixer 完全相同**；
- 每行的名字/分组名/`trackIndex` 直接取自 `TrackInfo`，不再由解码器名字表决定；
- 每行的**状态**来自“这条 track 对应的那个设备的那个声道”，映射方式不依赖任何 `deviceId`；
- 新增芯片（YM2608 的 6 FM + ADPCM-A/B + SSG、YM2203 的 SSG 等）走同一条路，**不写芯片特例**。

## 四、方案

### 4.1 让实例自报“我是谁的哪个声道”

在 `VgmReplayer::Instance`（`VgmReplayer.h:39-65`）增加两个字段，构造时填好：

- `int headIndex`：该实例所属 head 在 `deviceInfo` 里的下标，直接取 `device->parentIdx == -1 ? 自身下标 : device->parentIdx`；
- `int channelBase`：该实例的声道在**轨道空间**的起点。head 为 0；第 k 个 linked 子设备为 `父声道数 + 前面所有兄弟的声道数`（与 `describeSong` 的 `linkedBase` 同一算术，但用 `parentIdx` 排序，不靠遍历顺序）。

这样 `instance` 自己就能给出 `(headIndex, channelBase)`，`capture` 不再需要任何“猜偏移”的代码。

### 4.2 `capture` 改为遍历 `tracks`

```
for (track : *tracks)  // channelIndex >= 0
    instance = 找 instances 中 headIndex == track.deviceIndex 且 channelBase <= track.channelIndex < channelBase + 该实例声道数
    channel  = track.channelIndex - instance.channelBase
    state    = instance.decoder->channelState (channel)
    state.name / chipName / trackIndex = track.name / track.groupName / track.id
```

- 找不到实例（设备未参与播放）→ 该行给出空状态，但**行仍然存在**（Mixer 有推子）；
- `track.channelIndex < 0`（不可分层设备）→ 一行，状态取该设备声道 0，或按 `All` 表示；
- 采样账本（`ledger`）与“保留上一帧波形”的逻辑按 `state.trackIndex` 继续工作，无需改动。

### 4.3 改动面（预计 3 个文件）

| 文件 | 改动 |
|---|---|
| `backends/libvgm/VgmReplayer.h/.cpp` | `Instance` 增加 `headIndex`、`channelBase`，在 `:51-131` 的构造循环里填；其余不变 |
| `backends/libvgm/LibvgmTrackPlayer.cpp` | `capture` 的设备循环（`:655-819`）改为按 `tracks` 巡检；`trackFor` 可直接删除（不再需要翻译） |
| `chips/OpnDecoder.*` | **不改**（YM2610 仍是 13 声道；ADPCM-A 波形与速率不动） |

`playback/*.h`、`ContentSlot.cpp`、`MixerPanel.cpp`、`PlaybackEngine.cpp` 均**不需要改动**。

### 4.4 YM2610 这类复杂形态为什么不再需要特例

- linked SSG 不再是“另一个从 0 开始编号的设备”，而是“`channelBase = 13` 的实例”，它的 0..2 号声道落在轨道 13..15；
- 状态与行的关联由 `channelBase` 一次算清，且这个数来自 `parentIdx` 与各实例的 `channelCount`，与 `describeSong` 用的是同一组输入；
- 未来的 YM2608（6 FM + ADPCM-A/B + SSG 3）只是“声道数不同”，代码路径不变。

## 五、验收（缺一不可）

1. `07 Stage 2`（YM2610 + SSG）：16 行，名字/顺序与 Mixer 逐一对应；`SSG 1..3` 是**方波**、`FM 1..6` 无方波、ADPCM-A 1..6 有采样波形、ADPCM-B 空；
2. 任意一行 Solo/Mute 只影响它自己（含 SSG）；
3. 其它芯片回归：`Segment PCM`（16 声道）、`YMZ280B`、`C140`、`NES`、`YM2612` 的行数与之前一致；
4. `04 Stage 1` 同验；
5. 加载时间保持 250–350 ms（不新增每帧扫描、不新增解码）；
6. `46/46` + 芯片断言 PASS。

## 六、风险与回退

- 风险：`tracks` 与 `instances` 对同一设备给出的声道数不一致时，落入“空状态行”（可见、可静音、但无波形），**不会串到别的行**；
- 回退：全部改动集中在 3 个文件的 4 处，`git revert` 单次即可；分支独立，不影响 `feature/libvgm-visualisation`。

## 七、做完之后

1. 删除此前留下的三处补丁（排序、`SSG` 前缀、列出空置声道）；
2. 文档 §9.17 补一节“键盘页的行与 Mixer 同源”，并记录本 Review；
3. 再由你决定是否合并。
