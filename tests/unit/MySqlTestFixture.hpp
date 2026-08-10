#pragma once

#include "db/ConnectionPool.hpp"
#include "db/MySQL.hpp"

#include <gtest/gtest.h>

#include <string>

// 真实 MySQL 集成 fixture：独立 chat_test 库，从空 schema 建 User 表。
// 连接参数与服务器一致（127.0.0.1:3306 root，密码 DB_PASSWORD 默认 123456）。
// 本 fixture 不提供 skip：MySQL 不可用时测试必须失败（P2-02 完成定义）。
class MySqlTestFixture {
public:
    static std::string password()
    {
        const char* pwd = getenv("DB_PASSWORD");
        return pwd ? std::string(pwd) : std::string("123456");
    }

    static void resetSchema()
    {
        MySQL admin;
        ASSERT_TRUE(admin.connect("127.0.0.1", "root", password(), "", 3306))
            << "MySQL unavailable (this test requires a local MySQL server)";
        ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_test"));
        ASSERT_TRUE(admin.update("CREATE DATABASE chat_test DEFAULT CHARSET utf8"));
        ASSERT_TRUE(admin.update("USE chat_test"));
        // 与 sql/chat.sql 保持一致（migration 从空 schema 执行）。
        ASSERT_TRUE(admin.update(
            "CREATE TABLE User("
            "id INT PRIMARY KEY AUTO_INCREMENT,"
            "name VARCHAR(50) NOT NULL UNIQUE,"
            "password VARCHAR(50) NOT NULL,"
            "state ENUM('online', 'offline') DEFAULT 'offline'"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8"));
        ASSERT_TRUE(admin.update(
            "CREATE TABLE Friend("
            "userid INT NOT NULL,"
            "friendid INT NOT NULL,"
            "PRIMARY KEY(userid, friendid),"
            "FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE,"
            "FOREIGN KEY (friendid) REFERENCES User(id) ON DELETE CASCADE"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8"));
        ASSERT_TRUE(admin.update(
            "CREATE TABLE AllGroup("
            "id INT PRIMARY KEY AUTO_INCREMENT,"
            "groupname VARCHAR(50) NOT NULL,"
            "groupdesc VARCHAR(200) DEFAULT ''"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8"));
        ASSERT_TRUE(admin.update(
            "CREATE TABLE GroupUser("
            "groupid INT NOT NULL,"
            "userid INT NOT NULL,"
            "grouprole ENUM('creator', 'normal') DEFAULT 'normal',"
            "PRIMARY KEY(groupid, userid),"
            "FOREIGN KEY (groupid) REFERENCES AllGroup(id) ON DELETE CASCADE,"
            "FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8"));
    }

    static ConnectionPool& pool()
    {
        static ConnectionPool* instance = [] {
            ConnectionPool* p = &ConnectionPool::getInstance();
            p->init("127.0.0.1", "root", password(), "chat_test", 3306, 2);
            return p;
        }();
        return *instance;
    }
};
