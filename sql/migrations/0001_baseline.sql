-- 聊天室数据库基线表结构（无种子）
-- 与 sql/chat.sql 的 CREATE TABLE 语句逐字节一致（SchemaMigrationTest 漂移契约）
-- 旧五表库已存在这些表时，CREATE TABLE IF NOT EXISTS 为 no-op，不报错

CREATE TABLE IF NOT EXISTS User(
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(50) NOT NULL,
    state ENUM('online', 'offline') DEFAULT 'offline'
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS Friend(
    userid INT NOT NULL,
    friendid INT NOT NULL,
    PRIMARY KEY(userid, friendid),
    FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE,
    FOREIGN KEY (friendid) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS AllGroup(
    id INT PRIMARY KEY AUTO_INCREMENT,
    groupname VARCHAR(50) NOT NULL,
    groupdesc VARCHAR(200) DEFAULT ''
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS GroupUser(
    groupid INT NOT NULL,
    userid INT NOT NULL,
    grouprole ENUM('creator', 'normal') DEFAULT 'normal',
    PRIMARY KEY(groupid, userid),
    FOREIGN KEY (groupid) REFERENCES AllGroup(id) ON DELETE CASCADE,
    FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS OfflineMessage(
    id INT PRIMARY KEY AUTO_INCREMENT,
    userid INT NOT NULL,
    message VARCHAR(500) NOT NULL,
    FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;
