# 实现细节

修改代码前先读本文。以下每条都来自实机验证，而非文档转述：其中若干条官方文档没有记载，或文档描述与插件的实际行为不符。

所有结论都经过实机复现——未经复现的推测不收录，因此每项都附有可重跑的对照数据或命令。

## 这份库管什么、不管什么

管：把一个调用方渲染好的 BGRA 帧，注册成 `Stream/Output/Video` 节点，按协商帧率推给消费者。

不管：渲染、布局、字体、网络、CLI、配置。`provider` 是唯一入口，也是唯一契约面。

## 进程与线程

主线程跑 `pw_main_loop_run()`，PipeWire 的 `process` 回调也在其中执行；另有一条驱动线程按目标帧率调 `pw_stream_trigger_process()` 启动图周期。

| 线程 | 职责 |
| --- | --- |
| 主循环线程（`run()` 所在） | `process` 回调 → 调用 `provider` 填帧 → queue；`state_changed` → `onStreaming` |
| driver 线程 | 按协商帧率触发图周期 |

**关键约束**：`provider` 在主循环线程里跑，只做渲染与拷贝，**绝不进行网络访问**。需要网络/磁盘准备的资源（封面、表情、头像）由调用方自己的后台线程准备好，挂到共享指针上，渲染时只取一次引用。

没有消费者时流停在 `PAUSED`，driver 线程只在 `STREAMING` 状态下触发，`provider` 因此**一次都不会被调用**：0 帧 = 0 开销。实测（`examples/demo` + `gst-launch pipewiresrc num-buffers=10`）渲染计数恰好等于消费者拉走的帧数。

**帧率协商**：向外声明的区间是 `[max(1, N/4), N]`（N 为 `Options::fpsCap`）。driver 线程按协商值取整后推帧，并 clamp 到 `[min(5, N), N]`，避免消费者要得过低时画面冻结。代码不查 `pw_stream_is_driving()`：消费者（OBS）也在图中，谁被 WirePlumber 选为 driver 不该影响本端产帧，只要流进入 `PW_STREAM_STATE_STREAMING` 就推进图周期。

**`pw_init` / `pw_deinit` 是进程级的**：本库用引用计数包住，多个 `VideoNode` 实例共存时不会互相拆台。

## PipeWire 集成：四项硬性要求

### 1. 必须显式声明 `SPA_PARAM_Buffers`

`pw_stream_connect()` 之后需要再调用一次 `pw_stream_update_params()`，提交 `SPA_TYPE_OBJECT_ParamBuffers`。

未声明时端口上的 `Buffers` 参数为空，**PipeWire 不会分配内存**：`process` 中 `pw_stream_dequeue_buffer()` 拿到的 `spa_buffer` 全部是 `maxsize=0`，按 `stride` 写入会直接越界（ASan 报 heap-buffer-overflow / SEGV）。

声明内容：

```
size     = W*H*4                       // 一帧字节数，同时作为 range 的默认值与最小值
stride   = W*4
buffers  = [4, 2, 16]                  // 默认 / 最小 / 最大
blocks   = 1
dataType = 1 << SPA_DATA_MemFd         // 位掩码，不是枚举值
```

定位方法：用 `gst-launch-1.0 ... ! pipewiresink mode=provide` 构造一个**已知可用**的生产者，再 `pw-dump` 导出其端口参数逐项对比——参照对象的 `Buffers` 有值，自建节点为空。

### 2. 使用 `PW_STREAM_FLAG_TRIGGER`，不要叠加 `PW_STREAM_FLAG_DRIVER`

输出流不会自动调度，必须自行调用 `pw_stream_trigger_process()` 启动图周期。`pw_loop_add_timer` 实测无效，因此改用独立线程，也就是官方文档推荐的辅助线程做法。

**叠加 `DRIVER` 会静默失效**，症状极具迷惑性：

| | TRIGGER | TRIGGER + DRIVER |
| --- | --- | --- |
| 节点属性 | 正常 | 更完整（`node.driver=true`） |
| 节点状态 | `suspended`（无消费者时） | `running` |
| OBS 日志 | 正常 | 同样报 `state: "streaming"` |
| 格式协商 | 正常 | 同样正常 |
| **实际帧投递** | **正常** | **0 帧，`process` 回调一次都不触发** |

结论由「以 OBS 为消费者做 A/B 对比」得出。遇到「OBS 里连上了但一片空白」，第一项就查这个。

同理，**不要先判断 `pw_stream_is_driving()` 再触发**；只要流进入 `PW_STREAM_STATE_STREAMING` 就推进即可。消费者是否在图中、由谁担任 driver（WirePlumber 决定）都不该影响本端产帧。

### 3. 需要 `PW_STREAM_FLAG_MAP_BUFFERS` 才能拿到可写的 `datas[0].data`

不加这个标志时 `data` 指针不可用。

### 4. `media.role` 必须为 `"Production"`

否则 obs-pwvideo 的下拉框里**看不到该节点**，而且插件**连日志都不打**——它只在过滤通过之后才打印 `Found new target`，现象看上去就像「进程没在运行」。

反汇编 `obs-pwvideo.so` 的 `on_registry_global_cb` 可以确认它硬编码了三个 `strcmp`：

```
media.type  == "Video"
media.class == "Stream/Output/Video"
media.role  == "Production"      ← 差这一项就完全不显示
```

复现命令（把 .rodata 中的字符串偏移换算为虚拟地址，再从反汇编里定位引用点）：

```bash
SO=~/.config/obs-studio/plugins/obs-pwvideo/bin/64bit/obs-pwvideo.so
objdump -d --no-show-raw-insn "$SO" | grep -B20 -A25 "$(printf '%x' $((0x$(readelf -SW "$SO" | awk '$2==".rodata"{print $4}') + $(grep -abo 'Stream/Output/Video' "$SO" | head -1 | cut -d: -f1) - 0x$(readelf -SW "$SO" | awk '$2==".rodata"{print $5}'))))"
```

这条结论没有任何文档来源：做法是对照一个「确认能被列出」的节点（用户虚拟形象的 spout2pw 节点，`media.role = Production`）逐项 diff 属性。

**上游的版本差异**：当前上游 `plugin-main.c` 的 `on_registry_global_cb` 对 `media.role` 是「等于 `Production` **或缺失**」都放行（注释写明是为向后兼容），只有 `media.type` 与 `media.class` 是严格相等。本库一律显式写入 `Production`，所以在任何插件版本上都能被列出；不要依赖「缺失也能通过」这条兼容行为。

**另外**：插件是在**打开对话框的那一刻**新建 registry 枚举一次节点的，之后不会刷新已经打开的窗口。所以「节点不在」要先确认进程真的在跑。

#### 下拉框里显示的是哪个字段（`nodeDescription` vs `nodeName`）

结论：**显示 `node.description`（即 `Options::nodeDescription`），连接时用的是 `node.name`（即 `Options::nodeName`）。** 名字与取值不是同一个字段，改参数时别混。

依据是上游 `plugin-main.c` 的 `populate_target_list()`：

```c
obs_property_list_add_string(list, tgt->friendly_name, tgt->node_name);
```

`friendly_name` 依次取 `node.description` → `node.nick` → `node.name`，所以显示名优先是描述。两个补充事实：

- 同名节点（多个进程共用同一个 `node.name`）：标签变成 `friendly_name (id)`，取值变成 `#serial`，用来唯一区分。
- 打开属性窗口时插件会把标签与取值打进 OBS 日志，可直接查：

```bash
grep -E 'Add string|Found new target' ~/.config/obs-studio/logs/*.txt | tail
# 本机实测：[pwvideo] Add string Demo Node pwnode-demo
```

## 调试方法

这套东西出问题大多是**静默失败**（没画面但状态全对、节点不显示但日志不报），所以下面的手段比读代码有用：

```bash
# 节点在不在、属性对不对
pw-dump | grep -A20 <nodeName>

# 链路有没有建立
pw-link -l | grep -B1 -A1 <nodeName>

# 图有没有真的在跑（注意：对非 driver 节点这项显示不一定准）
pw-top -b -n 1

# 造一个「已知能工作」的参照生产者，逐项 diff
gst-launch-1.0 videotestsrc is-live=true ! \
  video/x-raw,format=RGBA,width=320,height=240,framerate=30/1 ! \
  pipewiresink mode=provide client-name=ref-node

# 当消费者，把真实帧落盘验证（尺寸 = W*H*4）
gst-launch-1.0 -q pipewiresrc target-object=<nodeName> num-buffers=5 ! \
  video/x-raw,format=BGRA ! filesink location=/tmp/f.raw

# OBS 侧看插件到底枚举到了什么
grep -E '\[pwvideo\]|\[pipewire\]' ~/.config/obs-studio/logs/$(ls -t ~/.config/obs-studio/logs/ | head -1)
```

### 验证帧内容是否正确的两个不变量

比肉眼可靠：

1. **旋转不变性**（适用于任何绕中心旋转的绘制）：旋转是保面积映射，圆形区域内的**平均颜色在任何角度下都必须相同**。通道被打飞会让均值剧烈漂移。
2. **角度 0 的恒等性**：不旋转时输出应与源图逐字节一致。

### 测量注意事项

- **测 CPU 别用日志里的推帧计数**——那是每 60 帧打一行，取样会滞后。数消费者实际收到的帧数除以精确时长才准。
- **改热循环前先写微基准**。凭直觉会写出慢 25% 的「优化」。
