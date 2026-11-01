#include "pch.h"

#include "Panel.h"
#include "Link.h"
#include "Planner.h"
#include "Settings.h"

// SKSEMenuFramework.h includes <windows.h>, so it is kept to this one file.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include "SKSEMenuFramework.h"

// The arrival panel (a SKSE Menu Framework window that blocks the map behind it) and a small
// status page in the framework's own menu. Rendering happens on the render thread: the panel
// only reads a copy of the planner's data and hands the player's choice back to the planner.

namespace FTAP::Panel
{
	namespace
	{
		using Planner::Timing;
		using Weather::Choice;

		MENU_WINDOW       g_window = nullptr;
		std::atomic<bool> g_available{ false };

		// Panel state (render thread only), reset whenever a new panel opens.
		struct UiState
		{
			std::uint64_t serial{ 0 };
			int           timing{ static_cast<int>(Timing::kNormal) };
			int           weather{ static_cast<int>(Choice::kNatural) };
			int           laterDay{ 0 };
			int           laterHour{ 0 };
		};
		UiState g_ui;

		// Later arrivals, in whole hours. Times are counted from today's midnight; the day is
		// counted from the day of the normal arrival.
		struct LaterRange
		{
			double etaAbs{ 0.0 };   // normal arrival, hours since today's midnight
			int    etaDay{ 0 };     // days after today
			int    etaHour{ 0 };    // whole hour of the day of the normal arrival
			int    dayMin{ 0 };
			int    dayMax{ Planner::MAX_LATER_DAYS };

			[[nodiscard]] int HourMin(int a_day) const { return a_day == 0 ? etaHour + 1 : 0; }
			[[nodiscard]] int HourMax(int a_day) const { return a_day == Planner::MAX_LATER_DAYS ? etaHour : 23; }
			// Arrival in hours from now.
			[[nodiscard]] double HoursFromNow(int a_day, int a_hour, double a_nowHour) const
			{
				return (etaDay + a_day) * 24.0 + a_hour - a_nowHour;
			}
		};

		LaterRange MakeRange(double a_nowHour, double a_eta)
		{
			LaterRange r;
			r.etaAbs = a_nowHour + a_eta;
			r.etaDay = static_cast<int>(std::floor(r.etaAbs / 24.0));
			r.etaHour = static_cast<int>(std::floor(r.etaAbs - r.etaDay * 24.0));
			r.etaHour = std::clamp(r.etaHour, 0, 23);
			r.dayMin = r.etaHour >= 23 ? 1 : 0;
			return r;
		}

		void Tooltip(const char* a_text)
		{
			if (ImGuiMCP::IsItemHovered()) {
				ImGuiMCP::SetTooltip("%s", a_text);
			}
		}

		bool Radio(const char* a_label, int* a_value, int a_option, bool a_enabled, const char* a_disabledReason)
		{
			ImGuiMCP::BeginDisabled(!a_enabled);
			const bool changed = ImGuiMCP::RadioButton(a_label, a_value, a_option);
			ImGuiMCP::EndDisabled();
			if (!a_enabled && a_disabledReason && ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGuiMCP::SetTooltip("%s", a_disabledReason);
			}
			return changed;
		}

		void Submit(Planner::Choice::Action a_action, const Planner::PanelData& a_data, double a_laterHours, int a_otherIndex = -1)
		{
			Planner::Choice choice;
			choice.action = a_action;
			choice.otherIndex = a_otherIndex;
			choice.timing = static_cast<Timing>(g_ui.timing);
			choice.laterHours = a_laterHours;
			choice.weather = static_cast<Choice>(g_ui.weather);
			choice.serial = a_data.serial;
			Planner::Submit(choice);
		}

		void RenderPanelContents(const Planner::PanelData& a_data)
		{
			const auto& trip = a_data.trip;
			const bool  hasEstimate = trip.valid && trip.hours > 0.0;
			const bool  fasterAllowed = hasEstimate && (a_data.minHoursForFaster <= 0.0f || trip.hours >= a_data.minHoursForFaster);
			const auto  fasterReason = std::format("Needs a travel time of at least {}.", Link::FormatDuration(a_data.minHoursForFaster, false));

			// Normal trip
			if (hasEstimate) {
				ImGuiMCP::Text("Travel time: %s", Link::FormatDuration(trip.hours, !trip.calibrated).c_str());
				ImGuiMCP::Text("Normal arrival: %s", Link::FormatArrival(trip.hours).c_str());
			} else if (trip.otherWorldspace) {
				ImGuiMCP::TextWrapped("%s", "No estimate: the destination is in another worldspace. Only a normal trip is available.");
			} else {
				ImGuiMCP::TextWrapped("%s", "No estimate for this trip. Only a normal trip is available.");
			}

			// Timing
			ImGuiMCP::SeparatorText("Arrival");
			if (!hasEstimate) {
				g_ui.timing = static_cast<int>(Timing::kNormal);
			} else if (!fasterAllowed && g_ui.timing < static_cast<int>(Timing::kNormal)) {
				g_ui.timing = static_cast<int>(Timing::kNormal);
			}
			const char* noEstimate = "No estimate for this trip.";
			const char* fasterWhy = hasEstimate ? fasterReason.c_str() : noEstimate;
			Radio("Instant", &g_ui.timing, static_cast<int>(Timing::kInstant), fasterAllowed, fasterWhy);
			ImGuiMCP::SameLine();
			Radio("1/3", &g_ui.timing, static_cast<int>(Timing::kThird), fasterAllowed, fasterWhy);
			ImGuiMCP::SameLine();
			Radio("1/2", &g_ui.timing, static_cast<int>(Timing::kHalf), fasterAllowed, fasterWhy);
			ImGuiMCP::SameLine();
			Radio("Normal ETA", &g_ui.timing, static_cast<int>(Timing::kNormal), true, nullptr);
			ImGuiMCP::SameLine();
			Radio("Later", &g_ui.timing, static_cast<int>(Timing::kLater), hasEstimate, noEstimate);

			double arrivalHours = hasEstimate ? trip.hours : 0.0;
			double laterHours = 0.0;
			switch (static_cast<Timing>(g_ui.timing)) {
			case Timing::kInstant:
				arrivalHours = 0.0;
				break;
			case Timing::kThird:
				arrivalHours = trip.hours / 3.0;
				break;
			case Timing::kHalf:
				arrivalHours = trip.hours / 2.0;
				break;
			case Timing::kLater:
				{
					const auto range = MakeRange(a_data.nowHour, trip.hours);
					g_ui.laterDay = std::clamp(g_ui.laterDay, range.dayMin, range.dayMax);
					ImGuiMCP::SetNextItemWidth(260.0f);
					ImGuiMCP::SliderInt("Day##later", &g_ui.laterDay, range.dayMin, range.dayMax,
						g_ui.laterDay == 0 ? "Same day as the normal arrival" : "+%d day(s)");
					Tooltip("Days after the day of the normal arrival (up to 30). Ctrl+click to type a number.");
					g_ui.laterDay = std::clamp(g_ui.laterDay, range.dayMin, range.dayMax);
					const int hourMin = range.HourMin(g_ui.laterDay);
					const int hourMax = (std::max)(hourMin, range.HourMax(g_ui.laterDay));
					g_ui.laterHour = std::clamp(g_ui.laterHour, hourMin, hourMax);
					ImGuiMCP::SetNextItemWidth(260.0f);
					ImGuiMCP::SliderInt("Hour##later", &g_ui.laterHour, hourMin, hourMax, "%02d:00");
					Tooltip("Arrival hour on that day. Later arrivals are whole hours.");
					g_ui.laterHour = std::clamp(g_ui.laterHour, hourMin, hourMax);
					laterHours = range.HoursFromNow(g_ui.laterDay, g_ui.laterHour, a_data.nowHour);
					arrivalHours = laterHours;
					break;
				}
			default:
				break;
			}

			// Weather
			if (a_data.weatherOptions) {
				ImGuiMCP::SeparatorText("Weather");
				for (int i = 0; i < Weather::CHOICE_COUNT; ++i) {
					if (i > 0) {
						ImGuiMCP::SameLine();
					}
					ImGuiMCP::RadioButton(Weather::Name(static_cast<Choice>(i)), &g_ui.weather, i);
				}
				ImGuiMCP::TextDisabled("%s", "If the destination has no weather of that kind, it stays natural.");
			} else {
				g_ui.weather = static_cast<int>(Choice::kNatural);
			}

			// Result
			ImGuiMCP::Separator();
			auto arrival = hasEstimate || g_ui.timing == static_cast<int>(Timing::kInstant) ? Link::FormatArrival(arrivalHours) : std::string("as the game decides");
			if (g_ui.weather != static_cast<int>(Choice::kNatural)) {
				arrival += std::format(" ({})", Weather::Name(static_cast<Choice>(g_ui.weather)));
			}
			ImGuiMCP::Text("Arrival: %s", arrival.c_str());
			if (hasEstimate && g_ui.timing == static_cast<int>(Timing::kLater) && laterHours - trip.hours > 24.0) {
				ImGuiMCP::TextWrapped("%s", "More than a day after the normal arrival. Time passes as if you waited: timed effects end, "
											"vampirism and survival needs advance, and followers you left waiting may leave.");
			}
			if (hasEstimate && !trip.calibrated && g_ui.timing != static_cast<int>(Timing::kNormal)) {
				ImGuiMCP::TextDisabled("%s", "Fast Travel Time Info is still on its starting guess, so the arrival may be off.");
			}

			// Buttons
			ImGuiMCP::Spacing();
			if (ImGuiMCP::Button("Proceed")) {
				Submit(Planner::Choice::Action::kProceed, a_data, laterHours);
				return;
			}
			for (const auto& [index, text] : a_data.extraButtons) {
				ImGuiMCP::SameLine();
				const auto label = std::format("{}##extra{}", text, index);
				if (ImGuiMCP::Button(label.c_str())) {
					Submit(Planner::Choice::Action::kOther, a_data, laterHours, index);
					return;
				}
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Cancel") || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false) ||
				ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_GamepadFaceRight, false)) {
				Submit(Planner::Choice::Action::kCancel, a_data, laterHours);
				return;
			}
		}

		void __stdcall RenderPanel()
		{
			try {
				const auto data = Planner::GetPanelData();
				if (!data.open) {
					if (g_window) {
						g_window->IsOpen = false;
					}
					return;
				}
				if (data.serial != g_ui.serial) {
					g_ui = UiState{};
					g_ui.serial = data.serial;
					if (data.trip.valid && data.trip.hours > 0.0) {
						const auto range = MakeRange(data.nowHour, data.trip.hours);
						g_ui.laterDay = range.dayMin;
						g_ui.laterHour = range.HourMin(range.dayMin);
					}
				}

				const auto viewport = ImGuiMCP::GetMainViewport();
				if (viewport) {
					ImGuiMCP::SetNextWindowPos(
						ImGuiMCP::ImVec2{ viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f },
						ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2{ 0.5f, 0.5f });
				}
				const auto title = std::format("Fast travel to {}###FTAPArrivalPanel", data.destination);
				const auto flags = ImGuiMCP::ImGuiWindowFlags_NoCollapse | ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize |
				                   ImGuiMCP::ImGuiWindowFlags_NoSavedSettings;
				if (ImGuiMCP::Begin(title.c_str(), nullptr, flags)) {
					RenderPanelContents(data);
				}
				ImGuiMCP::End();
			} catch (const std::exception& e) {
				logs::error("Panel render failed: {}", e.what());
			} catch (...) {
				logs::error("Panel render failed: unknown exception");
			}
		}

		void __stdcall RenderStatus()
		{
			try {
				const auto settings = Settings::Current();
				ImGuiMCP::SeparatorText("Prototype status");
				ImGuiMCP::TextWrapped("%s", Link::Describe().c_str());
				ImGuiMCP::Text("Panel: %s   Weather options: %s   Faster options from: %.2f h",
					settings->enabled && settings->showPanel ? "on" : "off", settings->weatherOptions ? "on" : "off",
					static_cast<double>(settings->minHoursForFaster));

				ImGuiMCP::SeparatorText("Last planned trip");
				const auto lines = Planner::LastTripLines();
				if (lines.empty()) {
					ImGuiMCP::TextDisabled("%s", "None yet in this session.");
				}
				for (const auto& line : lines) {
					ImGuiMCP::BulletText("%s", line.c_str());
				}
				ImGuiMCP::TextDisabled("%s", "Full details: Documents\\My Games\\Skyrim Special Edition\\SKSE\\FastTravelArrivalPlanner.log");

				ImGuiMCP::Spacing();
				if (ImGuiMCP::Button("Reload INI")) {
					Settings::Reload();
				}
				Tooltip("Reads FastTravelArrivalPlanner.ini again after you edit it.");
			} catch (...) {
				logs::error("Status page render failed");
			}
		}
	}

	bool Register()
	{
		if (REL::Module::IsVR()) {
			return false;
		}
		if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule()) {
			logs::error("SKSE Menu Framework is not installed or not loaded: the arrival panel is unavailable and the game's own box is used");
			return false;
		}
		g_window = SKSEMenuFramework::AddWindow(RenderPanel, true);
		if (!g_window) {
			logs::error("SKSE Menu Framework did not create the panel window");
			return false;
		}
		g_window->IsOpen = false;
		SKSEMenuFramework::SetSection("Fast Travel Arrival Planner");
		SKSEMenuFramework::AddSectionItem("Status", RenderStatus);
		g_available = true;
		logs::info("Arrival panel ready (SKSE Menu Framework {:.2f})", static_cast<double>(SKSEMenuFramework::GetMenuFrameworkVersion()));
		return true;
	}

	bool IsAvailable()
	{
		return g_available.load();
	}

	bool IsOpen()
	{
		return g_window && g_window->IsOpen.load();
	}

	bool IsReleased()
	{
		if (!g_available) {
			return true;
		}
		return !IsOpen() && !SKSEMenuFramework::IsAnyBlockingWindowOpened();
	}

	void Open()
	{
		if (g_window) {
			g_window->IsOpen = true;
		}
	}

	void Close()
	{
		if (g_window) {
			g_window->IsOpen = false;
		}
	}
}
