#include "pch.h"

#include "Panel.h"
#include "GamePanel.h"
#include "Link.h"
#include "Planner.h"
#include "LaterRange.h"
#include "Settings.h"

// SKSEMenuFramework.h includes <windows.h>, so it is kept to this one file.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include "SKSEMenuFramework.h"

// The arrival panel, in one of two forms:
//  - in the game's own interface (GamePanel.cpp), the default;
//  - a SKSE Menu Framework window that blocks the map behind it (this file), plus a small
//    status page in the framework's own menu. It renders on the render thread: it only reads
//    a copy of the planner's data and hands the player's choice back to the planner.
// The functions at the end choose between them each time a panel opens.

namespace FTAP::Panel
{
	namespace
	{
		using Planner::Timing;

		MENU_WINDOW       g_window = nullptr;
		std::atomic<bool> g_available{ false };  // SKSE Menu Framework's panel

		enum class Kind
		{
			kNone,
			kGame,
			kFramework
		};
		std::atomic<Kind> g_active{ Kind::kNone };  // the panel opened last

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

			// Text, as the flyout draws it (Fast Travel Time Info 0.3.0 or newer).
			std::string                   rowFont;    // SKSE Menu Framework font name ("" = the framework's own)
			std::string                   titleFont;
			float                         rowSize{ 0.0f };    // screen pixels (0 = keep the framework's size)
			float                         titleSize{ 0.0f };
			bool                          shadow{ false };
			// Each pass draws the text once more, offset and in the shadow's colour: a drop shadow
			// is a dark pass plus a fainter, wider one for its soft edge; a glow is four thin ones.
			std::vector<std::pair<ImGuiMCP::ImVec2, ImGuiMCP::ImU32>> shadowPasses;
			std::string                   summary;        // for the log
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
			std::string   fontName;         // the font the panel asks the framework for
			int           fontPushed{ 0 };  // 1 = pushed this frame
			bool          sizesLogged{ false };
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
			// Exactly FTTI's background, including none: the panel keeps the look the player chose
			// for the flyout.
			const float opacity = style.background ? std::clamp(style.backgroundOpacity, 0.0f, 1.0f) : 0.0f;
			look.background = Rgb(style.backgroundColor, opacity);
			// FTTI's corner radius is in the map's 1280 x 720 layout; the panel draws in screen pixels.
			float scale = 1.5f;
			if (const auto viewport = ImGuiMCP::GetMainViewport(); viewport && viewport->Size.y > 0.0f) {
				scale = viewport->Size.y / 720.0f;
			}
			look.rounding = style.cornerRadius * scale;

			// Font, sizes and shadow. When FTTI cannot tell whether the flyout's text has a shadow
			// (most interfaces draw it in a way that cannot be read), the panel uses a shadow like
			// the vanilla and Dear Diary ones: black, 2 map pixels down and to the right, slightly soft.
			FTTI_API::FlyoutText text;
			const bool           textKnown = Link::GetFlyoutText(text);
			look.rowFont = text.rowFont;
			look.titleFont = text.titleFont;
			// Sizes outside what any screen could show are ignored (the framework's size is kept).
			const auto plausible = [](float a_px) { return a_px >= 9.0f && a_px <= 240.0f; };
			look.rowSize = plausible(text.rowSize) ? text.rowSize : 0.0f;
			look.titleSize = plausible(text.titleSize) ? text.titleSize : 0.0f;
			float         alpha = 0.85f;
			float         distance = 2.0f * scale;
			float         angle = 45.0f;
			float         blur = 2.0f * scale;
			std::uint32_t color = 0x000000;
			if (textKnown && text.shadowKnown) {  // FTTI 0.3.1+ could read the flyout's shadow
				look.shadow = text.shadow && text.shadowAlpha > 0.02f;
				alpha = text.shadowAlpha;
				distance = text.shadowDistance;
				angle = text.shadowAngle;
				blur = text.shadowBlur;
				color = text.shadowColor;
			} else {
				look.shadow = true;
			}
			if (look.shadow) {
				const float a = std::clamp(alpha, 0.0f, 1.0f);
				if (distance >= 0.5f) {
					const float radians = angle * 3.14159265f / 180.0f;
					const float d = (std::max)(distance, 1.0f);
					const float cx = std::cos(radians);
					const float cy = std::sin(radians);
					look.shadowPasses.emplace_back(ImGuiMCP::ImVec2{ cx * d, cy * d }, ImGuiMCP::GetColorU32(Rgb(color, a)));
					if (blur >= 1.0f) {
						const float e = d + (std::min)(blur * 0.5f, d);
						look.shadowPasses.emplace_back(ImGuiMCP::ImVec2{ cx * e, cy * e }, ImGuiMCP::GetColorU32(Rgb(color, a * 0.45f)));
					}
				} else {  // a glow: a thin outline all round
					const float d = std::clamp(blur * 0.4f, 1.0f, 2.0f);
					const auto  c = ImGuiMCP::GetColorU32(Rgb(color, a * 0.7f));
					for (const auto& o : { ImGuiMCP::ImVec2{ -d, -d }, ImGuiMCP::ImVec2{ d, -d }, ImGuiMCP::ImVec2{ -d, d }, ImGuiMCP::ImVec2{ d, d } }) {
						look.shadowPasses.emplace_back(o, c);
					}
				}
			}
			look.summary = std::format("fonts \"{}\" / \"{}\", sizes {:.1f} / {:.1f} px, {}", look.titleFont, look.rowFont,
				look.titleSize, look.rowSize,
				!look.shadow ? "no shadow"s :
				               std::format("shadow #{:06X} {:.0f}% {:.1f} px at {:.0f} deg{}", color, alpha * 100.0f, distance, angle,
								   textKnown && text.shadowKnown ? "" : " (default: Fast Travel Time Info could not tell)"));
			if (text.rowSize != look.rowSize || text.titleSize != look.titleSize) {
				look.summary += std::format("; ignored implausible sizes {:.1f} / {:.1f} px", text.titleSize, text.rowSize);
			}
			return look;
		}

		// Fonts SKSE Menu Framework does not have (or that change nothing), so they are not asked
		// for again: the framework logs a warning every time it is asked for an unknown font.
		std::set<std::string> g_fontsWithoutEffect;

		// The name SKSE Menu Framework gave a loaded font ("" when unknown).
		std::string FontName(const ImGuiMCP::ImFont* a_font)
		{
			if (!a_font || !a_font->ConfigData) {
				return {};
			}
			const auto& name = a_font->ConfigData->Name;
			const auto  end = std::find(std::begin(name), std::end(name), '\0');
			return std::string(std::begin(name), end);
		}

		std::string Lowercase(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		// Asks SKSE Menu Framework for a font by name. Undone with FontAwesome::Pop, the framework's
		// call that clears every font pushed this frame. Only a font whose own name contains the one
		// asked for counts: some framework versions hand back their default font instead.
		bool PushFontByName(const std::string& a_name)
		{
			if (a_name.empty() || g_fontsWithoutEffect.contains(a_name)) {
				return false;
			}
			// Without the framework's reset call (older versions) a pushed font would stay pushed.
			static const bool canReset = SKSEMenuFramework::Model::Internal::GetFunction<SKSEMenuFramework::Model::ActionFunction>("Pop") != nullptr &&
			                             SKSEMenuFramework::Model::Internal::GetFunction<SKSEMenuFramework::Model::PushFontFunction>("PushFont") != nullptr;
			if (!canReset) {
				g_fontsWithoutEffect.insert(a_name);
				logs::info("Panel: this SKSE Menu Framework cannot switch fonts for other mods; using its own font");
				return false;
			}
			const auto before = ImGuiMCP::GetFont();
			SKSEMenuFramework::PushFont(a_name);
			const auto after = ImGuiMCP::GetFont();
			const auto got = FontName(after);
			if (after != before && (got.empty() || Lowercase(got).find(Lowercase(a_name)) != std::string::npos)) {
				return true;
			}
			FontAwesome::Pop();
			g_fontsWithoutEffect.insert(a_name);
			if (after == before) {
				logs::info("Panel: SKSE Menu Framework has no separate font named \"{}\"; using the current one (\"{}\")", a_name, FontName(before));
			} else {
				logs::info("Panel: SKSE Menu Framework has no font named \"{}\" (it offered \"{}\"); using its own font", a_name, got);
			}
			return false;
		}

		// The font scale that gives a_size pixels, from the framework font's own size.
		float ScaleFor(float a_size, float a_base)
		{
			if (a_size <= 0.0f || a_base <= 0.0f) {
				return 1.0f;
			}
			return std::clamp(a_size / a_base, 0.6f, 2.0f);
		}

		// Text with the flyout's shadow under it.
		void ShadowText(const Look& a_look, const ImGuiMCP::ImVec4& a_color, const char* a_text)
		{
			if (a_look.match && a_look.shadow) {
				const auto pos = ImGuiMCP::GetCursorScreenPos();
				auto*      drawList = ImGuiMCP::GetWindowDrawList();
				for (auto it = a_look.shadowPasses.rbegin(); it != a_look.shadowPasses.rend(); ++it) {  // soft edge first
					ImGuiMCP::ImDrawListManager::AddText(drawList, ImGuiMCP::ImVec2{ pos.x + it->first.x, pos.y + it->first.y }, it->second, a_text);
				}
			}
			ImGuiMCP::TextColored(a_color, "%s", a_text);
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
				// Tooltips have no counterpart on the flyout, so they stay readable.
				{ ImGuiCol_PopupBg, WithAlpha(a_look.background, 0.92f) },
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
			auto*        drawList = GetWindowDrawList();
			if (a_look.shadow) {
				for (auto it = a_look.shadowPasses.rbegin(); it != a_look.shadowPasses.rend(); ++it) {
					const auto& o = it->first;
					ImDrawListManager::AddQuadFilled(drawList, ImVec2{ c.x + o.x, c.y - r * 1.35f + o.y }, ImVec2{ c.x + r + o.x, c.y + o.y },
						ImVec2{ c.x + o.x, c.y + r * 1.35f + o.y }, ImVec2{ c.x - r + o.x, c.y + o.y }, it->second);
				}
			}
			ImDrawListManager::AddQuadFilled(drawList, ImVec2{ c.x, c.y - r * 1.35f }, ImVec2{ c.x + r, c.y },
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
			ShadowText(a_look, a_look.label, a_label);
			ImGuiMCP::SameLine(0.0f, ImGuiMCP::GetFontSize() * 0.4f);
			ShadowText(a_look, a_look.value, a_value.c_str());
		}

		// One timing choice: a word that lights up when chosen, as in SkyUI's category bars.
		bool Toggle(const Look& a_look, const char* a_label, int* a_value, int a_option, bool a_enabled, const char* a_disabledReason)
		{
			using namespace ImGuiMCP;
			const bool selected = *a_value == a_option;
			BeginDisabled(!a_enabled);
			const auto  text = CalcTextSize(a_label, nullptr, true);
			const float pad = GetFontSize() * 0.45f;
			bool        clicked = false;
			if (a_look.match) {
				// The label is drawn here, with the flyout's colours and shadow.
				const auto id = std::format("##timing{}", a_option);
				clicked = Selectable(id.c_str(), selected, ImGuiSelectableFlags_None, ImVec2{ text.x + pad * 2.0f, text.y });
				const auto min = GetItemRectMin();
				const auto max = GetItemRectMax();
				const ImVec2 at{ min.x + (max.x - min.x - text.x) * 0.5f, min.y + (max.y - min.y - text.y) * 0.5f };
				auto*        drawList = GetWindowDrawList();
				if (a_look.shadow && a_enabled) {
					for (auto it = a_look.shadowPasses.rbegin(); it != a_look.shadowPasses.rend(); ++it) {
						ImDrawListManager::AddText(drawList, ImVec2{ at.x + it->first.x, at.y + it->first.y }, it->second, a_label);
					}
				}
				// GetColorU32 also applies the disabled fade.
				ImDrawListManager::AddText(drawList, at, GetColorU32(selected ? a_look.title : a_look.label), a_label);
			} else {
				PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2{ 0.5f, 0.5f });
				clicked = Selectable(a_label, selected, ImGuiSelectableFlags_None, ImVec2{ text.x + pad * 2.0f, 0.0f });
				PopStyleVar();
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
				// The flyout's title font and size, then back to the row font and size.
				const bool titleFont = !a_look.titleFont.empty() && a_look.titleFont != g_ui.fontName;
				if (titleFont) {
					FontAwesome::Pop();
					PushFontByName(a_look.titleFont);
				}
				ImGuiMCP::SetWindowFontScale(1.0f);
				ImGuiMCP::SetWindowFontScale(ScaleFor(a_look.titleSize, ImGuiMCP::GetFontSize()));
				ShadowText(a_look, a_look.title, heading.c_str());
				if (titleFont) {
					FontAwesome::Pop();
					g_ui.fontPushed = PushFontByName(g_ui.fontName) ? 1 : 0;
				}
				ImGuiMCP::SetWindowFontScale(1.0f);
				ImGuiMCP::SetWindowFontScale(ScaleFor(a_look.rowSize, ImGuiMCP::GetFontSize()));
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
				ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 26.0f);
				ImGuiMCP::TextWrapped("%s", "More than a day after the normal arrival. Time passes as if you waited: timed effects end, "
											"vampirism and survival needs advance, and followers you left waiting may leave.");
				ImGuiMCP::PopTextWrapPos();
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

				// Font: the one named in the settings, else the flyout's, else the framework's own.
				const auto settings = Settings::Current();
				if (g_ui.fontName.empty()) {
					g_ui.fontName = !settings->font.empty() ? settings->font : look.match ? look.rowFont : std::string{};
					logs::info("Panel look: {}{}", look.match ? look.summary : "SKSE Menu Framework's theme"s,
						g_ui.fontName.empty() ? ""s : std::format("; asking SKSE Menu Framework for font \"{}\"", g_ui.fontName));
				}
				g_ui.fontPushed = PushFontByName(g_ui.fontName) ? 1 : 0;
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
					// The window remembers its font scale, so start from the framework's own size.
					ImGuiMCP::SetWindowFontScale(1.0f);
					if (look.match) {
						const float base = ImGuiMCP::GetFontSize();
						if (!g_ui.sizesLogged) {
							g_ui.sizesLogged = true;
							logs::info("Panel sizes: font \"{}\" {:.1f} px; title {:.1f} px (x{:.2f}), rows {:.1f} px (x{:.2f})",
								FontName(ImGuiMCP::GetFont()), base, look.titleSize, ScaleFor(look.titleSize, base), look.rowSize, ScaleFor(look.rowSize, base));
						}
						ImGuiMCP::SetWindowFontScale(ScaleFor(look.rowSize, base));
					}
					RenderPanelContents(data, look);
				}
				ImGuiMCP::End();

				ImGuiMCP::PopStyleVar(vars);
				ImGuiMCP::PopStyleColor(colors);
				if (g_ui.fontPushed > 0) {
					FontAwesome::Pop();
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
				const bool game = GamePanel::IsAvailable() && settings->gameInterface;
				ImGuiMCP::Text("Drawn by: %s", game ? "the game's own interface" : "SKSE Menu Framework");
				if (settings->gameInterface && !GamePanel::IsAvailable()) {
					ImGuiMCP::TextDisabled("%s", "Interface\\FastTravelArrivalPlanner.swf is missing, so SKSE Menu Framework draws it.");
				}

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
			logs::info("SKSE Menu Framework is not installed or not loaded: only the panel in the game's own interface can be used, and there is no status page");
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

	bool RegisterGameInterface()
	{
		return GamePanel::Register();
	}

	bool IsAvailable()
	{
		return GamePanel::IsAvailable() || g_available.load();
	}

	bool IsOpen()
	{
		switch (g_active.load()) {
		case Kind::kGame:
			return GamePanel::IsOpen();
		case Kind::kFramework:
			return g_window && g_window->IsOpen.load();
		default:
			return false;
		}
	}

	bool IsReleased()
	{
		switch (g_active.load()) {
		case Kind::kGame:
			return GamePanel::IsReleased();
		case Kind::kFramework:
			return !g_available || (!(g_window && g_window->IsOpen.load()) && !SKSEMenuFramework::IsAnyBlockingWindowOpened());
		default:
			return true;
		}
	}

	std::string_view Name()
	{
		switch (g_active.load()) {
		case Kind::kGame:
			return "the game's own interface";
		case Kind::kFramework:
			return "SKSE Menu Framework";
		default:
			return "no panel";
		}
	}

	void Open()
	{
		const auto settings = Settings::Current();
		const bool game = GamePanel::IsAvailable() && (settings->gameInterface || !g_available);
		if (game) {
			g_active = Kind::kGame;
			GamePanel::Open();
		} else if (g_window) {
			g_active = Kind::kFramework;
			g_window->IsOpen = true;
		} else {
			g_active = Kind::kNone;
		}
		logs::info("  Panel drawn by {}{}", Name(),
			settings->gameInterface && !game && g_window ? " (Interface\\FastTravelArrivalPlanner.swf is missing)" :
			!settings->gameInterface && game         ? " (SKSE Menu Framework is not available)" :
			                                            "");
	}

	void Close()
	{
		switch (g_active.load()) {
		case Kind::kGame:
			GamePanel::Close();
			break;
		case Kind::kFramework:
			if (g_window) {
				g_window->IsOpen = false;
			}
			break;
		default:
			break;
		}
	}
}
