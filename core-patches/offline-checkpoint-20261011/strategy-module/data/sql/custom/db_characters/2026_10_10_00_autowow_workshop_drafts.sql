-- Pending characters schema. Unfunded draft orders only; no items, money or craft progress are created here.
CREATE TABLE `autowow_workshop_drafts` (
  `order_id` BIGINT UNSIGNED NOT NULL,
  `crafter_guid` BIGINT UNSIGNED NOT NULL,
  `recipient_guid` BIGINT UNSIGNED NOT NULL,
  `output_item_entry` INT UNSIGNED NOT NULL,
  `recipe_id` INT UNSIGNED NOT NULL,
  `profile_id` INT UNSIGNED NOT NULL,
  `profile_version` INT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL,
  `quantity` INT UNSIGNED NOT NULL,
  `stage_count` TINYINT UNSIGNED NOT NULL,
  `duration_0_ms` BIGINT UNSIGNED NOT NULL,
  `duration_1_ms` BIGINT UNSIGNED NOT NULL,
  `duration_2_ms` BIGINT UNSIGNED NOT NULL,
  `duration_3_ms` BIGINT UNSIGNED NOT NULL,
  `duration_4_ms` BIGINT UNSIGNED NOT NULL,
  `draft_state` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 unfunded draft; no transition admitted',
  `recorded_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`order_id`),
  KEY `crafter_drafts` (`crafter_guid`,`order_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
