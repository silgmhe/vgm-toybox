# 默认值散在五个地方（2026-11-09，设置层）

**起点**：`MidiSequence.h` 里有一个 `defaultPitchBendRange = 2`。用户问：全局设置与默认值不是都该在
`SettingsStore` 吗？改一处、忘另一处不就出 bug？

## 盘点：同一个概念，五种落点

| 落点 | 例子 | 谁读 |
|---|---|---|
| `settings/KeyboardSettings.h` | `defaultNoteHoldMs`、`defaultMiddleCOctave` | Store、设置页 |
| `settings/NsfSettings.h` | `defaultDurationSeconds`、`defaultOutputGainDb` | Store、设置页 |
| `settings/ExportSettings.h` | `defaultRetentionHours`、`defaultCleanupEnabled` | Store、设置页 |
| `SettingsStore` 的类内静态 | `defaultRefreshHz`、`defaultExportBits/Seconds` | Store、设置页 |
| `SettingsStore.cpp` 的字面量 | `getBoolValue (kReuseWindowKey, true)` 等 11 处 | Store 自己 |
| **设置层之外** | `playback::defaultPitchBendRange`（`MidiSequence.h`） | 编码器 clamp、引擎初值、设置页 |
| **重复的数值** | `NsfBackend.cpp` / `NsfPlayer.h` 各写一次 `180.0` | NSF 后备时长、探针扫描 |

不是"已经有两个值不一致"（那时值恰好都对），而是**五种机制**：改一处必须记得改另外四处，而没有任何
编译期约束。`180.0` 那两处是真重复：设置默认值若从 180 改成别的，NSF 后端与探针扫描仍会说 180。

## 什么该进表，什么不该

1. **用户可改的偏好**进表：有 key、有默认、有边界/步长，就该在一处声明。
2. **协议/硬件事实**不进表：8 kHz 参照率、ADPCM 步表、VGM block 类型、MIDI 音符号范围——它们不是偏好，
   跟着会它们的代码走。

## 改法

新建 `settings/SettingsDefaults.h`，按命名空间分组（`midi` / `keyboard` / `view` / `nsf` / `exporting`
/ `window` / `audio`），把上面 1 类全部收进来：

- 三个子头文件删除（名字与内容一起并进来，调用点的 `settings::keyboard::...` 等**不需要改**）；
- `SettingsStore` 的 6 个静态常量删除，改读表；
- 11 处字面默认值改成具名常量（`window::reuseWindow`、`keyboard::defaultStartNote`、`view::contentIndex` 等）；
- 弯音范围（1..24、默认 2）从 `playback/MidiSequence.h` 移到 `settings::midi`：**编码器仍在 clamp**，
  但范围与默认值只有一处；`MidiSequence.h` 直接 include 这个叶子头（无依赖，不产生 playback → 设置存储的
  反向依赖）；
- `PlaybackEngine` 的 `midiPitchBendEnabled { true }` 初值改读表；
- NSF 的 `180.0` 两处改读 `settings::nsf::defaultDurationSeconds`。

## 边界与验证

- 依赖方向：`SettingsDefaults.h` 是**无 include 的叶子头**，谁都能引用，不会出现循环；
- 定义唯一性：脚本核对 19 个默认值，**每个都只有一处 `constexpr ... =`**；
- 行为不变：设置页脚本（默认 2、改 7、改 12、关闭灰化）全过；`46/46`、芯片与 NSF 断言 PASS；
- `SettingsStore.h` 的类注释补了一句：**key 与读取在 store，默认值在表**，两处不互相复述数字。

## 教训

规则第 13 条（一份数据一个来源）此前只被用来找"两个写者"。默认值给出的是它的另一种形态：
**不是两个写者，而是同一种事实散在五种存储机制里**——没有一处能回答"这个偏好的默认值是多少"。
`180.0` 那两处说明代价是具体的：改了默认时长，NSF 的另外两条路径不会跟着变，而且不会报错。
