#include "pch.h"

#include "Link.h"
#include "Panel.h"
#include "Planner.h"
#include "Settings.h"

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Every SKSE plugin is loaded by now, so SKSE Menu Framework can be found.
			FTAP::Panel::Register();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			// Fast Travel Time Info has sent its interface by now (it does so at kPostLoad).
			if (FTAP::Link::Get()) {
				logs::info("{}", FTAP::Link::Describe());
			} else {
				logs::error("{}. The planner stays inactive and the game's own box is used.", FTAP::Link::Describe());
			}
			FTAP::Planner::Install();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			FTAP::Planner::OnGameLoad();
			break;
		default:
			break;
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logs::info("Fast Travel Arrival Planner {} loading (prototype)", SKSE::GetPluginVersion().string());

	if (REL::Module::IsVR()) {
		logs::warn("Skyrim VR is not supported; the plugin will stay inactive");
		return true;
	}

	FTAP::Settings::Reload();

	// Fast Travel Time Info sends its interface once every plugin has loaded, so the
	// listener has to be in place now.
	if (!FTAP::Link::RegisterListener()) {
		logs::error("Could not listen for Fast Travel Time Info's messages");
	}

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		logs::error("Could not register for SKSE messages");
		return false;
	}
	return true;
}
