function onUpdateDatabase()
	logger.info("Updating database to version 59 (add PlayerBot planner checkpoints)")

	if db.tableExists("player_bot_planner_state") then
		logger.warn("Table player_bot_planner_state already exists, skipping migration")
		return true
	end

	return db.query([[
		CREATE TABLE `player_bot_planner_state` (
			`player_id` int(11) NOT NULL,
			`schema_version` smallint UNSIGNED NOT NULL,
			`checkpoint_revision` bigint UNSIGNED NOT NULL,
			`policy_revision` bigint UNSIGNED NOT NULL,
			`goal_id` bigint UNSIGNED NOT NULL,
			`goal_type` tinyint UNSIGNED NOT NULL,
			`plan_revision` bigint UNSIGNED NOT NULL,
			`verified_step_index` smallint UNSIGNED NOT NULL,
			`verified_subsystem` tinyint UNSIGNED NOT NULL,
			`failure_count` tinyint UNSIGNED NOT NULL,
			`retry_count` tinyint UNSIGNED NOT NULL,
			`configured_target_id` bigint UNSIGNED NOT NULL,
			`region_x` smallint UNSIGNED NOT NULL,
			`region_y` smallint UNSIGNED NOT NULL,
			`region_z` tinyint UNSIGNED NOT NULL,
			`safe_boundary` tinyint(1) NOT NULL,
			`checksum` bigint UNSIGNED NOT NULL,
			CONSTRAINT `player_bot_planner_state_pk` PRIMARY KEY (`player_id`),
			CONSTRAINT `player_bot_planner_state_player_fk`
				FOREIGN KEY (`player_id`) REFERENCES `players` (`id`)
				ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8;
	]])
end
