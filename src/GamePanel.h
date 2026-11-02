#pragma once

// The arrival panel built in the game's own interface: a game menu (like the map or the game's
// message boxes) whose movie, Interface/FastTravelArrivalPlanner.swf, is an empty stage. The
// panel is built on it at runtime with the same text style as the map's location flyout, so
// the game draws it with the interface mod's font, sizes, colours, shadow and cursor.

namespace FTAP::GamePanel
{
	// Registers the menu with the game (once the game's data has loaded). False when the movie
	// file is missing, in which case the SKSE Menu Framework panel (or the game's box) is used.
	bool Register();
	[[nodiscard]] bool IsAvailable();

	void Open();
	void Close();
	// Asked to open, or open.
	[[nodiscard]] bool IsOpen();
	// Fully closed: the game no longer has the menu.
	[[nodiscard]] bool IsReleased();
}
