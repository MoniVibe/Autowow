-- AutoWow League v0: persistent campaign state. Character, item, and gold rows remain
-- authoritative in their normal databases; this schema records league ownership and rules.

CREATE TABLE IF NOT EXISTS `autowow_league_team` (
  `team_id` varchar(32) NOT NULL,
  `display_name` varchar(64) NOT NULL,
  `faction` enum('Alliance','Horde','Neutral') NOT NULL,
  `roster_cap` tinyint unsigned NOT NULL DEFAULT 20,
  `worker_cap` tinyint unsigned NOT NULL DEFAULT 10,
  `treasury_copper` bigint NOT NULL DEFAULT 0,
  `created_at` timestamp NOT NULL DEFAULT current_timestamp(),
  `updated_at` timestamp NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  PRIMARY KEY (`team_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_league_member` (
  `character_guid` int unsigned NOT NULL,
  `team_id` varchar(32) DEFAULT NULL,
  `affiliation` enum('guild','wayfarer') NOT NULL DEFAULT 'guild',
  `role` enum('adventurer','worker','captain') NOT NULL DEFAULT 'adventurer',
  `class_plan` varchar(32) NOT NULL DEFAULT '',
  `profession_one` varchar(32) NOT NULL DEFAULT '',
  `profession_two` varchar(32) NOT NULL DEFAULT '',
  `active` tinyint(1) NOT NULL DEFAULT 0,
  `enrolled_at` timestamp NOT NULL DEFAULT current_timestamp(),
  `retired_at` timestamp NULL DEFAULT NULL,
  PRIMARY KEY (`character_guid`),
  KEY `idx_autowow_member_team_active` (`team_id`,`active`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_league_unlock` (
  `team_id` varchar(32) NOT NULL,
  `unlock_key` varchar(64) NOT NULL,
  `quantity` int unsigned NOT NULL DEFAULT 0,
  `updated_at` timestamp NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  PRIMARY KEY (`team_id`,`unlock_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_league_ledger` (
  `entry_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `team_id` varchar(32) NOT NULL,
  `entry_type` varchar(48) NOT NULL,
  `copper_delta` bigint NOT NULL DEFAULT 0,
  `material_key` varchar(64) NOT NULL DEFAULT '',
  `material_delta` int NOT NULL DEFAULT 0,
  `actor_guid` int unsigned DEFAULT NULL,
  `reference_id` bigint unsigned DEFAULT NULL,
  `note` varchar(255) NOT NULL DEFAULT '',
  `created_at` timestamp NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`entry_id`),
  KEY `idx_autowow_ledger_team_time` (`team_id`,`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_league_contract` (
  `contract_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `worker_guid` int unsigned NOT NULL,
  `sponsor_team_id` varchar(32) DEFAULT NULL,
  `contract_type` varchar(48) NOT NULL,
  `status` enum('offered','active','completed','cancelled') NOT NULL DEFAULT 'offered',
  `price_copper` bigint unsigned NOT NULL DEFAULT 0,
  `starts_at` timestamp NULL DEFAULT NULL,
  `expires_at` timestamp NULL DEFAULT NULL,
  `created_at` timestamp NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`contract_id`),
  KEY `idx_autowow_contract_worker` (`worker_guid`,`status`),
  KEY `idx_autowow_contract_sponsor` (`sponsor_team_id`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_league_challenge` (
  `challenge_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `issuer_team_id` varchar(32) NOT NULL,
  `target_team_id` varchar(32) NOT NULL,
  `battleground` varchar(32) NOT NULL,
  `squad_size` tinyint unsigned NOT NULL,
  `status` enum('pending','accepted','declined','forfeit','resolved','cancelled') NOT NULL DEFAULT 'pending',
  `issued_at` timestamp NOT NULL DEFAULT current_timestamp(),
  `respond_by` timestamp NULL DEFAULT NULL,
  `winner_team_id` varchar(32) DEFAULT NULL,
  `boon_key` varchar(64) NOT NULL DEFAULT '',
  `boon_expires_at` timestamp NULL DEFAULT NULL,
  `notes` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`challenge_id`),
  KEY `idx_autowow_challenge_status` (`status`,`respond_by`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `autowow_league_event` (
  `event_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `event_type` varchar(64) NOT NULL,
  `team_id` varchar(32) DEFAULT NULL,
  `character_guid` int unsigned DEFAULT NULL,
  `payload_json` text NOT NULL,
  `created_at` timestamp NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`event_id`),
  KEY `idx_autowow_event_time` (`created_at`),
  KEY `idx_autowow_event_team` (`team_id`,`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The controller stores only its current intent here. Quest status, rewards, XP, and inventory remain
-- authoritative in the character database and can be compared independently during each proof run.
CREATE TABLE IF NOT EXISTS `autowow_league_quest_state` (
  `leader_guid` int unsigned NOT NULL,
  `team_id` varchar(32) NOT NULL,
  `quest_id` int unsigned NOT NULL DEFAULT 0,
  `quest_status` tinyint unsigned NOT NULL DEFAULT 0,
  `phase` enum('acquire','objective','turnin','blocked') NOT NULL DEFAULT 'acquire',
  `destination` varchar(160) NOT NULL DEFAULT '',
  `accept_attempts` tinyint unsigned NOT NULL DEFAULT 0,
  `turnin_attempts` tinyint unsigned NOT NULL DEFAULT 0,
  `updated_at` timestamp NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  PRIMARY KEY (`leader_guid`),
  KEY `idx_autowow_quest_team_phase` (`team_id`,`phase`,`updated_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
