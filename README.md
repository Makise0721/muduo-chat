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
ChatServer/
├── bin/                # 编译生成的二进制可执行文件
├── build/              # CMake 编译中间文件（临时目录）
├── include/            # 头文件 (.h / .hpp)
│   ├── server/         # 网络层核心头文件 (ChatServer, Connection)
│   ├── service/        # 业务层头文件 (ChatService, Redis, Model)
│   └── db/             # 数据库操作相关头文件 (MySQL, ConnectionPool)
├── src/                # 源文件 (.cpp)
│   ├── server/         # 封装网络库回调、启动事件循环
│   ├── service/        # 编写单聊、群聊、登录等业务逻辑
│   ├── db/             # 数据库连接逻辑、SQL 执行封装
│   ├── model/          # 数据对象映射 (User, Group, Friend)
│   └── main.cpp        # 程序入口：解析配置、初始化并启动服务器
├── test/               # 测试代码 (可以使用 GTest 或简单的客户端模拟)
├── sql/                # 数据库初始化脚本 (.sql 文件)
├── thirdparty/         # 第三方库头文件 (如 json.hpp)
├── CMakeLists.txt      # 顶层 CMake 构建脚本
└── README.md           # 项目说明文档


## 🏗️ 快速开始
1. **安装依赖**：安装 MySQL, Redis 以及 `nlohmann/json`。
2. **数据库配置**：运行 `sql/` 目录下的脚本初始化表结构。
3. **编译**：
   ```bash
   mkdir build && cd build
   cmake ..
   make
