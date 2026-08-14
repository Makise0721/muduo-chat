#pragma once

#include "app/MySQLMessageStore.hpp"
#include "db/ConnectionPool.hpp"

#include <cstdint>
#include <string>
#include <vector>

// P3-10 旧 OfflineMessage 迁移 backfill runner（docs/tasks/P3-10.md、
// message-reliability.md §5.2 步骤 3/4）：可重入、checkpoint、dry-run、批次有界。
// 逐行解析旧表 message（msgid 6/10 整段协议 JSON）→ legacy identity
// `legacy:<offline_id>` → MessageStore accept 路径写入新 ledger（同
// (sender, client_message_id) 幂等，重复运行不增加 Message）；不可解析行进入
// 可查询 quarantine，绝不静默删。源行不删除（保留至 contract migration）。
//
// 冻结参数（卡登记）：batchSize 生产默认 100；checkpoint 用 runner 自建辅助表
// `CREATE TABLE IF NOT EXISTS`（不经 versioned migration，sql/migrations/ 仅
// contract migration 允许，卡登记决策）；dry-run 只统计+hash 不写库；legacy
// identity `legacy:<offline_id>`（spec §5.1）。
namespace offline_backfill {

// runner 自建辅助表（可查询）。checkpoint 表：scope/last_id 单行高水位。
extern const char* kCheckpointTable;  // "OfflineBackfillCheckpoint"
extern const char* kQuarantineTable;  // "OfflineBackfillQuarantine"

struct BackfillConfig {
    uint32_t batchSize = 100;   // 每批迁移行数上限（生产默认，卡冻结参数）
    uint32_t maxBatches = 0;    // 0 = 不限批次数（测试注入小值分段中断）
    bool dryRun = false;        // true：只统计+hash，不写 ledger/quarantine/checkpoint
};

struct BackfillStats {
    uint64_t sourceRows = 0;        // 本 run 读取的源行数（id > checkpoint）
    uint64_t migrated = 0;          // 成功写入 ledger 的行数
    uint64_t quarantined = 0;       // 进入 quarantine 的行数（本 run 新增）
    uint64_t skippedIdempotent = 0; // 已存在（同 legacy:<offline_id> / 已 quarantine）跳过
    std::string sourceHash;         // 全部源行（id 升序）FNV-1a 64 十六进制，跨批/进度稳定
};

// quarantine 行（可查询，供人工处理；含源 id/用户/原因/原始 payload）。
struct QuarantineEntry {
    int64_t offlineId = 0;
    int64_t userId = 0;
    std::string reason;   // 不可解析/写 ledger 失败原因
    std::string payload;  // 原始 payload 字节
};

// backfill runner（单机串行，经 ConnectionPool 直连 MySQL）：run() 从 checkpoint
// 继续迁移，每完成一批持久化 checkpoint；中断重跑从高水位继续。
class Runner {
public:
    explicit Runner(ConnectionPool& pool);
    // 可选故障注入点（卡登记，docs/tasks/P3-10.md M4）：转递给内部 MySQLMessageStore
    // 的既有 FaultHook（生产路径传 nullptr）。用于验证 DependencyBusy 类瞬态错误
    // fail-fast（重抛、checkpoint 未写、重跑自动重试），不是新公开业务 seam。
    Runner(ConnectionPool& pool, MySQLMessageStore::FaultHook* faultHook);

    BackfillStats run(const BackfillConfig& cfg);

    // 当前持久化 checkpoint（已迁移高水位 last_id；无则为 0）。
    uint64_t checkpoint() const;

    // 全部 quarantine 行（按 offline_id 升序）。
    std::vector<QuarantineEntry> quarantine() const;

private:
    ConnectionPool& pool_;
    MySQLMessageStore::FaultHook* faultHook_;
};

}  // namespace offline_backfill
