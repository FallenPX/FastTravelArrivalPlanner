#pragma once

#include "FTTI_API.h"

namespace FTAP::Link
{
	// Listens for Fast Travel Time Info's interface. Call while the plugin loads.
	bool RegisterListener();

	// The interface, or nullptr when FTTI is missing or too old.
	[[nodiscard]] const FTTI_API::Interface* Get();

	// One line for the log and the status page, e.g. "connected to Fast Travel Time Info 0.1.4".
	[[nodiscard]] std::string Describe();

	// Formats through FTTI, so the panel shows exactly what FTTI's flyout shows.
	[[nodiscard]] std::string FormatDuration(double a_hours, bool a_approx);
	[[nodiscard]] std::string FormatArrival(double a_hoursFromNow);

	// The map flyout's colours and FTTI's background settings. False (and defaults) with an
	// FTTI older than 0.2.0.
	bool GetFlyoutStyle(FTTI_API::FlyoutStyle& a_out);

	// The flyout's font, text sizes and shadow. False (and defaults) with an FTTI older than
	// 0.3.0, or before a flyout has been shown.
	bool GetFlyoutText(FTTI_API::FlyoutText& a_out);
}
