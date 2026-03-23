-- 聊天室数据库表结构
-- 创建数据库
CREATE DATABASE IF NOT EXISTS chat;
USE chat;

-- 用户表
CREATE TABLE IF NOT EXISTS User(
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(50) NOT NULL,
    state ENUM('online', 'offline') DEFAULT 'offline'
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- 好友表
CREATE TABLE IF NOT EXISTS Friend(
    userid INT NOT NULL,
    friendid INT NOT NULL,
    PRIMARY KEY(userid, friendid),
    FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE,
    FOREIGN KEY (friendid) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- 群组表
CREATE TABLE IF NOT EXISTS AllGroup(
    id INT PRIMARY KEY AUTO_INCREMENT,
    groupname VARCHAR(50) NOT NULL,
    groupdesc VARCHAR(200) DEFAULT ''
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- 群组用户表
CREATE TABLE IF NOT EXISTS GroupUser(
    groupid INT NOT NULL,
    userid INT NOT NULL,
    grouprole ENUM('creator', 'normal') DEFAULT 'normal',
    PRIMARY KEY(groupid, userid),
    FOREIGN KEY (groupid) REFERENCES AllGroup(id) ON DELETE CASCADE,
    FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- 离线消息表
CREATE TABLE IF NOT EXISTS OfflineMessage(
    id INT PRIMARY KEY AUTO_INCREMENT,
    userid INT NOT NULL,
    message VARCHAR(500) NOT NULL,
    FOREIGN KEY (userid) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- 插入测试数据
INSERT INTO User(name, password, state) VALUES
('makise', '123456', 'offline'),
('alice', '123456', 'offline'),
('bob', '123456', 'offline');

INSERT INTO Friend(userid, friendid) VALUES
(1, 2),
(1, 3);

INSERT INTO AllGroup(groupname, groupdesc) VALUES
('聊天室', '这是一个测试群组');

INSERT INTO GroupUser(groupid, userid, grouprole) VALUES
(1, 1, 'creator'),
(1, 2, 'normal'),
(1, 3, 'normal');
