#include "MySqlTestFixture.hpp"

#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// P3-01 版本化 migration 契约。独立库 chat_p301（不触碰 chat_test/chat 开发库）。
// 迁移目录与 sql/chat.sql 均从仓库根解析（__FILE__ 编译期绝对路径）。
namespace {

const char* kTestDb = "chat_p301";

std::string repoRoot()
{
    std::string file(__FILE__);
    size_t pos = file.find("tests/unit/");
    if (pos == std::string::npos) {
        return "";
    }
    return file.substr(0, pos);
}

std::string migrationsDir()
{
    return repoRoot() + "sql/migrations";
}

std::string chatSqlPath()
{
    return repoRoot() + "sql/chat.sql";
}

std::string mainCppPath()
{
    return repoRoot() + "chatserver/main.cpp";
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << content;
    return true;
}

std::string trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

// 与 runner 相同的受控拆分约定：忽略空行与 `--` 注释行，按 `;` 切语句。
std::vector<std::string> splitStatements(const std::string& sql)
{
    std::vector<std::string> out;
    std::istringstream ss(sql);
    std::string line;
    std::string buf;
    while (std::getline(ss, line)) {
        std::string t = trim(line);
        if (t.empty() || (t.size() >= 2 && t[0] == '-' && t[1] == '-')) {
            continue;
        }
        buf += t;
        buf += '\n';
        size_t pos;
        while ((pos = buf.find(';')) != std::string::npos) {
            std::string stmt = trim(buf.substr(0, pos));
            buf.erase(0, pos + 1);
            if (!stmt.empty()) {
                out.push_back(stmt);
            }
        }
    }
    std::string last = trim(buf);
    if (!last.empty()) {
        out.push_back(last);
    }
    return out;
}

std::string upperFirst(const std::string& s, size_t n)
{
    std::string out = s.substr(0, n);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return out;
}

// 只取 CREATE TABLE 语句（跳过 CREATE DATABASE/USE/INSERT 种子），
// 用于旧五表快照构建与 chat.sql/baseline 漂移比较。
std::vector<std::string> tableStatements(const std::string& sql)
{
    std::vector<std::string> out;
    for (const std::string& stmt : splitStatements(sql)) {
        if (upperFirst(stmt, 13) == "CREATE TABLE ") {
            out.push_back(stmt);
        }
    }
    return out;
}

schema_migration::Migrator makeMigrator()
{
    return schema_migration::Migrator("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306);
}

// 重建空库（独立测试库，串行运行，无并发冲突）。
void resetDb()
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "", 3306));
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_p301"));
    ASSERT_TRUE(admin.update("CREATE DATABASE chat_p301 DEFAULT CHARSET utf8"));
}

// 用 sql/chat.sql 的 CREATE TABLE 语句构造旧五表快照（模拟已发布库）。
void buildOldSnapshot()
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    std::string chatSql = readFile(chatSqlPath());
    ASSERT_FALSE(chatSql.empty()) << "cannot read sql/chat.sql";
    for (const std::string& stmt : tableStatements(chatSql)) {
        ASSERT_TRUE(admin.update(stmt)) << "old snapshot build failed for: " << stmt;
    }
}

std::set<std::string> tableNames()
{
    MySQL conn;
    if (!conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306)) {
        return {};
    }
    std::string sql = "SELECT table_name FROM information_schema.tables WHERE table_schema='"
                      + std::string(kTestDb) + "'";
    MYSQL_RES* res = conn.query(sql);
    if (!res) {
        return {};
    }
    std::set<std::string> names;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0]) {
            names.insert(row[0]);
        }
    }
    mysql_free_result(res);
    return names;
}

std::string replaceOnce(std::string s, const std::string& from, const std::string& to)
{
    size_t pos = s.find(from);
    if (pos != std::string::npos) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

} // namespace

TEST(SchemaMigrationTest, EmptyDatabaseAppliesBaselineOnce)
{
    resetDb();

    schema_migration::MigrateResult r1 = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r1.ok) << r1.error;
    ASSERT_EQ(1u, r1.applied.size());
    EXPECT_EQ("0001", r1.applied[0]);

    std::set<std::string> names = tableNames();
    EXPECT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage",
                                     "schema_migrations"}),
              names);

    schema_migration::StatusResult st = makeMigrator().status();
    ASSERT_TRUE(st.ok) << st.error;
    ASSERT_EQ(1u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);
    EXPECT_EQ(64u, st.versions[0].checksum.size());
    EXPECT_EQ(0, std::count_if(st.versions[0].checksum.begin(), st.versions[0].checksum.end(),
                               [](char c) { return !std::isxdigit(static_cast<unsigned char>(c)); }));
    EXPECT_FALSE(st.versions[0].appliedAt.empty()) << "applied_at 必须可查询";

    // 幂等：重复执行不重复应用，checksum 稳定。
    schema_migration::MigrateResult r2 = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_TRUE(r2.applied.empty());
    schema_migration::StatusResult st2 = makeMigrator().status();
    ASSERT_TRUE(st2.ok);
    ASSERT_EQ(1u, st2.versions.size());
    EXPECT_EQ(st.versions[0].checksum, st2.versions[0].checksum);
}

TEST(SchemaMigrationTest, OldFiveTableDatabaseUpgrade)
{
    resetDb();
    buildOldSnapshot();
    ASSERT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage"}),
              tableNames());

    // 旧五表库首次升级：已存在表视为已应用，不报错、不重复建。
    schema_migration::MigrateResult r = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(1u, r.applied.size());
    EXPECT_EQ("0001", r.applied[0]);

    std::set<std::string> names = tableNames();
    EXPECT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage",
                                     "schema_migrations"}),
              names);

    // 升级后库上重跑幂等。
    schema_migration::MigrateResult r2 = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_TRUE(r2.applied.empty());
}

TEST(SchemaMigrationTest, ChecksumMismatchFailsFast)
{
    resetDb();

    char tmpl[] = "/tmp/muduo-p301-tamper-XXXXXX";
    char* dirP = mkdtemp(tmpl);
    ASSERT_NE(nullptr, dirP) << "mkdtemp failed";
    const std::string dir(dirP);

    std::string src = readFile(migrationsDir() + "/0001_baseline.sql");
    ASSERT_FALSE(src.empty()) << "cannot read baseline migration";

    // 首次应用定义 checksum：篡改内容可以应用成功。
    std::string tampered1 = replaceOnce(src, "VARCHAR(50) NOT NULL", "VARCHAR(80) NOT NULL");
    ASSERT_NE(src, tampered1) << "tamper target not found in baseline";
    ASSERT_TRUE(writeFile(dir + "/0001_baseline.sql", tampered1));
    schema_migration::MigrateResult r1 = makeMigrator().migrateTo(dir, "", 30);
    ASSERT_TRUE(r1.ok) << r1.error;
    ASSERT_EQ(1u, r1.applied.size());

    // 已执行文件被再次修改 → checksum 不符 → fail-fast，不改任何已记录行。
    std::string tampered2 = replaceOnce(src, "VARCHAR(50) NOT NULL", "VARCHAR(60) NOT NULL");
    ASSERT_TRUE(writeFile(dir + "/0001_baseline.sql", tampered2));
    schema_migration::MigrateResult r2 = makeMigrator().migrateTo(dir, "", 30);
    EXPECT_FALSE(r2.ok);
    EXPECT_NE(std::string::npos, r2.error.find("0001")) << r2.error;

    schema_migration::StatusResult st = makeMigrator().status();
    ASSERT_TRUE(st.ok);
    ASSERT_EQ(1u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);

    // 改回首次应用的内容（checksum 与已记录一致）→ 校验通过，无重复应用。
    ASSERT_TRUE(writeFile(dir + "/0001_baseline.sql", tampered1));
    schema_migration::MigrateResult r3 = makeMigrator().migrateTo(dir, "", 30);
    ASSERT_TRUE(r3.ok) << r3.error;
    EXPECT_TRUE(r3.applied.empty());

    std::remove((dir + "/0001_baseline.sql").c_str());
    rmdir(dir.c_str());
}

TEST(SchemaMigrationTest, ConcurrentRunnersSingleLockHolder)
{
    resetDb();

    std::atomic<int> go{0};
    schema_migration::MigrateResult ra;
    schema_migration::MigrateResult rb;
    std::thread ta([&] {
        while (go.load() == 0) {
            std::this_thread::yield();
        }
        ra = makeMigrator().migrateTo(migrationsDir(), "", 60);
    });
    std::thread tb([&] {
        while (go.load() == 0) {
            std::this_thread::yield();
        }
        rb = makeMigrator().migrateTo(migrationsDir(), "", 60);
    });
    go.store(1);
    ta.join();
    tb.join();

    ASSERT_TRUE(ra.ok) << ra.error;
    ASSERT_TRUE(rb.ok) << rb.error;
    // advisory lock 保证只有持锁者执行 0001；另一个等锁后看到已应用。
    EXPECT_LE(ra.applied.size(), 1u);
    EXPECT_LE(rb.applied.size(), 1u);
    EXPECT_EQ(1u, ra.applied.size() + rb.applied.size());

    schema_migration::StatusResult st = makeMigrator().status();
    ASSERT_TRUE(st.ok) << st.error;
    ASSERT_EQ(1u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);
}

TEST(SchemaMigrationTest, DdlDriftBetweenChatSqlAndBaseline)
{
    std::string chatSql = readFile(chatSqlPath());
    std::string baseline = readFile(migrationsDir() + "/0001_baseline.sql");
    ASSERT_FALSE(chatSql.empty()) << "cannot read sql/chat.sql";
    ASSERT_FALSE(baseline.empty()) << "cannot read sql/migrations/0001_baseline.sql";

    std::vector<std::string> chatTables = tableStatements(chatSql);
    std::vector<std::string> baselineTables = tableStatements(baseline);
    ASSERT_EQ(5u, chatTables.size()) << "chat.sql 应包含 5 张表的 CREATE TABLE";
    ASSERT_EQ(5u, baselineTables.size()) << "baseline 应包含 5 张表的 CREATE TABLE";
    EXPECT_EQ(chatTables, baselineTables) << "sql/chat.sql 与 0001_baseline.sql DDL 漂移";
}

TEST(SchemaMigrationTest, ServerStartupDoesNotRunMigrations)
{
    std::string mainSrc = readFile(mainCppPath());
    ASSERT_FALSE(mainSrc.empty()) << "cannot read chatserver/main.cpp";
    std::string lower = mainSrc;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    EXPECT_EQ(std::string::npos, lower.find("migrat"))
        << "生产服务器启动路径不得调用 migration runner";
}
