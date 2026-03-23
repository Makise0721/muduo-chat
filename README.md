# ChatServer - 基于mymuduo的聊天室系统

一个基于mymuduo网络库实现的单聊群聊聊天室系统，支持用户注册登录、好友添加、群组创建、消息收发等功能。

## 项目结构

```
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

- 用户注册和登录
- 单对单聊天
- 群组聊天
- 好友管理
- 离线消息存储
- 在线状态管理

## 环境要求

- C++11或更高版本
- CMake 3.10+
- Linux系统
- MySQL 5.7+
- Redis 5.0+
- hiredis库

## 安装依赖

### 安装MySQL

```bash
sudo apt-get update
sudo apt-get install mysql-server mysql-client libmysqlclient-dev
```

### 安装Redis

```bash
sudo apt-get install redis-server hiredis-dev
```

### 安装其他依赖

```bash
sudo apt-get install cmake build-essential
```

## 数据库配置

1. 创建数据库和表结构：

```bash
mysql -u root -p < sql/chat.sql
```

2. 修改数据库连接信息：

编辑 `chatserver/main.cpp`，修改MySQL连接参数：

```cpp
connPool.init("127.0.0.1", "root", "your_password", "chat", 3306, 5);
```

## 编译项目

```bash
mkdir build && cd build
cmake ..
make
```

编译成功后，可执行文件位于 `bin/ChatServer`。

## 运行服务器

```bash
./bin/ChatServer 127.0.0.1 6000
```

参数说明：
- 第一个参数：服务器IP地址
- 第二个参数：服务器端口号

## 消息协议
所有消息使用JSON格式，包含以下字段：
每条消息必须以换行符 `\n` 结束（即“一行一个 JSON”），否则服务器可能无法正确解析。

### 登录消息 (msgid: 1)
```json
{
  "msgid": 1,
  "id": 123,
  "password": "123456"
}
```

### 注册消息 (msgid: 4)
```json
{
  "msgid": 4,
  "name": "username",
  "password": "123456"
}
```

### 单聊消息 (msgid: 6)
```json
{
  "msgid": 6,
  "id": 123,
  "name": "sender",
  "toid": 456,
  "msg": "Hello",
  "time": "2024-01-01 12:00:00"
}
```

### 添加好友 (msgid: 7)
```json
{
  "msgid": 7,
  "id": 123,
  "friendid": 456
}
```

### 创建群组 (msgid: 8)
```json
{
  "msgid": 8,
  "id": 123,
  "groupname": "聊天群",
  "groupdesc": "这是一个测试群组"
}
```

### 加入群组 (msgid: 9)
```json
{
  "msgid": 9,
  "id": 123,
  "groupid": 1
}
```

### 群聊消息 (msgid: 10)
```json
{
  "msgid": 10,
  "id": 123,
  "name": "sender",
  "groupid": 1,
  "msg": "Hello everyone",
  "time": "2024-01-01 12:00:00"
}
```

## 客户端测试

可以使用telnet或netcat进行简单测试：

```bash
# 连接服务器
nc 127.0.0.1 6000

# 发送注册消息
{"msgid":4,"name":"test","password":"123456"}

# 发送登录消息
{"msgid":1,"id":1,"password":"123456"}
```

## 核心组件说明

### ChatServer
封装mymuduo的TcpServer，处理网络连接和消息分发。

### ChatService
核心业务逻辑单例类，维护消息处理器映射表，处理各种业务逻辑。

### MySQL
数据库操作封装，提供连接、查询、更新等功能。

### ConnectionPool
MySQL连接池，提高数据库访问效率。

### Redis
Redis订阅发布封装，用于跨服务器消息同步。

## 技术架构

- **网络层**：基于mymuduo的Reactor模式，非阻塞IO
- **业务层**：单例模式的ChatService，消息处理器映射
- **数据层**：MySQL存储用户数据，Redis用于消息分发
- **协议层**：JSON格式的消息协议

## 注意事项

1. 确保MySQL和Redis服务已启动
2. 修改main.cpp中的数据库连接信息
3. 首次运行需要导入数据库脚本
4. 服务器默认监听6000端口，可根据需要修改

## 参考资料

- mymuduo网络库
- nlohmann/json库
- MySQL官方文档
- Redis官方文档

## 许可证

本项目仅供学习交流使用。
