#pragma once

#include "FTTI_API.h"
#include "Weather.h"

namespace FTAP::Planner
{
	enum class Timing : int
	{
		kInstant = 0,
		kThird,
		kHalf,
		kNormal,
		kLater
	};
	inline constexpr int TIMING_COUNT = 5;

	inline constexpr int MAX_LATER_DAYS = 30;

	// Everything the panel needs, copied out under the planner's lock.
	struct PanelData
	{
		std::uint64_t                            serial{ 0 };  // changes whenever a new panel opens
		bool                                     open{ false };
		std::string                              destination;
		FTTI_API::TripInfo                       trip;
		double                                   nowHour{ 0.0 };     // game hour when the panel opened (time stands still on the map)
		std::vector<std::pair<int, std::string>> extraButtons;      // the game's other buttons, e.g. Place Marker
		bool                                     weatherOptions{ true };
		float                                    minHoursForFaster{ 1.0f };
	};
	[[nodiscard]] PanelData GetPanelData();

	struct Choice
	{
		enum class Action
		{
			kProceed,
			kCancel,
			kOther
		};
		Action          action{ Action::kCancel };
		int             otherIndex{ -1 };    // kOther: which of the game's buttons
		Timing          timing{ Timing::kNormal };
		double          laterHours{ 0.0 };   // kLater: arrival, in hours from now
		Weather::Choice weather{ Weather::Choice::kNatural };
		std::uint64_t   serial{ 0 };
	};

	// Called by the panel (render thread). The choice is carried out on the UI thread.
	void Submit(const Choice& a_choice);

	// Lines describing the last planned trip, for the status page.
	[[nodiscard]] std::vector<std::string> LastTripLines();

	// Event sinks and the map hook. Call at kDataLoaded.
	void Install();

	// A save is being loaded or a new game started: undo anything still in effect.
	void OnGameLoad();
}
