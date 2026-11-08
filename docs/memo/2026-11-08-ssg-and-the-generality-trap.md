# YM2610 的 SSG 与“通用解”的陷阱：重构复盘（2026-11-08）

> 关联：`docs/memo/2026-11-08-a-row-needs-an-identity.md`（行身份的根因）、
> 设计文档 §9.18。相关文件：`LibvgmSongInstance.cpp`（`describeSong`）、
> `LibvgmTrackPlayer.cpp`（`captureState`、`trackForVoice`）、`playback/SongBackend.h`。

## 一、整件事的形状

键盘页与 Mixer 描述同一批声部，却各自枚举一次设备。重构要做的只有一句话：**让行与状态都来自 `tracks`**。真正花掉时间的不是这句话，而是“用什么把一行和它的状态对上”。

## 二、五个阶段与各自的失败点

| 阶段 | 做法 | 结局 |
|---|---|---|
| 1 | 解码器把 YM2610 的 `channelCount` 从 13 改到 11 | 主机随之换名字表：重复 FM 行、Solo 连体 |
| 2 | 快照按“主设备在前、联动词道组在后”排序 | 顺序对、状态错 |
| 3 | 用 `deviceId` / `device` 指针区分主芯片与 SSG | 三重相同，判断恒真/恒假 |
| 4 | 用累计声道数、块基准（`channelBases`）推位置 | 两种枚举顺序不同 → 整体错位 4 行（PSG 显示 YM2612 的音符） |
| 5 | 行身份写进 `TrackInfo`：`sourceDeviceId` + `deviceChannel` + `sourceDeviceType` | 通过 |

## 三、YM2610 的三个坑（按咬人顺序）

### 1. 链接设备的轨道带的是父的 id

`describeSong` 在 head 的循环里创建子设备的行：`track.deviceId = device.id`（父）。而实例带的是自己的 id。**按 id 匹配永远找不到这些行**——它们就一直空着，或者被别的设备的状态顶上。

### 2. 三重相同：id、指针、类型

libvgm 把 YM2610 的 SSG 挂在**父设备条目**下：

| 字段 | YM2610 | 其 SSG |
|---|---|---|
| `deviceId` | 相同 | 相同 |
| `PLR_DEV_INFO` | 同一个 | 同一个 |
| `device->type` | YM2610 | **也是 YM2610** |

但轨道是按**子设备**建的（type = AY8910）。于是任何“用设备字段配对”的写法在这一颗上必然失效：id 与类型都不匹配，只能落回 FM 1..3。

**唯一可靠的差别是声道数**（13 对 3）。这是纯粹的硬件/库事实，无法从通用规则推出。

### 3. 两种顺序天生不同

- `tracks`：文件头/设备表顺序（本例 SEGA PSG 先、YM2612 后）；
- `instances()`：命令到达顺序（YM2612 先被写）。

**YM2610 曾“通过”只是因为两者恰好一致**；换成 Mega Drive 的曲子立刻暴露。任何依赖顺序的对齐都是定时炸弹。

## 四、最终解法（简、且只有一处特例）

```cpp
// SongBackend.h：行身份三列，写下来而不是推出来
DeviceId sourceDeviceId;      // 真正属于哪个设备（链接设备用 child.id）
int      deviceChannel;       // 设备内声道号（-1 = 整设备）
uint16_t sourceDeviceType;    // libvgm 设备类型：寻址设备用的就是它

// LibvgmTrackPlayer.cpp：唯一的一条芯片特例，写明理由
uint16_t voiceType = device->type;
if (device->type == DEVID_YM2610 && instance.decoder->channelCount () == 3)
{
    voiceType = DEVID_AY8910;   // YM2610 名下只有 3 个声道的，就是它的 SSG
}
state.trackIndex = trackForVoice (voiceType, channel);
```

- 主通路：`(类型, 声道号)` 精确配对，不看顺序、不算偏移；
- 特例：一条 if，集中一处，注释写清“为什么字段不可用”；
- 删除：`channelBases`、按声道数排序、按 id 去重、`trackFor`、末尾兜底追加——它们都是为猜顺序而生的脚手架。

## 五、教训

1. **“通用解”不等于“更好”**。当某个对象的身份字段被库抹平时，通用规则无论怎么写都会退化成猜；这时**一条写明理由的特例**比第十个通用补丁可靠。
2. **按下葫芦起瓢是信号**，不是运气差。它说明抽象选错了层：我在“顺序/偏移”这一层做通用，而问题在“身份”这一层缺失。
3. **先问一句再动手**。这类“要么特例、要么大改”的岔路，应当在实现前与作者确认取向——本次反复正是因为每次都自行选择了更通用的那条路。
4. **兜底分支要能暴露错误**。“未匹配就追加到最后”把映射错误变成 UI 上多出来的一组行；正确做法是缺行。
5. **跨型号验证必须选顺序相反、声道数不同的素材**（YM2610 13+3 与 Mega Drive 7+4 正好互补）。
6. **改共享计数前先看它的消费者**：`channelCount` 同时决定名字表、静音掩码、行数，动它等于同时改三处契约。

## 六、验收数据（2026-11-08，最终版）

```
07 Stage 2（YM2610）  16 行：FM 1..6 / ADPCM-A 1..6 / ADPCM-B / SSG 1..3
                      SSG 1..3 显示方波；FM 1..6 为合成声部
02 Green Hill Zone     11 行：SEGA PSG 1..3 + Noise / YM2612 FM 1..6 + DAC
03 Scene Select        24 行
加载 212 ms · 46/46 · 芯片断言 PASS
```
