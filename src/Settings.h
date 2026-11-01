#pragma once

namespace FTAP
{
	// User options, read from Data/SKSE/Plugins/FastTravelArrivalPlanner.ini.
	// Every value has a safe default, so a missing or partial INI is fine.
	// The live settings are an immutable snapshot swapped as a whole, so the panel
	// (render thread) and the planner (UI and main threads) can read them at any time.
	struct Settings
	{
		bool  enabled{ true };                 // master switch
		bool  showPanel{ true };               // 0 = always use the game's own confirmation box
		float minHoursForFaster{ 1.0f };       // Instant, 1/3 and 1/2 need at least this ETA (0 = no limit)
		bool  laterByClock{ true };            // Later: normal trip, then the clock moves forward (0 = stretch the trip, as in 0.0.2)
		bool  skipAfterConversation{ true };   // map opened from a conversation: leave it to the game
		float conversationSeconds{ 2.0f };     // "right after a conversation" window

		[[nodiscard]] static std::shared_ptr<const Settings> Current();
		static void                                         Reload();
	};
}
