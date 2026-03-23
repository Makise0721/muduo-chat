#include "Redis.hpp"
#include <iostream>
#include <thread>
#include <cstring>

Redis::Redis() : _publish_context(nullptr), _subscribe_context(nullptr) {
}

Redis::~Redis() {
    if (_publish_context != nullptr) {
        redisFree(_publish_context);
    }
    if (_subscribe_context != nullptr) {
        redisFree(_subscribe_context);
    }
}

bool Redis::connect() {
    _publish_context = redisConnect("127.0.0.1", 6379);
    if (_publish_context == nullptr) {
        std::cerr << "connect redis failed!" << std::endl;
        return false;
    }
    _subscribe_context = redisConnect("127.0.0.1", 6379);
    if (_subscribe_context == nullptr) {
        std::cerr << "connect redis failed!" << std::endl;
        return false;
    }
    
    std::thread t(std::bind(&Redis::observer_channel_message, this));
    t.detach();
    return true;
}

bool Redis::publish(int channel, std::string message) {
    redisReply* reply = (redisReply*)redisCommand(_publish_context, "PUBLISH %d %s", channel, message.c_str());
    if (reply == nullptr) {
        std::cerr << "publish command failed!" << std::endl;
        return false;
    }
    freeReplyObject(reply);
    return true;
}

bool Redis::subscribe(int channel) {
    if (REDIS_ERR == redisAppendCommand(this->_subscribe_context, "SUBSCRIBE %d", channel)) {
        std::cerr << "subscribe command failed!" << std::endl;
        return false;
    }
    int done = 0;
    while (!done) {
        if (REDIS_ERR == redisGetReply(this->_subscribe_context, (void**)&_subscribe_context)) {
            std::cerr << "subscribe command failed!" << std::endl;
            return false;
        }
        done++;
    }
    return true;
}

bool Redis::unsubscribe(int channel) {
    if (REDIS_ERR == redisAppendCommand(this->_subscribe_context, "UNSUBSCRIBE %d", channel)) {
        std::cerr << "unsubscribe command failed!" << std::endl;
        return false;
    }
    int done = 0;
    while (!done) {
        if (REDIS_ERR == redisGetReply(this->_subscribe_context, (void**)&_subscribe_context)) {
            std::cerr << "unsubscribe command failed!" << std::endl;
            return false;
        }
        done++;
    }
    return true;
}

void Redis::observer_channel_message() {
    redisReply* reply = nullptr;
    while (REDIS_OK == redisGetReply(this->_subscribe_context, (void**)&reply)) {
        if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            if (strcmp(reply->element[0]->str, "message") == 0) {
                _notify_message_handler(atoi(reply->element[1]->str), reply->element[2]->str);
            }
        }
        freeReplyObject(reply);
    }
}

void Redis::init_notify_handler(NotifyMessageCallback cb) {
    this->_notify_message_handler = cb;
}
