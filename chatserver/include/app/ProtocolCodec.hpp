#pragma once

// P3-06 协议 adapter（docs/tasks/P3-06.md 冻结决策表）：
// - msgid 11/12/13 冻结于 ChatService.hpp EnMsgType（B-22 不占 1..10；本层回包
//   构造以 msgid 参数接收，避免协议层反向依赖 ChatService）；
// - 稳定错误码 errno 101..107 本层冻结（决策表第 5 行）；content 上限 16KB
//   （第 6 行，UTF-8 字节）；typed content 保持最简字符串（第 7 行）；
// - legacy 判定（缺 client_message_id）与 legacy identity 生成（第 10/11 行）。
//
// 头文件自包含（不包含 ReliableMessaging.hpp）：领域类型（class Clock 等）与
// mymuduo TimerQueue.h 的 using Clock 全局同名冲突，同一 TU 无法共存；域间
// 操作（accept 编排、错误映射、legacy identity）全部经本层自由函数，实现在
// ProtocolCodec.cpp（mymuduo 无关 TU）。ChatService 只做 codec 调用/session/
// executor/Reply 映射，不接触领域类型。

#include "app/ChatApplication.hpp"  // 群成员查询入口（mymuduo 无关）

#include <cstdint>
#include <string>

#include "json.hpp"

// ---- 稳定错误码（决策表第 5 行冻结；golden 落地后不再变更）----
constexpr int kErrnoNotConversationMember = 101;
constexpr int kErrnoTooManyRecipients = 102;
constexpr int kErrnoIdempotencyConflict = 103;
constexpr int kErrnoDependencyBusy = 104;
constexpr int kErrnoContentTooLong = 105;
constexpr int kErrnoNotFound = 106;
constexpr int kErrnoInvalidClientMessageId = 107;

// content 字段上限（UTF-8 字节；决策表第 6 行：16KB）。
// 旧 B-13 整条序列化 payload >500 判定废止，不再支配新路径。
constexpr size_t kMaxContentBytes = 16 * 1024;

// 命令种类（本层自包含枚举；与 SendMessageCommand::Kind 对应，映射在 .cpp）。
enum class ChatCommandKind {
    Direct,
    Group,
};

// ONE_CHAT/GROUP_CHAT 解析结果：legacy 判定 + 校验后的命令字段。
// legacy=true（缺 client_message_id）时 hasClientMessageId 必为 false。
struct ParsedChatMessage {
    bool legacy = false;
    bool hasClientMessageId = false;
    std::string clientMessageId;  // 原始值；非法时仍保留供错误响应回显
    std::string content;
    int64_t directRecipient = 0;  // Direct 的 toid
    int64_t groupId = 0;          // Group 的 groupid
};

// accept 结果视图（worker 线程产生，EventLoop 线程 completion 消费；纯值类型）。
// ok=true：messageId/conversationId/sequence/duplicate 有效；ok=false：errnoCode
// 有效（errmsg 由 protocolErrmsg(errnoCode) 派生，不重复携带）。
// clientMessageId 为最终幂等键（v2 原值或 legacy identity），v2 回显用。
struct AcceptResultView {
    bool ok = false;
    bool duplicate = false;
    int errnoCode = 0;
    std::string clientMessageId;
    uint64_t messageId = 0;
    uint64_t conversationId = 0;
    uint64_t sequence = 0;
};

// 解析 ONE_CHAT/GROUP_CHAT 命令（v1/v2 共用）。返回 0=通过；否则为 accept 前
// 拒绝码：kErrnoInvalidClientMessageId（107，非 ASCII 1..64）、
// kErrnoContentTooLong（105，>16KB）。字段缺失/类型错误抛异常——B-25：
// handler 层兜底静默，不产生响应（与旧行为一致）。
int parseChatMessage(ChatCommandKind kind, const nlohmann::json& js,
                     ParsedChatMessage* out);

// worker 线程调用（executor 单 worker = ReliableMessaging 单一调用者，串行驱动）：
// 预检（Direct 目标存在/Group 成员资格+拒空群）→ 构造 SendMessageCommand →
// ReliableMessaging.accept → MessageStoreError 异常映射 errno；结果写 view。
// app 仅用于群成员查询；legacy 命令在此生成 identity 并计数（spec §5.1）。
void acceptChatCommand(ChatApplication* app, int64_t userId, int64_t generation,
                       const ParsedChatMessage& parsed, ChatCommandKind kind,
                       AcceptResultView* view);

// 101..107 固定 errmsg（golden 同步 pin 这些字符串）；未知码返回通用文案。
const char* protocolErrmsg(int errnoCode);

// MESSAGE_ACCEPTED（msgid=11）：五字段 + duplicate（spec §2.2）。
nlohmann::json buildMessageAcceptedReply(int msgid, const std::string& clientMessageId,
                                         uint64_t messageId, uint64_t conversationId,
                                         uint64_t sequence, bool duplicate);

// 错误响应（msgid=13）：{msgid, errno, errmsg[, client_message_id]}（决策表第 4 行；
// client_message_id 可解析时回显，nullptr 省略）。
nlohmann::json buildErrorReply(int msgid, int errnoCode, const std::string& errmsg,
                               const std::string* clientMessageId);

// legacy-mode 计数（spec §5.1 能力差异可观测；正式指标 P3-12 暴露）。
uint64_t legacyModeCount();

// P3-12：SIGUSR1 METRICS 行追加的 reliable_* 字段（key=value 空格分隔，无前导
// 空格）。实现于 ProtocolCodec.cpp（领域 TU，可引 ReliableMessageMetrics.hpp）；
// mymuduo TU（main.cpp）经此入口取快照，避免引入领域 class Clock 同名冲突。
std::string reliableMetricsLine();

// ---- P3-07 投递与 ACK（executor worker 线程调用；ReliableMessaging 单一调用者
// 串行驱动）。DeliverySink 仅前向声明：完整定义在 ChatService 侧 mymuduo TU
// （SessionDeliverySink.cpp），本头保持 mymuduo 无关。----
class DeliverySink;

// 接线：ChatService::bindLoop 注册真实 SessionDeliverySink（wiring() 惰性构造时
// 绑定；未注册时 forwarding adapter fail-closed）。返回 false 表示重复绑定了
// 不同出口或传入空指针，调用方必须 fail-fast。
bool registerDeliverySink(DeliverySink* sink);

// 接收端按 MessageId 显式确认（msgid=12；UserId 取 Session，spec §2.3）。
// 静默：重复/迟到/他人 ACK 幂等且不越权（spec §4 故障点 4/5），无 ACK 回执。
void acknowledgeDelivery(int64_t userId, uint64_t generation, uint64_t messageId);

// Session 上线 claim 名下 Pending Delivery（登录 completion 提交）。
void sessionAvailableDelivery(int64_t userId, uint64_t generation);

// Session 下线：名下 InFlight 立即回 Pending（断开/登出提交）。
void sessionClosedDelivery(int64_t userId, uint64_t generation);

// 背压 low-water 恢复（TcpConnection pressure 回调提交；PauseProducer 不自旋）。
void resumeDelivery(int64_t userId, uint64_t generation);

// ---- P3-08 可靠消息生产接线（生命周期与参数注入；缺省 = 卡冻结值）----
struct RetryConfig;  // 定义见 app/Config.hpp（mymuduo-safe，本头不引入领域类型）

// main 在加载 AppConfig 后、首个 accept 前调用：生产可靠消息参数注入
// （P2-09 "reliable" 段 → cfg.reliable）。未调用时 wiring 使用卡冻结默认；
// 测试直接构造 RetryConfig 注入 ReliableMessaging，不经本入口。
void configureReliableMessaging(const RetryConfig& cfg);

// ---- P3-09 outbox relay 生产接线（与上面同构；缺省 = 卡冻结值）----
struct OutboxConfig;  // 定义见 app/Config.hpp

// main 在加载 AppConfig 后调用（"outbox" 段 → cfg.outbox）。未调用时 wiring 使用
// 卡冻结默认；测试直接构造 OutboxConfig 注入 LocalOutboxRelay，不经本入口。
void configureOutboxRelay(const OutboxConfig& cfg);

// server 就绪（bindLoop，pool 已 init）后启动 relay 单 worker 周期扫描（幂等）。
void startOutboxRelay();

// 挂进 shutdown 顺序：EXECUTOR_SHUTDOWN 之后、MESSAGING_STOP 之前调用——relay
// worker（其 wakeup 消费 wiring().messaging）先于 messaging stop 有界 join。
// wiring 从未构造时 no-op；deadlineMs 为有界 drain 软提示。
void stopOutboxRelay(int64_t deadlineMs);

// server 就绪（ChatService::bindLoop，pool 已 init、sink 已注册）后启动内部
// scheduler（timer 驱动，幂等）。
void startReliableMessaging();

// 挂进既有 shutdown 顺序：EXECUTOR_SHUTDOWN（executor worker 已 join）之后、
// POOL_SHUTDOWN 之前调用——scheduler 线程先于 store/pool 失效退出（有界 join）。
// wiring 从未构造时 no-op；deadlineMs 为有界 drain 软提示。
void stopReliableMessaging(int64_t deadlineMs);
