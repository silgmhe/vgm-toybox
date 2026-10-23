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

## 6.1 交接：`CChunk` 已侦察，未移植

读 `Chunk.h`（133 行）得到的事实，下次可直接照此开工：

**五种数据元素**（多态，`GetSize()` + `GetData()`）：

| 类型 | 尺寸 | 用途 |
|---|---|---|
| `CChunkDataByte` | 1 | 立即数 |
| `CChunkDataWord` | 2 | 立即数（16 位） |
| `CChunkDataReference` | 2 | 对某个 label 的引用，稍后回填 |
| `CChunkDataBank` | 1 | 引用某 label 所在的 bank |
| `CChunkDataString` | 变长 | 原始字节块 |

**17 种 chunk 类型**：`CHUNK_HEADER`、`CHUNK_SEQUENCE`、`CHUNK_INSTRUMENT_LIST`、`CHUNK_INSTRUMENT`、
`CHUNK_SAMPLE_LIST`、`CHUNK_SAMPLE_POINTERS`、`CHUNK_GROOVE_LIST`、`CHUNK_GROOVE`、`CHUNK_SONG_LIST`、
`CHUNK_SONG`、`CHUNK_FRAME_LIST`、`CHUNK_FRAME`、`CHUNK_PATTERN`、`CHUNK_WAVETABLE`、`CHUNK_WAVES`、
`CHUNK_CHANNEL_MAP`、`CHUNK_CHANNEL_TYPES`。

**`CChunk` 公开接口**（后半部分与 `Chunk.cpp` 尚未读）：

```cpp
CChunk (chunk_type_t Type, CStringA label);
void Clear();
chunk_type_t GetType() const;
LPCSTR GetLabel() const;
void SetBank (unsigned char);
unsigned char GetBank() const;
int GetLength() const;
unsigned short GetData (int index) const;
unsigned short GetDataSize (int index) const;
void StoreByte (unsigned char);
void StoreWord (unsigned short);
void StoreReference (CStringA refName);
```

**替换表**：

| FamiTracker | 本项目 |
|---|---|
| `CStringA label` | `std::string` |
| `LPCSTR GetLabel()` | `const std::string&` |
| 重定位表（label → 偏移） | 保留原样 |

`Chunk.cpp`（140 行）只做“按 label 查偏移并回填”，无 MFC 逻辑，可直接改写。

**开工顺序**：读 `Chunk.h` 剩余 ~50 行 + `Chunk.cpp` → 复制进 `formats/ftm/compiler/` →
`CStringA` 换 `std::string` → 单元测试（写入、读取、写入引用、应用重定位）→ 与 FT 同输入对比字节。

## 6.2 交接：DPCM 的两个格式（已确认，未实现）

`CInstrument2A03::Compile` 还需要采样数据，而 loader 目前有两处缺口。

**① `DPCM SAMPLES` 块**（`WriteBlock_DSamples`）：

```
u8   count
per sample:
  u8   索引
  i32  名称长度 + 名称字节
  i32  数据长度 + 数据字节
```

**② 乐器里的 DPCM 分配表**——现在是**跳过**的：

```cpp
const int bytesPerAssignment = (block.version > 5) ? 3 : 2;
reader.skip ((std::size_t) (octaves * 12 * bytesPerAssignment));
```

它其实是 `8 八度 × 12 音 × {sample, pitch, delta}`，正是 `GetSampleIndex` / `GetSamplePitch` /
`GetSampleDeltaValue` 的数据源。

**已确认的排列**（用只读探针在 `VietnameseMuskrats` 上验证）：

| 项 | 值 |
|---|---|
| 表尺寸 | 192 字节 = 8 八度 × 12 音 × 2 字节 |
| 索引 | `index = octave * 12 + note` |
| 每条 | `sample`、`pitch`（块版本 > 5 时再加一个 `delta`） |
| **适用范围** | **仅 `type == 1`（INST_2A03）** |

实例：该曲的 "DPCM" 乐器在 index 36/37/38 上是 `(sample=1,pitch=15)`、`(2,15)`、`(3,15)`。

**关键**：其他乐器类型（VRC6=2、N163=9…）的记录里**没有**这张表，布局也不同。loader 目前遇到
`type != 1` 就停止，这个行为是对的，读表也只能写在 `type == 1` 分支内。

**一次失败的尝试（记录教训）**：按“8×12×2”实现后，真实文件音符数出现异常（TMNT 4666 → 3736），
代码已回退。事后看，验证命令**把编译错误过滤掉了**，于是“构建失败”被误读成“数据异常”。下次的验证
顺序应是：先看 `0 errors`，再看 7 首音符数是否与
`2980 / 7284 / 6637 / 7267 / 9335 / 4666 / 3385` 完全一致，最后才谈提交。

**实现顺序**：

| 步骤 | 内容 | 验证 | 风险 |
|---|---|---|---|
| 1 | `Module::DpcmSample { name, data }` + `parseDpcmSamples` | 合成模块解析出 1 个采样 | 低（新增块） |
| 2 | 把上面那个 `skip` 换成**读进** `Instrument::dpcm[8][12]` | 断言某 (oct, note) 的 sample/pitch/delta | **低**（纯加法，不影响转谱） |
| 3 | `Seq2A03Instrument::GetSample*` 改为查表 | 单元断言 | 低 |
| 4 | `CInstrument2A03::Compile` 追加 DPCM 表 | 与 FT 输出逐字节对比 | 中 |

第 2 步做完后，现有的 4/7 转谱结果不应有任何变化——这是它的验收条件。

## 7. 与 MIDI 工作的关系

两条线互不影响：

- **MIDI 转谱**：`FtmSongLoader` 直接产出 `NoteStream`，4/7 首与 FamiTracker 逐音符一致
  （见 `2026-10-24-ftm-parity-status.md`）；
- **音频**：需要编译器，与 `NoteStream` 无关。

也就是说 FTM 可以先"导出 MIDI（已可用）"，后"播放音频（本路线）"。
