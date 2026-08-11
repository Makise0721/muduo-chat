-- P3-03 expand：可靠消息六表（Conversation/DirectConversation/GroupConversation/
-- ChatMessage/MessageDelivery/OutboxEvent），additive 追加，不改、不删 0001 五表。
-- 契约 docs/specs/message-reliability.md（计划 §6）：
--   UNIQUE(sender_id, client_message_id)     幂等接受
--   UNIQUE(conversation_id, sequence)        局部顺序
--   UNIQUE(event_type, aggregate_message_id) outbox 事件幂等
--   UNIQUE(user_low_id, user_high_id)        direct 对话唯一对
--   UNIQUE(group_id)                         群对话一表一
--   PRIMARY KEY(message_id, recipient_id)    delivery 每接收者一行
--   INDEX(recipient_id, state, next_attempt_at) 投递扫描
-- FK 类型与引用列同型（INT/BIGINT UNSIGNED），沿用 0001 的 ON DELETE CASCADE；
-- ENGINE=InnoDB DEFAULT CHARSET=utf8（0001 风格）；全部 IF NOT EXISTS（中断重跑幂等）。
-- 拆分契约：每条语句以 `;` 结尾，字符串字面量内无 `;`。

CREATE TABLE IF NOT EXISTS Conversation(
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    kind ENUM('DIRECT','GROUP') NOT NULL,
    next_sequence BIGINT UNSIGNED NOT NULL DEFAULT 0
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS DirectConversation(
    conversation_id BIGINT UNSIGNED NOT NULL,
    user_low_id INT NOT NULL,
    user_high_id INT NOT NULL,
    PRIMARY KEY(conversation_id),
    UNIQUE(user_low_id, user_high_id),
    FOREIGN KEY (conversation_id) REFERENCES Conversation(id) ON DELETE CASCADE,
    FOREIGN KEY (user_low_id) REFERENCES User(id) ON DELETE CASCADE,
    FOREIGN KEY (user_high_id) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS GroupConversation(
    conversation_id BIGINT UNSIGNED NOT NULL,
    group_id INT NOT NULL,
    PRIMARY KEY(conversation_id),
    UNIQUE(group_id),
    FOREIGN KEY (conversation_id) REFERENCES Conversation(id) ON DELETE CASCADE,
    FOREIGN KEY (group_id) REFERENCES AllGroup(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS ChatMessage(
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    conversation_id BIGINT UNSIGNED NOT NULL,
    sender_id INT NOT NULL,
    client_message_id VARCHAR(64) CHARACTER SET ascii NOT NULL,
    sequence BIGINT UNSIGNED NOT NULL,
    content MEDIUMBLOB NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(conversation_id, sequence),
    UNIQUE(sender_id, client_message_id),
    FOREIGN KEY (conversation_id) REFERENCES Conversation(id) ON DELETE CASCADE,
    FOREIGN KEY (sender_id) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS MessageDelivery(
    message_id BIGINT UNSIGNED NOT NULL,
    recipient_id INT NOT NULL,
    state TINYINT NOT NULL DEFAULT 0,
    attempt_count INT NOT NULL DEFAULT 0,
    next_attempt_at DATETIME NULL,
    lease_owner VARCHAR(64) NULL,
    lease_until DATETIME NULL,
    last_sent_at DATETIME NULL,
    acknowledged_at DATETIME NULL,
    expires_at DATETIME NULL,
    PRIMARY KEY(message_id, recipient_id),
    INDEX(recipient_id, state, next_attempt_at),
    FOREIGN KEY (message_id) REFERENCES ChatMessage(id) ON DELETE CASCADE,
    FOREIGN KEY (recipient_id) REFERENCES User(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS OutboxEvent(
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    aggregate_message_id BIGINT UNSIGNED NOT NULL,
    event_type VARCHAR(64) NOT NULL,
    payload MEDIUMBLOB NOT NULL,
    available_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    lease_owner VARCHAR(64) NULL,
    lease_until DATETIME NULL,
    attempt_count INT NOT NULL DEFAULT 0,
    processed_at DATETIME NULL,
    UNIQUE(event_type, aggregate_message_id),
    FOREIGN KEY (aggregate_message_id) REFERENCES ChatMessage(id) ON DELETE CASCADE
)ENGINE=InnoDB DEFAULT CHARSET=utf8;
