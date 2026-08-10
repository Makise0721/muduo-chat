#include "app/InMemoryMessageRepository.hpp"

#include <utility>

StoreResult InMemoryMessageRepository::storeOffline(int64_t userId, const std::string& payload)
{
    OfflineMessage m;
    m.id = nextId_++;
    m.userId = userId;
    m.payload = payload;
    queues_[userId].push_back(std::move(m));
    StoreResult result;
    result.ok = true;
    return result;
}

std::vector<OfflineMessage> InMemoryMessageRepository::takeOffline(int64_t userId)
{
    std::vector<OfflineMessage> taken;
    auto it = queues_.find(userId);
    if (it == queues_.end()) {
        return taken;
    }
    taken.assign(it->second.begin(), it->second.end());
    queues_.erase(it);
    return taken;
}
