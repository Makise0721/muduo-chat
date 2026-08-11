#include "db/SchemaMigration.hpp"

#include "db/MySQL.hpp"
#include "db/MySQLGuards.hpp"

#include <dirent.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace schema_migration {

namespace {

const char* kMigrationsTable =
    "CREATE TABLE IF NOT EXISTS schema_migrations("
    "version VARCHAR(32) NOT NULL PRIMARY KEY,"
    "checksum CHAR(64) NOT NULL,"
    "applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8";

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

std::string lastError(MySQL& conn)
{
    return mysql_error(conn.getConnection());
}

// 受控拆分约定：忽略空行与 `--` 注释行，按 `;` 切语句；
// 文件内不得在字符串字面量中使用 `;`。
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

std::string readFile(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return "";
    }
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return out;
}

// 自包含 SHA-256（FIPS 180-4），输出小写十六进制。
std::string sha256Hex(const std::string& data)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    std::string msg = data;
    msg.push_back('\x80');
    while (msg.size() % 64 != 56) {
        msg.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bitlen >> (i * 8)) & 0xff));
    }
    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4])) << 24)
                 | (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 1])) << 16)
                 | (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 2])) << 8)
                 | static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25))
                        ^ ((w[i - 15] >> 18) | (w[i - 15] << 14))
                        ^ (w[i - 15] >> 3);
            uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15))
                        ^ ((w[i - 2] >> 19) | (w[i - 2] << 13))
                        ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
        uint32_t e = H[4], f = H[5], g = H[6], h = H[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + s1 + ch + K[i] + w[i];
            uint32_t s0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        H[0] += a;
        H[1] += b;
        H[2] += c;
        H[3] += d;
        H[4] += e;
        H[5] += f;
        H[6] += g;
        H[7] += h;
    }
    char hex[65];
    for (int i = 0; i < 8; ++i) {
        snprintf(hex + i * 8, 9, "%08x", H[i]);
    }
    return std::string(hex, 64);
}

struct MigrationFile {
    int version = 0;
    std::string versionStr;
    std::string path;
    std::string content;
    std::string checksum;
};

bool isDigit(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

bool loadMigrationFiles(const std::string& dir, std::vector<MigrationFile>* out, std::string* error)
{
    DIR* d = opendir(dir.c_str());
    if (!d) {
        *error = "migrations dir not found: " + dir;
        return false;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        size_t us = name.find('_');
        if (us == std::string::npos || name.size() < us + 5) {
            continue;
        }
        if (name.compare(name.size() - 4, 4, ".sql") != 0) {
            continue;
        }
        std::string digits = name.substr(0, us);
        if (!isDigit(digits)) {
            continue;
        }
        MigrationFile f;
        f.version = atoi(digits.c_str());
        char buf[16];
        snprintf(buf, sizeof buf, "%04d", f.version);
        f.versionStr = buf;
        f.path = dir + "/" + name;
        f.content = readFile(f.path);
        if (f.content.empty()) {
            *error = "cannot read migration file: " + f.path;
            closedir(d);
            return false;
        }
        f.checksum = sha256Hex(f.content);
        out->push_back(f);
    }
    closedir(d);
    std::sort(out->begin(), out->end(),
              [](const MigrationFile& a, const MigrationFile& b) { return a.version < b.version; });
    return true;
}

// advisory lock RAII：析构时释放，任何返回路径都解锁。
struct LockGuard {
    MySQL* conn = nullptr;
    std::string lockName;
    bool held = false;

    ~LockGuard()
    {
        if (conn && held) {
            MYSQL_RES* res = conn->query("SELECT RELEASE_LOCK('" + lockName + "')");
            if (res) {
                mysql_free_result(res);
            }
        }
    }
};

}  // namespace

Migrator::Migrator(std::string host, std::string user, std::string password,
                   std::string dbname, unsigned int port)
    : host_(std::move(host)),
      user_(std::move(user)),
      password_(std::move(password)),
      dbname_(std::move(dbname)),
      port_(port)
{
}

MigrateResult Migrator::migrateTo(const std::string& migrationsDir,
                                  const std::string& targetVersion,
                                  int lockTimeoutSecs)
{
    MigrateResult result;
    MySQL conn;
    if (!conn.connect(host_, user_, password_, dbname_, port_)) {
        result.error = "cannot connect to database: " + dbname_;
        return result;
    }

    int target = 0x7fffffff;
    if (!targetVersion.empty()) {
        char* end = nullptr;
        long v = strtol(targetVersion.c_str(), &end, 10);
        if (end == targetVersion.c_str() || *end != '\0' || v < 0 || v > 0x7fffffff) {
            result.error = "invalid target version: " + targetVersion;
            return result;
        }
        target = static_cast<int>(v);
    }

    LockGuard lock;
    lock.conn = &conn;
    // dbname 可能含 SQL 特殊字符（如引号），转义后再拼进 GET_LOCK/RELEASE_LOCK。
    lock.lockName = "muduo_chat_schema_migration:" + escapeString(conn.getConnection(), dbname_);
    {
        char sql[256];
        snprintf(sql, sizeof sql, "SELECT GET_LOCK('%s', %d)", lock.lockName.c_str(), lockTimeoutSecs);
        MYSQL_RES* res = conn.query(sql);
        if (!res) {
            result.error = "GET_LOCK failed: " + lastError(conn);
            return result;
        }
        MYSQL_ROW row = mysql_fetch_row(res);
        int got = (row && row[0]) ? atoi(row[0]) : 0;
        mysql_free_result(res);
        if (got != 1) {
            result.error = "could not acquire migration lock after " + std::to_string(lockTimeoutSecs)
                           + "s (another migration may be running)";
            return result;
        }
        lock.held = true;
    }

    if (!conn.update(kMigrationsTable)) {
        result.error = "cannot create schema_migrations: " + lastError(conn);
        return result;
    }

    std::vector<MigrationFile> files;
    if (!loadMigrationFiles(migrationsDir, &files, &result.error)) {
        return result;
    }

    std::map<std::string, std::string> appliedChecksums;
    {
        MYSQL_RES* res = conn.query("SELECT version, checksum FROM schema_migrations");
        if (!res) {
            result.error = "cannot read schema_migrations: " + lastError(conn);
            return result;
        }
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            if (row[0] && row[1]) {
                appliedChecksums[row[0]] = row[1];
            }
        }
        mysql_free_result(res);
    }

    // fail-fast：已应用版本的文件缺失或 checksum 不符，在任何新应用之前报错。
    for (std::map<std::string, std::string>::const_iterator it = appliedChecksums.begin();
         it != appliedChecksums.end(); ++it) {
        const MigrationFile* f = nullptr;
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].versionStr == it->first) {
                f = &files[i];
                break;
            }
        }
        if (!f) {
            result.error = "checksum fail-fast: applied migration file missing for version "
                           + it->first;
            return result;
        }
        if (f->checksum != it->second) {
            result.error = "checksum fail-fast: migration file modified after apply, version "
                           + it->first;
            return result;
        }
    }

    for (size_t i = 0; i < files.size(); ++i) {
        const MigrationFile& f = files[i];
        if (f.version > target) {
            continue;
        }
        if (appliedChecksums.count(f.versionStr)) {
            continue;
        }
        const std::vector<std::string> statements = splitStatements(f.content);
        for (size_t j = 0; j < statements.size(); ++j) {
            if (!conn.update(statements[j])) {
                result.error = "migration " + f.versionStr + " statement " + std::to_string(j + 1)
                               + " failed: " + lastError(conn);
                return result;
            }
        }
        char sql[512];
        snprintf(sql, sizeof sql, "INSERT INTO schema_migrations(version, checksum) VALUES('%s', '%s')",
                 f.versionStr.c_str(), f.checksum.c_str());
        if (!conn.update(sql)) {
            result.error = "cannot record migration " + f.versionStr + ": " + lastError(conn);
            return result;
        }
        result.applied.push_back(f.versionStr);
    }

    result.ok = true;
    return result;
}

StatusResult Migrator::status()
{
    StatusResult result;
    MySQL conn;
    if (!conn.connect(host_, user_, password_, dbname_, port_)) {
        result.error = "cannot connect to database: " + dbname_;
        return result;
    }
    MYSQL_RES* res = conn.query("SELECT version, checksum, applied_at FROM schema_migrations ORDER BY version");
    if (!res) {
        result.error = "cannot read schema_migrations: " + lastError(conn);
        return result;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!row[0]) {
            continue;
        }
        AppliedVersion v;
        v.version = row[0];
        v.checksum = row[1] ? row[1] : "";
        v.appliedAt = row[2] ? row[2] : "";
        result.versions.push_back(v);
    }
    mysql_free_result(res);
    result.ok = true;
    return result;
}

}  // namespace schema_migration
