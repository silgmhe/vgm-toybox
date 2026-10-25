# 变量的定义没了，用它的人还在

> 起因：移除 `vgmplayer/` 时改了 `build.ps1`，删掉复制命令行播放器的那一段——连同段首的
> `$exe = Get-ChildItem ...` 一起删了。后面「打印结果」和「检查依赖」两段还在用 `$exe`。

## 一、发生了什么

`build.ps1` 里原本有这么一段：

```powershell
$exe = Get-ChildItem -LiteralPath $BuildDir -Filter 'VGMPlay*.exe' -File -Recurse | ...   # 定义
Copy-Item -LiteralPath $exe.FullName -Destination $DistDir -Force                          # 使用
```

删除 `vgmplayer/` 时，这一段整个被替换成一行注释。**`$exe` 的定义随之消失。**

而在六十行之后：

```powershell
Write-Host ("executable : {0} ({1:N0} bytes)" -f $exePath, (Get-Item -LiteralPath $exePath).Length)
$dependencies = & dumpbin /nologo /dependents $exePath | ...                                # 还在用
```

**删的是定义，用的是另一段代码。**

## 二、为什么写的时候没发现

`build.ps1` 第 131 行就是：

```powershell
Set-StrictMode -Version Latest
```

**这项检查一直是开着的**，而它确实抓到了——报的正是：

```
VariableIsUndefined: 'exe' 变量未定义
```

**但它只在执行到第 586 行时才抛出。** PowerShell 没有编译期，`Set-StrictMode` 没有别的地方可以检查。

而第 586 行位于「构建成功之后、打印结果之时」。也就是说：

| 时间点 | 状态 |
|---|---|
| 保存文件 | 一切正常 |
| 配置阶段 | 一切正常 |
| **编译三分钟** | 一切正常 |
| 打印结果 | **炸** |

**最坏的一种失败位置**：它不在开头，不在结尾，在一个人已经等了三分钟、以为成功的时候。

## 三、谁发现的

**我自己跑了一次 `build.ps1`。**

不是靠读代码，不是靠搜索，是靠执行。如果那一次没跑，它会等到用户第一次构建才出现——而那时他刚把 `vgmplayer/` 的移除当成一件已完成的事。

## 四、和「两份清单」是同一个病的两面

`2026-10-25-the-second-list.md` 记的是同一件事被**写了两遍**：改一处、漏一处。

这次是反过来：

| 事故 | 结构 | 方向 |
|---|---|---|
| N163 芯片 / 光晕 / 测试目标 | 同一事实写在两处，只改一处 | **改动方漏了消费方** |
| **这次** | 同一条变量的定义与使用分居两段，删了一处 | **删除方漏了消费方** |

**都是「一件事分散在多个地方」，只是一个发生在写，一个发生在删。**

删除比修改更容易漏，因为**被删掉的代码不会留下任何痕迹**——你 grep 一个名字，找到的永远是还在的那些；找不到的那些，恰恰是删掉的。

## 五、规则

**1. 删东西之前，先搜它的名字，不是只看它的定义。**

```powershell
Select-String -Path build.ps1 -Pattern '\$exe\b'
```

一条命令就能看到全部使用点。**删定义时看到六个使用点，就知道要一起处理。**

**2. `Set-StrictMode` 只能抓「执行到」的路径。**

没跑到的分支不会报错，而 CI 跑的分支和本地的未必相同。**开着的检查不等于有效的检查**——它只是让「跑到就报」变成「跑到一定报」。

**3. 改 `build.ps1` 之后至少跑一次。**

它是这个仓库唯一的构建入口，却没有测试。`-NoDeploy` 加一次已经构建过的树，几秒钟就能走完到变量使用的那一段——**这是唯一能让第 586 行执行的办法**。

**4. 中间变量跟着它的来源一起改名。**

`$exe` 属于「命令行播放器的可执行文件」这个概念。这个概念没了，`$exe` 就该跟着没。**现在它叫 `$exePath`，且来源是 `$standaloneApp`**——名字和它代表的东西重新对上了。

## 六、这次改成了什么

```powershell
# The standalone application is the only executable this repository produces now that the
# command-line player is gone - and with -NoDeploy there may not even be that.
$exePath = if ($standaloneApp) { $standaloneApp.FullName } else { $null }
if ($exePath) {
    Write-Host ("executable : {0} ({1:N0} bytes)" -f $exePath, (Get-Item -LiteralPath $exePath).Length)
}
```

依赖检查那一行也加了同样的保护：

```powershell
if ($dumpbin -and $exePath) {
```

**因为 `-NoDeploy` 是一个合法用法，而它现在意味着「没有可执行文件」**——这不是错误，只是没有东西可打印。原来那种写法把「没有」当成了「必须有」。
