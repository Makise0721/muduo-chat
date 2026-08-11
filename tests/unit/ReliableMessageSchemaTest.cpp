#include "MySqlTestFixture.hpp"

#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>

#include <mysql/mysql.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// P3-03 expand schema 契约。独立库 chat_p303（对照库 chat_p303b），不触碰
// chat/chat_test/chat_p301。迁移目录与 sql/chat.sql 均从仓库根解析。
namespace {

const char* kTestDb = "chat_p303";
const char* kTestDb2 = "chat_p303b";

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
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

// 只取 CREATE TABLE 语句（跳过 CREATE DATABASE/USE/INSERT 种子），用于旧五表快照构建。
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

schema_migration::Migrator makeMigrator(const std::string& db)
{
    return schema_migration::Migrator("127.0.0.1", "root", MySqlTestFixture::password(), db, 3306);
}

// 重建空库（独立测试库，串行运行，无并发冲突）。
void resetDb(const std::string& db)
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "", 3306));
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS " + db));
    ASSERT_TRUE(admin.update("CREATE DATABASE " + db + " DEFAULT CHARSET utf8"));
}

// 用 sql/chat.sql 的 CREATE TABLE 语句构造旧五表快照（模拟已发布库）。
void buildOldSnapshot(const std::string& db)
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), db, 3306));
    std::string chatSql = readFile(chatSqlPath());
    ASSERT_FALSE(chatSql.empty()) << "cannot read sql/chat.sql";
    for (const std::string& stmt : tableStatements(chatSql)) {
        ASSERT_TRUE(admin.update(stmt)) << "old snapshot build failed for: " << stmt;
    }
}

void seedLegacyRows(MySQL& conn)
{
    ASSERT_TRUE(conn.update("INSERT INTO User(name,password) VALUES('u1','p1'),('u2','p2'),('u3','p3')"));
    ASSERT_TRUE(conn.update("INSERT INTO Friend(userid,friendid) VALUES(1,2),(1,3)"));
    ASSERT_TRUE(conn.update("INSERT INTO AllGroup(groupname) VALUES('g1')"));
    ASSERT_TRUE(conn.update("INSERT INTO GroupUser(groupid,userid) VALUES(1,1),(1,2)"));
    ASSERT_TRUE(conn.update("INSERT INTO OfflineMessage(userid,message) VALUES(1,'m1'),(2,'m2')"));
}

// MySQL 不可拷贝（隐式浅拷贝会在临时对象析构时二次 close 同一句柄），
// 测试连接就地打开，不用返回值传递。
void makeConn(MySQL& conn, const std::string& db)
{
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), db, 3306));
}

std::string fieldOrNull(MYSQL_ROW row, int i)
{
    return row[i] ? std::string(row[i]) : std::string("NULL");
}

long long countRows(MySQL& conn, const std::string& table)
{
    MYSQL_RES* res = conn.query("SELECT COUNT(*) FROM " + table);
    if (!res) {
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    long long n = (row && row[0]) ? atoll(row[0]) : -1;
    mysql_free_result(res);
    return n;
}

std::set<std::string> tableNames(MySQL& conn)
{
    std::set<std::string> names;
    MYSQL_RES* res = conn.query("SELECT table_name FROM information_schema.tables "
                                "WHERE table_schema=DATABASE()");
    if (!res) {
        return names;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0]) {
            names.insert(row[0]);
        }
    }
    mysql_free_result(res);
    return names;
}

std::map<std::string, std::string> columnInfo(MySQL& conn, const std::string& table)
{
    // column_name -> "column_type|is_nullable|column_key"
    std::map<std::string, std::string> out;
    MYSQL_RES* res = conn.query("SELECT column_name, column_type, is_nullable, column_key "
                                "FROM information_schema.columns "
                                "WHERE table_schema=DATABASE() AND table_name='" + table + "'");
    if (!res) {
        return out;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0] && row[1] && row[2] && row[3]) {
            out[row[0]] = std::string(row[1]) + "|" + row[2] + "|" + row[3];
        }
    }
    mysql_free_result(res);
    return out;
}

// client_message_id 的字符集（契约要求 ASCII，DB 层落地为 ascii charset）。
std::string columnCharset(MySQL& conn, const std::string& table, const std::string& column)
{
    MYSQL_RES* res = conn.query("SELECT character_set_name FROM information_schema.columns "
                                "WHERE table_schema=DATABASE() AND table_name='" + table
                                + "' AND column_name='" + column + "'");
    if (!res) {
        return "";
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    std::string cs = (row && row[0]) ? row[0] : "";
    mysql_free_result(res);
    return cs;
}

// table|index_name|non_unique|col1,col2,...（按 seq_in_index 排序聚合）。
std::set<std::string> indexSet(MySQL& conn, const std::string& table)
{
    std::set<std::string> out;
    MYSQL_RES* res = conn.query("SELECT index_name, non_unique, column_name "
                                "FROM information_schema.statistics "
                                "WHERE table_schema=DATABASE() AND table_name='" + table
                                + "' ORDER BY index_name, seq_in_index");
    if (!res) {
        return out;
    }
    std::map<std::string, std::string> cols;
    std::map<std::string, std::string> nu;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0] && row[2]) {
            std::string name = row[0];
            if (!cols[name].empty()) {
                cols[name] += ",";
            }
            cols[name] += row[2];
            nu[name] = row[1];
        }
    }
    mysql_free_result(res);
    for (std::map<std::string, std::string>::const_iterator it = cols.begin(); it != cols.end(); ++it) {
        out.insert(table + "|" + it->first + "|" + nu[it->first] + "|" + it->second);
    }
    return out;
}

// 六张新表的 FK 集合：table|column|referenced_table|referenced_column。
// （旧五表 FK 属 0001 契约，由 SchemaMigrationTest 漂移测试单独覆盖。）
std::set<std::string> foreignKeySet(MySQL& conn)
{
    std::set<std::string> out;
    MYSQL_RES* res = conn.query("SELECT table_name, column_name, referenced_table_name, "
                                "referenced_column_name FROM information_schema.key_column_usage "
                                "WHERE table_schema=DATABASE() AND referenced_table_name IS NOT NULL "
                                "AND table_name NOT IN ('User','Friend','AllGroup','GroupUser','OfflineMessage') "
                                "ORDER BY table_name, column_name");
    if (!res) {
        return out;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0] && row[1] && row[2] && row[3]) {
            out.insert(std::string(row[0]) + "|" + row[1] + "|" + row[2] + "|" + row[3]);
        }
    }
    mysql_free_result(res);
    return out;
}

// information_schema 结构 dump（columns/statistics/key_column_usage/tables/referential_constraints），
// 用于空库与旧库升级结果的一致性比较（含 ENGINE 与 FK delete_rule 维度）。
std::string schemaDump(MySQL& conn)
{
    std::ostringstream os;
    MYSQL_RES* res = conn.query("SELECT table_name, column_name, column_type, is_nullable, "
                                "column_key, column_default, extra "
                                "FROM information_schema.columns "
                                "WHERE table_schema=DATABASE() ORDER BY table_name, ordinal_position");
    if (!res) {
        return "";
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        for (int i = 0; i < 7; ++i) {
            os << fieldOrNull(row, i) << "|";
        }
        os << "\n";
    }
    mysql_free_result(res);

    os << "--indexes--\n";
    res = conn.query("SELECT table_name, index_name, non_unique, seq_in_index, column_name "
                     "FROM information_schema.statistics "
                     "WHERE table_schema=DATABASE() ORDER BY table_name, index_name, seq_in_index");
    if (!res) {
        return "";
    }
    while ((row = mysql_fetch_row(res))) {
        for (int i = 0; i < 5; ++i) {
            os << fieldOrNull(row, i) << "|";
        }
        os << "\n";
    }
    mysql_free_result(res);

    os << "--fks--\n";
    res = conn.query("SELECT table_name, column_name, referenced_table_name, referenced_column_name "
                     "FROM information_schema.key_column_usage "
                     "WHERE table_schema=DATABASE() AND referenced_table_name IS NOT NULL "
                     "ORDER BY table_name, column_name");
    if (!res) {
        return "";
    }
    while ((row = mysql_fetch_row(res))) {
        for (int i = 0; i < 4; ++i) {
            os << fieldOrNull(row, i) << "|";
        }
        os << "\n";
    }
    mysql_free_result(res);

    os << "--tables--\n";
    res = conn.query("SELECT table_name, engine FROM information_schema.tables "
                     "WHERE table_schema=DATABASE() ORDER BY table_name");
    if (!res) {
        return "";
    }
    while ((row = mysql_fetch_row(res))) {
        os << fieldOrNull(row, 0) << "|" << fieldOrNull(row, 1) << "\n";
    }
    mysql_free_result(res);

    os << "--delete_rules--\n";
    res = conn.query("SELECT table_name, referenced_table_name, delete_rule "
                     "FROM information_schema.referential_constraints "
                     "WHERE constraint_schema=DATABASE() ORDER BY table_name, referenced_table_name");
    if (!res) {
        return "";
    }
    while ((row = mysql_fetch_row(res))) {
        os << fieldOrNull(row, 0) << "|" << fieldOrNull(row, 1) << "|" << fieldOrNull(row, 2) << "\n";
    }
    mysql_free_result(res);
    return os.str();
}

long long lastInsertId(MySQL& conn)
{
    MYSQL_RES* res = conn.query("SELECT LAST_INSERT_ID()");
    if (!res) {
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    long long id = (row && row[0]) ? atoll(row[0]) : -1;
    mysql_free_result(res);
    return id;
}

int lastErrno(MySQL& conn)
{
    return mysql_errno(conn.getConnection());
}

std::string hexEncode(const std::string& data)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

void migrateAll(const std::string& db)
{
    schema_migration::MigrateResult r = makeMigrator(db).migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
}

void seedTwoUsers(MySQL& conn)
{
    ASSERT_TRUE(conn.update("INSERT INTO User(name,password) VALUES('u1','p1'),('u2','p2')"));
}

void insertConversation(MySQL& conn, const std::string& kind)
{
    ASSERT_TRUE(conn.update("INSERT INTO Conversation(kind) VALUES('" + kind + "')"));
}

} // namespace

TEST(ReliableMessageSchemaTest, MigrationExpandsSchemaWithSixReliableTables)
{
    resetDb(kTestDb);

    schema_migration::MigrateResult r = makeMigrator(kTestDb).migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(2u, r.applied.size());
    EXPECT_EQ("0001", r.applied[0]);
    EXPECT_EQ("0002", r.applied[1]);

    MySQL conn;
    makeConn(conn, kTestDb);
    std::set<std::string> names = tableNames(conn);
    EXPECT_EQ((std::set<std::string>{
                  "User", "Friend", "AllGroup", "GroupUser", "OfflineMessage",
                  "Conversation", "DirectConversation", "GroupConversation",
                  "ChatMessage", "MessageDelivery", "OutboxEvent", "schema_migrations"}),
              names);

    // 幂等：重复执行不重复应用，0002 只记录一次。
    schema_migration::MigrateResult r2 = makeMigrator(kTestDb).migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_TRUE(r2.applied.empty());
    schema_migration::StatusResult st = makeMigrator(kTestDb).status();
    ASSERT_TRUE(st.ok) << st.error;
    ASSERT_EQ(2u, st.versions.size());
    EXPECT_EQ("0001", st.versions[0].version);
    EXPECT_EQ("0002", st.versions[1].version);
    EXPECT_EQ(64u, st.versions[1].checksum.size());
}

TEST(ReliableMessageSchemaTest, SchemaContractAssertsColumnsIndexesAndForeignKeys)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);

    // 列：名称 -> "类型|可空|键标记"（契约 §6 逐列落地）。
    // 注：column_key 只标记索引首列；复合 PK 的全部成员均为 PRI，
    // 复合 UNIQUE 的非首列（如 client_message_id）与独立 FK 列（event_type）为 MUL。
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"id", "bigint unsigned|NO|PRI"},
                  {"kind", "enum('DIRECT','GROUP')|NO|"},
                  {"next_sequence", "bigint unsigned|NO|"}}),
              columnInfo(conn, "Conversation"));
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"conversation_id", "bigint unsigned|NO|PRI"},
                  {"user_low_id", "int|NO|MUL"},
                  {"user_high_id", "int|NO|MUL"}}),
              columnInfo(conn, "DirectConversation"));
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"conversation_id", "bigint unsigned|NO|PRI"},
                  {"group_id", "int|NO|UNI"}}),
              columnInfo(conn, "GroupConversation"));
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"id", "bigint unsigned|NO|PRI"},
                  {"conversation_id", "bigint unsigned|NO|MUL"},
                  {"sender_id", "int|NO|MUL"},
                  {"client_message_id", "varchar(64)|NO|"},
                  {"sequence", "bigint unsigned|NO|"},
                  {"content", "mediumblob|NO|"},
                  {"created_at", "datetime|NO|"}}),
              columnInfo(conn, "ChatMessage"));
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"message_id", "bigint unsigned|NO|PRI"},
                  {"recipient_id", "int|NO|PRI"},
                  {"state", "tinyint|NO|"},
                  {"attempt_count", "int|NO|"},
                  {"next_attempt_at", "datetime|YES|"},
                  {"lease_owner", "varchar(64)|YES|"},
                  {"lease_until", "datetime|YES|"},
                  {"last_sent_at", "datetime|YES|"},
                  {"acknowledged_at", "datetime|YES|"},
                  {"expires_at", "datetime|YES|"}}),
              columnInfo(conn, "MessageDelivery"));
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"id", "bigint unsigned|NO|PRI"},
                  {"aggregate_message_id", "bigint unsigned|NO|MUL"},
                  {"event_type", "varchar(64)|NO|MUL"},
                  {"payload", "mediumblob|NO|"},
                  {"available_at", "datetime|NO|"},
                  {"lease_owner", "varchar(64)|YES|"},
                  {"lease_until", "datetime|YES|"},
                  {"attempt_count", "int|NO|"},
                  {"processed_at", "datetime|YES|"}}),
              columnInfo(conn, "OutboxEvent"));

    // client_message_id 在 DB 层强制 ASCII（契约 ASCII(1..64) 的上界与字符集）。
    EXPECT_EQ("ascii", columnCharset(conn, "ChatMessage", "client_message_id"));

    // 不改、不删 OfflineMessage（旧表 DDL 原样）。
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"id", "int|NO|PRI"},
                  {"userid", "int|NO|MUL"},
                  {"message", "varchar(500)|NO|"}}),
              columnInfo(conn, "OfflineMessage"));

    // 索引：PK/UNIQUE 全落地 + MessageDelivery 的 (recipient_id,state,next_attempt_at)。
    // 注：MySQL 为 FK 自动补齐前缀索引（user_high_id、aggregate_message_id）一并断言。
    EXPECT_EQ((std::set<std::string>{"Conversation|PRIMARY|0|id"}),
              indexSet(conn, "Conversation"));
    EXPECT_EQ((std::set<std::string>{
                  "DirectConversation|PRIMARY|0|conversation_id",
                  "DirectConversation|user_high_id|1|user_high_id",
                  "DirectConversation|user_low_id|0|user_low_id,user_high_id"}),
              indexSet(conn, "DirectConversation"));
    EXPECT_EQ((std::set<std::string>{
                  "GroupConversation|PRIMARY|0|conversation_id",
                  "GroupConversation|group_id|0|group_id"}),
              indexSet(conn, "GroupConversation"));
    EXPECT_EQ((std::set<std::string>{
                  "ChatMessage|PRIMARY|0|id",
                  "ChatMessage|conversation_id|0|conversation_id,sequence",
                  "ChatMessage|sender_id|0|sender_id,client_message_id"}),
              indexSet(conn, "ChatMessage"));
    EXPECT_EQ((std::set<std::string>{
                  "MessageDelivery|PRIMARY|0|message_id,recipient_id",
                  "MessageDelivery|recipient_id|1|recipient_id,state,next_attempt_at"}),
              indexSet(conn, "MessageDelivery"));
    EXPECT_EQ((std::set<std::string>{
                  "OutboxEvent|PRIMARY|0|id",
                  "OutboxEvent|aggregate_message_id|1|aggregate_message_id",
                  "OutboxEvent|event_type|0|event_type,aggregate_message_id"}),
              indexSet(conn, "OutboxEvent"));

    // 外键：每条 FK 引用与被引用列精确匹配（六张新表共 10 条）。
    EXPECT_EQ((std::set<std::string>{
                  "ChatMessage|conversation_id|Conversation|id",
                  "ChatMessage|sender_id|User|id",
                  "DirectConversation|conversation_id|Conversation|id",
                  "DirectConversation|user_high_id|User|id",
                  "DirectConversation|user_low_id|User|id",
                  "GroupConversation|conversation_id|Conversation|id",
                  "GroupConversation|group_id|AllGroup|id",
                  "MessageDelivery|message_id|ChatMessage|id",
                  "MessageDelivery|recipient_id|User|id",
                  "OutboxEvent|aggregate_message_id|ChatMessage|id"}),
              foreignKeySet(conn));
}

TEST(ReliableMessageSchemaTest, DuplicateClientMessageIdRejected)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'c1',1,'first')"));
    // 同 (sender_id, client_message_id) 第二次 → UNIQUE 拒绝。
    EXPECT_FALSE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                             "sequence,content) VALUES(1,1,'c1',2,'second')"));
    EXPECT_EQ(1062, lastErrno(conn));
    EXPECT_EQ(1ll, countRows(conn, "ChatMessage"));
}

TEST(ReliableMessageSchemaTest, ClientMessageIdCaseSensitive)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    // 大小写敏感（ascii_bin）：同一 sender 的 'abc' 与 'ABC' 是不同 ClientMessageId，
    // 均插入成功，不触发 UNIQUE(sender_id, client_message_id)。
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'abc',1,'first')"));
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'ABC',2,'second')"));
    EXPECT_EQ(2ll, countRows(conn, "ChatMessage"));
}

TEST(ReliableMessageSchemaTest, OversizedClientMessageIdRejectedInStrictMode)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    // 65 字节 ASCII 超长：锁定 STRICT_TRANS_TABLES 部署语义——error 1406 拒绝。
    // 非严格 sql_mode 会静默截断（已知限制，docs/specs/schema.md §4），此处跳过并说明。
    MYSQL_RES* res = conn.query("SELECT @@SESSION.sql_mode");
    ASSERT_TRUE(res);
    MYSQL_ROW row = mysql_fetch_row(res);
    std::string mode = (row && row[0]) ? row[0] : "";
    mysql_free_result(res);
    if (mode.find("STRICT_TRANS_TABLES") == std::string::npos) {
        GTEST_SKIP() << "server sql_mode 非严格（无 STRICT_TRANS_TABLES），超长列值截断而非拒绝";
    }

    std::string id65(65, 'a');
    EXPECT_FALSE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                             "sequence,content) VALUES(1,1,'" + id65 + "',1,'x')"));
    EXPECT_EQ(1406, lastErrno(conn));
    EXPECT_EQ(0ll, countRows(conn, "ChatMessage"));
}

TEST(ReliableMessageSchemaTest, DuplicateConversationSequenceRejected)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'c1',1,'a')"));
    // 同 (conversation_id, sequence) 第二次（不同发送者）→ UNIQUE 拒绝。
    EXPECT_FALSE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                             "sequence,content) VALUES(1,2,'c2',1,'b')"));
    EXPECT_EQ(1062, lastErrno(conn));
    EXPECT_EQ(1ll, countRows(conn, "ChatMessage"));
}

TEST(ReliableMessageSchemaTest, ForeignKeyViolationsRejected)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    // ChatMessage.conversation_id 引用不存在的 Conversation。
    EXPECT_FALSE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                             "sequence,content) VALUES(999,1,'c1',1,'x')"));
    EXPECT_EQ(1452, lastErrno(conn));
    // ChatMessage.sender_id 引用不存在的 User。
    EXPECT_FALSE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                             "sequence,content) VALUES(1,999,'c2',1,'x')"));
    EXPECT_EQ(1452, lastErrno(conn));
    // DirectConversation.user_low_id 引用不存在的 User。
    insertConversation(conn, "DIRECT");
    EXPECT_FALSE(conn.update("INSERT INTO DirectConversation(conversation_id,user_low_id,"
                             "user_high_id) VALUES(2,999,1)"));
    EXPECT_EQ(1452, lastErrno(conn));
    // GroupConversation.group_id 引用不存在的 AllGroup。
    insertConversation(conn, "GROUP");
    EXPECT_FALSE(conn.update("INSERT INTO GroupConversation(conversation_id,group_id) VALUES(3,999)"));
    EXPECT_EQ(1452, lastErrno(conn));

    // 合法行可插入（基座数据就绪后）。
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'c9',9,'x')"));
    // MessageDelivery.message_id 引用不存在的 ChatMessage。
    EXPECT_FALSE(conn.update("INSERT INTO MessageDelivery(message_id,recipient_id) VALUES(999,1)"));
    EXPECT_EQ(1452, lastErrno(conn));
    // MessageDelivery.recipient_id 引用不存在的 User。
    EXPECT_FALSE(conn.update("INSERT INTO MessageDelivery(message_id,recipient_id) VALUES(1,999)"));
    EXPECT_EQ(1452, lastErrno(conn));
    // OutboxEvent.aggregate_message_id 引用不存在的 ChatMessage。
    EXPECT_FALSE(conn.update("INSERT INTO OutboxEvent(aggregate_message_id,event_type,payload) "
                             "VALUES(999,'MessageAccepted','{}')"));
    EXPECT_EQ(1452, lastErrno(conn));
    EXPECT_EQ(0ll, countRows(conn, "MessageDelivery"));
    EXPECT_EQ(0ll, countRows(conn, "OutboxEvent"));
}

TEST(ReliableMessageSchemaTest, DuplicateDeliveryPairRejected)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'c1',1,'x')"));
    ASSERT_TRUE(conn.update("INSERT INTO MessageDelivery(message_id,recipient_id) VALUES(1,1)"));
    // 同 (message_id, recipient_id) 第二次 → PK 拒绝（重复 recipient Delivery）。
    EXPECT_FALSE(conn.update("INSERT INTO MessageDelivery(message_id,recipient_id) VALUES(1,1)"));
    EXPECT_EQ(1062, lastErrno(conn));
    EXPECT_EQ(1ll, countRows(conn, "MessageDelivery"));
}

TEST(ReliableMessageSchemaTest, LargePayloadStoredBeyondLegacyLimit)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);
    insertConversation(conn, "DIRECT");

    // 600B > 旧 OfflineMessage.message VARCHAR(500) 上限，新容量内可存。
    std::string p600(600, 'a');
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'big1',1,0x" + hexEncode(p600) + ")"));
    // 1MB 往返完整一致。
    std::string p1m(1024 * 1024, 'b');
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'big2',2,0x" + hexEncode(p1m) + ")"));

    // 接近 MEDIUMBLOB 上限：max（16777215）可存，max+1 → error 1406 拒绝。
    // （服务器 max_allowed_packet 默认 64MB 已覆盖 ~33.5MB 的 hex 字面量查询，
    //  MySQL 8.0 的 max_allowed_packet 为 SESSION 只读，不再显式放宽。）
    std::string pMax(16777215, 'c');
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(1,1,'big3',3,0x" + hexEncode(pMax) + ")"));
    std::string pOver(16777216, 'd');
    EXPECT_FALSE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                             "sequence,content) VALUES(1,1,'big4',4,0x" + hexEncode(pOver) + ")"));
    EXPECT_EQ(1406, lastErrno(conn));

    MYSQL_RES* res = conn.query("SELECT content FROM ChatMessage ORDER BY sequence");
    ASSERT_TRUE(res);
    MYSQL_ROW row = mysql_fetch_row(res);
    ASSERT_TRUE(row && row[0]);
    unsigned long* lens = mysql_fetch_lengths(res);
    ASSERT_TRUE(lens);
    EXPECT_EQ(600ul, lens[0]);
    EXPECT_EQ(p600, std::string(row[0], lens[0]));
    row = mysql_fetch_row(res);
    ASSERT_TRUE(row && row[0]);
    lens = mysql_fetch_lengths(res);
    ASSERT_TRUE(lens);
    EXPECT_EQ(1024ul * 1024, lens[0]);
    EXPECT_EQ(p1m, std::string(row[0], lens[0]));
    row = mysql_fetch_row(res);
    ASSERT_TRUE(row && row[0]);
    lens = mysql_fetch_lengths(res);
    ASSERT_TRUE(lens);
    EXPECT_EQ(16777215ul, lens[0]);
    EXPECT_EQ(pMax, std::string(row[0], lens[0]));
    EXPECT_EQ(nullptr, mysql_fetch_row(res));
    mysql_free_result(res);
}

TEST(ReliableMessageSchemaTest, TransactionRollbackLeavesNoOrphans)
{
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);
    seedTwoUsers(conn);

    // 裸事务：Message+Delivery+OutboxEvent 全部插入后 ROLLBACK → 无孤儿残留。
    ASSERT_TRUE(conn.update("START TRANSACTION"));
    ASSERT_TRUE(conn.update("INSERT INTO Conversation(kind) VALUES('DIRECT')"));
    long long convId = lastInsertId(conn);
    ASSERT_GT(convId, 0ll);
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(" + std::to_string(convId) + ",1,'c1',1,'x')"));
    long long msgId = lastInsertId(conn);
    ASSERT_GT(msgId, 0ll);
    ASSERT_TRUE(conn.update("INSERT INTO MessageDelivery(message_id,recipient_id) VALUES("
                            + std::to_string(msgId) + ",2)"));
    ASSERT_TRUE(conn.update("INSERT INTO OutboxEvent(aggregate_message_id,event_type,payload) VALUES("
                            + std::to_string(msgId) + ",'MessageAccepted','{}')"));
    ASSERT_TRUE(conn.update("ROLLBACK"));
    EXPECT_EQ(0ll, countRows(conn, "Conversation"));
    EXPECT_EQ(0ll, countRows(conn, "ChatMessage"));
    EXPECT_EQ(0ll, countRows(conn, "MessageDelivery"));
    EXPECT_EQ(0ll, countRows(conn, "OutboxEvent"));

    // 事务中途违规语句（FK 失败）后 ROLLBACK → 同样无残留。
    ASSERT_TRUE(conn.update("START TRANSACTION"));
    ASSERT_TRUE(conn.update("INSERT INTO Conversation(kind) VALUES('DIRECT')"));
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,client_message_id,"
                            "sequence,content) VALUES(2,1,'c2',1,'y')"));
    EXPECT_FALSE(conn.update("INSERT INTO MessageDelivery(message_id,recipient_id) VALUES(999,2)"));
    EXPECT_EQ(1452, lastErrno(conn));
    ASSERT_TRUE(conn.update("ROLLBACK"));
    EXPECT_EQ(0ll, countRows(conn, "Conversation"));
    EXPECT_EQ(0ll, countRows(conn, "ChatMessage"));
    EXPECT_EQ(0ll, countRows(conn, "MessageDelivery"));
}

TEST(ReliableMessageSchemaTest, InterruptedMigrationRerunConverges)
{
    resetDb(kTestDb);
    MySQL conn;
    makeConn(conn, kTestDb);

    // 模拟中断：0001 已应用，0002 只执行前 3 条（部分建表，0002 未记录）。
    // 注：不能在空库上先建 DirectConversation/GroupConversation——MySQL 8.0
    // 拒绝引用尚不存在表的 FK（error 1824）；先落 0001 才是真实中断场景。
    std::string base = readFile(migrationsDir() + "/0001_baseline.sql");
    ASSERT_FALSE(base.empty()) << "0001_baseline.sql missing";
    for (const std::string& stmt : splitStatements(base)) {
        ASSERT_TRUE(conn.update(stmt)) << stmt;
    }
    std::string sql = readFile(migrationsDir() + "/0002_expand.sql");
    ASSERT_FALSE(sql.empty()) << "0002_expand.sql missing";
    std::vector<std::string> stmts = splitStatements(sql);
    ASSERT_EQ(6u, stmts.size()) << "0002 应为 6 条 CREATE TABLE";
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(conn.update(stmts[i])) << stmts[i];
    }
    EXPECT_EQ(8u, tableNames(conn).size());  // 0001 五表 + 0002 前三表

    // 中断后重跑：0001 全部 + 0002 剩余语句补齐（IF NOT EXISTS 幂等）。
    schema_migration::MigrateResult r = makeMigrator(kTestDb).migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(2u, r.applied.size());
    EXPECT_EQ("0001", r.applied[0]);
    EXPECT_EQ("0002", r.applied[1]);
    EXPECT_EQ(12u, tableNames(conn).size());

    // 已完成后重跑幂等。
    schema_migration::MigrateResult r2 = makeMigrator(kTestDb).migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_TRUE(r2.applied.empty());

    // 结果结构 = 全新空库迁移结果。
    resetDb(kTestDb2);
    migrateAll(kTestDb2);
    MySQL conn2;
    makeConn(conn2, kTestDb2);
    EXPECT_EQ(schemaDump(conn2), schemaDump(conn));
}

TEST(ReliableMessageSchemaTest, EmptyAndLegacyUpgradeConvergeToSameSchema)
{
    // 空库路径。
    resetDb(kTestDb);
    migrateAll(kTestDb);
    MySQL connEmpty;
    makeConn(connEmpty, kTestDb);
    std::string emptyDump = schemaDump(connEmpty);
    ASSERT_FALSE(emptyDump.empty());

    // 旧五表库路径：chat.sql DDL 快照 + 种子行。
    resetDb(kTestDb2);
    buildOldSnapshot(kTestDb2);
    MySQL connLegacy;
    makeConn(connLegacy, kTestDb2);
    seedLegacyRows(connLegacy);
    ASSERT_EQ(3ll, countRows(connLegacy, "User"));
    ASSERT_EQ(2ll, countRows(connLegacy, "Friend"));
    ASSERT_EQ(1ll, countRows(connLegacy, "AllGroup"));
    ASSERT_EQ(2ll, countRows(connLegacy, "GroupUser"));
    ASSERT_EQ(2ll, countRows(connLegacy, "OfflineMessage"));

    schema_migration::MigrateResult r = makeMigrator(kTestDb2).migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(2u, r.applied.size());
    EXPECT_EQ("0002", r.applied[1]);

    // 升级后旧 5 表数据/行数不变。
    EXPECT_EQ(3ll, countRows(connLegacy, "User"));
    EXPECT_EQ(2ll, countRows(connLegacy, "Friend"));
    EXPECT_EQ(1ll, countRows(connLegacy, "AllGroup"));
    EXPECT_EQ(2ll, countRows(connLegacy, "GroupUser"));
    EXPECT_EQ(2ll, countRows(connLegacy, "OfflineMessage"));
    MYSQL_RES* res = connLegacy.query("SELECT message FROM OfflineMessage WHERE userid=1");
    ASSERT_TRUE(res);
    MYSQL_ROW row = mysql_fetch_row(res);
    ASSERT_TRUE(row && row[0]);
    EXPECT_EQ(std::string("m1"), row[0]);
    mysql_free_result(res);

    // 空库与旧库升级结果结构 checksum 一致（columns/statistics/key_column_usage/
    // tables/referential_constraints dump，含 ENGINE 与 delete_rule 维度）。
    EXPECT_EQ(emptyDump, schemaDump(connLegacy));
}
