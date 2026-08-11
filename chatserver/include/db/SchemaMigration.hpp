#pragma once

#include <string>
#include <vector>

namespace schema_migration {

struct AppliedVersion {
    std::string version;    // 规范形式："0001"
    std::string checksum;   // 迁移文件 SHA-256 十六进制
    std::string appliedAt;  // schema_migrations.applied_at 原文
};

struct MigrateResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> applied;  // 本次实际执行的版本（规范形式），按序
};

struct StatusResult {
    bool ok = false;
    std::string error;
    std::vector<AppliedVersion> versions;
};

// 版本化 additive migration runner：
// - 迁移文件 <migrations-dir>/<版本>_<名字>.sql，版本为 4 位零填充数字，按序执行；
// - 旧五表库已存在表时用 CREATE TABLE IF NOT EXISTS 视为已应用，不报错；
// - 已应用版本的文件被修改（checksum 不符）或缺失 → fail-fast，不改任何表；
// - GET_LOCK/RELEASE_LOCK 保证同一数据库并发执行只有一个持锁者（等待
//   lockTimeoutSecs，超时 fail-fast）。
class Migrator {
public:
    Migrator(std::string host, std::string user, std::string password,
             std::string dbname, unsigned int port = 3306);

    // targetVersion 为空 = 应用全部；只应用版本 <= target 的未应用迁移。
    MigrateResult migrateTo(const std::string& migrationsDir,
                            const std::string& targetVersion,
                            int lockTimeoutSecs = 10);

    // 查询已应用版本（版本/checksum/执行时间）。
    StatusResult status();

private:
    std::string host_;
    std::string user_;
    std::string password_;
    std::string dbname_;
    unsigned int port_;
};

}  // namespace schema_migration
