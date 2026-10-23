# FTM 音频：移植路线与进度

日期：2026-10-24　分支：`feature/multiformat-core`

## 1. 为什么需要移植编译器

FamiTracker 的音频不是"把模块渲染成 WAV"，而是：

```
.ftm ──(Compiler)──► 6502 驱动 + 歌曲数据 ──► 逐帧写 APU 寄存器 ──► 声音
```

我们的播放引擎（libvgm）吃的是**寄存器写入流**，所以缺的正是中间那个编译器。
`Compiler.cpp` 有 3,057 行，直接改它会散落 117 处改动，因此先立接口再搬实现。

## 2. 已验证的关键事实

| 事实 | 依据 |
|---|---|
| 编译器**不汇编**驱动，导出时复制内嵌字节数组 | `Compiler.cpp:37` 引入 `Driver.h`；驱动来自构建期生成的 `drv_*.h` |
| 2A03 驱动本体 | `DRIVER_2A03[]` = **7,076 字节**；`NSFDRV_2A03[]` = **8 字节**（`Dn-FT 2.10`） |
| 编译器对文档的依赖 | **117 处调用，37 个不同方法** |
| 编译器对乐器的依赖 | **13 处调用，11 个不同方法** |
| `Compile` 签名 | `int Compile (CChunk* pChunk, int Index)`，返回写入字节数 |
| 图案读取方式 | 指针 + 行号，**按行索引的稠密数组** |
| 空单元格哨兵 | `Note=0`、`Vol=0x10`、`Instrument=0x40`（**不是零**） |
| `CChunk` | `Chunk.h` 133 行 + `Chunk.cpp` 140 行，无 MFC |

## 3. 进度

| 阶段 | 内容 | 规模 | 提交 | 状态 |
|---|---|---|---|---|
| M1 | `ISongSource` 接口 | 37 方法 | `2c1d6db` | ✅ |
| M1 | `ModuleSongSource` 适配层 + 9 断言 | ~200 行 | `c25ad75` | ✅ |
| M2 | 2A03 驱动数据（精确尺寸校验） | 43 KB | `ca42add` | ✅ |
| M3-1 | 图案物化（稀疏 → 稠密，含哨兵） | ~60 行 | `d9648ad` | ✅ |
| M3-2 | 乐器接口（11 方法，指回 Module） | ~120 行 | `e6bf90b` | ✅ |
| **M3-3a** | **`CChunk`（字节缓冲 + 重定位）** | **273 行** | — | **下一步** |
| M3-3b | `CSeqInstrument::Compile`（宏序列编码） | 数百行 | — | — |
| M3-3c | `CInstrument2A03::Compile` + DPCM | 数百行 | — | — |
| M3-4 | 歌曲数据打包（图案 + 帧） | 数百行 | — | — |
| M3-5 | NSF 容器输出 | ~200 行 | — | — |

## 4. 设计约定（已写入代码，勿随意推翻）

1. **不改编译器**：所有差异在适配层消解。`ISongSource` 的签名刻意照抄 `FamiTrackerDoc.h`；
2. **宁可缺失，不可猜**：`GetDataAtPattern`/`GetInstrument` 在数据不具备时返回 `nullptr`，
   DPCM 返回 0。错误答案会表现为"音频不对劲"，缺失会表现为"没有音频"，后者更容易发现；
3. **哨兵值必须保留**：空单元格是 `Vol=0x10`、`Instrument=0x40`。清零会让编译器把空行编码成
   "以音量 0 弹 C-0"，且不会报错；
4. **精确值断言**：驱动尺寸写成 7,076 而非"> 8000"。FT 的驱动变了，移植应当报错而不是静默适配。

## 5. 验证方法（已就位）

```
FT:   Dn-FamiTracker.exe <module>.ftm -export out.nsf
我们: 编译器输出寄存器流
      ↓
逐帧逐寄存器对比（可复用 VgmCommandReader 的思路）
```

FT 的导出就是金标准，不需要人工判断。

## 6. 下次的入口：M3-3a

1. 把 `Chunk.h` / `Chunk.cpp` 复制到 `formats/ftm/compiler/`；
2. 去掉 `LPCTSTR` 等 Windows 类型；
3. 加单元测试：写入、读取、写入重定位、应用重定位。

完成后 `Compile` 的两个实现才有落脚点。

## 7. 与 MIDI 工作的关系

两条线互不影响：

- **MIDI 转谱**：`FtmSongLoader` 直接产出 `NoteStream`，4/7 首与 FamiTracker 逐音符一致
  （见 `2026-10-24-ftm-parity-status.md`）；
- **音频**：需要编译器，与 `NoteStream` 无关。

也就是说 FTM 可以先"导出 MIDI（已可用）"，后"播放音频（本路线）"。
