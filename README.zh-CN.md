# pw-video-simple-interface

[English](README.md) · [简体中文](README.zh-CN.md)

一个 C++ 库：把调用方渲染好的一帧发布成 **PipeWire 视频节点**，在 OBS 里通过
[obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 选用。它负责注册节点、处理缓冲与帧率
协商，并在消费者每拉一帧时回调。布局、文字与网络 I/O 都留在调用方。

核心之外还有一组可选的 cairo 辅助模块：直传提供方缓冲的帧、带描边的文字、图片缓存、
HTTP 客户端。它们存在的原因是：在这之上写的每个悬浮层，否则都要把同一批代码再推导一遍。

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

## 模块

| 头文件 | 内容 | 依赖 |
| --- | --- | --- |
| `src/pwvideo.hpp` | `VideoNode`、`Options`、`FrameProvider`：节点本体 | libpipewire |
| `extras/cairo_util.hpp` | `SurfacePtr`/`ContextPtr`、`CairoFrame`（表面 + 上下文、`blitTo`、`writePng`）、`roundedRect`、`Rgba` | cairo |
| `extras/text.hpp` | `TextRenderer`：复用一份 pango layout，填充与字形路径描边 | cairo、pangocairo |
| `extras/assetcache.hpp` | `AssetCache`：URL → cairo 表面，LRU，`Cover` / `Contain` 缩放 | cairo、gdk-pixbuf、libcurl |
| `extras/http.hpp` | `HttpClient`：单个复用 curl handle、字节上限、支持 `file://` | libcurl |

`CairoFrame::blitTo` 直接接收提供方的 `dst`、`stride`、`w`、`h`，调用方不必再处理 stride。
`TextRenderer::outline` 描的是字形路径，约定在 `fill` 之前调用，用两遍递减宽度近似柔和投影，
这是文字叠在视频上仍然可读的做法。

```cpp
#include "cairo_util.hpp"
#include "text.hpp"

pwvideo::CairoFrame frame(480, 200);      // ARGB32，预乘 alpha
pwvideo::TextRenderer text;

cairo_t* cr = frame.cr();
PangoLayout* l = text.layout(cr, "message", pwvideo::LabelSpec{30, true, 300, 1, true, false});
pwvideo::TextRenderer::outline(cr, l, 16, 60, 4.0, pwvideo::Rgba{0, 0, 0, 0.6}, 1.5);
pwvideo::TextRenderer::fill(cr, l, 16, 60, pwvideo::Rgba{1, 1, 1, 1});

// 帧回调内：
frame.blitTo(dst, stride, w, h);
```

## 构建

```bash
make        # -> libpwvideo.a + demo，以及 libpwvideo-cairo.a + demo-cairo
make run    # 跑示例节点
./demo-cairo --dump /tmp/extras.png --image file:///path/to/image.png
```

核心只需要 `g++`、`libpipewire-0.3` 与 `pkg-config`。辅助模块只在 `cairo`、`pangocairo`、
`gdk-pixbuf-2.0`、`libcurl` 都在时才构建；缺了它们 `make` 只编核心并给出提示。

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

消费方用**自己的编译参数**编源码，并加两条 include 路径：

```make
PWNODE := lib/pw-video-simple-interface
PWNODE_SRC := $(wildcard $(PWNODE)/src/*.cpp) $(wildcard $(PWNODE)/extras/*.cpp)
CXXFLAGS += -I$(PWNODE)/src -I$(PWNODE)/extras
```

只要视频节点时去掉 `$(PWNODE)/extras/*.cpp` 与那条 include 路径即可。也可以就地构建子模块的
`libpwvideo.a`，但会在子模块目录里留下 .o 文件。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `src/pwvideo.hpp` | 公开接口与帧回调契约 |
| `src/pwvideo.cpp` | PipeWire 节点、驱动线程、缓冲与帧率协商 |
| `extras/cairo_util.{hpp,cpp}` | 表面与上下文封装、`CairoFrame`、`roundedRect` |
| `extras/text.{hpp,cpp}` | pango layout 复用、填充与字形路径描边 |
| `extras/assetcache.{hpp,cpp}` | URL → cairo 表面，带 LRU、`Cover` 与 `Contain` |
| `extras/http.{hpp,cpp}` | 基于单个复用 curl handle 的 HTTP(S) GET 客户端 |
| `examples/demo.cpp` | 最小示例，验证命令就用它 |
| `examples/demo-cairo.cpp` | 辅助模块的完整用例：帧、描边文字、缓存图片 |
| `docs/internals.md` | 流配置约束及其对应的实测数据 |
| `Makefile` | `libpwvideo.a`、`libpwvideo-cairo.a`、`demo`、`demo-cairo` |

## 贡献

- 行为改动：附命令与其输出。
- 性能结论：附测量数据。
- 改流配置前先读 [docs/internals.md](docs/internals.md)。

## 许可

MIT，版权 ZokuTe（自 2026 年）。PipeWire 通过动态链接使用，仍遵循其自身许可
（MIT、LGPL-2.1-or-later）。
