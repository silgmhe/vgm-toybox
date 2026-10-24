# seek 为什么会卡，以及怎么把它挪到后台

- **状态**：已实现并验证（`c1fb3e8`、`e50aec7`、`2c9fcb5`）
- **日期**：2026-10-24
- **分支**：`feature/nsf-format-multiinstance`
- **现象**：`fod_hopeless.nsf` 拖动进度条会卡住，有时无响应；`fod_snowburnb.nsf` 正常

## 1. 先分清两件事：路径与成本

用户能感觉到的"卡"，是**消息线程被占用**。而 seek 的耗时分两部分：

| 部分 | 内容 | 是否阻塞 UI |
|---|---|---|
| 路径 | 谁在调 seek、在哪个线程 | 是（同步时） |
| 成本 | 一次 seek 要做多少模拟 | 决定卡多久 |

NSF 的答案是"成本极大"：它没有可跳转的状态，**seek 只能从头重放**。而多实例隔离下，"一次 seek"其实是"每个轨道各一次重放"。

## 2. 第一步：确认成本量级

用两个已有的测量手段，不需要新写代码：

```
单实例（探针，无扩展芯片）   seek 到 90 s ≈ 0.05 s      ← 便宜得误导
引擎内整体（混音验收程序）    8 轨  8.3 s / 13 轨 46.4 s  ← 真实的量级
```

探针的数字为什么骗人：`vgm_nsf_probe` 直接构造 `NsfPlayer`，**不经过后端**，因此没有 N163；而引擎里每个实例都带着扩展芯片。

结论：成本 ≈ 曲目位置 × 轨道数 × 单实例模拟速度，13 轨时是**几十秒量级**。

## 3. 第二步：把 13 次重放并行化（`c1fb3e8`）

13 个播放器彼此独立（各自一份模拟，区别只在 `SetMask` 掩码），所以可以同时重放：

```cpp
const size_t workers = jlimit (1, (int) trackPlayers.size (), SystemStats::getNumCpus ());
std::atomic<size_t> next { 0 };

// 每个 worker 反复取下一个索引，直到取完
const size_t index = next.fetch_add (1);
```

全过程持有引擎锁，音频线程的 try-lock 失败 → 渲染静音，不会读到"seek 到一半"的播放器。

| 文件 | 改前 | 改后 |
|---|---|---|
| `fod_snowburnb`（8 轨） | 8.3 s | 4.1 s |
| `fod_hopeless`（13 轨） | 46.4 s | 13.9 s |

落点仍是 `90.00 s`，`spread 0.0000 s`。**但消息线程仍然被占住 13.9 秒的相当一部分**——只是变短，不是消失。

## 4. 第三步：挪到后台（`8a911e4`，经 `e50aec7` 恢复）

新增一个接口，而不是改现有语义：

```cpp
void setPositionSeconds (double);          // 同步：seek 完立刻读播放器的地方依赖它
void requestPositionSeconds (double);      // 异步：发布请求即返回
```

只有 UI 的进度条改用后者：

```cpp
// TransportPanel.cpp
engine.requestPositionSeconds (seconds);
```

工作线程：

```cpp
seekCv.wait (ul, ...);                     // 等请求或退出
seekRequested.store (false);               // 先清标志，期间的请求成为下一轮而不是丢失
{ const ScopedLock sl (lock); seekAllPlayersTo (target); positionSeconds.store (target); }
```

两处细节决定了它是否"看起来正确"：

1. **请求时立刻发布位置**——UI 画的是用户拖到的数字，它无法知道后台进度；
2. **seek 期间 `render()` 不发布位置**——否则进度条会先跳回旧位置再跳到目标。

实测（`fod_hopeless`，13 轨）：

```
async seek: returned in 0.0 ms, settled after 797 ms (wall clock)
```

## 5. 期间的误判（这段比代码值得记）

改完后 `ctest` 出现 **18 个 `engine.*` 失败**。我判断是异步 seek 破坏了播放，`git revert` 了整个提交。

真实原因两条，**都与异步 seek 无关**：

1. **测试二进制没有重新编译**，还带着一份未提交的探针代码，而它调用的 `requestPositionSeconds` 刚被回退；
2. 那份探针写死"seek 到 60 秒"，而 `minimal.vgm` 只有约 **10 秒**——引擎钳制到曲长，播放器永远到不了 60，断言必然失败。

修法：目标改为 `min(60, duration × 0.5)`，即"曲长的一半，上限一分钟"。

教训：**出现大量失败时，先确认失败的是"新代码"还是"旧产物"**，再决定回退。回退一个正常的改动，比多花十分钟定位更贵。

## 6. 仍然存在的限制

| 限制 | 说明 | 是否可解 |
|---|---|---|
| `getTrackPositionSeconds()` 同锁 | seek 期间逐轨查询位置会等这一次重放 | 可以：改用原子发布 |
| seek 期间静音 | NSF 13 轨约 0.8 s，能听出一个停顿 | 可以：淡出/淡入，或继续播旧位置 |
| 成本 ∝ 位置 × 轨道数 | 拖到越靠后越慢，与轨道数线性 | 只有单实例方案能根治，已证伪 |

进度条本身不受第一条影响：它读的是 `getPositionSeconds()`（原子）。

## 7. 对 VGM/VGZ 的影响

路径统一，成本不同：

| | VGM/VGZ | NSF |
|---|---|---|
| seek 实现 | 重放寄存器命令流，不产生音频 | 重跑 6502 驱动与芯片 |
| 到 90 秒 | 毫秒级 | 0.7–0.9 s（13 轨） |

所以 VGM 的用户体验**基本无变化**（本来就不卡），但代码路径一致，将来不会再因为"某个格式慢"而重新发明一次。DAW 同步走的是 `setPositionSecondsLocked()`，保持同步语义不变。

## 8. 验证

```bash
ctest --test-dir build                                  # 35/35
build/plugin/vgm_mixer_test.exe test_file/nsf/fod_hopeless.nsf 1
```

该文件应打印 `async seek: returned in 0.0 ms, settled after <1 s`、`seek to 90.00 s -> position 90.00 s, spread 0.0000 s` 与 `Result : all checks passed`。

## 9. 若还要继续优化

1. **淡出/淡入**：把 0.8 秒的静音变成一次短淡出，听感上更像"跳转"而不是"断音"；
2. **原子化逐轨位置**：让电平表在 seek 期间也能读，不必等锁；
3. **跨实例共享状态**：13 个实例的模拟状态在任一瞬间完全相同——理论上可以只算一份再分发，但需要设备级的状态序列化，NSFPlay 没有提供。这条一旦成立，多实例的代价会接近单实例，是唯一值得投入的方向。
