# 备忘录：一次被旧二进制骗过的本地验证（v0.1.0 发布事故）

- **状态**：已修复（`a13da3b`），流程建议待落地
- **日期**：2026-10-17
- **来源**：v0.1.0 发布时 CI 在 5 个平台全部失败；本地却报过"all checks passed"
- **一句话**：`build.ps1 -Test` 只**运行已经存在的**测试二进制、缺的就跳过，不负责重新构建；旧二进制让本地的"全绿"早于被测代码，而真正的新代码在 CI 的干净构建里一跑就露馅。

---

## 1. 发生了什么

| 时间 | 事件 |
|---|---|
| 15:47 | 最后一次真正构建 `vgm_core_test.exe`（此时 M-1 还没提交） |
| 16:36 | M-1 / M-2 / L-4 提交（`c89b3b3`） |
| 16:41 | 审阅文档与混音推子备忘录提交（`38bd0c1`） |
| 16:43 | 合入 master（`acd83c1`） |
| 16:5x | 推送 master、打 tag `v0.1.0`、推送 tag，触发发布 |
| — | **本地 `build.ps1 -Test` 报"all checks passed"**，但二进制还是 15:47 的 |
| 17:0x | CI 发布运行 `39018581951`：5 个平台的 `vgm_core_test` 都停在 `Play after a restore still starts the song` |
| 17:02 | 修好测试并提交（`a13da3b`），重新构建后本地同样失败 → 修复后通过 |
| 17:17 | tag 移到 `a13da3b` 重推，发布运行 `39019628337` 全绿，release 生成 |

两件事叠在一起：

1. **测试本身写错了**（见 §2）。它从 H-1 就在仓库里，但审阅分支不触发 CI（`.github/workflows/GHA.yml` 只在 master 上跑），所以这是它第一次进 CI。
2. **本地验证不可信**（见 §3）。本地跑的是 15:47 的旧二进制，比被测代码早 49 分钟。

## 2. 测试为什么错：只渲染 master，却把声音路由到了别的总线

- 状态用例保存的是 `defaultRouting = spreadAcrossBuses`，同时把轨道 0 的输出设成 bus 2。
- 恢复时引擎按保存的路由重建，轨道 i 落到 bus i：轨道 1 → bus 1，轨道 2 → bus 2 且 muted。
- 用例里 solo 的是轨道 1，所以**唯一有声音的轨道在 bus 1**。
- 但 `renderSeconds()` 的 `OutputBusBuffers` 只提供 master 一条总线；`mixTracks()` 对"目标总线未连接"的轨道按设计静音（`PlaybackEngine.cpp:1102` 起），而 solo 又让非 solo 的轨道静音，master 上自然是 0。
- 断言 `rms > 0.0` 因此失败；`isPlaying()` 其实是 true，走带一直是对的。

**修法**（`plugin/Tests/PlaybackCoreTest.cpp:666` 附近）：检查前把 solo 的轨道放回 master，并写明这条检查测的是走带、不是路由——路由上面两条检查已经覆盖。

**结论**：不是引擎 bug，是测试对"声音从哪条总线出来"的假设错。凡是"恢复后应该有声音"的检查，都必须说清楚看哪条总线。

## 3. 为什么本地会绿：`-Test` 不构建测试

`build.ps1:432-441` 的语义：

- 构建阶段只构建 `-Target` 列出的目标（默认不含四个测试）；
- `-Test` 阶段只是**查找** `vgm_core_test.exe` 等文件，**存在就跑，不存在就打印 "skipping … (not built - add '-Target …')" 然后跳过**；
- 它从不比较二进制与源码的时间戳。

所以"`-Test` 通过"只能证明"**某个历史版本**的测试通过"。M-1 改的是 `PlaybackEngine.cpp`，测试目标没被构建，ninja 也就没机会重编。

发布工作流用的命令是带目标列表的：

```powershell
.\build.ps1 -Config Release -Arch x64 -Target vgm_core_test,vgm_ui_test,vgm_app_test -Test
```

CI 每次都从干净检出完整构建，因此它才是唯一可靠的那一票。本地增量构建 + 不带目标的 `-Test` 是最坏的组合：看起来跑了测试，其实跑的是旧程序。

## 4. 怎么避免（下次发布前照做）

1. **本地验证必须带目标**：

   ```powershell
   .\build.ps1 -Config Release -Target vgm_core_test,vgm_ui_test,vgm_app_test -Test
   ```

2. **看时间戳**：跑完测试后核对 `build\plugin\vgm_*_test.exe` 是否晚于 `git log -1 --format=%ci`。早于被测提交的二进制，结果一律作废。
3. **先让 master 的 Build Tests 绿，再打 tag**。这次是合完立刻打 tag，等 CI 失败时 release 已经建了一半（tag 已推、五条流水线已跑完四条），只能删 tag 重推。顺序应当是：推 master → 等 `Build Tests` 绿 → 打 tag → 推 tag。
4. **tag 就是快照**：如果要修，先确认该 tag 下没有任何已发布的 release，再删 tag、移动、重推；一旦有人下载过，就应改版本号（0.1.1）而不是移 tag。

## 5. 可以加但这次没加的护栏（待评估）

- `build.ps1` 在 `-Test` 时自动把测试目标加入构建列表（最直接，代价是每次 `-Test` 多编几个目标）；
- 或至少比较二进制与 `plugin/Tests`、`plugin/Source` 的最新时间，旧了就警告或拒绝运行（改动小，但要处理"测试源没变、被测源变了"的依赖传播，判断容易错）；
- 或在 `-Test` 的跳过分支里明确打印测试二进制的构建时间。

没有顺手改脚本：这轮已经在发布窗口里，脚本改动本身需要一轮验证；先记录，下一轮和 CI 检查一起做。

## 6. 决策记录

| 日期 | 结论 |
|---|---|
| 2026-10-17 | 修测试（`a13da3b`）；发布顺序改为"master CI 绿 → 打 tag"；脚本护栏待评估，不随本次发布改 |

## 7. 什么时候重开这份备忘录

- 再次出现"本地绿、CI 红"时；
- 要给 `build.ps1` 加 `-Test` 自动构建或时间戳检查时；
- 给发布流程加自动化（例如 tag 前检查 master 的 Build Tests 结论）时。
