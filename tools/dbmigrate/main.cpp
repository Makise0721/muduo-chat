// dbmigrate：版本化 schema migration 命令行工具
//
// 用法：
//   dbmigrate --to <version> [--host H] [--user U] [--password P] [--db N] [--port N]
//             [--migrations-dir D] [--lock-timeout SECS]
//   dbmigrate status [--host H] [--user U] [--password P] [--db N] [--port N]
//
// 密码缺省读 DB_PASSWORD 环境变量（再缺省 "123456"），与测试 fixture 一致。
#include "db/SchemaMigration.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

bool needValue(int argc, char** argv, int* i, const std::string& flag, std::string* out)
{
    if (*i + 1 >= argc) {
        std::cerr << "dbmigrate: missing value for " << flag << std::endl;
        return false;
    }
    *out = argv[++(*i)];
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    std::string user = "root";
    std::string password;
    std::string dbname;
    std::string migrationsDir = "sql/migrations";
    std::string targetVersion;
    unsigned int port = 3306;
    int lockTimeout = 10;
    bool statusCmd = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "status") {
            statusCmd = true;
        } else if (arg == "--host") {
            if (!needValue(argc, argv, &i, arg, &host)) return 1;
        } else if (arg == "--user") {
            if (!needValue(argc, argv, &i, arg, &user)) return 1;
        } else if (arg == "--password") {
            if (!needValue(argc, argv, &i, arg, &password)) return 1;
        } else if (arg == "--db") {
            if (!needValue(argc, argv, &i, arg, &dbname)) return 1;
        } else if (arg == "--port") {
            std::string v;
            if (!needValue(argc, argv, &i, arg, &v)) return 1;
            port = static_cast<unsigned int>(atoi(v.c_str()));
        } else if (arg == "--migrations-dir") {
            if (!needValue(argc, argv, &i, arg, &migrationsDir)) return 1;
        } else if (arg == "--to") {
            if (!needValue(argc, argv, &i, arg, &targetVersion)) return 1;
        } else if (arg == "--lock-timeout") {
            std::string v;
            if (!needValue(argc, argv, &i, arg, &v)) return 1;
            lockTimeout = atoi(v.c_str());
        } else {
            std::cerr << "dbmigrate: unknown argument '" << arg << "'" << std::endl;
            return 1;
        }
    }

    if (dbname.empty()) {
        std::cerr << "dbmigrate: --db <name> is required" << std::endl;
        return 1;
    }
    if (password.empty()) {
        const char* p = getenv("DB_PASSWORD");
        password = p ? p : "123456";
    }

    schema_migration::Migrator migrator(host, user, password, dbname, port);

    if (statusCmd) {
        schema_migration::StatusResult st = migrator.status();
        if (!st.ok) {
            std::cerr << "dbmigrate: " << st.error << std::endl;
            return 1;
        }
        for (size_t i = 0; i < st.versions.size(); ++i) {
            std::cout << st.versions[i].version << " " << st.versions[i].checksum << " "
                      << st.versions[i].appliedAt << std::endl;
        }
        return 0;
    }

    schema_migration::MigrateResult r = migrator.migrateTo(migrationsDir, targetVersion, lockTimeout);
    if (!r.ok) {
        std::cerr << "dbmigrate: " << r.error << std::endl;
        return 1;
    }
    if (r.applied.empty()) {
        std::cout << "migration up to date" << std::endl;
    } else {
        for (size_t i = 0; i < r.applied.size(); ++i) {
            std::cout << "applied " << r.applied[i] << std::endl;
        }
    }
    return 0;
}
