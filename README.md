# muduo-chat

基于自研 mymuduo 网络库（Reactor 模式，C++11/Linux）的可靠聊天系统。

## 特性总览

- **双协议**：v1（换行分隔 JSON）与 v2（二进制帧），独立监听端口。
- **durable 消息接受**：Message/sequence/Delivery 同事务提交后应答发送方。
- **at-least-once 投递**：接收端 ACK、超时重投、过期保留；客户端按 MessageId 去重。不宣称 exactly-once。
- **Conversation 局部顺序**：同一会话内保序，不做全局顺序。
- **跨网关投递**：Outbox 表 → Kafka → 幂等 consumer；每 Gateway 独立消费组。
- **Redis presence fencing**：SessionEpoch 校验，防止旧连接误投（旧 epoch 丢弃并重路由）。
- **keyed serial executor**：阻塞 DB 工作移出 EventLoop，按键串行执行，多 Reactor 安全扩并。
- **统一 Telemetry**：SIGUSR1 METRICS 兼容行 + Prometheus `/metrics` 端点（配置默认关闭）+ Grafana dashboard 与告警规则（SLO 标注 experimental）。

## 快速开始

### 依赖

- Linux；CMake 3.10+；GCC 或 Clang（C++11）
- MySQL 8.x（必需；验证环境为 8.0.46）
- 可选：Redis、Kafka（仅 M4/M5 集群投递与对应集成测试需要）

Debian/Ubuntu 示例：

```bash
sudo apt-get install cmake build-essential libmysqlclient-dev
```

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

构建产物在 `bin/`（ChatServer、ChatClient、dbmigrate）。测试默认开启（`option(ENABLE_TESTS ... ON)`），纯部署可加 `-DENABLE_TESTS=OFF`。

### 数据库迁移

版本化迁移脚本位于 `sql/migrations/`（0001_baseline 起）。使用 `bin/dbmigrate` 应用：

```bash
export DB_PASSWORD=your_password
bin/dbmigrate --to 3 --db chat          # 迁移到指定版本
bin/dbmigrate status --db chat          # 查看已应用版本与 checksum
```

`--db` 必填；其余连接参数有 `--host/--user/--password/--port/--migrations-dir/--lock-timeout` 可选。密码缺省读 `DB_PASSWORD` 环境变量。

### 配置与运行

服务端支持 `--config <json>` 或位置参数 `ip port [threads]`（位置参数覆盖 v1 端点）：

```bash
bin/ChatServer --config config.json
# 或
DB_PASSWORD=your_password bin/ChatServer 127.0.0.1 6000
```

JSON 配置段与缺省值（未出现的字段保持默认）：

| 段 | 字段 | 缺省 |
|---|---|---|
| `server.v1` | `ip` / `port` / `threads` | 127.0.0.1 / 6000 / 1 |
| `server.v2` | `port`（ip 继承 v1） | 7000 |
| `db` | `host` / `port` / `user` / `password` / `dbname` / `pool_size` | 127.0.0.1 / 3306 / root / （DB_PASSWORD） / chat / 5 |
| `executor` | `workers` / `queue_capacity` | 1 / 64 |
| `reliable` | ACK 超时、退避、保留期、清理批次等 | 卡冻结值 |
| `outbox` | `claim_batch` / `scan_interval_ms` / `claim_lease_ms` | 卡冻结值 |
| `gateway` | `id`（默认 1）、`presence`（Redis）、`kafka`、`consumer` | 卡冻结值 |
| `metrics` | `enabled` / `port` | false / 7001 |

完整字段示例见 `docker/configs/gw1.json`。

### 客户端

```bash
bin/ChatClient 127.0.0.1 6000
```

v1 协议为换行分隔 JSON（登录 msgid=1、注册 msgid=4、单聊 msgid=6 等），可用 telnet/netcat 直接调试；协议细节见 [docs/specs/message-reliability.md](docs/specs/message-reliability.md)。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

- 共 497 个用例（M5 验收口径）。真实 MySQL 必需；Redis/Kafka 用于集群集成用例，同样要求本地真实依赖（测试不 skip）。
- 单测基于 GTest；`ENABLE_TESTS=ON` 时随主构建生成。

## 多节点部署

`docker/compose.yml` 提供 3 Gateway 容器拓扑（host 网络，复用宿主本机 MySQL/Redis/Kafka）：

```bash
bash docker/build_rootfs.sh                 # 构建最小 rootfs 镜像
docker compose -f docker/compose.yml up -d
```

端口布局：gw1 v1=16201/v2=16211，gw2 16202/16212，gw3 16203/16213（gateway.id=1/2/3）。同文件含 Prometheus 与 Grafana 服务（抓取宿主 `/metrics`，experimental 观测面）；告警规则在 `docker/prometheus/rules/`，dashboard 在 `docker/grafana/dashboards/`。

## 文档入口

- 导航总览：[docs/README.md](docs/README.md)
- 里程碑验收报告：[docs/reports/p2-m2-gates.md](docs/reports/p2-m2-gates.md)、[p3-m3-gates.md](docs/reports/p3-m3-gates.md)、[p4-m4-gates.md](docs/reports/p4-m4-gates.md)、[p5-m5-gates.md](docs/reports/p5-m5-gates.md)
- 性能数据：[docs/reports/performance/](docs/reports/performance/)
- 协议与领域行为：[docs/specs/](docs/specs/)；设计决定：[docs/adr/](docs/adr/)
