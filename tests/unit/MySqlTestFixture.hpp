#pragma once

#include "db/ConnectionPool.hpp"
#include "db/MySQL.hpp"
#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>

#include <string>

// 真实 MySQL 集成 fixture：独立 chat_test 库，由版本化 migration runner
// 建表（schema 唯一真相为 sql/migrations/，漂移由 SchemaMigrationTest 捕获）。
// 连接参数与服务器一致（127.0.0.1:3306 root，密码 DB_PASSWORD 默认 123456）。
// 本 fixture 不提供 skip：MySQL 不可用时测试必须失败（P2-02 完成定义）。
class MySqlTestFixture {
public:
    static std::string password()
    {
        const char* pwd = getenv("DB_PASSWORD");
        return pwd ? std::string(pwd) : std::string("123456");
    }

    // 从 __FILE__ 推导仓库根（编译期为绝对路径），migration 目录随 repo 移动。
    static std::string migrationsDir()
    {
        std::string file(__FILE__);
        size_t pos = file.find("tests/unit/");
        if (pos == std::string::npos) {
            return "sql/migrations";
        }
        return file.substr(0, pos) + "sql/migrations";
    }

    static void resetSchema()
    {
        MySQL admin;
        ASSERT_TRUE(admin.connect("127.0.0.1", "root", password(), "", 3306))
            << "MySQL unavailable (this test requires a local MySQL server)";
        ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_test"));
        ASSERT_TRUE(admin.update("CREATE DATABASE chat_test DEFAULT CHARSET utf8"));
        schema_migration::Migrator migrator("127.0.0.1", "root", password(), "chat_test", 3306);
        schema_migration::MigrateResult r = migrator.migrateTo(migrationsDir(), "", 30);
        ASSERT_TRUE(r.ok) << "schema migration failed: " << r.error;
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
