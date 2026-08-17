-- P4-04 expand：消费侧 dead-letter 表 KafkaDeadLetter（docs/tasks/P4-04.md D3），
-- additive 追加，只 CREATE TABLE IF NOT EXISTS，不改、不删 0001/0002 既有表。
-- 契约：
--   UNIQUE(topic, partition_id, kafka_offset)  dead-letter 幂等落库（kill-前已
--                                             落库的事件重放时不双插）
--   reason 取值 poison_payload|unknown_event_type|sequence_regression|
--            sequence_conflict|message_missing（消费处置管线冻结词汇）
--   raw_value 保留原始 record bytes（信封原文，绝不只日志丢弃的证据面）
-- ENGINE=InnoDB DEFAULT CHARSET=utf8（0001/0002 风格）；IF NOT EXISTS（重跑幂等）。
-- 拆分契约：每条语句以 `;` 结尾，字符串字面量内无 `;`（loadMigrationFiles 解析依赖，
-- SchemaMigration runner 同款约定）。

CREATE TABLE IF NOT EXISTS KafkaDeadLetter(
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    topic VARCHAR(191) NOT NULL,
    partition_id INT NOT NULL,
    kafka_offset BIGINT UNSIGNED NOT NULL,
    message_id BIGINT UNSIGNED NULL,
    conversation_id BIGINT UNSIGNED NULL,
    sequence BIGINT UNSIGNED NULL,
    event_type VARCHAR(64) NULL,
    reason VARCHAR(32) NOT NULL,
    raw_value MEDIUMBLOB NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(topic, partition_id, kafka_offset)
)ENGINE=InnoDB DEFAULT CHARSET=utf8;
