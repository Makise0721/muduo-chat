#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <functional>

using NotifyMessageCallback = std::function<void(int, std::string)>;

class Redis {
public:
    Redis();
    ~Redis();

    bool connect();
    bool publish(int channel, std::string message);
    bool subscribe(int channel);
    bool unsubscribe(int channel);
    void observer_channel_message();

    void init_notify_handler(NotifyMessageCallback cb);

private:
    redisContext* _publish_context;
    redisContext* _subscribe_context;
    NotifyMessageCallback _notify_message_handler;
};
