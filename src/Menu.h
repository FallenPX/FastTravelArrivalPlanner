#pragma once

namespace FTAP::Menu
{
	// Adds the settings page to SKSE Menu Framework, if it is installed. Call once every plugin
	// has loaded (kPostLoad). Without the framework, the settings stay INI-only.
	void Register();
}
