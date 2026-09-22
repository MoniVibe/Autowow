-- AutoWow Oracle race-seed campaign v1.
--
-- This schema is controller state only.  Characters, quest status, skills, talents, spells,
-- items, and money remain authoritative in their normal AzerothCore databases.  Every statement
-- is idempotent so the campaign can be installed again after a restart or a failed run.

CREATE TABLE IF NOT EXISTS `autowow_oracle_race_seed` (
  `character_guid` int unsigned NOT NULL,
  `race_id` tinyint unsigned NOT NULL,
  `race_name` varchar(32) NOT NULL,
  `faction` varchar(16) NOT NULL,
  `class_id` tinyint unsigned NOT NULL,
  `class_plan` varchar(48) NOT NULL DEFAULT '',
  `profession_one` varchar(32) NOT NULL DEFAULT '',
  `profession_two` varchar(32) NOT NULL DEFAULT '',
  `cohort` varchar(48) NOT NULL DEFAULT 'oracle-race-seeds',
  `oracle_enabled` tinyint(1) NOT NULL DEFAULT 1,
  `active` tinyint(1) NOT NULL DEFAULT 0,
  `last_level` tinyint unsigned NOT NULL DEFAULT 1,
  `last_xp` int unsigned NOT NULL DEFAULT 0,
  `last_snapshot_at` timestamp NULL DEFAULT NULL,
  `created_at` timestamp NOT NULL DEFAULT current_timestamp(),
  `updated_at` timestamp NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  PRIMARY KEY (`character_guid`),
  UNIQUE KEY `uq_autowow_oracle_race_seed_race` (`race_id`),
  KEY `idx_autowow_oracle_race_seed_active` (`active`,`oracle_enabled`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_oracle_progress` (
  `snapshot_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `run_id` varchar(64) NOT NULL,
  `character_guid` int unsigned NOT NULL,
  `race_id` tinyint unsigned NOT NULL,
  `race_name` varchar(32) NOT NULL,
  `level` tinyint unsigned NOT NULL DEFAULT 1,
  `xp` int unsigned NOT NULL DEFAULT 0,
  `money_copper` bigint unsigned NOT NULL DEFAULT 0,
  `online` tinyint(1) NOT NULL DEFAULT 0,
  `map_id` int unsigned NOT NULL DEFAULT 0,
  `zone_id` int unsigned NOT NULL DEFAULT 0,
  `quest_count` int unsigned NOT NULL DEFAULT 0,
  `completed_quest_count` int unsigned NOT NULL DEFAULT 0,
  `talent_count` int unsigned NOT NULL DEFAULT 0,
  `spell_count` int unsigned NOT NULL DEFAULT 0,
  `profession_json` text NOT NULL,
  `ai_json` text NOT NULL,
  `failure_codes` text NOT NULL,
  `captured_at` timestamp NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`snapshot_id`),
  KEY `idx_autowow_oracle_progress_guid_time` (`character_guid`,`captured_at`),
  KEY `idx_autowow_oracle_progress_run` (`run_id`,`captured_at`),
  KEY `idx_autowow_oracle_progress_race_time` (`race_id`,`captured_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_oracle_failure` (
  `character_guid` int unsigned NOT NULL,
  `domain` varchar(48) NOT NULL,
  `failure_code` varchar(96) NOT NULL,
  `status` varchar(16) NOT NULL DEFAULT 'open',
  `occurrences` int unsigned NOT NULL DEFAULT 1,
  `first_seen_at` timestamp NOT NULL DEFAULT current_timestamp(),
  `last_seen_at` timestamp NOT NULL DEFAULT current_timestamp(),
  `last_level` tinyint unsigned NOT NULL DEFAULT 1,
  `latest_payload_json` text NOT NULL,
  PRIMARY KEY (`character_guid`,`domain`,`failure_code`),
  KEY `idx_autowow_oracle_failure_status_time` (`status`,`last_seen_at`),
  KEY `idx_autowow_oracle_failure_domain` (`domain`,`failure_code`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
