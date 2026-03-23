# mymuduo

基于muduo网络库的C++高性能网络库实现

## 项目简介

mymuduo是一个现代化的C++网络库，采用Reactor模式设计，提供高性能的事件驱动网络编程框架。该项目参考了陈硕的muduo网络库设计思想，实现了完整的TCP网络通信功能。

## 主要特性

- **Reactor模式**：基于事件驱动的反应器模式设计
- **非阻塞IO**：使用epoll实现高效的IO多路复用
- **多线程支持**：支持线程池，充分利用多核CPU
- **现代C++**：使用C++11特性，如智能指针、lambda表达式等
- **简洁易用**：提供简洁的API接口，易于集成和使用

## 核心组件

### 网络核心
- **EventLoop**：事件循环，负责事件分发和处理
- **Channel**：事件通道，封装文件描述符和事件回调
- **Poller/EPollPoller**：IO多路复用封装

### 服务器组件
- **TcpServer**：TCP服务器，管理连接和线程池
- **TcpConnection**：TCP连接，处理具体连接的读写操作
- **Acceptor**：连接接受器，处理新连接

### 辅助组件
- **Buffer**：缓冲区管理，支持高效的读写操作
- **EventLoopThreadPool**：事件循环线程池
- **EventLoopThread**：事件循环线程
- **Thread**：线程封装
- **InetAddress**：网络地址封装
- **Socket**：Socket封装
- **Logger**：日志系统
- **Timestamp**：时间戳

## 构建方法

### 环境要求
- C++11或更高版本
- CMake 3.10+
- Linux系统（使用epoll）

### 编译步骤

```bash
cd mymuduo
mkdir build && cd build
cmake ..
make
```

编译完成后，动态库将生成在`lib/`目录下。

## 使用示例

### Echo服务器

项目提供了一个简单的Echo服务器示例（`example/testserver.cc`）：

```cpp
#include "../EventLoop.h"
#include "../InetAddress.h"
#include "../TcpServer.h"

class EchoServer 
{
public:
    EchoServer(EventLoop *loop, const InetAddress &listenAddr)
        : server_(loop, listenAddr, "EchoServer") 
    {
        server_.setConnectionCallback(
            std::bind(&EchoServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&EchoServer::onMessage, this, std::placeholders::_1,
                      std::placeholders::_2, std::placeholders::_3));
    }

    void start() {
        server_.start();
    }

private:
    void onConnection(const TcpConnectionPtr &conn) {
        // 处理连接事件
    }

    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time) {
        // 处理消息事件
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
    }

    TcpServer server_;
};

int main() {
    EventLoop loop;
    InetAddress listenAddr(8000);
    EchoServer server(&loop, listenAddr);
    server.start();
    loop.loop();
    return 0;
}
```

## 项目结构

```
mymuduo/
├── EventLoop.h/cc          # 事件循环
├── TcpServer.h/cc          # TCP服务器
├── TcpConnection.h/cc      # TCP连接
├── Channel.h/cc            # 事件通道
├── Poller.h/cc             # IO多路复用基类
├── EPollPoller.h/cc        # epoll实现
├── Buffer.h/cc             # 缓冲区
├── Acceptor.h/cc           # 连接接受器
├── EventLoopThreadPool.h/cc # 线程池
├── EventLoopThread.h/cc    # 事件循环线程
├── Thread.h/cc             # 线程封装
├── Socket.h/cc             # Socket封装
├── InetAddress.h/cc        # 网络地址
├── Logger.h/cc             # 日志系统
├── Timestamp.h/cc          # 时间戳
├── CurrentThread.h/cc      # 线程信息
├── noncopyable.h           # 不可拷贝基类
├── Callbacks.h             # 回调函数类型定义
├── DefaultPoller.cc        # 默认Poller工厂
├── example/                # 示例代码
│   └── testserver.cc       # Echo服务器示例
├── lib/                    # 编译输出目录
├── build/                  # 构建目录
└── CMakeLists.txt          # CMake配置文件
```

## 技术要点

1. **One Loop Per Thread**：每个线程运行一个事件循环
2. **异步非阻塞**：所有IO操作都是异步非阻塞的
3. **事件回调**：使用回调函数处理各种事件
4. **线程安全**：通过互斥锁和原子操作保证线程安全
5. **资源管理**：使用智能指针自动管理对象生命周期

## 参考资料

- 《Linux多线程服务端编程》- 陈硕
- muduo网络库：https://github.com/chenshuo/muduo

## 许可证

本项目仅供学习交流使用。
