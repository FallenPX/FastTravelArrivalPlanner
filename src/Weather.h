#pragma once

namespace FTAP::Weather
{
	// Weather choices offered in the panel. Each maps to the classification the weather's
	// author set in its record (pleasant, cloudy, rainy, snow). Weathers without a
	// classification, such as Solstheim's ash storms, are never picked, but can still
	// happen under "Natural".
	enum class Choice : int
	{
		kNatural = 0,
		kClear,
		kCloudy,
		kRain,
		kSnow
	};
	inline constexpr int CHOICE_COUNT = 5;

	[[nodiscard]] const char* Name(Choice a_choice);

	struct Pick
	{
		RE::TESWeather* weather{ nullptr };
		std::string     reason;  // why nothing was picked, or where the weather came from
	};

	// Picks a weather of the chosen class from the region the player is standing in now,
	// using the game's own region weather lists (so DLC, Creation Club and mod weathers count).
	[[nodiscard]] Pick PickHere(Choice a_choice);

	// Makes a_weather the current weather at once. Not locked: the region's normal weather
	// changes take over afterwards.
	void Apply(RE::TESWeather* a_weather);

	[[nodiscard]] RE::TESWeather* Current();
	[[nodiscard]] std::string     Describe(const RE::TESWeather* a_weather);

	// Writes the weather lists of the regions the player is in to the log (each region once
	// per game session). Prototype test 7.
	void LogRegionsHere();
}
