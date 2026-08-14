// P3-10 cutover（docs/tasks/P3-10.md Commit 2 / RED 计划 7，spec
// message-reliability.md §5.2 步骤 5/7）：cutover 后 ChatApplication port 不再暴露
// storeOffline/takeOffline，登录补投只走 ledger claim。
// 编译期接口存在性断言：SFINAE 探测 ChatApplication 无
// storeOfflineMessage/takeOfflineMessages 成员（cutover 前接口存在 → static_assert
// 失败 = 合法 RED；cutover 实现移除后通过）。MessageRepository port 已全量删除（M1）。
// 本文件只做编译期断言、不触库，可独立于 MySQL 运行。

#include "app/ChatApplication.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace {

// 编译期接口存在性断言（CutoverDisablesLegacyPath）：cutover 后
// ChatApplication port 不再暴露 storeOffline/takeOffline。
template <typename T, typename = void>
struct HasStoreOfflineMessage : std::false_type {};
template <typename T>
struct HasStoreOfflineMessage<T, decltype((void)std::declval<T&>().storeOfflineMessage(
                                     std::declval<int64_t>(), std::declval<const std::string&>()))>
    : std::true_type {};

template <typename T, typename = void>
struct HasTakeOfflineMessages : std::false_type {};
template <typename T>
struct HasTakeOfflineMessages<T,
                              decltype((void)std::declval<T&>().takeOfflineMessages(
                                  std::declval<int64_t>()))> : std::true_type {};

} // namespace

TEST(LegacyCutoverTest, CutoverDisablesLegacyPath)
{
    // cutover + 禁止旧写（spec §5.2 步骤 5/7，docs/tasks/P3-10.md Commit 2）后：
    // 登录补投只走 ledger claim，线上代码不再调用 storeOffline/takeOffline。
    static_assert(!HasStoreOfflineMessage<ChatApplication>::value,
                  "cutover must remove ChatApplication::storeOfflineMessage");
    static_assert(!HasTakeOfflineMessages<ChatApplication>::value,
                  "cutover must remove ChatApplication::takeOfflineMessages");
}
