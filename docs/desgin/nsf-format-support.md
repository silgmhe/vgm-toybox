# NSF 格式支持：解析层与三个真实缺陷

- **状态**：已实现（`feature/nsf-format-multiinstance`）
- **日期**：2026-10-24
- **相关**：`nsf-multiinstance-exploration.md`（隔离方案）、`../memo/2026-10-24-nsf-shelved.md`（放弃与重启）

本文只讲**解析层**：怎么读一个 NSF、读错过什么、为什么错、以及这些错误为什么能连续躲过测试。播放与混音见隔离方案那一篇。

## 1. 解析层由四块组成

| 文件 | 职责 |
|---|---|
| `formats/nsf/NsfFile.{h,cpp}` | 头解析：魔数、版本、曲目数、加载/初始化/播放地址、速度（PAL 位）、芯片位、三个文本字段 |
| `backends/nsf/NsfBus.{h,cpp}` | `xgm::NES_MEM` 子类：bank 窗口、声音寄存器写转发与计数、扩展芯片地址段 |
| `backends/nsf/NsfPlayer.{h,cpp}` | 总线 + CPU + 芯片的组装，逐帧驱动、逐声道渲染、seek |
| `backends/nsf/NsfBackend.{h,cpp}` | `ISongBackend` 适配：轨道表、芯片表、能力声明、未声明芯片探测 |

## 2. 缺陷 A：v1 的加载字段被当成了“大小”（`Entry025.nsf`）

### 症状

播放时**只有一段直流**（`peak == rms == 0.0585`），三角形声道有电平但听不见，其它声道全静音；探针显示 `writes=0`（五秒内一次声音寄存器都没写）。

### 我当时写下的、从未验证的断言

```cpp
// NsfFile.cpp  —— 修复前
// Version 1 stores the program's size in the load field and always loads at $8000;
if (file.version == 1)
{
    file.loadAddress = 0x8000;
}
```

**这句话是错的。** NSF 头 `0x08` 在**所有版本**里都是加载地址，`0` 才是 `$8000` 的简写。

### 这个文件为什么会暴露它

```
load $8AFF   init $AA55   play $AA58   rom 14,529
```

语料里另外三首恰好都声明 `$8000`，所以这个错误对它们**完全没有副作用**。而这个文件被整体平移了 `$2FF`：

```
$AA55 处本该执行 4C 88 AA（JMP $AA88）
实际执行了 $8000 处的 BD C9 0A（别人的代码）
```

### 定位过程（四次测量，前三次都没找对）

| 测量 | 结果 | 结论 |
|---|---|---|
| 计数声音寄存器写 | `writes=0` | 只说明“没写”，不说明为什么 |
| 计数 `$0200-$02FF` 写（自解码页） | 每秒 +120 | 误导：以为是“解码器很慢” |
| 统计每帧执行的 CPU 时钟 | 1,828,571/秒 | 排除“CPU 饥饿” |
| `render()` 内采样 PC | 100% 落在 `$4100` 页 | 排除“init 在跑但慢” |
| 在 `Start()` 内采样 init 的 PC | 只执行了 **10 条**指令，之后到哨兵 | 排除“驱动逻辑问题” |
| **单步并把操作码与文件对照** | `$AA55` 处执行 `BD`，文件里是 `4C` | **镜像装错了地址** |

最后一次是决定性的。前五次都在回答“驱动做了什么”，而真正的问题在“**驱动根本没被送到该去的地方**”。

### 修复

```cpp
const std::uint16_t declaredLoad = readLE16 (bytes + offsetLoadAddress);
file.loadAddress = declaredLoad != 0 ? declaredLoad : 0x8000;
```

## 3. 缺陷 B：bank 窗口以 `$8000` 为基准，忽略加载地址（`1983.nsf`）

### 症状

完全静音（`writes=0`，常量直流），PC 始终停在 `$8000` 页。

```
ver 1   load $8080   init $8080   play $8084   rom 89,984   chips 0x00
```

三个特征叠加：加载地址 **非 4KB 对齐**（`$8080`）、程序 **> 32 KB**（走 bank 分支）、bank 表全 0。

### 根因

```cpp
// NsfBus.cpp —— 修复前
const std::uint32_t offset = banks[window] * 0x1000u + (adr & 0x0FFFu);
```

这个公式假设 ROM 从 banked 空间的 4KB 边界开始。`$8080` 意味着首字节落在窗口 0 的 `$80` 偏移上，于是**每个窗口都整体偏移 `0x80`**：

- `$8080` 应读 `rom[0x00]`，实际读 `rom[0x080]`；
- `$8000`（本应未映射）实际读 `rom[0x000]`。

### 修复

```cpp
const std::uint32_t bankBase = (std::uint32_t) (file.loadAddress & 0x0FFF);
const std::uint32_t offset = banks[window] * 0x1000u + (adr & 0x0FFFu) - bankBase;
```

`bankBase` 为 0 时公式退化为原样，所以三首 `$8000` 的文件数值**完全未变**——这既是修复的正确性证据，也说明这个缺陷同样被语料的同质化掩盖了。

### 为什么 A 的修复没有覆盖 B

两者在不同层：A 在 `NsfFile`（镜像装到哪），B 在 `NsfBus`（bank 窗口从哪算）。修好 A 之后 `1983` 的镜像位置对了，但 bank 读取仍然偏移 `0x80`。

## 4. 缺陷 C：标签显示（同日文文件相关，与播放无关）

`1983.nsf` 的标题在 Audio Overload 里显示为 `1983◆▲◆W◆◆◆◆◆`。那不是损坏：

```
name   cp932 -> 1983年の夏休み      artist/copyright -> 低級ジャム
```

两个独立问题：

1. **编码**：头部的三个文本字段是 Shift-JIS，而我们直接把它们当 UTF-8 交给 `juce::String`；
2. **键名**：后端发布 `tags["title"]`（小写），卡片读 `getTag("TITLE")`（大写，与 libvgm 后端一致）——所以**卡片是空的**，连乱码都没有。

修复：

- `decodeTextField()`：纯 ASCII 原样返回（CP932 是 ASCII 超集，语料四首不受影响）；已是合法 UTF-8 则原样返回；其余走 `MultiByteToWideChar(932)`，平台无此 API 时退回原始字节；
- 键名改为 `TITLE` / `ARTIST` / `COPYRIGHT`，并补 `SYSTEM = "Nintendo NES"`；
- 卡片新增“版权”一行，中英文案同步。

## 5. 为什么这些缺陷能连续躲过测试

| 原因 | 具体表现 |
|---|---|
| **语料同质化** | 四首测试文件全部声明 `load = $8000`、全部不 banked、全部纯 ASCII。三条路径的边界条件一个都没被覆盖 |
| **测试把 bug 写成了规格** | `check (file.loadAddress == 0x8000, "version 1 always loads at $8000, whatever the field says")` —— 这条断言在修复后应当失败，却因为**二进制过期**一直是绿的 |
| **过期产物谎报绿灯** | `vgm_nsf_test.exe` 09:13 编译，`NsfFile.cpp` 12:13 修改；重新编译后立刻暴露 3 条失败。同一问题本轮出现两次（另一次是测试带着未提交的探针调用已回退的 API） |

## 6. 现在的覆盖与验证

```bash
ctest --test-dir build                                        # 35/35
build/plugin/vgm_nsf_probe.exe test_file/nsf/Entry025.nsf     # writes 应 > 0，peak 应非直流
build/plugin/vgm_mixer_test.exe test_file/nsf/1983.nsf 1      # 有声，5 轨
```

`NsfFormatTest` 现有断言覆盖：头字段逐项、1-based 曲目、PAL 位、**非对齐加载地址（`$8AFF`）**、**Shift-JIS 标题转 UTF-8**、bank 窗口切换、跳板在镜像装载后仍然有效。

## 7. 以后再加解析字段的检查清单

1. **这个字段的语义，是查到的还是想当然的？** 查到的请在注释里写明出处；
2. **语料里有没有一个“非默认值”的样本？** 没有就造一个——否则新分支永远是死代码；
3. **单测断言的是规格，还是当前实现的输出？** 后者会在修复时变成“必须失败的绿灯”；
4. **改动后测试二进制真的重编了吗？** 比对可执行文件与源文件的修改时间；
5. **同一字段是否有多条读取路径**（镜像装载 / bank 读取 / 缓存副本）？改一处不够。

## 8. 教训

> 我在 `NsfFile.cpp` 里写下的那句注释——“v1 把大小放在加载字段里”——是一个我从未验证过的断言，而它决定了整个镜像的装载位置。它躲过了三首测试文件、躲过了 35 个单测，因为它们恰好都用 `$8000`。

**语料覆盖不到的分支，等于没有测试。** 而一句语气笃定的注释，会让后来的人（包括写它的自己）不去验证它——这一轮为了找到它，先后做了六次测量，前五次全部指向错误的层。
