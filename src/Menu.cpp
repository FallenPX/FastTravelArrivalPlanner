#include "pch.h"

#include "Menu.h"
#include "Link.h"
#include "Panel.h"
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

// Optional in-game settings page for SKSE Menu Framework, laid out like Fast Travel Time
// Info's.
//
// The framework's header looks the framework up at runtime (GetModuleHandle/GetProcAddress),
// so this plugin never links to it: without the framework, Register() returns early and
// nothing else in this file ever runs.
//
// Every change is applied to the live settings at once and written to the player's settings
// file, which stays the single source of truth for the next game start.

namespace FTAP::Menu
{
	namespace
	{
		constexpr auto   SECTION_NAME = "Fast Travel Arrival Planner";
		constexpr auto   PAGE_NAME = "Settings";
		constexpr double MESSAGE_SECONDS = 4.0;
		constexpr int    STEPS_PER_HOUR = 2;   // the slider moves in half hours
		constexpr int    SLIDER_HOURS = 24;    // the INI takes up to Settings::MAX_HOURS_FOR_FASTER

		int           g_minimumBuffer{ 2 };  // in half hours
		int           g_minimumSynced{ 2 };
		std::uint32_t g_syncedVersion{ 0 };
		std::string   g_message;
		double        g_messageUntil{ 0.0 };

		void ShowMessage(std::string a_text)
		{
			g_message = std::move(a_text);
			g_messageUntil = ImGuiMCP::GetTime() + MESSAGE_SECONDS;
		}

		int ToSteps(float a_hours)
		{
			return std::clamp(static_cast<int>(std::lround(a_hours * STEPS_PER_HOUR)), 0, SLIDER_HOURS * STEPS_PER_HOUR);
		}

		// The slider keeps its own value while it is dragged; refresh it whenever the live
		// settings were replaced (by a change here or by "Reload INI").
		void SyncBuffers(const Settings& a_settings)
		{
			const auto version = Settings::Version();
			if (version == g_syncedVersion) {
				return;
			}
			g_syncedVersion = version;
			g_minimumBuffer = g_minimumSynced = ToSteps(a_settings.minHoursForFaster);
		}

		// "1 h 30 min", in Fast Travel Time Info's units when it is there.
		std::string Duration(double a_hours)
		{
			if (a_hours <= 0.0) {
				return "no minimum";
			}
			if (auto text = Link::FormatDuration(a_hours, false); !text.empty()) {
				return text;
			}
			const auto minutes = static_cast<long long>(std::llround(a_hours * 60.0));
			if (minutes < 60) {
				return std::format("{} min", minutes);
			}
			return minutes % 60 ? std::format("{} h {} min", minutes / 60, minutes % 60) : std::format("{} h", minutes / 60);
		}

		std::string Escaped(const std::string& a_text)  // for an ImGui format string
		{
			std::string out;
			for (const char c : a_text) {
				out += c;
				if (c == '%') {
					out += '%';
				}
			}
			return out;
		}

		void Commit(Settings a_settings)
		{
			Settings::Set(std::move(a_settings));
			if (!Settings::SaveToIni(*Settings::Current())) {
				ShowMessage("Applied, but could not save to your settings file.");
			}
		}

		void Tooltip(const char* a_text)
		{
			if (ImGuiMCP::IsItemHovered()) {
				ImGuiMCP::SetTooltip("%s", a_text);
			}
		}

		void Toggle(const char* a_label, const char* a_tooltip, const Settings& a_current, bool Settings::* a_member)
		{
			bool       value = a_current.*a_member;
			const bool changed = ImGuiMCP::Checkbox(a_label, &value);
			Tooltip(a_tooltip);
			if (changed) {
				Settings next = a_current;
				next.*a_member = value;
				Commit(std::move(next));
			}
		}

		void RenderPanel(const Settings& a_s)
		{
			ImGuiMCP::SeparatorText("Arrival panel");
			Toggle("Open the arrival panel when you pick a destination on the map",
				"Off: the game's own \"Fast travel to ...?\" box, as without this mod.", a_s, &Settings::showPanel);
			if (!Panel::IsAvailable()) {
				ImGuiMCP::TextWrapped("%s", "Interface\\FastTravelArrivalPlanner.swf is missing, so the game's own box is used. Reinstall the mod.");
			}
			if (!Link::Get()) {
				ImGuiMCP::TextWrapped("%s. The game's own box is used.", Link::Describe().c_str());
			}

			ImGuiMCP::BeginDisabled(!a_s.showPanel);
			// While the slider is untouched it shows the value in use, even one outside its range.
			const bool untouched = g_minimumBuffer == g_minimumSynced;
			const auto shown = untouched ? static_cast<double>(a_s.minHoursForFaster) : static_cast<double>(g_minimumBuffer) / STEPS_PER_HOUR;
			const auto format = Escaped(Duration(shown));
			ImGuiMCP::SetNextItemWidth(220.0f);
			ImGuiMCP::SliderInt("Instant, 1/3 and 1/2 from", &g_minimumBuffer, 0, SLIDER_HOURS * STEPS_PER_HOUR, format.c_str(),
				ImGuiMCP::ImGuiSliderFlags_AlwaysClamp | ImGuiMCP::ImGuiSliderFlags_NoInput);
			const bool minimumCommitted = ImGuiMCP::IsItemDeactivatedAfterEdit();
			Tooltip("The faster arrivals are offered only for trips whose normal travel time is at least this long, in game time. "
					"Travel times grow with the timescale: at the default timescale of 20 a trip takes 2.5 times as many game hours "
					"as at 8. Far left: no minimum.");
			if (minimumCommitted) {
				Settings next = a_s;
				next.minHoursForFaster = static_cast<float>(g_minimumBuffer) / STEPS_PER_HOUR;
				Commit(std::move(next));
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Default##minimum")) {
				Settings next = a_s;
				next.minHoursForFaster = Settings{}.minHoursForFaster;
				Commit(std::move(next));
			}
			ImGuiMCP::EndDisabled();

			Toggle("Leave maps opened from a conversation to the game",
				"Some mods let you pick a destination on the map while talking to someone. With this on, the panel stays out of "
				"those maps and the game's own box (or the mod that opened the map) handles them.",
				a_s, &Settings::skipAfterConversation);
		}

		void RenderLastTrip()
		{
			ImGuiMCP::SeparatorText("Last planned trip");
			const auto lines = Planner::LastTripLines();
			if (lines.empty()) {
				ImGuiMCP::TextDisabled("%s", "None yet in this session.");
			}
			for (const auto& line : lines) {
				ImGuiMCP::BulletText("%s", line.c_str());
			}
			ImGuiMCP::TextDisabled("%s", "Full details: FastTravelArrivalPlanner.log, next to your settings file.");
		}

		void RenderFile()
		{
			ImGuiMCP::SeparatorText("Settings file");
			ImGuiMCP::TextWrapped("%s", "Changes made here are saved straight away to your settings file, which updates never replace:");
			ImGuiMCP::TextDisabled("%s", Settings::FilePath().c_str());
			if (ImGuiMCP::Button("Reload INI")) {
				Settings::Reload();
				ShowMessage("Settings reloaded from your settings file.");
			}
			Tooltip("Use this after editing your settings file while the game is running.");
		}

		void __stdcall Render()
		{
			try {
				SyncBuffers(*Settings::Current());
				RenderPanel(*Settings::Current());
				ImGuiMCP::Spacing();
				RenderLastTrip();
				ImGuiMCP::Spacing();
				RenderFile();
				if (!g_message.empty() && ImGuiMCP::GetTime() < g_messageUntil) {
					ImGuiMCP::Spacing();
					ImGuiMCP::TextWrapped("%s", g_message.c_str());
				}
			} catch (const std::exception& e) {
				logs::error("Settings page failed: {}", e.what());
			} catch (...) {
				logs::error("Settings page failed: unknown exception");
			}
		}
	}

	void Register()
	{
		if (REL::Module::IsVR()) {
			return;
		}
		if (!SKSEMenuFramework::IsInstalled()) {
			logs::info("SKSE Menu Framework not installed; settings stay INI-only");
			return;
		}
		if (!GetMenuFrameworkModule()) {
			logs::warn("SKSE Menu Framework is installed but not loaded; settings stay INI-only");
			return;
		}
		SKSEMenuFramework::SetSection(SECTION_NAME);
		SKSEMenuFramework::AddSectionItem(PAGE_NAME, Render);
		logs::info("Added settings page to SKSE Menu Framework {:.2f}", static_cast<double>(SKSEMenuFramework::GetMenuFrameworkVersion()));
	}
}
