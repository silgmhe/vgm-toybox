# 2026-10-24 NSF：放弃，以及两次尝试留下的东西

## 结论

**播放器只支持 VGM / VGZ**，NSF 不再进入 `master`。两条分支保留作为记录，不删除、不合并：

| 分支 | 做法 | 终点 |
|---|---|---|
| `feature/nsf-format` | libgme 适配层（vendored 于 `libs/libgme`） | `48a0567` |
| `feature/nsf-format-support-ref-nsfplay` | vendored NSFPlay 设备核心，自研头解析与总线 | `e3d5baa` |

`master` @ `e1c3fd4`，`ctest` 34/34，dist 二进制中不含 `NESM` / `nsfplay` / `libgme` 任何标识。

## 为什么放弃（不是写不出来，是收益与代价不成比例）

1. **NSF 不是一个数据文件，而是一个 6502 程序。** 长度、轨道、结束位置三件事都不存在，只能推断——这与 VGM 的"寄存器日志"是两种东西，不能共用同一套假设。
2. **长度不可得，且已经证伪到底。** 试过每帧把寄存器写折叠成哈希再找重复：10,800 帧、3,589 个不同哈希、其中 3,580 个重复出现，但**没有任何 20 帧连续重复**；改成只哈希地址（怕驱动写回会变化的值）、把上限从 3 分钟提到 10 分钟，三首文件结果相同。结论是这些驱动的写序列在 10 分钟内不精确重复——不是阈值问题。
3. **分轨要改的是渲染路径，不是开关。** libgme 的 `Classic_Emu::play_` 只读**一个共享 buffer**，所以 `osc_output(i, nullptr)` 根本不生效：实测求和比原混音高 **+16.9 dB ≈ 7 倍能量**，即八条轨各自在放整曲。真正的做法是每 voice 一个 `Blip_Buffer` 并覆盖 `play_`——这条在独立探针里跑通了（`vgm_nsf_iso_probe`，solo voice 0 → `peak=-514`），接进后端仍然 `0xC0000005`。
4. **扩展芯片与不规范文件叠在一起。** libgme 只有 VRC6 / N163 / S5B，且只信头标志；而 `fod_snowburnb.nsf` 头写 `chip_flags = 0x00` 却在写 `$9000-$B002`。为此加了"跑两秒看它写不写"的探测，但探测是启发式：`Entry080.nsf` 同样被判定有 VRC6，无法从计数上区分"用而不声明"与"把那段当内存"。

## 第二次尝试（NSFPlay 核心）确实走到了哪一步

vendored `libs/nsfplay`（19 个翻译单元，`nsfplay_core` 静态库，`libs/nsfplay/CMakeLists.txt`）。可用部分：

- 头解析（1-based 曲目、PAL 位在速度字最低位、v1 的 load 字段是长度）；
- 8 个 4KB bank 窗口，`$5FF8-$5FFF` 选择，`(adr >> 12) & 7`；
- 5 / 8 / 13 轨（2A03、VRC6、N163），扩展芯片创建、写分派、逐声道 `RenderChannels`；
- 引擎侧 `inPlaceChannelMix` 单实例多轨路径（`renderTracks`），seek 用 `Exec(4096)` 快进，时长走可配置的默认值。

未解决：`1983.nsf` 从不上报寄存器写（无输出）；`fod_hopeless.nsf` 的混音验收仍红；FME7 没有 `RenderChannels`（其 `mask` 与其余芯片不同名）；扩展芯片的 `RenderChannels` 是"每声道一次 Render"的正确但不高明的形态。

## 比代码值钱的四条（任何"程序驱动型"格式都适用）

1. `NES_CPU::Reset()` 装的是取指回调，`Start()` **不代劳**——不调就卡死在 init。
2. `SetImage()` 会 memset 64KB，`$4100` 跳板必须在它**之后**写。
3. `NES_DMC::Tick()` 不算包络与长度计数器，必须另调 `TickFrameSequence()`。
4. `NES_DMC` 实为 **TND 单元**：`NES_APU` 只收 `$4000-$4007`（`reg[adr-0x4000]`），`NES_DMC` 收 `$4008-$4013`（`reg[adr-0x4008]`，含 `enable[2] // tri/noise`、`tphase`、`nfreq`），`$4015/$4017` 两边都要。只按类名分派会漏掉三角波与噪声。

## 重启条件（任意一条满足就可以捡回来）

1. 接受"固定时长"作为格式事实（现已在分支上实现为设置项），且接受多实例隔离（一份模拟 × 轨道数）；
2. 或有人提供带长度的 NSFe 全集，循环点不再需要推断；
3. 或者安排一个**有调试器**的完整时段，解决后端那处 `0xC0000005`——机制本身已被独立探针证明可行。

## 复核方式（master 上 30 秒）

```bash
ctest --test-dir build            # 应 34/34
```

分支上另有三个检查目标，切过去即可用：`vgm_nsf_probe <file> --check`（各声道之和 vs 混音）、`vgm_nsf_iso_probe <file>`（逐 voice 缓冲）、`vgm_mixer_test <file> 3`（引擎级混音验收）。

## 顺带记录（与本格式无关，但值得在 master 上修）

撤销时发现的一处**通用缺陷**：UI 与引擎两处 `jlimit(0.0, duration, seconds)`，在 `duration == 0`（长度未知）时会把**每一次 seek 都变成 seek 到 0**。同样的坑还出现过第二次（`getTrackPositionSeconds` 在多实例下返回 −1，被当成"位置漂移 91 秒"）。

修复目前只保留在 NSF 分支：`duration > 0` 才钳制、`inPlace` 模式下位置取 0 号播放器。**master 将来若接入任何"长度未知"的格式会再次踩到**，建议在引擎里预先处理。
