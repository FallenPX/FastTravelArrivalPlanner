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

		MENU_WINDOW       g_window = nullptr;
		std::atomic<bool> g_available{ false };

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

		// ---- Look: matches the map's location flyout (colours from Fast Travel Time Info)

		struct Look
		{
			bool             match{ false };  // false: SKSE Menu Framework's own theme
			ImGuiMCP::ImVec4 title{ 1, 1, 1, 1 };
			ImGuiMCP::ImVec4 label{ 0.73f, 0.73f, 0.73f, 1 };
			ImGuiMCP::ImVec4 value{ 1, 1, 1, 1 };
			ImGuiMCP::ImVec4 background{ 0, 0, 0, 0.6f };
			float            rounding{ 4.5f };  // screen pixels
		};

		// Panel state (render thread only), reset whenever a new panel opens.
		struct UiState
		{
			std::uint64_t serial{ 0 };
			int           timing{ static_cast<int>(Timing::kNormal) };
			int           laterDay{ 0 };
			int           laterHour{ 0 };
			double        openedAt{ 0.0 };  // ImGui time
			Look          look;
		};
		UiState g_ui;

		ImGuiMCP::ImVec4 Rgb(std::uint32_t a_rgb, float a_alpha = 1.0f)
		{
			return { ((a_rgb >> 16) & 0xFF) / 255.0f, ((a_rgb >> 8) & 0xFF) / 255.0f, (a_rgb & 0xFF) / 255.0f, a_alpha };
		}

		ImGuiMCP::ImVec4 WithAlpha(ImGuiMCP::ImVec4 a_color, float a_alpha)
		{
			a_color.w = a_alpha;
			return a_color;
		}

		Look MakeLook()
		{
			Look look;
			look.match = Settings::Current()->matchMapStyle;
			if (!look.match) {
				return look;
			}
			FTTI_API::FlyoutStyle style;  // defaults when FTTI is older than 0.2.0
			if (Link::GetFlyoutStyle(style) && !style.colorsKnown) {
				logs::info("Panel: the map flyout's colours are not known yet; using defaults");
			}
			look.title = Rgb(style.titleColor);
			look.label = Rgb(style.labelColor);
			look.value = Rgb(style.valueColor);
			// The panel holds controls, so it never gets fainter than half opaque.
			const float opacity = style.background ? (std::max)(style.backgroundOpacity, 0.5f) : 0.75f;
			look.background = Rgb(style.backgroundColor, opacity);
			// FTTI's corner radius is in the map's 1280 x 720 layout; the panel draws in screen pixels.
			float scale = 1.5f;
			if (const auto viewport = ImGuiMCP::GetMainViewport(); viewport && viewport->Size.y > 0.0f) {
				scale = viewport->Size.y / 720.0f;
			}
			look.rounding = style.cornerRadius * scale;
			return look;
		}

		// Pushes the panel's colours and spacing; returns how many of each to pop.
		std::pair<int, int> PushLook(const Look& a_look)
		{
			if (!a_look.match) {
				return { 0, 0 };
			}
			using namespace ImGuiMCP;
			const auto& l = a_look.label;
			const std::array<std::pair<ImGuiCol, ImVec4>, 20> colors{ {
				{ ImGuiCol_WindowBg, a_look.background },
				{ ImGuiCol_PopupBg, WithAlpha(a_look.background, (std::max)(a_look.background.w, 0.92f)) },
				{ ImGuiCol_Border, WithAlpha(l, 0.35f) },
				{ ImGuiCol_Text, a_look.value },
				{ ImGuiCol_TextDisabled, WithAlpha(l, 0.55f) },
				{ ImGuiCol_FrameBg, WithAlpha(l, 0.15f) },
				{ ImGuiCol_FrameBgHovered, WithAlpha(l, 0.25f) },
				{ ImGuiCol_FrameBgActive, WithAlpha(l, 0.35f) },
				{ ImGuiCol_SliderGrab, WithAlpha(l, 0.9f) },
				{ ImGuiCol_SliderGrabActive, a_look.title },
				{ ImGuiCol_CheckMark, a_look.title },
				{ ImGuiCol_Button, WithAlpha(l, 0.18f) },
				{ ImGuiCol_ButtonHovered, WithAlpha(l, 0.32f) },
				{ ImGuiCol_ButtonActive, WithAlpha(l, 0.45f) },
				{ ImGuiCol_Header, WithAlpha(l, 0.25f) },
				{ ImGuiCol_HeaderHovered, WithAlpha(l, 0.35f) },
				{ ImGuiCol_HeaderActive, WithAlpha(l, 0.45f) },
				{ ImGuiCol_Separator, WithAlpha(l, 0.4f) },
				{ ImGuiCol_SeparatorHovered, WithAlpha(l, 0.4f) },
				{ ImGuiCol_SeparatorActive, WithAlpha(l, 0.4f) },
			} };
			for (const auto& [index, color] : colors) {
				PushStyleColor(index, color);
			}
			const float pad = GetFontSize() * 0.5f;
			PushStyleVar(ImGuiStyleVar_WindowRounding, a_look.rounding);
			PushStyleVar(ImGuiStyleVar_PopupRounding, a_look.rounding);
			PushStyleVar(ImGuiStyleVar_FrameRounding, a_look.rounding * 0.66f);
			PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ pad * 1.4f, pad });
			return { static_cast<int>(colors.size()), 6 };
		}

		// The flyout's diamond bullet, drawn (the game's font may not have the character).
		void Diamond(const Look& a_look)
		{
			using namespace ImGuiMCP;
			const float  size = GetFontSize();
			const float  r = size * 0.16f;
			const auto   pos = GetCursorScreenPos();
			const ImVec2 c{ pos.x + r + size * 0.05f, pos.y + size * 0.52f };
			ImDrawListManager::AddQuadFilled(GetWindowDrawList(), ImVec2{ c.x, c.y - r * 1.35f }, ImVec2{ c.x + r, c.y },
				ImVec2{ c.x, c.y + r * 1.35f }, ImVec2{ c.x - r, c.y }, GetColorU32(a_look.title));
			Dummy(ImVec2{ r * 2.0f + size * 0.15f, size });
			SameLine(0.0f, size * 0.2f);
		}

		// "Travel time:  2 h 55 min", as the flyout shows it.
		void Row(const Look& a_look, const char* a_label, const std::string& a_value)
		{
			if (!a_look.match) {
				ImGuiMCP::Text("%s %s", a_label, a_value.c_str());
				return;
			}
			Diamond(a_look);
			ImGuiMCP::TextColored(a_look.label, "%s", a_label);
			ImGuiMCP::SameLine(0.0f, ImGuiMCP::GetFontSize() * 0.4f);
			ImGuiMCP::TextColored(a_look.value, "%s", a_value.c_str());
		}

		// One timing choice: a word that lights up when chosen, as in SkyUI's category bars.
		bool Toggle(const Look& a_look, const char* a_label, int* a_value, int a_option, bool a_enabled, const char* a_disabledReason)
		{
			using namespace ImGuiMCP;
			const bool selected = *a_value == a_option;
			BeginDisabled(!a_enabled);
			if (a_look.match) {
				PushStyleColor(ImGuiCol_Text, selected ? a_look.title : a_look.label);
			}
			const auto  text = CalcTextSize(a_label, nullptr, true);
			const float pad = GetFontSize() * 0.45f;
			PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2{ 0.5f, 0.5f });
			const bool clicked = Selectable(a_label, selected, ImGuiSelectableFlags_None, ImVec2{ text.x + pad * 2.0f, 0.0f });
			PopStyleVar();
			if (a_look.match) {
				PopStyleColor();
			}
			EndDisabled();
			if (!a_enabled && a_disabledReason && IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				SetTooltip("%s", a_disabledReason);
			}
			if (clicked && a_enabled) {
				*a_value = a_option;
				return true;
			}
			return false;
		}

		void Submit(Planner::Choice::Action a_action, const Planner::PanelData& a_data, double a_laterHours, int a_otherIndex = -1)
		{
			Planner::Choice choice;
			choice.action = a_action;
			choice.otherIndex = a_otherIndex;
			choice.timing = static_cast<Timing>(g_ui.timing);
			choice.laterHours = a_laterHours;
			choice.serial = a_data.serial;
			Planner::Submit(choice);
		}

		// Clicks and keys are ignored this soon after the panel opens, so the second click of a
		// double-click on the map (or a key still held) cannot press one of its buttons.
		constexpr double INPUT_GUARD_SECONDS = 0.3;

		bool Ready(const char* a_what)
		{
			const double age = ImGuiMCP::GetTime() - g_ui.openedAt;
			if (age >= INPUT_GUARD_SECONDS) {
				return true;
			}
			logs::info("Panel: {} ignored, {:.0f} ms after the panel opened", a_what, age * 1000.0);
			return false;
		}

		void RenderPanelContents(const Planner::PanelData& a_data, const Look& a_look)
		{
			const auto& trip = a_data.trip;
			const bool  hasEstimate = trip.valid && trip.hours > 0.0;
			const bool  fasterAllowed = hasEstimate && (a_data.minHoursForFaster <= 0.0f || trip.hours >= a_data.minHoursForFaster);
			const auto  fasterReason = std::format("Needs a travel time of at least {}.", Link::FormatDuration(a_data.minHoursForFaster, false));

			// Heading and the normal trip
			const auto heading = std::format("Travel to {}", a_data.destination);
			if (a_look.match) {
				ImGuiMCP::TextColored(a_look.title, "%s", heading.c_str());
			} else {
				ImGuiMCP::Text("%s", heading.c_str());
			}
			ImGuiMCP::Separator();
			if (hasEstimate) {
				Row(a_look, "Travel time:", Link::FormatDuration(trip.hours, !trip.calibrated));
				Row(a_look, "Normal arrival:", Link::FormatArrival(trip.hours));
			} else if (trip.otherWorldspace) {
				ImGuiMCP::TextWrapped("%s", "No estimate for a trip into this area. Only a normal trip is available.");
			} else {
				ImGuiMCP::TextWrapped("%s", "No estimate for this trip. Only a normal trip is available.");
			}

			// Timing
			if (a_look.match) {
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, a_look.label);
			}
			ImGuiMCP::SeparatorText("Arrival");
			if (a_look.match) {
				ImGuiMCP::PopStyleColor();
			}
			if (!hasEstimate) {
				g_ui.timing = static_cast<int>(Timing::kNormal);
			} else if (!fasterAllowed && g_ui.timing < static_cast<int>(Timing::kNormal)) {
				g_ui.timing = static_cast<int>(Timing::kNormal);
			}
			const char* noEstimate = "No estimate for this trip.";
			const char* fasterWhy = hasEstimate ? fasterReason.c_str() : noEstimate;
			Toggle(a_look, "Instant", &g_ui.timing, static_cast<int>(Timing::kInstant), fasterAllowed, fasterWhy);
			ImGuiMCP::SameLine();
			Toggle(a_look, "1/3", &g_ui.timing, static_cast<int>(Timing::kThird), fasterAllowed, fasterWhy);
			ImGuiMCP::SameLine();
			Toggle(a_look, "1/2", &g_ui.timing, static_cast<int>(Timing::kHalf), fasterAllowed, fasterWhy);
			ImGuiMCP::SameLine();
			Toggle(a_look, "Normal ETA", &g_ui.timing, static_cast<int>(Timing::kNormal), true, nullptr);
			ImGuiMCP::SameLine();
			Toggle(a_look, "Later", &g_ui.timing, static_cast<int>(Timing::kLater), hasEstimate, noEstimate);

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
					const auto  range = MakeRange(a_data.nowHour, trip.hours);
					const float width = ImGuiMCP::GetFontSize() * 13.0f;
					g_ui.laterDay = std::clamp(g_ui.laterDay, range.dayMin, range.dayMax);
					ImGuiMCP::SetNextItemWidth(width);
					ImGuiMCP::SliderInt("Day##later", &g_ui.laterDay, range.dayMin, range.dayMax,
						g_ui.laterDay == 0 ? "Same day as the normal arrival" : "+%d day(s)");
					Tooltip("Days after the day of the normal arrival (up to 30). Ctrl+click to type a number.");
					g_ui.laterDay = std::clamp(g_ui.laterDay, range.dayMin, range.dayMax);
					const int hourMin = range.HourMin(g_ui.laterDay);
					const int hourMax = (std::max)(hourMin, range.HourMax(g_ui.laterDay));
					g_ui.laterHour = std::clamp(g_ui.laterHour, hourMin, hourMax);
					ImGuiMCP::SetNextItemWidth(width);
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

			// Result
			ImGuiMCP::Separator();
			const auto arrival = hasEstimate || g_ui.timing == static_cast<int>(Timing::kInstant) ? Link::FormatArrival(arrivalHours) : std::string("as the game decides");
			Row(a_look, "Arrival:", arrival);
			if (hasEstimate && g_ui.timing == static_cast<int>(Timing::kLater) && laterHours - trip.hours > 24.0) {
				ImGuiMCP::TextWrapped("%s", "More than a day after the normal arrival. Time passes as if you waited: timed effects end, "
											"vampirism and survival needs advance, and followers you left waiting may leave.");
			}
			if (hasEstimate && !trip.calibrated && g_ui.timing != static_cast<int>(Timing::kNormal)) {
				ImGuiMCP::TextDisabled("%s", "The travel time is approximate, so the arrival may be off.");
			}

			// Buttons
			ImGuiMCP::Spacing();
			if (ImGuiMCP::Button("Proceed") && Ready("Proceed")) {
				Submit(Planner::Choice::Action::kProceed, a_data, laterHours);
				return;
			}
			for (const auto& [index, text] : a_data.extraButtons) {
				ImGuiMCP::SameLine();
				const auto label = std::format("{}##extra{}", text, index);
				if (ImGuiMCP::Button(label.c_str()) && Ready(text.c_str())) {
					Submit(Planner::Choice::Action::kOther, a_data, laterHours, index);
					return;
				}
			}
			ImGuiMCP::SameLine();
			const char* cancelSource = nullptr;
			if (ImGuiMCP::Button("Cancel")) {
				cancelSource = "the Cancel button";
			} else if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
				cancelSource = "the Escape key";
			} else if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_GamepadFaceRight, false)) {
				cancelSource = "the controller's B button";
			}
			if (cancelSource && Ready(cancelSource)) {
				logs::info("Panel: cancelled with {}", cancelSource);
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
					g_ui.openedAt = ImGuiMCP::GetTime();
					g_ui.look = MakeLook();
					if (data.trip.valid && data.trip.hours > 0.0) {
						const auto range = MakeRange(data.nowHour, data.trip.hours);
						g_ui.laterDay = range.dayMin;
						g_ui.laterHour = range.HourMin(range.dayMin);
					}
				}
				const auto& look = g_ui.look;

				// The player's chosen font, if SKSE Menu Framework has it (the framework pushes
				// nothing for an unknown name, so check before popping).
				const auto settings = Settings::Current();
				bool       pushedFont = false;
				if (!settings->font.empty()) {
					const auto before = ImGuiMCP::GetFont();
					SKSEMenuFramework::PushFont(settings->font);
					pushedFont = ImGuiMCP::GetFont() != before;
				}
				const auto [colors, vars] = PushLook(look);

				const auto viewport = ImGuiMCP::GetMainViewport();
				if (viewport) {
					ImGuiMCP::SetNextWindowPos(
						ImGuiMCP::ImVec2{ viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f },
						ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2{ 0.5f, 0.5f });
				}
				const auto flags = ImGuiMCP::ImGuiWindowFlags_NoDecoration | ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize |
				                   ImGuiMCP::ImGuiWindowFlags_NoSavedSettings;
				if (ImGuiMCP::Begin("Fast Travel Arrival Planner###FTAPArrivalPanel", nullptr, flags)) {
					RenderPanelContents(data, look);
				}
				ImGuiMCP::End();

				ImGuiMCP::PopStyleVar(vars);
				ImGuiMCP::PopStyleColor(colors);
				if (pushedFont) {
					ImGuiMCP::PopFont();
				}
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
				ImGuiMCP::Text("Panel: %s   Faster options from: %.2f h", settings->enabled && settings->showPanel ? "on" : "off",
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
				ImGuiMCP::TextDisabled("Settings: %s", Settings::FilePath().c_str());

				ImGuiMCP::Spacing();
				if (ImGuiMCP::Button("Reload INI")) {
					Settings::Reload();
				}
				Tooltip("Reads your settings file again after you edit it.");
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
