#include "pch.h"

#include "Link.h"
#include "Menu.h"
#include "Panel.h"
#include "Planner.h"
#include "Settings.h"

namespace
{
	// bDetailedLog: every step of each trip, the panel's inputs and layout details go to the
	// log as well. Switched at once, from the settings file or the settings page.
	void ApplyLogLevel(const FTAP::Settings& a_settings)
	{
		static std::optional<bool> applied;
		if (applied == a_settings.detailedLog) {
			return;
		}
		const auto level = a_settings.detailedLog ? spdlog::level::debug : spdlog::level::info;
		if (const auto logger = spdlog::default_logger()) {
			logger->set_level(level);
			logger->flush_on(level);
		}
		if (applied.has_value() || a_settings.detailedLog) {
			logs::info("Detailed log {}", a_settings.detailedLog ? "on" : "off");
		}
		applied = a_settings.detailedLog;
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Every SKSE plugin is loaded by now, so SKSE Menu Framework can be found.
			FTAP::Menu::Register();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			// Fast Travel Time Info has sent its interface by now (it does so at kPostLoad); the
			// connection itself was logged when it came.
			if (!FTAP::Link::Get()) {
				logs::error("{}. The planner stays inactive and the game's own box is used.", FTAP::Link::Describe());
			}
			FTAP::Panel::Register();
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
	logs::info("Fast Travel Arrival Planner {} loading", SKSE::GetPluginVersion().string());

	if (REL::Module::IsVR()) {
		logs::warn("Skyrim VR is not supported; the plugin will stay inactive");
		return true;
	}

	FTAP::Settings::SetListener(ApplyLogLevel);
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
