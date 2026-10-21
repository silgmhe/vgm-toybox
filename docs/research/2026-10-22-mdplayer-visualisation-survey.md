# MDPlayer 的键盘窗/钢琴窗是怎么算出音符的

> 调研对象：`reference/MDPlayer-stable`（C#，WinForms）。目的：为我们尚未覆盖的芯片（M5 的 PWM / uPD7759 / OKIM6258，B 批的 MultiPCM / YMZ280B 等）找一个可参照的寄存器口径。
> 结论先行：**它只有一条统一规则——把寄存器换算成 Hz，再在固定表里取最近频率**；采样自身的根音完全不参与。这一点与我们的 L1 层一致，但它没有 L2，也没有"可算/推断"的区分。

## 1. 两套实现并存

| 位置 | 形式 | 覆盖 |
|---|---|---|
| `PianoRoll/*.cs`（11 个类） | 旧版，逐芯片一个类 | AY8910、DMG、HuC6280、K051649、SN76489、YM2151、YM2413、YM2608、YM2610、YM2612 + `Common.cs` |
| `form/KB/**/frm*.cs`（约 40 个） | 现在在用的**键盘窗**，每芯片一个窗口 | 见下表；`ChipFactory.cs` 只对 `PianoRoll` 那 11 个返回实例，其余一律 null |

我们早先看到 `ChipFactory.cs:25` 对 RF5C164 返回 `null` 就以为它不支持，**是错的**：RF5C164 的窗口是 `form/KB/PCM/frmMegaCD.cs`（Mega CD 那颗就叫 RF5C164）。同理 `frmRf5c68.cs` 是 RF5C68，两者分开实现。

## 2. 唯一的换算范式

```csharp
// form/KB/PCM/frmMegaCD.cs:166-180 (RF5C164)
private int searchRf5c164Note(uint freq) {
    double m = double.MaxValue; int n = 0;
    for (int i = 0; i < 12 * 8; i++) {
        double a = Math.Abs(freq - (0x0800 * Tables.pcmMulTbl[i % 12 + 12]
                                            * Math.Pow(2, (int)(i / 12) - 4)));
        if (m > a) { m = a; n = i; }
    }
    return n;
}
```

三件事固定：

1. 候选频率表 = `REF × pcmMulTbl[半音] × 2^(八度-4)`，`REF` 是**每芯片一个魔数**；
2. 取**最近**（`Math.Abs` 最小），不是对数域比较——所以它在低频段精度差（`freq` 是 `uint`，`<1` 全撞在一起）；
3. 搜索范围固定 `12*8 = 96` 个半音。

`REF` 的物理含义就是"哪个寄存器值算中央那个键"，即我们的 `refRate`。**没有一处读采样数据。**

## 3. 逐芯片：输入、公式、REF

`REF` 的单位随芯片而定（步进、字节率、Hz），右侧括号是我们实测或推断的等效频率。

| 芯片 | 送进 search 的量 | REF | 文件:行 |
|---|---|---|---|
| RF5C164 | `Step_B`（step 高字节）直接 | **0x0800 = 2048** | `PCM/frmMegaCD.cs:166` |
| RF5C68 | 同一 shape | 0x0800 | `PCM/frmRf5c68.cs:182` |
| C140 | `clock/384` 归一后的频率 | `0x0800<<2 = 8192` | `PCM/frmC140.cs:129` |
| C352 | `0x10000×8000/clock` 反推出的 Hz | 8000 | `PCM/frmC352.cs:147` |
| K053260 | 同上 | 8000 | `PCM/frmK053260.cs:184` |
| K054539 | `hz = (clock/384)/(0x10000/rate)` | 4000 | `PCM/frmK054539.cs:148` |
| QSound | `searchSegaPCMNote(rate/16/166)` | 2656（16×166） | `PCM/frmQSound.cs:162` |
| SegaPCM | `searchSegaPCMNote(ml)` | 8000 | `PCM/frmSegaPCM.cs:205` |
| GA20 | `(clock/4)/(256 - rate)` | 4000 | `PCM/frmGA20.cs:147` |
| **OKIM6295** | **没有换算函数**（grep 全项目无 `search*Note`） | — | `PCM/frmOKIM6295.cs` |
| **OKIM6258** | 同上 | — | `PCM/frmOKIM6258.cs` |
| PWM / PCM8 / MultiPCM / MPCMX68k | 同上 | — | `PCM/frm*.cs` |
| PPZ8（PC-98 驱动） | `frequency / 0x8000` | 32768 | `Driver/frmPPZ8.cs:212` |
| YM2413 / VRC7（FM） | `freq/172 + (oct-4)*12` | 172 | `OPL/frmYM2413.cs:113`、`NES/frmVRC7.cs:114` |
| YM3526 / YM3812 / Y8950（FM） | `fmus/523.3` | 523.3（= C5） | `OPL/frmYM3812.cs:171` 等 |
| YMF262 / YMF278B（FM） | `inst[12]/344 + (inst[11]-4)*12` | 344 | `OPL/frmYMF262.cs:253` |
| YMF271 | `searchSSGNote(inst[14]) + (((inst[13]+8)&0xf)-11)*12 - 7` | 由 SSG 表给 | `OPX/frmYMF271.cs:179` |
| PSG 族（SN76489/AY/HuC6280/SCC/DMG/S5B/YM2203/2608/2610/2609 SSG） | 分频值 → `searchSSGNote` | `Tables.freqTbl` | `PianoRoll/SN76489.cs:132` 等 |
| ADPCM-B（Y8950/YM2608/2610） | `deltaN × 50000 / 65536`，再 `/8000` | 8000 | `OPL/frmY8950.cs:265-270` |
| YM2610 FM（旧版） | `fnum × clock/(24·div·2^21) × 2^(oct+2)` | 直接 Hz | `PianoRoll/YM2610.cs:162` |

`Common.searchSegaPCMNote(f)`（`common.cs:453` 附近）就是把 Hz 送进上面那个最近值循环；`Common.searchSSGNote` 是同一件事的 SSG 版，但返回 `95 - n` 的**倒序**编号——很多窗口里写的是 `95 - Common.searchSSGNote(...)`，这个 95 的约定只在他们内部成立，不能直接当 MIDI 号用。

## 4. 可以照搬的

1. **范式本身**：先 Hz、后音符、永不看波形。我们 §14 的 L1 已经是这个，`quantisePlaybackRate(rate, refRate)` 与 `REF × pcmMul` 是同一件事。
2. **RF5C164 的 REF = 2048**：与我们实现完全一致（step 2048 = 一字节每输出样本）。这是它给我们最直接的一次验证。
3. **ADPCM-B 的 `deltaN × 50 kHz / 2^16`**：50 kHz 正是 `clock/144`，我们先前 288 倍的 bug 就是靠这条对照定位的。
4. **FM 侧 `fnum × clock / (24·div·2^21)`**：与我们 OPN 的音符表同源。

## 5. **不要**照搬的（原版的 bug 与局限）

| # | 问题 | 影响 |
|---|---|---|
| 1 | **没有 L1/L2 之分**：所有采样芯片都按"寄存器 → Hz"直接出音，八度完全由魔数 REF 决定 | 这正是我们补的一层；他们无法回答"这个音符有多可信" |
| 2 | `REF` 全是无出处的魔数（QSound 的 `16×166`、GA20 的 4000、YMF262 的 344） | 换成另一批曲库就整体偏八度，且无法判断是素材问题还是常量问题 |
| 3 | C140 在 `searchC140Note(frequency) + 1`（`frmC140.cs:193`）——搜索本身就差一个半音，用 `+1` 补 | 我们 P1 实测：直接照它的 8192 会让 Submerged City 类曲子整体偏高 |
| 4 | K053260 用 `bit[0]` 判断发声（`frmK053260.cs:180`），而真实门控在 `pitch`/`start` 附近 | 会把静音通道画出音符 |
| 5 | `uint freq` 参与 `Math.Abs(freq - double)`，低频段（几十 Hz）比较失真 | 贝斯线低八度容易错 |
| 6 | 搜索范围固定 96 个半音，且以 95 为最高音（`95 - n`） | 超过两点的音域无法表示；编号与 MIDI 基准不同 |
| 7 | **OKIM6295 / OKIM6258 / PWM / PCM8 / MultiPCM / MPCMX68k 根本没有音高换算** | 只能显示"某路在响"，对我们没有参考价值 |
| 8 | 采样周期/根音从不测量 | 对"驱动换了个采样"的场景，它们的音高是对的，对我们就是缺 L2 |

## 6. 与我们现状的对照（M4 九颗）

| 芯片 | 我们的 `refRate` | MDPlayer 的 REF | 结论 |
|---|---|---|---|
| RF5C164 | step 2048 | 2048 | **一致**，直接采用 |
| C352 | `clock/576` | 8000 | 我们 P1 实测照它偏高约 2 个八度（L1 65→82），故保留 |
| K053260 | `clock/64` | 8000 | 同上（48→82），保留 |
| K054539 | `clock/384` | 4000 | 我们实测 106，明显过高，保留 |
| QSound | `clock/2496` | 2656 | 魔数无出处，保留 |
| SegaPCM | `clock/256`（pitch 128） | 8000 | **照它更合理**（L1 34→46），已改 |
| GA20 | 4000 | 4000 | **一致**，已改（58→70） |
| ADPCM-B | 8 kHz（16000 nibble/s） | 8000 | 一致 |
| ES5503 / OKIM6295 | 无 L1，实测周期 | 无 | 我们比它多给了音高 |

一句话：**RF5C164、SegaPCM、GA20、ADPCM-B 可以直接引用它的常量；C352/K053260/K054539/QSound 按我们的实测保留。**

## 7. 对后续芯片的用法

- **M5（PWM、uPD7759、OKIM6258）**：MDPlayer 对它们没有音高换算，说明"只能 one-shot"这个判断是对的——直接按 §14 的 C4 触发实现即可，不必再找寄存器。
- **YMZ280B / MultiPCM**：前者解码器已写、搁置；后者无核心。两者在 MDPlayer 里**都没有音高换算**（`frmYmz280B.cs` 无 `search*`），所以即便做了也要按我们的 L2 处理。
- **PPZ8 / YM2413 / YMF262** 的 `searchSegaPCMNote(x)` 用法可以照抄：`x = 寄存器导出的 Hz / REF`，REF 取它的魔数作起点，再用真实曲目复核。

## 8. 引用位置

- `reference/MDPlayer-stable/MDPlayer/MDPlayerx64/form/KB/PCM/frmMegaCD.cs:166-180`（RF5C164）
- `.../form/KB/PCM/frmRf5c68.cs:182-196`、`frmC140.cs:129-146,193`、`frmC352.cs:147-160`、`frmK053260.cs:180-196`、`frmK054539.cs:148-160`、`frmQSound.cs:162`、`frmSegaPCM.cs:205`、`frmGA20.cs:147-158`
- `.../form/KB/OPL/frmY8950.cs:265-270`（ADPCM）、`frmYM2413.cs:113`、`frmYM3812.cs:171`、`frmYMF262.cs:253`、`frmYMF278B.cs:236,404`
- `.../form/KB/OPX/frmYMF271.cs:179`、`.../form/KB/Driver/frmPPZ8.cs:128-145,212`
- `.../PianoRoll/YM2610.cs:162`、`SN76489.cs:132`、`AY8910.cs:45`
- `.../common.cs:453-469`（`searchSegaPCMNote` / `searchYM2608Adpcm`）
