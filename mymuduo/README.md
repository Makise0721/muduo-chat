# mymuduo

参考陈硕 muduo 设计思想自研的 C++11 高性能网络库：Reactor 模式、epoll、one loop per thread，在经典设计之上补齐了**有界发送与背压语义**和 **v1/v2 双协议编解码器**。本库是上层聊天系统 [muduo-chat](../README.md) 的网络底座。

## 主要特性

- **Reactor 模式**：事件驱动，非阻塞 IO，epoll 多路复用
- **One Loop Per Thread**：每个线程至多一个 EventLoop，多核扩展经 EventLoopThreadPool
- **有界发送 / 背压**：send 返回结构化结果（Accepted/WouldBlock/Closed/TooLarge），写缓冲超限主动通知生产者暂停，慢消费者不拖垮进程
- **双协议编解码器**：换行分隔 JSON 与二进制帧两种 OutputCodec，可按连接注入
- **毫秒级定时器**：`runAfter` / `runEvery` / `cancel`
- **异步日志**：前端格式化、后台线程落盘
- **C++11**：智能指针、std::function/bind、thread、atomic，无第三方依赖

## 核心组件

### 事件核心

| 组件 | 职责 |
|---|---|
| `EventLoop` | 事件循环：`runInLoop`/`queueInLoop` 跨线程投递、毫秒定时器、`loopLagProbeMs()` loop 滞后探针 |
| `Channel` | fd + 关注事件 + 回调封装 |
| `Poller` / `EPollPoller` | IO 多路复用抽象与 epoll 实现（`DefaultPoller.cc` 工厂）|
| `TimerQueue` | 定时器队列（timerfd 驱动）|

### 服务器组件

| 组件 | 职责 |
|---|---|
| `Acceptor` | 新连接接受；accept 失败按原因分类计数（EMFILE 恢复等）|
| `TcpServer` | 连接管理 + IO 线程池；提供 `stopAccept()`、`forceCloseAllConnections()`、`connectionCount()`、`acceptReasonCounts()`、`totalOutstandingBytes()` 运维接口 |
| `TcpConnection` | 连接读写；有界发送、背压回调、`outstandingBytes()` 观测 |
| `EventLoopThreadPool` / `EventLoopThread` | IO 线程池与单 IO 线程 |

### 协议编解码器

统一的 `OutputCodec` 接口（`encodedSize` + `encode`），经 `TcpConnection::setOutputCodec()` 按连接注入：

| Codec | 协议 | 说明 |
|---|---|---|
| `LegacyJsonLineCodec` | v1 | 换行分隔 JSON，encode 自动追加 `\n`，可用 telnet 直接调试 |
| `StreamCodec` / `BinaryFrameCodec` | v2 | 二进制帧封装任意 payload；20 字节大端帧头：`magic=0x4D434854("MCHT") · version=2 · flags · headerLength=20 · bodyLength · messageType · contentType=JSON · reserved · requestId` |

v2 校验规则：帧体默认上限 **1 MiB**（构造参数可调，硬上限 **16 MiB**）；magic/version/flags/reserved/contentType 不合法返回 `ProtocolError` / `UnsupportedVersion`。帧格式与应用层协议详见[主项目 README](../README.md)。

### 基础设施

`Buffer`（应用层读写缓冲）、`Logger`（异步日志）、`Socket`、`InetAddress`、`Timestamp`、`CurrentThread`、`Thread`、`noncopyable`、`Callbacks`

## 背压与有界发送

普通网络库的 `send()` 要么无限堆积要么静默丢弃，本库把"发不进去"变成一等公民：

```cpp
SendOutcome out = conn->send(msg);
// out.disposition: Accepted / WouldBlock / Closed / TooLarge
// out.pressure:    Normal / PauseProducer（建议上游暂停生产）
conn->setPressureCallback([] { /* 通知生产者限流 */ });
```

写缓冲水位由 `WriteBufferLimits` 控制，缺省：暂停读入 16MB / 恢复 8MB / 硬上限 64MB / 停滞超时 5s。连接级 `outstandingBytes()` 与服务端 `totalOutstandingBytes()` 支撑运行时观测。

## 构建方法

环境要求：Linux（epoll）、CMake 3.10+、支持 C++11 的 GCC 或 Clang。

```bash
# 方式一：随主项目构建（推荐，产物在 <build>/lib/libmymuduo.so）
cmake -S .. -B build && cmake --build build

# 方式二：独立构建（产物在 mymuduo/build/ 下）
cmake -S . -B build && cmake --build build
```

`autobuild.sh` 是编译并安装到系统目录（头文件 `/usr/include/mymuduo`、动态库 `/usr/lib` 后 `ldconfig`）的辅助脚本；注意它从 `mymuduo/lib/` 取产物，独立构建时需先把 `build/libmymuduo.so` 拷到该位置。

## 使用示例

完整可运行示例见 [`example/testserver.cc`](example/testserver.cc)（Echo 服务器，监听 8000）：

```cpp
#include "../EventLoop.h"
#include "../InetAddress.h"
#include "../TcpServer.h"

class EchoServer {
public:
    EchoServer(EventLoop *loop, const InetAddress &listenAddr)
        : server_(loop, listenAddr, "EchoServer") {
        server_.setConnectionCallback(
            std::bind(&EchoServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&EchoServer::onMessage, this, std::placeholders::_1,
                      std::placeholders::_2, std::placeholders::_3));
    }
    void start() { server_.start(); }

private:
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time) {
        conn->send(buf->retrieveAllAsString());
    }
    TcpServer server_;
};

int main() {
    EventLoop loop;
    EchoServer server(&loop, InetAddress(8000));
    server.start();
    loop.loop();
}
```

## 项目结构

```
mymuduo/
├── EventLoop.h/cc               事件循环（跨线程投递 / 定时器 / lag 探针）
├── Channel.h/cc                 fd 事件与回调封装
├── Poller.h/cc                  IO 多路复用抽象
├── EPollPoller.h/cc             epoll 实现
├── DefaultPoller.cc             Poller 工厂
├── Acceptor.h/cc                新连接接受（拒绝原因分类）
├── TcpServer.h/cc               TCP 服务器（连接管理 / 线程池 / 优雅停止）
├── TcpConnection.h/cc           TCP 连接（有界发送 / 背压）
├── EventLoopThreadPool.h/cc     IO 线程池
├── EventLoopThread.h/cc         单 IO 线程
├── Thread.h/cc                  线程封装
├── TimerQueue.h/cc              定时器队列
├── StreamCodec.h/cc             v2 二进制帧编解码
├── BinaryFrameCodec.h/cc        v2 对外 codec（OutputCodec 实现）
├── LegacyJsonLineCodec.h/cc     v1 换行 JSON codec
├── Buffer.h/cc                  应用层缓冲区
├── Logger.h/cc                  异步日志
├── Socket.h/cc                  RAII socket 封装
├── InetAddress.h/cc             网络地址封装
├── Timestamp.h/cc               时间戳
├── CurrentThread.h/cc           线程局部 tid 缓存
├── Callbacks.h                  回调类型定义
├── noncopyable.h                不可拷贝基类
├── example/testserver.cc        Echo 示例（端口 8000）
├── autobuild.sh                 系统安装辅助脚本
└── CMakeLists.txt
```

## 技术要点

1. **One Loop Per Thread**：每个线程至多一个 EventLoop，用 `eventfd` 唤醒跨线程任务投递
2. **全异步非阻塞**：所有 IO 走 epoll LT 事件驱动，IO 线程上无阻塞调用
3. **背压优先**：写缓冲有界，压力显式上抛而不是默默堆积
4. **线程安全边界清晰**：连接对象只在 owner loop 上操作，跨线程只允许 `runInLoop`/`queueInLoop`
5. **RAII 资源管理**：socket/Channel/连接生命周期全部智能指针托管，回调经弱引用防悬垂

## 参考资料

- 《Linux 多线程服务端编程》— 陈硕
- muduo 网络库：<https://github.com/chenshuo/muduo>

本项目仅供学习交流使用。
