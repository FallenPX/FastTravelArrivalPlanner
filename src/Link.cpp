#include "pch.h"

#include "Link.h"

namespace FTAP::Link
{
	namespace
	{
		// Version 1 (FTTI 0.1.4) has everything the planner needs; version 2 (FTTI 0.2.0) adds the
		// flyout style for the panel's look.
		constexpr std::uint32_t MIN_INTERFACE_VERSION = 1;
		constexpr std::size_t   MIN_INTERFACE_SIZE = offsetof(FTTI_API::Interface, GetFlyoutStyle);

		std::atomic<const FTTI_API::Interface*> g_interface{ nullptr };
		std::atomic<std::uint32_t>              g_offeredVersion{ 0 };

		void OnMessage(SKSE::MessagingInterface::Message* a_message)
		{
			if (!a_message || a_message->type != FTTI_API::MESSAGE_INTERFACE || !a_message->data) {
				return;
			}
			const auto iface = static_cast<const FTTI_API::Interface*>(a_message->data);
			g_offeredVersion = iface->version;
			// Members are only ever added at the end, so a bigger interface from a newer FTTI is fine.
			if (iface->version < MIN_INTERFACE_VERSION || iface->size < MIN_INTERFACE_SIZE || a_message->dataLen < MIN_INTERFACE_SIZE ||
				!iface->GetTripTo || !iface->ExcludeNextTrip || !iface->FormatDuration || !iface->FormatArrival) {
				logs::error("Fast Travel Time Info offered interface version {}, but version {} or newer is needed. Update Fast Travel Time Info.",
					iface->version, MIN_INTERFACE_VERSION);
				return;
			}
			g_interface = iface;
			logs::info("{}", Describe());
		}
	}

	bool RegisterListener()
	{
		const auto messaging = SKSE::GetMessagingInterface();
		return messaging && messaging->RegisterListener(FTTI_API::PLUGIN_NAME, OnMessage);
	}

	const FTTI_API::Interface* Get()
	{
		return g_interface.load();
	}

	std::string Describe()
	{
		if (const auto iface = Get()) {
			return std::format("Connected to Fast Travel Time Info {} (interface version {})",
				iface->pluginVersion ? iface->pluginVersion : "?", iface->version);
		}
		if (const auto offered = g_offeredVersion.load(); offered != 0) {
			return std::format("Fast Travel Time Info is too old (interface version {}, need {})", offered, MIN_INTERFACE_VERSION);
		}
		return "Fast Travel Time Info 0.1.4 or newer was not found";
	}

	std::string FormatDuration(double a_hours, bool a_approx)
	{
		std::array<char, 128> buffer{};
		if (const auto iface = Get()) {
			iface->FormatDuration(a_hours, a_approx, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
		}
		return buffer.data();
	}

	bool GetFlyoutStyle(FTTI_API::FlyoutStyle& a_out)
	{
		a_out = FTTI_API::FlyoutStyle{};
		const auto iface = Get();
		if (!iface || iface->version < FTTI_API::FLYOUT_STYLE_VERSION || iface->size < sizeof(FTTI_API::Interface) || !iface->GetFlyoutStyle) {
			return false;  // FTTI older than 0.2.0: defaults
		}
		iface->GetFlyoutStyle(&a_out);
		return true;
	}

	std::string FormatArrival(double a_hoursFromNow)
	{
		std::array<char, 256> buffer{};
		if (const auto iface = Get()) {
			iface->FormatArrival(a_hoursFromNow, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
		}
		return buffer.data();
	}
}
