# 一个 RAM 字节被当成了寄存器：WonderSwan 波表为什么全是直线

> 三个事实同时成立：文件里有 144 条 `0xC6`；loader 发出了它们；解码器有 `isMemory` 分支。
> 而用户看到的四行都是**一条直线**。三者都真，说明**中间有一层把写入改写了**。

## 一、症状

`07 Main Theme of Final Fantasy IV` 播到 12 秒，键盘页四个通道的波形格都是平线；
`01 The Prelude` 当时"看起来正常"，于是第一轮验证被判为通过。

## 二、定位：从"数据在不在"一路缩到"谁改了它"

| 步骤 | 做法 | 结果 |
|---|---|---|
| 1 | 用 Python 独立解析 `.vgz` | 144 条 `0xC6`，地址 `0xC00-0xC3F`，基址 `0x8F = 0x30`，数据确实在文件里 |
| 2 | 在 `VgmSongLoader` 的 `case 0xC6` 打点 | `load C6 0C00 = 00 …` 出现 → reader 与 loader 都通过了 |
| 3 | 在 `WswanDecoder::write()` 的 `isMemory` 分支打点 | **一次都没进** → 丢在 loader 之后、解码器之前 |
| 4 | 读 `ChipWriteRouter::route` 的 WS 分支 | 见下，写入被**重建**了 |
| 5 | 修复后 dump 12.47 s 的 4×16 字节 | 与第 1 步的解析结果逐字节一致 |

第 4 步看到的是：

```cpp
// ChipWriteRouter.cpp（修复前）
case midi::ChipKind::wonderswan:
    decoders.decoder<chips::WswanDecoder> ().queue (deviceFor (DEVID_WSWAN, instance), reg, value, time);
    break;
```

`queue` 的签名是 `queue (device, reg, value, sampleTime, port = 0)`——它只认识**寄存器写**。
WonderSwan 的 `0xC6` 是 **RAM 写**：地址在 `port`/`reg` 里，性质由 `isMemory` 说明。两个字段都被丢掉，
于是这条写变成了"往寄存器 0x00 写 0x00"，落到解码器 `switch (w.reg)` 的 `default`，**静默丢弃**。

修复是把整条写入原样入队：

```cpp
void queue (const PLR_DEV_INFO* device, const chips::RegisterWrite& source);   // 新增重载
…
case midi::ChipKind::wonderswan:
    decoders.decoder<chips::WswanDecoder> ().queue (deviceFor (DEVID_WSWAN, instance), w);
```

## 三、根因

**中间层"解构再构造"了一个值对象。** 路由拿到的是 `const RegisterWrite&`，却按它"以为"的字段
（`reg, value, time`）重建了一条新的——只要 `RegisterWrite` 里多出任何一个字段，这条路就必然丢。

它是本项目**唯一**需要新字段的芯片：其它每颗的数据都能塞进 `(reg, value, port, time)`（X1-010 的
16 位偏移、SCC 的两条写、K054539 的 `port`），所以这个"重建"在此之前从未出过问题。这不是"运气好"，
而是**没有被字段增长考验过**。

第二个根因是**失败是静默的**：`reg = 0x00` 不是非法值，它只是一条没人关心的寄存器写；空表也不是
非法状态，它同样是合法的 32 点波形。整条链上没有任何一处有条件能失败。

## 四、为什么测试没抓住（四个独立原因）

1. **单元测试直连解码器**：`testWswanWaveTable` 自己构造 `RegisterWrite`，绕过了 router——测的是
   "解码器对不对"，不是"数据到不到"。缺的是**接缝测试**（跨层那一段）。
2. **断言没有区分力**：`engine wave.0 == 32` 只比大小，而**空表也有 32 点**。这正是项目规则里
   "容差比缺陷大，等于没有断言"的实例。
3. **端到端只用了 `01 The Prelude`**：它四张表同形且恒定，看不出"上传有没有发生"；`07` 的表会随
   乐器变化，才把问题暴露出来。**一首歌通过不代表通路正确。**
4. **探针的采样窗口错了**：第一次打点是"前 8 次快照"，全部落在 `t≈0.27 s`，那时表的前 16 字节本来
   就是 `0x00`——打印出来像是"数据没到"，其实是**表还没写完**。探针应按**时间**门控，不该按次数。

还有一条不算原因但值得记：**我把缩略图上的平线读成了方波**。40×20 px 的波形格里，一条贴底的直线
与一个满幅方波都能读成"有形状"。截图能证明"看得见"，不能证明"算得对"。

## 五、修好之后立了什么护栏

| 护栏 | 位置 |
|---|---|
| `ChipBuffer::queue (device, const RegisterWrite&)`：写入原样入队 | `ChipWriteRouter.h` |
| `engine wave.<n>.peak`：读行内最高样本，方波 `0.875`、空表 `-1` | `AppHarnessTest.cpp` |
| `wave-wonderswan.txt`：尺寸 + 峰值双断言 | `plugin/Tests/scripts/` |
| 设计与调研文档记录本次失败 | `libvgm-visualisation.md` §9.26、`docs/research/2026-11-12-wonderswan-waveform.md` §10 |

## 六、下次的检查清单

**新增 `RegisterWrite` 字段时：**

```powershell
# 谁在重建写入？任何一个都可能是下一个丢字段的地方
Select-String -Path plugin\Source -Pattern 'RegisterWrite write;|\.queue \(deviceFor' | Select-Object -First 40
```

**新增命令族时：**

- reader 有没有分支？（`0xC6` 当初连命令都没生成）
- loader 有没有 `case`？
- **router 有没有 `case`，它是传整条写入还是重建？**
- 断言的输入来源，是否与用户看到的是**同一条链路**？

**写端到端断言时，问三个问题：**

1. 数据完全没到时，这条断言会失败吗？
2. 数据到错通道时，会失败吗？
3. 数据是上一小节留下的旧值时，会失败吗？

三个都不会，就换一个有区分力的量（峰值、首值、数量），或者补一条。

**做可视验证时：**

- 至少用两首**形态不同**的曲子（一张恒定表 + 一张会变的表）；
- 看数值（`peak`、`size`），不要只看缩略图；
- 按用户报的时刻复现（本例是 12 秒，不是 0.3 秒）。

## 七、结论

**能确证的**：`ChipWriteRouter` 的 WonderSwan 分支按 `(reg, value, time)` 重建写入，丢掉了 `port` 与
`isMemory`；解码器因此从未收到 `0xC6` 的字节；空表仍然是合法的 32 点波形，所以大小断言通过。

**根因一句话**：**一个写入在跨层时被重新发明了一次**，而新的表示比旧的窄。

**规则**：值对象跨层只传本身，不传它的投影；投影必须显式列出全部字段，并且在字段增加时由编译器
（而不是记忆）保证不遗漏。

相关提交：`e9f7731`（通路与绘制）、`e3a2dc0`（修复与护栏）。
