# pw-video-simple-interface

[English](README.md) · [简体中文](README.zh-CN.md)

一个很小的 C++ 静态库：把调用方渲染好的一帧发布成 **PipeWire 视频节点**，在 OBS 里通过
[obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 选用。它负责注册节点、处理缓冲与帧率
协商，并在消费者每拉一帧时回调。渲染、布局、文字与网络 I/O 都留在调用方。

## 接口

```cpp
#include "pwvideo.hpp"

pwvideo::Options opt;
opt.width = 460;  opt.height = 690;
opt.fpsCap = 30;
opt.nodeName = "my-overlay";        // OBS 实际连接的名字
opt.nodeDescription = "My Overlay"; // OBS 下拉框里显示的名字
opt.appName = "my-overlay-daemon";
opt.onStreaming = [](bool on) { /* 消费者接入 / 断开 */ };

pwvideo::VideoNode node(opt, [](uint8_t* dst, int stride, int w, int h) {
  // 按给定 stride 填 w x h 的 BGRA（预乘 alpha）
});

node.start();
node.run();   // 阻塞，直到 quit() 或 SIGINT/SIGTERM
```

| 方法 | 说明 |
| --- | --- |
| `start()` | 连接 PipeWire 并注册节点，失败抛 `std::runtime_error` |
| `run()` | 跑 PipeWire 主循环；帧回调就在这条线程上执行 |
| `quit()` | 让 `run()` 返回，可从任意线程调用 |
| `stop()` | 停驱动线程并释放资源，析构会自动调用。若 `run()` 正在另一条线程上跑，先 `quit()` 并 join 它 |
| `streaming()` | 当前是否有消费者在消费 |

帧回调的契约：

- 在 PipeWire 主循环线程上执行，不在调用方的线程上。
- 写满 `stride * h` 字节的预乘 alpha BGRA。
- 不联网、不阻塞、不分配。
- 没有消费者时不回调。
- 收到的 `w`/`h` 是协商后的尺寸，可能与配置的尺寸不同。

`Options` 各字段、`onStreaming` 语义与完整契约见 [`src/pwvideo.hpp`](src/pwvideo.hpp)。

## 构建

```bash
make        # -> libpwvideo.a + demo
make run    # 跑示例节点
```

依赖只有发行版系统库：`g++` 与 `libpipewire-0.3`，外加 `pkg-config`。

## 验证

```bash
pw-dump | grep -A20 pwnode-demo

gst-launch-1.0 -q pipewiresrc target-object=pwnode-demo num-buffers=10 \
    ! video/x-raw,format=BGRA ! filesink location=/tmp/f.raw
```

第二条命令落盘的文件大小为 `width * height * 4 * 帧数`。OBS 里把源尺寸设成
`Options::width`/`Options::height`，alpha 原样透传。

## 作为子模块使用

```bash
git submodule add https://github.com/zlinux-live-util/pw-video-simple-interface.git \
    lib/pw-video-simple-interface
```

消费方用**自己的编译参数**编 `lib/pw-video-simple-interface/src/*.cpp`，并加
`-I lib/pw-video-simple-interface/src`。也可以就地构建子模块的 `libpwvideo.a`，但会在子模块
目录里留下 .o 文件。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `src/pwvideo.hpp` | 公开接口与帧回调契约 |
| `src/pwvideo.cpp` | PipeWire 节点、驱动线程、缓冲与帧率协商 |
| `examples/demo.cpp` | 最小示例，上面两条命令就用它 |
| `docs/internals.md` | 流配置约束及其对应的实测数据 |
| `Makefile` | `libpwvideo.a` 与 `demo` |

## 贡献

- 行为改动：附命令与其输出。
- 性能结论：附测量数据。
- 改流配置前先读 [docs/internals.md](docs/internals.md)。

## 许可

MIT，版权 ZokuTe（自 2026 年）。PipeWire 通过动态链接使用，仍遵循其自身许可
（MIT、LGPL-2.1-or-later）。
