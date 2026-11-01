#pragma once

namespace FTAP::Panel
{
	// Creates the arrival panel window and the status page in SKSE Menu Framework.
	// Call once every plugin has loaded (kPostLoad). Returns false without the framework.
	bool Register();

	[[nodiscard]] bool IsAvailable();
	[[nodiscard]] bool IsOpen();
	// True once SKSE Menu Framework has fully let go of the screen and the game's input.
	[[nodiscard]] bool IsReleased();
	void               Open();
	void               Close();
}
