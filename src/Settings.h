#pragma once

namespace FTAP
{
	// User options, read from the player's own settings file (see FilePath), which is created
	// from Data/SKSE/Plugins/FastTravelArrivalPlanner.ini and survives updates: options a newer
	// version adds are copied in, options it drops are taken out, and nothing else changes.
	// Every value has a safe default, so a missing or partial INI is fine.
	// The live settings are an immutable snapshot swapped as a whole, so the panel, the
	// settings page and the planner can read them at any time.
	struct Settings
	{
		// The range of minHoursForFaster, in game hours.
		static constexpr float MIN_HOURS_FOR_FASTER = 1.0f;
		static constexpr float MAX_HOURS_FOR_FASTER = 3.0f;

		bool  showPanel{ true };               // 0 = always use the game's own confirmation box
		float minHoursForFaster{ 1.0f };       // Instant, 1/3 and 1/2 need at least this ETA (1 to 3 game hours)
		bool  skipAfterConversation{ true };   // map opened from a conversation: leave it to the game
		float conversationSeconds{ 2.0f };     // "right after a conversation" window

		[[nodiscard]] static std::shared_ptr<const Settings> Current();
		// Bumped whenever the live settings are replaced (for the settings page's own copies).
		[[nodiscard]] static std::uint32_t Version();
		// Replaces the live settings (from the settings page).
		static void Set(Settings a_settings);
		// Reads the player's settings file again (preparing it first).
		static void Reload();
		// Writes the given settings into the player's settings file. Comments, layout and
		// options the player commented out are kept.
		static bool                                         SaveToIni(const Settings& a_settings);
		[[nodiscard]] static std::string                    FilePath();
	};
}
