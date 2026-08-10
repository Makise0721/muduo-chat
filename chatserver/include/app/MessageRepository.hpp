#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 消息数据访问 port（P2 应用边界）：只负责当前单机的离线消息存取，
// 不包含分布式 ACK/outbox（P3 可靠消息语义另行引入）。
enum class MessageError {
    None,
    StorageFailure,
    Disconnected,
    Timeout,
};

struct StoreResult {
    bool ok = false;
    MessageError error = MessageError::StorageFailure;
};

struct OfflineMessage {
    int64_t id = 0;
    int64_t userId = 0;
    std::string payload;  // 协议原文（原样补投）
};

class MessageRepository {
public:
    virtual ~MessageRepository() = default;

    // 为离线用户存储一条消息；重复请求（同 payload 两次）产生两条记录
    //（去重属 P3 可靠消息语义，现状不处理）。
    virtual StoreResult storeOffline(int64_t userId, const std::string& payload) = 0;

    // 取走该用户全部离线消息（按存储顺序）并清空队列。
    virtual std::vector<OfflineMessage> takeOffline(int64_t userId) = 0;
};
