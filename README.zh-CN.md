# pw-video-simple-interface

[English](README.md) · [简体中文](README.zh-CN.md)

把你渲染好的一帧，直接作为 **PipeWire 视频节点**发布成 OBS 的源。

没有浏览器、没有 CEF、没有 HTTP、渲染侧不需要装任何插件：一个很小的 C++ 静态库，注册
`Stream/Output/Video` 节点，并在 [obs-pwvideo](https://github.com/tasokait/obs-pwvideo)
每拉一帧时回调你。它是
[pw-mpris-visualcard](https://github.com/zlinux-live-util/pw-mpris-visualcard) 的输出侧，
被单独拆出来是因为难点从来不是渲染，而是 PipeWire 那四条**会静默失败**的硬性要求。

## 为什么单独存在

一个视频节点要能被 OBS 列出并真的投递帧，取决于四件事；它们出问题时没有任何报错：
节点属性全对、状态显示 `streaming`、格式协商成功，画面就是黑的。四条都已在库内解决，
细节见 [docs/internals.md](docs/internals.md)：

| 要求 | 缺失时的症状 |
| --- | --- |
| 显式声明 `SPA_PARAM_Buffers` | 缓冲区 `maxsize=0`，越界写 / 段错误 |
| 用 `PW_STREAM_FLAG_TRIGGER` 且**不加** `PW_STREAM_FLAG_DRIVER` | 处处报成功，实际投递 0 帧 |
| `PW_STREAM_FLAG_MAP_BUFFERS` | `datas[0].data` 不可写 |
| `media.role = "Production"` | 节点永远不出现在 OBS 下拉框，插件连日志都不打 |

## 接口

公开面只有一个回调和一份配置：

```cpp
#include "pwvideo.hpp"

pwvideo::Options opt;
opt.width = 460;  opt.height = 690;
opt.fpsCap = 30;
opt.nodeName = "my-overlay";        // OBS 实际连接的名字
opt.nodeDescription = "My Overlay"; // OBS 下拉框里显示的名字
opt.appName = "my-overlay-daemon";
opt.onStreaming = [](bool on) { /* 接入时重置动画时间基等 */ };

pwvideo::VideoNode node(opt, [](uint8_t* dst, int stride, int w, int h) {
  // 按给定 stride 填 w×h 的 BGRA（预乘 alpha）
});

node.start();
node.run();   // 阻塞，直到 quit() 或 SIGINT/SIGTERM
```

| 方法 | 说明 |
| --- | --- |
| `start()` | 连接 PipeWire 并注册节点，失败抛 `std::runtime_error` |
| `run()` | 阻塞跑主循环；`process` 回调就在这条线程上 |
| `quit()` | 让 `run()` 返回，可从任意线程调用 |
| `stop()` | 停驱动线程并释放资源，析构会自动调用。若 `run()` 正在另一条线程上跑，必须先 `quit()` 并 join 它——PipeWire 要求 stream 的销毁发生在主循环线程上 |
| `streaming()` | 当前是否有消费者在消费 |

回调契约（同样写在头文件里，因为它**就是**接口）：

- 在 PipeWire 主循环线程执行，不在你的线程里——共享状态要么加锁，要么传只读快照。
- 必须写满 `stride × h` 字节的预乘 alpha BGRA。
- 不许联网、不许阻塞、不许频繁分配：这是每帧热路径。
- **没有消费者时一次都不会被调用**——0 帧就是 0 开销，别假设它定期跑。
- 收到的 `w`/`h` 是**协商后**的尺寸，可能与构造时给的相同也可能不同。

## 构建

```bash
make        # -> libpwvideo.a + demo
make run    # 跑示例节点
```

依赖只有发行版系统库：`g++` 与 `libpipewire-0.3`（外加 `pkg-config`）。

## 验证

```bash
# 节点在不在、obs-pwvideo 过滤的那三个属性对不对
pw-dump | grep -A20 pwnode-demo

# 帧是不是真的在流：落盘字节数必须等于 W*H*4*帧数
gst-launch-1.0 -q pipewiresrc target-object=pwnode-demo num-buffers=10 \
    ! video/x-raw,format=BGRA ! filesink location=/tmp/f.raw
```

OBS 侧用 obs-pwvideo 的 **PipeWire Video** 源：下拉框显示 `nodeDescription`，连接用的是
`nodeName`。源的宽高设成与 `Options::width`/`height` 一致；alpha 原样透传。

## 作为子模块使用

```bash
git submodule add https://github.com/zlinux-live-util/pw-video-simple-interface.git \
    lib/pw-video-simple-interface
```

消费方用**自己的编译参数**直接编 `lib/pw-video-simple-interface/src/*.cpp`（同一套编译
单元、没有 ABI 要追），再加 `-I lib/pw-video-simple-interface/src`。也可以就地构建子模块
自己的 `libpwvideo.a`，但那样会在子模块目录里留下 .o 文件——更好的做法是把源码编进消费方
自己的构建树。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `src/pwvideo.hpp` | 公开接口与回调契约 |
| `src/pwvideo.cpp` | PipeWire 节点、驱动线程、帧率协商、缓冲区处理 |
| `examples/demo.cpp` | 最小节点示例（画一根移动的亮条），上面的验证命令就用它 |
| `docs/internals.md` | 四条硬性要求、各自怎么定位出来的、调试命令 |
| `Makefile` | `libpwvideo.a` + `demo` |

## 范围

本库**刻意不包含**渲染、布局、文字、HTTP 和 CLI。那些属于调用方：音乐卡片和弹幕悬浮层
在那一层没有可共享的抽象。

## 贡献

无论人还是模型写的补丁，都按证据审，不按作者审：

- **行为改动**要给命令和它的输出。
- **性能结论**要给微基准和实测数据；没有复现路径的数字不进文档。
- **[docs/internals.md](docs/internals.md) 里的约束不是可商量项**，除非先有 A/B 证据——
  那四条每一条都花了实打实的调试时间，而且都是静默失败。
- **说明工作是怎么产出的**（建议而非要求），方便回溯。

## 许可

MIT，版权 ZokuTe（自 2026 年）。PipeWire 通过动态链接使用，仍遵循其自身许可
（MIT、LGPL-2.1-or-later）。
