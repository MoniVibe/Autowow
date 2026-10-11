-- AutoWoW discovery ledger. Pending characters migration; no reward or native recipe is granted here.
CREATE TABLE `autowow_knowledge` (
  `scope_kind` TINYINT UNSIGNED NOT NULL,
  `scope_id` BIGINT UNSIGNED NOT NULL,
  `knowledge_id` INT UNSIGNED NOT NULL,
  `source_kind` TINYINT UNSIGNED NOT NULL,
  `source_family_id` INT UNSIGNED NOT NULL,
  `original_event_id` BIGINT UNSIGNED NOT NULL,
  `contributor_guid` BIGINT UNSIGNED NOT NULL,
  `earned_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`scope_kind`, `scope_id`, `knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE `autowow_knowledge_receipts` (
  `source_kind` TINYINT UNSIGNED NOT NULL,
  `source_family_id` INT UNSIGNED NOT NULL,
  `event_id` BIGINT UNSIGNED NOT NULL,
  `contributor_guid` BIGINT UNSIGNED NOT NULL,
  `knowledge_id` INT UNSIGNED NOT NULL,
  `scope_kind` TINYINT UNSIGNED NOT NULL,
  `scope_id` BIGINT UNSIGNED NOT NULL,
  `outcome` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 pending, 1 granted, 2 already earned',
  `settled_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`source_kind`, `source_family_id`, `event_id`, `contributor_guid`, `knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
