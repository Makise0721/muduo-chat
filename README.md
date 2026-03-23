# ClusterChatServer

基于 C++11 重写的 muduo 网络库实现的高性能集群聊天服务器。

## 🚀 项目亮点
* **底层网络框架**：基于非阻塞 I/O 和 Reactor 模型重写了 `muduo` 核心库（去除了对 boost 的依赖，纯 C++11 实现）。
* **高并发模型**：采用 `One Loop Per Thread` 设计，主从 Reactor 模式有效利用多核 CPU。
* **解耦业务逻辑**：利用回调函数机制，将网络层与业务层（登录、注册、聊天）彻底解耦。
* **分布式支持**：集成 **Redis 发布/订阅机制**，解决跨服务器通信难题，支持集群部署。
* **可靠存储**：使用 **MySQL** 持久化存储用户信息、好友关系及离线消息，并实现简单的连接池。
* **数据格式**：使用 JSON 进行序列化与反序列化，通过 `nlohmann/json` 库实现。

## 🛠️ 技术栈
* 语言：C++11
* 网络库：自实现 muduo (Epoll + TimerQueue)
* 数据库：MySQL 8.0
* 缓存/中间件：Redis
* 构建工具：CMake

## 📂 项目结构说明

```text
.
├── mymuduo/            # 网络基础设施
│   ├── include/        # 网络库头文件
│   └── src/            # 网络库源文件
├── chatserver/         # 聊天服务器业务代码
│   ├── include/        # 业务层头文件
│   │   ├── ChatServer.hpp    # 封装 TcpServer
│   │   ├── ChatService.hpp   # 核心业务单例
│   │   ├── Redis.hpp         # Redis 订阅/发布封装
│   │   └── db/               # MySQL 操作类
│   ├── src/            # 业务层源文件
│   └── main.cpp        # 程序启动入口
├── thirdparty/         # 存放 json.hpp 等第三方库
├── sql/                # 存放 chat.sql 数据库脚本
└── CMakeLists.txt      # 顶层 CMake，同时编译 mymuduo 和 chatserver
```




## 🏗️ 快速开始
1. **安装依赖**：安装 MySQL, Redis 以及 `nlohmann/json`。
2. **数据库配置**：运行 `sql/` 目录下的脚本初始化表结构。
3. **编译**：
   ```bash
   mkdir build && cd build
   cmake ..
   make
