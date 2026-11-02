#pragma once

// The arrival panel, drawn by the game's own interface (by default) or by SKSE Menu
// Framework. Which one is decided each time a panel opens (the INI's bGameInterface, and
// which of the two is available).
namespace FTAP::Panel
{
	// Creates the SKSE Menu Framework panel window and the status page.
	// Call once every plugin has loaded (kPostLoad). Returns false without the framework.
	bool Register();
	// Registers the panel in the game's own interface. Call once the game's data has loaded
	// (kDataLoaded). Returns false when its movie file is missing.
	bool RegisterGameInterface();

	// Either panel can be shown.
	[[nodiscard]] bool IsAvailable();
	// The panel opened last is asked to be open, or is open.
	[[nodiscard]] bool IsOpen();
	// True once the panel opened last has fully let go of the screen and the game's input.
	[[nodiscard]] bool IsReleased();
	// Which one draws the panel opened last, for the log.
	[[nodiscard]] std::string_view Name();
	void               Open();
	void               Close();
}
