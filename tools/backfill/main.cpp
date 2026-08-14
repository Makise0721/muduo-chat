// backfill：旧 OfflineMessage 迁移 CLI（P3-10，docs/tasks/P3-10.md Interface/可观察
// 行为与验证节）。与 dbmigrate 并列（tools/backfill/main.cpp，共享 chatserver_core），
// 复用既有连接参数 flag 风格。
//
// 用法：
//   backfill --dry-run [--batch N] [--host H] [--user U] [--password P] [--db N] [--port P]
//   backfill --run     [--batch N] [--host H] [--user U] [--password P] [--db N] [--port P]
//
//   --dry-run：只统计 + hash，不写 ledger/quarantine/checkpoint；
//   --run：真正迁移（checkpoint 持久化、可重入，legacy:<offline_id> 幂等）。
//   密码缺省读 DB_PASSWORD 环境变量（再缺省 "123456"），与测试 fixture 一致。
//
// 输出 stats（sourceRows/migrated/quarantined/skippedIdempotent/sourceHash/checkpoint）
// 到 stdout（key=value，每行一个），exit 0 成功 / 1 失败。数据库不可用 fail-fast
// （不 skip）：连接池 init 或 acquire 失败即报错退出。
#include "db/ConnectionPool.hpp"
#include "db/MySQL.hpp"
#include "db/OfflineBackfill.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

namespace {

bool needValue(int argc, char** argv, int* i, const std::string& flag, std::string* out)
{
    if (*i + 1 >= argc) {
        std::cerr << "backfill: missing value for " << flag << std::endl;
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
    unsigned int port = 3306;
    uint32_t batch = 100;
    bool dryRun = false;
    bool modeSet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            dryRun = true;
            modeSet = true;
        } else if (arg == "--run") {
            dryRun = false;
            modeSet = true;
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
        } else if (arg == "--batch") {
            std::string v;
            if (!needValue(argc, argv, &i, arg, &v)) return 1;
            batch = static_cast<uint32_t>(atoi(v.c_str()));
        } else {
            std::cerr << "backfill: unknown argument '" << arg << "'" << std::endl;
            std::cerr << "usage: backfill --dry-run|--run [--batch N] [--host H] [--user U] "
                         "[--password P] [--db N] [--port P]" << std::endl;
            return 1;
        }
    }

    if (!modeSet) {
        std::cerr << "backfill: exactly one of --dry-run / --run is required" << std::endl;
        return 1;
    }
    if (dbname.empty()) {
        std::cerr << "backfill: --db <name> is required" << std::endl;
        return 1;
    }
    if (password.empty()) {
        const char* p = getenv("DB_PASSWORD");
        password = p ? p : "123456";
    }

    // 数据库不可用 fail-fast：预检连接立即失败，不等到池 acquire 超时（不 skip）。
    MySQL preflight;
    if (!preflight.connect(host, user, password, dbname, port)) {
        std::cerr << "backfill: cannot connect to database " << dbname
                  << " at " << host << ":" << port << std::endl;
        return 1;
    }

    ConnectionPool& pool = ConnectionPool::getInstance();
    pool.init(host, user, password, dbname, port, 4);
    offline_backfill::Runner runner(pool);
    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = batch;
    cfg.dryRun = dryRun;

    try {
        offline_backfill::BackfillStats s = runner.run(cfg);
        std::cout << "sourceRows=" << s.sourceRows << "\n";
        std::cout << "migrated=" << s.migrated << "\n";
        std::cout << "quarantined=" << s.quarantined << "\n";
        std::cout << "skippedIdempotent=" << s.skippedIdempotent << "\n";
        std::cout << "sourceHash=" << s.sourceHash << "\n";
        std::cout << "checkpoint=" << runner.checkpoint() << "\n";
        std::cout << "dryRun=" << (dryRun ? "true" : "false") << "\n";
        std::cout << "ok" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        // DB 不可用 / 表缺失 / 参数非法：fail-fast，exit 1。
        std::cerr << "backfill: " << e.what() << std::endl;
        return 1;
    }
}
