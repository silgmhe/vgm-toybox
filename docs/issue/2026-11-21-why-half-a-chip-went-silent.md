# 半颗芯片为什么不出声

> 两天里同一颗 YM2608 上出现两个症状：三个 SSG 行显示 FM 的波形，接着 FM4–6 变成直线。
>
> 两个根因都不在绘制代码里。第一个是设备身份，第二个是一个被复位关掉的模式位。
>
> 这份记录的重点是最后一节：**下一次实现新的 FM 芯片时，怎么一次写对。**

## 一、两个症状

`82 Ruins.vgz`（YM2608），键盘页十六行：

| 症状 | 表现 |
|---|---|
| **一** | `SSG 1/2/3` 三行显示的是 `FM 1/2/3` 的内容；`FM 2/3` 被画成方波 |
| **二** | 一修完之后，`FM 1–3` 正常了，`FM 4–6` 全是直线 |

**两天的排查里有四次判断是错的，两次是对的。** 记下来的是为什么错。

## 二、第一个动作永远是量峰值

### 三件不同的事，长得很像

| 现象 | `wave` | `peak` | 含义 |
|---|---|---|---|
| 没有渲染 | 0 | — | `voicePatch` 为空，或走了单周期分支 |
| 渲染了但静音 | > 0 | **0** | 核心没出声 |
| 渲染了有声 | > 0 | > 0 | 正常 |

**`wave.size() > 0` 只证明渲染跑完了。** 这个项目已经为它付过两次代价——`2026-11-18` 那份文档里写过一次，`2026-11-21` 的 YM2203 那次又踩了一遍。

### 空的两种：曲子没用它，和它本该出声却没声

**`peak = 0` 本身不是异常。** 一首曲子里一个 FM 通道可以整段都没有音符，那时它的行是空的，完全正确。

**异常是"有信号却没画出来"。** 区分这两件事需要一个第四维：**这一帧这个通道有没有被键控。**

| `voicePatch` | 这一帧在响（`gate` / `active`） | `peak` | 结论 |
|---|---|---|---|
| 空（全 0） | — | — | 曲子没用这个通道 → **正常** |
| 有 | **false** | 0 | 当前没按键 → **正常**，而且形状仍应正确 |
| 有 | **true** | 0 | **异常**：有键控、有音色、渲染静音 |
| 有 | true | > 0 | 正常 |

**`gate` 是"这一帧"，`peak` 是"整段音色"，两者不同步。** 所以判断异常必须要求 `gate` 为真；反过来 `peak > 0` 也不能证明它现在在响（可能是 release 的尾巴）。

`chipState.gate || chipState.released` 才是"这个通道此刻在发声"——`LibvgmTrackPlayer` 里处理采样通道的那段（`sounding`）用的就是这个组合，同一个道理。

**推论**：诊断打印必须带 `gate`，否则 `peak = 0` 无法定性。这次排查的打印没有它，靠的是"同一帧里另外几个通道 `peak = 1`，而它们的 patch 与出问题的三个来自同一个解码器、同一个驱动"——**间接但足够**。以后应当直接打出来。

**断言不受这条影响**：`OpnCoreOracleTest` 里的用例是我们自己 `noteOn` 的，核心必然处于键控状态，`peak > 0.5` 是可靠的判据。

### 加上第三个量：patch 的第一个寄存器

```cpp
std::fprintf (stderr, "[fm] ch=%d wave=%zu peak=%.4f tl=%u,%u,%u,%u alg=%u first=%04X\n", ...);
```

**`first` 是 `patch.regs.front().first`，它的 bit 8 是端口。**

`0x0030` 和 `0x0130` 的区别，就是"这个 patch 属于通道 1"和"属于通道 4"的区别。**没有它，`tl` 正常和 `peak=0` 并列出现时，无法判断是 patch 错了还是 key-on 错了。**

**三个量一起看，才能把问题切到某一层：**

```
wave=0                  → 渲染没跑（patch 空 / 门槛挡掉）
wave>0  peak=0  first 对  → 有 patch，核心不出声（key-on / 模式位 / 用错核）
wave>0  peak=0  first 错  → patch 落在了别的通道
wave>0  peak>0            → 正常，去看形状
```

## 三、症状一：SSG 用 FM 的轨

### 链条

**1. SSG 的写入带着父设备的身份**

```116:127:plugin/Source/backends/libvgm/ChipWriteRouter.cpp
	const PLR_DEV_INFO* device = deviceFor (type, instance);   // type 是 DEVID_YM2608

	if (port == 0 && reg <= kLastSsgRegister)
	{
		onSsgWrite (device, reg, (uint8_t) value, time);        // 父设备
```

**2. 于是 `Ay8910Decoder` 的 buffer 记着父设备的 `PLR_DEV_INFO`**

**3. 判定函数只认一个芯片**

```cpp
// 修改前
return instance.device != nullptr
       && instance.device->type == DEVID_YM2610      // ← 写死
       && instance.decoder != nullptr
       && instance.decoder->channelCount () == 3;
```

它的注释还写着 "the YM2610's SSG is the one in this project"——**诚实地记录了当时的范围，但没有人把它当成待办。**

### 实测确认

```
[inst] id=0 type=0x07 count=3  kind0=13 class=Ay8910Decoder  linked=0
[inst] id=0 type=0x07 count=13 kind0=37 class=OpnDecoder     linked=0
```

**两个实例都报 `type=0x07`（YM2608）和 `id=0`。** 只有通道数和 decoder 类型不同。

`linked=0` 对 SSG——那一个 `false` 产生三处错误：

| 后果 | 位置 |
|---|---|
| SSG 的 state 记到 FM 轨 | `voiceType = DEVID_YM2608` → `trackForVoice` 命中 FM 轨 0–2 |
| FM 的 kind 槽位被 SSG 覆盖 | `kindTable[{id, false}]` 用的是同一个标志 |
| SSG 行没有波形 | `kindFor` 找 `{id, true}`，不存在 → `unknown` |

**行对比（改前 / 改后）：**

```
改前  FM 2  kind=2 wave=0    FM 3 kind=2 wave=0    SSG 1-3 kind=0 wave=0
改后  FM 1-6 kind=7 wave=1                          SSG 1-3 kind=2 wave=0
```

`FM 1` 在坏版本里侥幸保住，因为它的 state 分数更高（`wave != nullptr` 得 4 分），压过了 SSG 那条。**它的邻居没有这个分数。**

### 修复：问解码器，不问设备名

```cpp
const bool opnParent = type == DEVID_YM2203 || type == DEVID_YM2608 || type == DEVID_YM2610;

return opnParent
       && instance.type == std::type_index (typeid (chips::Ay8910Decoder))
       && instance.decoder->channelCount () == 3;
```

**两个限定各自排除一类误判：**

- **父设备限定**排除独立的 AY8910（那时 `device->type` 就是 AY）
- **decoder 限定**排除 YM2203 的 FM——**它也是 3 通道**（`DEV_CHN_COUNT_YM2203 = 3`）

**只按通道数判断，YM2203 的 FM 会被当成 SSG。**

## 四、症状二：FM4–6 直线

### 先排除 patch

```
[fm] ch=3 wave=88889 peak=0.0000 tl=33,33,18,18 alg=7 first=0130
```

**`tl` 和 `alg` 都正常，`first=0x130` 说明端口位也对。** patch 有完整内容，只是渲染出来是 0。

### 再排除端口

`Core::write` 走 A8D8 的地址/数据锁存：

```cpp
writeFn (info.dataPtr, (uint8_t) (writePort * 2), reg);
writeFn (info.dataPtr, (uint8_t) (writePort * 2 + 1), value);
```

`ym2608_write` 的 `switch (a & 3)`：`2` = 地址 port 1，`3` = 数据 port 1，数据写入带 `| 0x100` 进 `OPNWriteReg`。

**端口没问题。**

### 然后看 key-on

```cpp
const auto channelKey = (uint8_t) ((port << 2) | channelInPort);
```

对 ch3（port 1, slot 0）得到 `0x04`，key on 是 `0xF4`。

核心的 `0x28`：

```1758:1762:libs/libvgm/emu/cores/fmopn.c
	case 0x28:  /* key on / off */
		c = v & 0x03;
		if( c == 3 ) break;
		if( (v&0x04) && (OPN->type & TYPE_6CH) ) c+=3;
```

**`(v & 0x04) && (OPN->type & TYPE_6CH)`——bank 位只在 `TYPE_6CH` 设了的时候才生效。**

**编码是对的，前提是芯片处在六通道模式。**

### 根因：复位把模式关掉了

```2997:3001:libs/libvgm/emu/cores/fmopn.c
	/* extend 3ch. enable/disable */
	if(v&0x80)
		OPN->type |= TYPE_6CH;  /* OPNA mode - 6 FM channels */
	else
		OPN->type &= ~TYPE_6CH; /* OPN mode - 3 FM channels */
```

**`TYPE_6CH` 由寄存器 `0x29` 的 bit 7 控制，不是由 `DEV_ID` 固定的。**

而核心的复位结尾：

```3301:3301:libs/libvgm/emu/cores/fmopn.c
	YM2608IRQMaskWrite(OPN, F2608, 0x1f );  /* default value for D4-D0 is 1 */
```

**`0x1F` 的 bit 7 是 0。**

`VoiceDriver::load` 每次都 `core->reset()`，于是每次都被切回三通道。

**这解释了为什么只有一半坏：**

| 路径 | 受模式影响 |
|---|---|
| `0x30 \| 0x100` 的 patch 写入 | **不**——靠地址判断端口 |
| `0x28` 的 key-on | **是**——bank 位被忽略，`0xF4` 启动的是通道 1 |

**通道 4–6 有完整音色，没有任何事件启动它们。**

### 修复

```cpp
if (patch.model == chips::ChipModel::ym2608 || patch.model == chips::ChipModel::ym2610)
{
    core->write (0, 0x29, 0x9F);   // 六通道，IRQ 掩码取复位默认
}
```

**`0x9F = 0x80 | 0x1F`**：bit 7 打开六通道，低五位保持复位的默认值（不启用中断）。

**只写 OPNA/OPNB**：YM2612 天生六通道且没有这个寄存器；YM2203 是三通道，写了会打开不存在的通道。

### A/B 数据

```
改前   ch0 1.0000  ch1 0.0000  ch2 1.0000  ch3 0.0000  ch4 0.0000  ch5 0.0000
改后   ch0 1.0000  ch1 0.0000  ch2 1.0000  ch3 1.0000  ch4 1.0000  ch5 1.0000
```

**把 `0x29` 那行 `if` 改成 `if (false && ...)`，通道 3–5 立刻回到 0.0000，而 `first` 仍是 `0x130/0x131/0x132`。** 同 patch、同端口，只是没被启动——这就把责任钉在 key-on 上。

（`ch1` 在两版里都是 0，因为第一帧那一行还没有 patch；后面有。）

## 五、四次错误判断

| # | 判断 | 为什么错 |
|---|---|---|
| 1 | 先查"波形画得对不对" | 数据在到达绘制前就是错的 |
| 2 | 以为 `channelCount() == 3` 足以区分 SSG | YM2203 的 FM 也是 3 通道 |
| 3 | 以为 `0x28` 的编码有误 | 编码对，是模式位没开 |
| 4 | 以为端口写入会丢 | 端口路径完整，`first` 已经证明 |

**前两次是同一个毛病：在没有量值的情况下，用"看起来合理"的解释推进。** 后两次是先打印再判断，一次就否掉了。

## 六、给下一颗 FM 芯片的清单

**这一节是这份文档存在的理由。** 下面每一条都对应一次真实的故障。

### 6.1 设备身份

**libvgm 把 linked voice set（SSG）报成父设备的 `type` 和 `id`。** 不要用 `device->type` 判断"这一路是 FM 还是 SSG"。

唯一可靠的判据是**解码器类型**：`instance.type == std::type_index (typeid (Ay8910Decoder))`。

**不要用通道数单独判断——OPN 的 FM 也可能只有 3 通道。**

### 6.2 通道规划的三个数必须一致

| 来源 | 值（YM2608） |
|---|---|
| libvgm `DEV_CHN_COUNT_YM2608` | 13（6 FM + 6 ADPCM-A + 1 ADPCM-B） |
| `OpnDecoder::layout.count()` | 6 + 6 + 1 = 13 ✓ |
| `ChipPlans` 的 `ym2608` | `{ 6, 6, 4 }` = 16（13 + 3 SSG）✓ |

**`linkedBase = channelCount`，所以 SSG 的轨号是 13–15。** 任何一处漏掉 ADPCM-B 或 SSG，整块行号就会平移。

### 6.3 型号必须一一映射，不能兜底

```cpp
// 曾经写成这样，YM2203 落到 OPNB
out.model = variant == ym2612 ? ym2612
          : variant == ym2608 ? ym2608
                              : ym2610;
```

**兜底会静默地用一个更宽的核心渲染一个更窄的芯片。** 见 `2026-11-21-the-patch-that-went-to-the-wrong-chip.md`。

### 6.4 时钟、采样率、分频三个数同源

| 量 | YM2612 | YM2203 | YM2608 / YM2610 |
|---|---|---|---|
| `clockFor` | 7 670 453 | 3 993 600 | 8 000 000 |
| `noteRateFor` | clock / 144 | clock / 144 | clock / 144 |
| **f-number 的分频** | **144** | **144** | **288** |

**`noteRateFor` 报的是输出采样率（clock / 144），而 OPNA/OPNB 核心的频率步进是 clock / 288。** 两者不一致时**整整差一个八度**。

### 6.5 核心的初始化寄存器清单

**每一条都是某个症状的全部原因：**

| 寄存器 | 值 | 不写的后果 |
|---|---|---|
| `0x22` | `0x00` | LFO 让电平随时间变化，波形不能复现 |
| `0x27` | `0x00` | 定时器残留；ch3 可能在特殊模式 |
| **`0x29`** | **`0x9F`（仅 OPNA/OPNB）** | **六通道模式被复位关掉，通道 4–6 静音** |
| `0x2B` | `0x00` | YM2612 的 DAC 打开，混入采样通道 |

**`0x29` 必须写在 `core->reset()` 之后**——复位会把它清掉。

### 6.6 验证四件套

**不要只看 `wave.size()`。** 一次打印四个量：

```cpp
[fm] ch=%d wave=%zu peak=%.4f gate=%d tl=%u,%u,%u,%u alg=%u first=%04X
```

| 组合 | 结论 |
|---|---|
| `wave=0` | 渲染没跑（门槛 / patch 空） |
| `wave>0`，`peak=0`，**`gate=0`** | **正常**：这一帧这个通道没有音符 |
| `peak=0`，`gate=1`，`tl` 正常，`first` 对 | 有键控有音色却没出声 → key-on / 模式位 / 用错核 |
| `peak=0`，`first` 的端口位错 | patch 落到别的通道 |
| `peak>0`，但 `kind` 是方波 | kind 表被另一路覆盖（6.1） |

**`gate` 这一列不能省。** 没有它，`peak = 0` 有两种解释——曲子没用这个通道（正常），和它该响却没响（异常）——而这两种的修法完全相反：前者什么都不用做，后者要去查核心。

**同一个道理在断言里也要成立**：断言要么自己 `noteOn`（此时 `gate` 必然为真），要么就不能只凭 `peak` 判断。

### 6.7 两个动刀点

**一处修好三处症状**的，通常就在"身份"或"键"上：

- `isLinkedVoiceSet` 一个 `false` → 行错位 + kind 覆盖 + SSG 空
- `0x29` 一个位 → 三个通道静音

**如果发现有两个以上的症状，先找它们共享的那个变量。**

## 七、一条只测了半边芯片的断言

**`vgm_opn_test` 里原有的两条断言（音高、无残留）在这个 bug 存在时都是通过的。** 它们都用一个 port 0 的 patch——那一半一直正常。

**新增的一条用 port 1：**

```
YM2608 second-port peak: 0.9971     ← 通过
```

把 `0x29` 那行去掉：

```
YM2608 second-port peak: 0.0000
FAIL  a patch written to the second port reaches its channel      exit 1
```

### 可以推广的判据

**一条修复如果只碰"半边"——某个端口、某一组通道、某个变体——验证就必须伸到另一半。**

否则测试通过只说明：一直正常的那半，仍然正常。

这和 `2026-11-19` 评审里那条"`ctest` 跑的是旧二进制"是同一个形状：**测试通过不等于被测的东西通过了，得先问它测的是哪一半。**
