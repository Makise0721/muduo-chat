#include "MySqlTestFixture.hpp"

#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>

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

std::string replaceOnce(std::string s, const std::string& from, const std::string& to)
{
    size_t pos = s.find(from);
    if (pos != std::string::npos) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

// 构造列形漂移库：User.password 为 VARCHAR(80) 而非 VARCHAR(50)，其余相同。
// 现状行为（文档化限制）：runner 只做表级 IF NOT EXISTS 判定，不检测列级漂移，
// 漂移库会被静默标记为已应用（P3-01 遗留，由 P3-03 故障测试兜底）。
void buildColumnDriftSnapshot()
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    std::string chatSql = readFile(chatSqlPath());
    ASSERT_FALSE(chatSql.empty()) << "cannot read sql/chat.sql";
    for (const std::string& stmt : tableStatements(chatSql)) {
        if (stmt.find("CREATE TABLE IF NOT EXISTS User") != std::string::npos) {
            std::string drifted = replaceOnce(stmt, "password VARCHAR(50) NOT NULL",
                                              "password VARCHAR(80) NOT NULL");
            ASSERT_NE(drifted, stmt) << "drift target not found in User DDL";
            ASSERT_TRUE(admin.update(drifted)) << "drift snapshot build failed for: " << stmt;
        } else {
            ASSERT_TRUE(admin.update(stmt)) << "drift snapshot build failed for: " << stmt;
        }
    }
}

// 递归收集目录下 .cpp/.hpp/.h 源码文件（用于生产隔离断言的全目录扫描）。
std::vector<std::string> collectSourceFiles(const std::string& dir)
{
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return out;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            std::vector<std::string> sub = collectSourceFiles(path);
            out.insert(out.end(), sub.begin(), sub.end());
        } else if (name.size() > 4 &&
                   (name.compare(name.size() - 4, 4, ".cpp") == 0 ||
                    name.compare(name.size() - 4, 4, ".hpp") == 0 ||
                    name.compare(name.size() - 2, 2, ".h") == 0)) {
            out.push_back(path);
        }
    }
    closedir(d);
    return out;
}

bool containsAny(const std::string& text, const std::vector<std::string>& patterns)
{
    for (size_t i = 0; i < patterns.size(); ++i) {
        if (text.find(patterns[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
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

} // namespace

TEST(SchemaMigrationTest, EmptyDatabaseAppliesBaselineOnce)
{
    resetDb();

    schema_migration::MigrateResult r1 = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r1.ok) << r1.error;
    ASSERT_EQ(3u, r1.applied.size());
    EXPECT_EQ("0001", r1.applied[0]);
    EXPECT_EQ("0002", r1.applied[1]);
    EXPECT_EQ("0003", r1.applied[2]);

    std::set<std::string> names = tableNames();
    EXPECT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage",
                                     "Conversation", "DirectConversation", "GroupConversation",
                                     "ChatMessage", "MessageDelivery", "OutboxEvent",
                                     "KafkaDeadLetter", "schema_migrations"}),
              names);

    schema_migration::StatusResult st = makeMigrator().status();
    ASSERT_TRUE(st.ok) << st.error;
    ASSERT_EQ(3u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);
    EXPECT_EQ(64u, st.versions[0].checksum.size());
    EXPECT_EQ(0, std::count_if(st.versions[0].checksum.begin(), st.versions[0].checksum.end(),
                               [](char c) { return !std::isxdigit(static_cast<unsigned char>(c)); }));
    EXPECT_FALSE(st.versions[0].appliedAt.empty()) << "applied_at 必须可查询";
    EXPECT_EQ("0002", st.versions[1].version);
    EXPECT_EQ(64u, st.versions[1].checksum.size());
    EXPECT_EQ(0, std::count_if(st.versions[1].checksum.begin(), st.versions[1].checksum.end(),
                               [](char c) { return !std::isxdigit(static_cast<unsigned char>(c)); }));
    EXPECT_EQ("0003", st.versions[2].version);
    EXPECT_EQ(64u, st.versions[2].checksum.size());
    EXPECT_EQ(0, std::count_if(st.versions[2].checksum.begin(), st.versions[2].checksum.end(),
                               [](char c) { return !std::isxdigit(static_cast<unsigned char>(c)); }));

    // 幂等：重复执行不重复应用，checksum 稳定。
    schema_migration::MigrateResult r2 = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_TRUE(r2.applied.empty());
    schema_migration::StatusResult st2 = makeMigrator().status();
    ASSERT_TRUE(st2.ok);
    ASSERT_EQ(3u, st2.versions.size());
    EXPECT_EQ(st.versions[0].checksum, st2.versions[0].checksum);
    EXPECT_EQ(st.versions[1].checksum, st2.versions[1].checksum);
    EXPECT_EQ(st.versions[2].checksum, st2.versions[2].checksum);
}

TEST(SchemaMigrationTest, OldFiveTableDatabaseUpgrade)
{
    resetDb();
    buildOldSnapshot();
    ASSERT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage"}),
              tableNames());

    // 旧五表库首次升级：已存在表视为已应用，不报错、不重复建；0001 记录 + 0002
    // 追加六表 + 0003 追加 KafkaDeadLetter。
    schema_migration::MigrateResult r = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(3u, r.applied.size());
    EXPECT_EQ("0001", r.applied[0]);
    EXPECT_EQ("0002", r.applied[1]);
    EXPECT_EQ("0003", r.applied[2]);

    std::set<std::string> names = tableNames();
    EXPECT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage",
                                     "Conversation", "DirectConversation", "GroupConversation",
                                     "ChatMessage", "MessageDelivery", "OutboxEvent",
                                     "KafkaDeadLetter", "schema_migrations"}),
              names);

    // 升级后库上重跑幂等。
    schema_migration::MigrateResult r2 = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_TRUE(r2.applied.empty());
}

TEST(SchemaMigrationTest, ColumnDriftLibraryMarkedApplied)
{
    resetDb();
    buildColumnDriftSnapshot();
    ASSERT_EQ((std::set<std::string>{"User", "Friend", "AllGroup", "GroupUser", "OfflineMessage"}),
              tableNames());

    // 现状行为（文档化限制）：列形漂移不可检测——迁移成功且记录 0001+0002+0003，
    // 漂移库被静默标记为已应用；由 P3-03 故障测试（逐条违反唯一约束/FK）兜底。
    schema_migration::MigrateResult r = makeMigrator().migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(3u, r.applied.size());
    EXPECT_EQ("0001", r.applied[0]);
    EXPECT_EQ("0002", r.applied[1]);
    EXPECT_EQ("0003", r.applied[2]);

    schema_migration::StatusResult st = makeMigrator().status();
    ASSERT_TRUE(st.ok) << st.error;
    ASSERT_EQ(3u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);
    EXPECT_EQ("0002", st.versions[1].version);
    EXPECT_EQ("0003", st.versions[2].version);

    // 漂移未被修复（IF NOT EXISTS no-op）：password 列仍是 varchar(80)。
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    MYSQL_RES* res = conn.query("SELECT column_type FROM information_schema.columns "
                                "WHERE table_schema='chat_p301' AND table_name='User' "
                                "AND column_name='password'");
    ASSERT_TRUE(res);
    MYSQL_ROW row = mysql_fetch_row(res);
    ASSERT_TRUE(row && row[0]);
    EXPECT_EQ(std::string("varchar(80)"), row[0]);
    mysql_free_result(res);
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

TEST(SchemaMigrationTest, MissingAppliedFileFailsFast)
{
    resetDb();

    char tmpl[] = "/tmp/muduo-p301-missing-XXXXXX";
    char* dirP = mkdtemp(tmpl);
    ASSERT_NE(nullptr, dirP) << "mkdtemp failed";
    const std::string dir(dirP);

    std::string src = readFile(migrationsDir() + "/0001_baseline.sql");
    ASSERT_FALSE(src.empty()) << "cannot read baseline migration";
    ASSERT_TRUE(writeFile(dir + "/0001_baseline.sql", src));

    schema_migration::MigrateResult r1 = makeMigrator().migrateTo(dir, "", 30);
    ASSERT_TRUE(r1.ok) << r1.error;
    ASSERT_EQ(1u, r1.applied.size());

    // 已应用版本的文件被删除后重跑 → fail-fast，不静默跳过。
    std::remove((dir + "/0001_baseline.sql").c_str());
    schema_migration::MigrateResult r2 = makeMigrator().migrateTo(dir, "", 30);
    EXPECT_FALSE(r2.ok);
    EXPECT_NE(std::string::npos, r2.error.find("missing")) << r2.error;

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
    // advisory lock 保证只有持锁者执行 0001+0002+0003；另一个等锁后看到已应用。
    EXPECT_LE(ra.applied.size(), 3u);
    EXPECT_LE(rb.applied.size(), 3u);
    EXPECT_EQ(3u, ra.applied.size() + rb.applied.size());

    schema_migration::StatusResult st = makeMigrator().status();
    ASSERT_TRUE(st.ok) << st.error;
    ASSERT_EQ(3u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);
    EXPECT_EQ("0002", st.versions[1].version);
    EXPECT_EQ("0003", st.versions[2].version);
}

TEST(SchemaMigrationTest, InvalidTargetVersionFailsFast)
{
    resetDb();

    // 非数字 / 负数 / 超 int 范围（dbmigrate --to 直传本参数，错误 → CLI exit 1）。
    const char* badVersions[] = {"abc", "-1", "9999999999"};
    for (size_t i = 0; i < 3; ++i) {
        schema_migration::MigrateResult r = makeMigrator().migrateTo(migrationsDir(), badVersions[i], 30);
        EXPECT_FALSE(r.ok) << badVersions[i];
        EXPECT_NE(std::string::npos, r.error.find("invalid target version")) << badVersions[i];
    }
}

TEST(SchemaMigrationTest, LockTimeoutFailsFast)
{
    resetDb();

    // 另一会话持锁，短 --lock-timeout 下确定性构造超时路径（无并发时序依赖）。
    MySQL holder;
    ASSERT_TRUE(holder.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    MYSQL_RES* held = holder.query("SELECT GET_LOCK('muduo_chat_schema_migration:chat_p301', 10)");
    ASSERT_TRUE(held);
    mysql_free_result(held);

    schema_migration::MigrateResult r = makeMigrator().migrateTo(migrationsDir(), "", 1);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("could not acquire migration lock")) << r.error;
    // 未拿到锁时不动任何表。
    EXPECT_TRUE(tableNames().empty());

    MYSQL_RES* released = holder.query("SELECT RELEASE_LOCK('muduo_chat_schema_migration:chat_p301')");
    if (released) {
        mysql_free_result(released);
    }
}

TEST(SchemaMigrationTest, DbNameWithQuoteMigratesSafely)
{
    // dbname 含单引号：lockName 未转义时 GET_LOCK 会因 SQL 语法错误而失败；
    // 转义后应正常加锁并完成迁移（不崩溃、不注入）。
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "", 3306));
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS `chat'p301`"));
    ASSERT_TRUE(admin.update("CREATE DATABASE `chat'p301` DEFAULT CHARSET utf8"));

    schema_migration::Migrator m("127.0.0.1", "root", MySqlTestFixture::password(), "chat'p301", 3306);
    schema_migration::MigrateResult r = m.migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(3u, r.applied.size());
    EXPECT_EQ("0001", r.applied[0]);
    EXPECT_EQ("0002", r.applied[1]);
    EXPECT_EQ("0003", r.applied[2]);

    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS `chat'p301`"));
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
    // 扫描整个 chatserver/ 源码树（排除 migration 自身文件），精确匹配调用形态
    // （runMigration/SchemaMigration/dbmigrate/migrateTo/Migrator），
    // 避免注释中 "migrat" 字样误报。
    std::vector<std::string> callForms;
    callForms.push_back("runMigration");
    callForms.push_back("SchemaMigration");
    callForms.push_back("dbmigrate");
    callForms.push_back("migrateTo");
    callForms.push_back("Migrator");

    std::vector<std::string> files = collectSourceFiles(repoRoot() + "chatserver");
    ASSERT_FALSE(files.empty()) << "cannot scan chatserver/ source tree";
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string& f = files[i];
        if (f.find("SchemaMigration") != std::string::npos) {
            continue;  // migration runner 自身文件，跳过
        }
        std::string src = readFile(f);
        ASSERT_FALSE(src.empty()) << "cannot read " << f;
        EXPECT_FALSE(containsAny(src, callForms))
            << "生产服务器启动路径不得调用 migration runner: " << f;
    }
}
