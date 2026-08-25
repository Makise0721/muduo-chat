#pragma once

// P5-03A RED：批量 ACK 合并契约。共享契约运行器，InMemory 与 MySQL 双 adapter
// 各自以真实 store 实例调用（沿 ReliableMessagingContract 双 adapter 先例）。
//
// 非构建目标（P5-03A FAILED/reverted，updateDeliveries 不存在）；保留为批量 ACK 候选规格。
//
// 本契约只穿公开 MessageStore interface；引用的批量 ACK seam
// `MessageStore::updateDeliveries(const std::vector<Delivery>&)` 尚不存在 →
// 当前编译失败即合法 RED（missing member，非语法错）。

#include "app/MessageStore.hpp"

namespace batched_ack_contract {

// 对 recipient 的单 conversation 建立 N 条 Pending Delivery（不同 messageId、
// sequence 递增），供批量 ACK 断言。store 需已就绪（MySQL 侧已 seed User FK）。
// 返回建立的 Delivery（Pending 态，未 acknowledged）。
std::vector<Delivery> seedPendingDeliveries(MessageStore& store, UserId recipient,
                                            unsigned n);

// 批量 ACK 契约主运行器：InMemory / MySQL 双 adapter 共用。
void runBatchedAckContract(MessageStore& store, UserId recipient, UserId other);

}  // namespace batched_ack_contract
