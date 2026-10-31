# 移走之后，它还在，但不再表示任何东西

> 起因：Keyboard 页加了「导出波表」。用户点了导出，界面上 13 行波形清清楚楚，提示窗写的是
> 「没有写入任何文件：这个视图当前没有可导出的波形」。
>
> 这是同一个功能连续第二次「导出不出内容」。**两次的原因完全不同，但形状是同一个。**

## 一、第一次：看错了字段

导出循环的第一句是：

```cpp
if (channel.wave.size () < 2)
{
    continue;
}
```

`ChannelState` 用两种方式描述波形：

| 通道类型 | `wave` | 界面 | 导出 |
|---|---|---|---|
| 从内存读形状（FDS、N163） | 非空 | 直接画 | ✅ |
| 合成（方波、三角、噪声、锯齿、FM） | **空** | `waveform::cycleFor()` 合成后画 | ❌ **跳过** |

而 `wave` **全仓只有两处填**：`NsfPlayer.cpp:1055`（FDS）和 `:1112`（N163）。

**所以 2A03、VRC6、VRC7、MMC5、FME7 的每一行都被跳过了。** 界面照常显示，因为**界面有另一条路**——它自己合成。

**`wave` 不是「这个通道的波形」，而是「这个后端顺手上报的波形」。** 导出把它当成了前者。

## 二、第二次：看错了对象

修好字段之后，提示窗换了句话：「没有写入任何文件」。

`written == 0` 且 `skippedSample == 0`，意味着**循环一次都没执行**，即 `channels.size()` 是 0。**但界面正在画 13 行。**

原因是每帧的这一行：

```cpp
// ContentSlot::timerCallback
keyboardPanel->setSnapshot (std::move (snapshot));
```

**快照从这里起归 panel 所有，slot 那份被搬空了。** 而导出读的是

```cpp
const int writable = (int) snapshot.channels.size ();   // ← slot 的，此时是 0
```

**这一行是我自己在做帧时间优化时加的**（`bb046df`），本意是省掉每帧第二次深拷贝——每帧 13 个通道的名字和波形数组。优化没错，**但它留下了第二个读者。**

## 三、为什么编译器和测试都不管

**编译器**：对被 move 走的对象做任何合法操作都是合法的。`snapshot.channels.size()` 在空 vector 上返回 0——**这是正确行为，不是错误**。

```cpp
std::vector<ChannelState> moved = std::move(a.channels);
assert (a.channels.size () == 0);   // 成立，且合法
```

**moved-from 不是「无效」，是「有效且未指定」**。C++ 没有任何机制让第二个读者知道自己迟到了。

**测试**：`46/46` 全绿。导出的文件夹创建和 FileChooser 那一段**没有测试覆盖**——`juce::FileChooser` 会弹真窗口，console harness 驱动不了。

**所以在我修好字段、跑完 46/46、重建 dist 之后，它依然是坏的。** 只有用户点了一次才知道。

## 四、和「两份清单」「活过定义的变量」是同一族

| 事故 | 读者以为拿到的 | 实际拿到的 | 发现方式 |
|---|---|---|---|
| `2026-10-25-the-second-list.md` | 一份清单 | 另一份，只改了一边 | 两处不一致 |
| `2026-10-26-the-variable-that-outlived-its-definition.md` | 定义的变量 | `$null`，定义被删了 | `Set-StrictMode`（跑到就报） |
| **这次（字段）** | 这个通道的波形 | 后端顺手报的波形 | 用户点了导出 |
| **这次（对象）** | 当前快照 | moved-from 的空壳 | 用户点了导出 |

**四件事的共同结构：读者与真值之间只有惯例连接，没有类型、没有断言、没有编译期约束。**

前两件至少还会「报错」或「不一致」。**后两件什么都不报**——一个空 vector 和一个没有元素的 vector 是一回事。

## 五、规则

**1. move 之后，源对象的用途就结束了——不要让它继续可读。**

`setSnapshot (std::move (snapshot))` 之后，`snapshot` 在语义上已经是「上一次的残骸」。**要么把它做成一个只在函数内存在的局部变量，要么在 move 之后立刻 `clear()` 并说明它已空。**

**2. 「读当前状态」之前，先问一句：这个状态现在归谁。**

一条快照在同一个文件里有两个持有者（slot 和 panel），而**只有一个是被更新的**。**问所有权比读声明更快**——`snapshot` 是成员，看起来随时可读，这正是问题所在。

**3. 界面能显示，不等于导出能拿到。**

界面和导出走了**两条不同的取数路径**（panel 的副本 vs slot 的成员）。**同一个页面上的两个功能，只要读的不是同一个对象，就迟早会分叉。**

修法就是这个：导出改读 `keyboardPanel->drawnSnapshot ()`——**画面是什么，文件就是什么**。

**4. 「看起来有数据」和「确实有数据」是两件事。**

`channel.wave` 非空是**一种情况**，不是**一般情况**。**当一个字段决定「有没有」时，先 grep「谁填它」**：

```powershell
Select-String -Path plugin\Source -Pattern '\.wave =|\.wave\.push|\.wave\.assign'
```

两条结果，就说明另外三十几颗芯片走的是别的路。

**5. 一个功能连着修两次同一个症状，停下来看第二次。**

第一次修的是条件，第二次修的是数据源。**如果第一次修完就相信了，第二次的提示语会把人引向「是不是又没有波形」——而真正的原因和波形毫无关系。**

## 六、这次改成了什么

```cpp
// KeyboardPanel
/** The snapshot this panel is drawing.

	The slot moves its own copy in here every frame - that is what the rvalue overload above
	bought - so by the time a FileChooser comes back, the slot's copy is empty.
*/
const toybox::playback::ChipStateSnapshot& drawnSnapshot (void) const { return snapshot; }
```

导出那四处（`writable`、失败计数、循环上界、取通道）全部改成读它。

## 七、一句话

**两次都不是「功能没写」，而是「它看的那个东西，不是它以为的那个东西」。**

第一次看错了字段，第二次看错了对象。**而这两件事，编译器和测试都不会告诉你。**
