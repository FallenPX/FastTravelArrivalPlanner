#include "pch.h"

#include "Panel.h"
#include "LaterRange.h"
#include "Link.h"
#include "Planner.h"
#include "Settings.h"

// How it works
// ------------
// The panel is a game menu, opened and closed through the game's own menu queue like the map or
// a message box. Its movie is an empty 1280 x 720 stage (the size the game lays its menus out
// at); everything on it is built here when it opens, with Flash's own means: text fields,
// drawing and button events. The text fields copy the text style of the map's location flyout
// (font, size, colour and Scaleform's text-shadow settings), read from the open map, so the
// game draws the panel exactly as it draws the flyout, for whatever interface mod draws that.
// Being a game menu, it also gets the game's cursor and input.
//
// Everything runs on the game's UI thread: building, Flash button events (from the movie's
// event handling) and the per-frame update.

namespace FTAP::Panel
{
	namespace
	{
		using Timing = Planner::Timing;

		constexpr std::string_view MENU_NAME = "FastTravelArrivalPlanner";
		constexpr const char*      MOVIE_NAME = "FastTravelArrivalPlanner";  // Interface/FastTravelArrivalPlanner.swf
		constexpr double           STAGE_WIDTH = 1280.0;
		constexpr double           STAGE_HEIGHT = 720.0;
		constexpr auto             INPUT_GUARD = std::chrono::milliseconds(300);

		std::atomic<bool> g_available{ false };
		std::atomic<bool> g_requested{ false };  // asked to open, not yet asked to close
		std::atomic<bool> g_alive{ false };      // the game has the menu
		std::atomic<std::int64_t> g_requestedAt{ 0 };  // steady_clock ticks of the last Open()
		constexpr auto            OPEN_TIMEOUT = std::chrono::seconds(3);

		// ---- Flash helpers

		bool Get(const RE::GFxValue& a_object, const char* a_name, RE::GFxValue& a_out)
		{
			return a_object.IsObject() && a_object.GetMember(a_name, &a_out);
		}

		std::optional<double> Number(const RE::GFxValue& a_object, const char* a_name)
		{
			RE::GFxValue value;
			if (Get(a_object, a_name, value) && value.IsNumber() && std::isfinite(value.GetNumber())) {
				return value.GetNumber();
			}
			return std::nullopt;
		}

		void Call(RE::GFxValue& a_object, const char* a_method, std::initializer_list<double> a_args = {})
		{
			std::vector<RE::GFxValue> args;
			args.reserve(a_args.size());
			for (const auto value : a_args) {
				args.emplace_back(value);
			}
			a_object.Invoke(a_method, nullptr, args.data(), args.size());
		}

		void Set(RE::GFxValue& a_object, const char* a_name, double a_value) { a_object.SetMember(a_name, RE::GFxValue(a_value)); }
		void Set(RE::GFxValue& a_object, const char* a_name, bool a_value) { a_object.SetMember(a_name, RE::GFxValue(a_value)); }
		void Set(RE::GFxValue& a_object, const char* a_name, const std::string& a_value) { a_object.SetMember(a_name, RE::GFxValue(a_value.c_str())); }

		// Rounded rectangle with the current fill, as Fast Travel Time Info draws its background.
		void RoundRect(RE::GFxValue& a_clip, double a_x, double a_y, double a_w, double a_h, double a_radius)
		{
			const double r = (std::max)(0.0, (std::min)({ a_radius, a_w / 2.0, a_h / 2.0 }));
			const std::array<std::array<double, 3>, 4> corners{ {
				{ a_x + r, a_y + r, 180.0 },
				{ a_x + a_w - r, a_y + r, 270.0 },
				{ a_x + a_w - r, a_y + a_h - r, 0.0 },
				{ a_x + r, a_y + a_h - r, 90.0 },
			} };
			const int steps = r < 0.5 ? 0 : (r < 4.0 ? 3 : 6);
			bool      first = true;
			for (const auto& [cx, cy, start] : corners) {
				for (int i = 0; i <= steps; ++i) {
					const double angle = (start + (steps > 0 ? 90.0 * i / steps : 0.0)) * 3.14159265358979 / 180.0;
					Call(a_clip, first ? "moveTo" : "lineTo", { cx + r * std::cos(angle), cy + r * std::sin(angle) });
					first = false;
				}
			}
			Call(a_clip, "lineTo", { a_x, a_y + r });
		}

		void FillRect(RE::GFxValue& a_clip, std::uint32_t a_color, double a_alpha, double a_x, double a_y, double a_w, double a_h, double a_radius)
		{
			Call(a_clip, "beginFill", { static_cast<double>(a_color), a_alpha });
			RoundRect(a_clip, a_x, a_y, a_w, a_h, a_radius);
			a_clip.Invoke("endFill");
		}

		// ---- The flyout's text style

		using PropertyValue = std::variant<double, bool, std::string>;

		struct TextStyle
		{
			std::string           font{ "$EverywhereFont" };
			double                size{ 20.0 };
			std::uint32_t         color{ 0xFFFFFF };
			bool                  bold{ false };
			bool                  italic{ false };
			std::optional<double> letterSpacing;
			std::vector<std::pair<std::string, PropertyValue>> properties;  // copied from the flyout as they are
			// A drop shadow or glow filter on the text or a clip holding it, rebuilt on the panel's text.
			struct Filter
			{
				std::string                                        className;
				std::vector<std::pair<std::string, PropertyValue>> members;
				std::string                                        where;
			};
			std::optional<Filter> filter;
			bool                  fromFlyout{ false };
		};

		constexpr std::array FILTER_MEMBERS{ "distance", "angle", "color", "alpha", "blurX", "blurY", "strength", "quality", "inner",
			"knockout", "hideObject" };

		// As Fast Travel Time Info looks for a shadow: the text's filters, then those of the clips
		// holding it. Only plain values are kept (objects of the map's movie cannot be moved to
		// another movie).
		std::optional<TextStyle::Filter> ReadFilter(const RE::GFxValue& a_field)
		{
			RE::GFxValue owner = a_field;
			for (int level = 0; level < 8 && owner.IsObject(); ++level) {
				RE::GFxValue filters;
				if (Get(owner, "filters", filters) && filters.IsArray()) {
					for (std::uint32_t i = 0; i < filters.GetArraySize(); ++i) {
						RE::GFxValue filter;
						if (!filters.GetElement(i, &filter) || !Number(filter, "color") || !Number(filter, "alpha")) {
							continue;  // not a drop shadow or a glow
						}
						TextStyle::Filter copy;
						copy.className = Number(filter, "distance") ? "flash.filters.DropShadowFilter" : "flash.filters.GlowFilter";
						for (const auto* name : FILTER_MEMBERS) {
							RE::GFxValue value;
							if (!Get(filter, name, value)) {
								continue;
							}
							if (value.IsNumber() && std::isfinite(value.GetNumber())) {
								copy.members.emplace_back(name, value.GetNumber());
							} else if (value.IsBool()) {
								copy.members.emplace_back(name, value.GetBool());
							}
						}
						copy.where = level == 0 ? "on the text"s : std::format("{} level(s) up", level);
						return copy;
					}
				}
				RE::GFxValue parent;
				if (!Get(owner, "_parent", parent) || !parent.IsDisplayObject()) {
					break;
				}
				owner = parent;
			}
			return std::nullopt;
		}

		// Text-field settings copied as they are: Scaleform's text shadow and font rendering.
		constexpr std::array COPIED_PROPERTIES{ "embedFonts", "antiAliasType", "gridFitType", "sharpness", "thickness", "shadowStyle",
			"shadowColor", "shadowAlpha", "shadowAngle", "shadowDistance", "shadowBlurX", "shadowBlurY", "shadowStrength", "shadowQuality",
			"shadowKnockOut", "shadowHideObject" };

		bool ReadStyle(const RE::GFxValue& a_field, TextStyle& a_style)
		{
			RE::GFxValue format;
			if (!a_field.IsObject() || !const_cast<RE::GFxValue&>(a_field).Invoke("getTextFormat", &format) || !format.IsObject()) {
				return false;
			}
			RE::GFxValue value;
			if (Get(format, "font", value) && value.IsString() && value.GetString() && *value.GetString()) {
				a_style.font = value.GetString();
			}
			if (const auto size = Number(format, "size"); size && *size > 0.0) {
				a_style.size = *size;
			}
			if (const auto color = Number(format, "color")) {
				a_style.color = static_cast<std::uint32_t>(*color) & 0xFFFFFFu;
			}
			if (Get(format, "bold", value) && value.IsBool()) {
				a_style.bold = value.GetBool();
			}
			if (Get(format, "italic", value) && value.IsBool()) {
				a_style.italic = value.GetBool();
			}
			a_style.letterSpacing = Number(format, "letterSpacing");
			a_style.properties.clear();
			for (const auto* name : COPIED_PROPERTIES) {
				if (!Get(a_field, name, value)) {
					continue;
				}
				if (value.IsNumber() && std::isfinite(value.GetNumber())) {
					a_style.properties.emplace_back(name, value.GetNumber());
				} else if (value.IsBool()) {
					a_style.properties.emplace_back(name, value.GetBool());
				} else if (value.IsString() && value.GetString() && *value.GetString()) {
					a_style.properties.emplace_back(name, std::string(value.GetString()));
				}
			}
			a_style.filter = ReadFilter(a_field);
			a_style.fromFlyout = true;
			return true;
		}

		std::string Describe(const TextStyle& a_style)
		{
			std::string text = std::format("\"{}\" {:g} #{:06X}", a_style.font, a_style.size, a_style.color);
			if (!a_style.properties.empty()) {
				text += " [";
				bool first = true;
				for (const auto& [name, value] : a_style.properties) {
					text += first ? "" : ", ";
					first = false;
					std::visit([&](const auto& v) {
						using T = std::decay_t<decltype(v)>;
						if constexpr (std::is_same_v<T, std::string>) {
							text += std::format("{} \"{}\"", name, v);
						} else {
							text += std::format("{} {}", name, v);
						}
					}, value);
				}
				text += "]";
			}
			if (a_style.filter) {
				text += std::format(" {} {}", a_style.filter->className.substr(a_style.filter->className.rfind('.') + 1), a_style.filter->where);
			}
			return text;
		}

		struct Look
		{
			TextStyle     title;
			TextStyle     label;
			TextStyle     value;
			bool          background{ true };
			std::uint32_t backgroundColor{ 0x000000 };
			double        backgroundAlpha{ 60.0 };  // 0 to 100
			double        radius{ 3.0 };            // in the map's pixels, as Fast Travel Time Info draws it
			const char*   rowsFrom{ "the flyout" };  // where the rows' look came from
			// The flyout's text shadow, for the drawn diamonds.
			std::optional<double> shadowDistance;
			double                shadowAngle{ 45.0 };
			std::uint32_t         shadowColor{ 0 };
			double                shadowAlpha{ 0.0 };
		};

		bool FindFlyout(RE::GFxValue& a_out)
		{
			const auto ui = RE::UI::GetSingleton();
			const auto map = ui ? ui->GetMenu<RE::MapMenu>() : RE::GPtr<RE::MapMenu>{};
			if (!map) {
				return false;
			}
			const auto isFlyout = [](const RE::GFxValue& a_value) {
				RE::GFxValue fn;
				return Get(a_value, "SetDescription", fn) && fn.IsObject();
			};
			if (const auto data = map->GetRuntimeData2()) {
				if (Get(data->mapMovie, "MarkerDescriptionObj", a_out) && isFlyout(a_out)) {
					return true;
				}
				RE::GFxValue holder;
				if (Get(data->mapMovie, "MarkerDescriptionHolder", holder) && Get(holder, "Description", a_out) && isFlyout(a_out)) {
					return true;
				}
			}
			return map->uiMovie && map->uiMovie->GetVariable(&a_out, "_root.MarkerDescriptionHolder.Description") && isFlyout(a_out);
		}

		Look ReadLook()
		{
			Look look;
			// Background and fallback colours from Fast Travel Time Info.
			FTTI_API::FlyoutStyle style;
			Link::GetFlyoutStyle(style);
			look.background = style.background;
			look.backgroundColor = style.backgroundColor;
			look.backgroundAlpha = std::clamp(static_cast<double>(style.backgroundOpacity), 0.0, 1.0) * 100.0;
			look.radius = style.cornerRadius;
			look.title.color = style.titleColor;
			look.label.color = style.labelColor;
			look.value.color = style.valueColor;
			look.title.size = 30.0;
			look.label.size = look.value.size = 20.0;

			// The text style straight from the map's flyout.
			RE::GFxValue flyout;
			if (FindFlyout(flyout)) {
				RE::GFxValue title;
				if (Get(flyout, "Title", title)) {
					ReadStyle(title, look.title);
				}
				RE::GFxValue list;
				if (Get(flyout, "DescriptionList", list) && list.IsArray()) {
					for (std::uint32_t i = 0; i < list.GetArraySize() && !look.label.fromFlyout; ++i) {
						RE::GFxValue entry;
						RE::GFxValue visible;
						if (!list.GetElement(i, &entry) || !Get(entry, "_visible", visible) || !visible.IsBool() || !visible.GetBool()) {
							continue;
						}
						RE::GFxValue field;
						if (Get(entry, "Item", field)) {
							ReadStyle(field, look.label);
						}
						if (Get(entry, "Value", field)) {
							ReadStyle(field, look.value);
						}
					}
				}
			}
			// The flyout shows no rows (no travel time for this place, the Forgotten Vale for one):
			// the rows of an earlier flyout this session, otherwise the title's look in the font
			// and size the game's own flyout rows use.
			static std::optional<std::pair<TextStyle, TextStyle>> s_lastRows;  // UI thread only
			if (look.label.fromFlyout && look.value.fromFlyout) {
				s_lastRows = { look.label, look.value };
			} else if (s_lastRows) {
				look.label = s_lastRows->first;
				look.value = s_lastRows->second;
				look.rowsFrom = "an earlier flyout";
			} else if (look.title.fromFlyout) {
				for (auto* row : { &look.label, &look.value }) {
					const auto color = row->color;
					*row = look.title;
					row->font = "$EverywhereFont";
					row->size = std::round(look.title.size * 2.0 / 3.0);
					row->color = color;
					row->fromFlyout = false;
				}
				look.rowsFrom = "the title";
			}
			for (const auto& [name, value] : look.title.properties) {
				const auto* number = std::get_if<double>(&value);
				if (!number) {
					continue;
				}
				if (std::string_view(name) == "shadowDistance") {
					look.shadowDistance = *number;
				} else if (std::string_view(name) == "shadowAngle") {
					look.shadowAngle = *number;
				} else if (std::string_view(name) == "shadowColor") {
					look.shadowColor = static_cast<std::uint32_t>(*number) & 0xFFFFFFu;
				} else if (std::string_view(name) == "shadowAlpha") {
					look.shadowAlpha = *number;
				}
			}
			// Scaleform's shadow style instead, e.g. "s{1,1}t{0,0}" (offsets in pixels).
			if (!look.shadowDistance) {
				for (const auto& [name, value] : look.title.properties) {
					const auto* text = std::get_if<std::string>(&value);
					if (!text || std::string_view(name) != "shadowStyle") {
						continue;
					}
					const auto open = text->find('{', text->find('s'));
					const auto close = open == std::string::npos ? std::string::npos : text->find('}', open);
					double     x = 0.0;
					double     y = 0.0;
					if (close != std::string::npos && std::sscanf(text->substr(open + 1, close - open - 1).c_str(), "%lf,%lf", &x, &y) == 2 &&
						(x != 0.0 || y != 0.0)) {
						look.shadowDistance = std::hypot(x, y);
						look.shadowAngle = std::atan2(y, x) * 180.0 / 3.14159265358979;
						if (look.shadowAlpha <= 0.0) {
							look.shadowAlpha = 1.0;
						}
					}
				}
			}
			// Or a drop shadow filter.
			if (!look.shadowDistance && look.title.filter && look.title.filter->className.ends_with("DropShadowFilter")) {
				for (const auto& [name, value] : look.title.filter->members) {
					const auto* number = std::get_if<double>(&value);
					if (!number) {
						continue;
					}
					if (name == "distance") {
						look.shadowDistance = *number;
					} else if (name == "angle") {
						look.shadowAngle = *number;
					} else if (name == "color") {
						look.shadowColor = static_cast<std::uint32_t>(*number) & 0xFFFFFFu;
					} else if (name == "alpha") {
						look.shadowAlpha = *number;
					}
				}
			}
			return look;
		}

		// ---- Button events from the movie

		enum Kind : std::uint32_t
		{
			kToggle = 1,
			kButton,
			kTrack,
			kKnob,
		};

		enum Event : std::uint32_t
		{
			kRollOver = 1,
			kRollOut,
			kPress,
			kRelease,
			kReleaseOutside,
		};

		constexpr std::array EVENT_NAMES{ std::pair{ kRollOver, "onRollOver" }, std::pair{ kRollOut, "onRollOut" }, std::pair{ kPress, "onPress" },
			std::pair{ kRelease, "onRelease" }, std::pair{ kReleaseOutside, "onReleaseOutside" } };

		constexpr std::uint32_t Code(Kind a_kind, std::uint32_t a_index, Event a_event)
		{
			return (static_cast<std::uint32_t>(a_kind) << 16) | ((a_index & 0xFFF) << 4) | static_cast<std::uint32_t>(a_event);
		}

		class ArrivalMenu;
		ArrivalMenu* g_menu = nullptr;  // UI thread only

		class Handler final : public RE::GFxFunctionHandler
		{
		public:
			void Call(Params& a_params) override;
		};

		Handler* EventHandler()
		{
			static Handler* handler = new Handler();  // held for the whole session
			return handler;
		}

		// ---- The menu

		struct Toggle
		{
			const char*  text;
			Timing       timing;
			bool         enabled{ true };
			RE::GFxValue clip;
			double       w{ 0.0 };
			double       h{ 0.0 };
		};

		struct Button
		{
			std::string            text;
			Planner::Choice::Action action{ Planner::Choice::Action::kCancel };
			int                    otherIndex{ -1 };
			RE::GFxValue           clip;
			double                 w{ 0.0 };
			double                 h{ 0.0 };
			bool                   hovered{ false };
			bool                   pressed{ false };
		};

		struct Slider
		{
			RE::GFxValue knob;
			RE::GFxValue valueText;
			double       x0{ 0.0 };     // track start and length, in the slider's own coordinates
			double       length{ 0.0 };
			double       y{ 0.0 };
			int          minimum{ 0 };
			int          maximum{ 0 };
		};

		class ArrivalMenu final : public RE::IMenu
		{
		public:
			ArrivalMenu()
			{
				using Flag = RE::UI_MENU_FLAGS;
				menuFlags.set(Flag::kPausesGame, Flag::kModal, Flag::kUsesCursor, Flag::kUsesMenuContext, Flag::kDisablePauseMenu,
					Flag::kRequiresUpdate);
				inputContext = Context::kMenuMode;
				depthPriority = PickDepth();
				if (const auto scaleform = RE::BSScaleformManager::GetSingleton()) {
					scaleform->LoadMovie(this, uiMovie, MOVIE_NAME, RE::BSScaleformManager::ScaleModeType::kShowAll, 0.0f);
				}
				if (uiMovie) {
					// Scaleform's text-field extensions (the text shadow among them).
					uiMovie->SetVariable("_global.gfxExtensions", RE::GFxValue(true));
				} else {
					logs::error("Panel: Interface\\FastTravelArrivalPlanner.swf could not be loaded");
				}
				g_alive = true;
				g_menu = this;
			}

			~ArrivalMenu() override
			{
				if (g_menu == this) {
					g_menu = nullptr;
				}
				g_alive = false;
			}

			static RE::IMenu* Create() { return new ArrivalMenu(); }

			// Where the game's own message boxes go (10): above the map (3), below the cursor (13).
			static std::int8_t PickDepth()
			{
				int        depth = 10;
				int        map = -1;
				int        cursor = -1;
				const auto ui = RE::UI::GetSingleton();
				if (ui) {
					if (const auto menu = ui->GetMenu(RE::MapMenu::MENU_NAME)) {
						map = menu->depthPriority;
					}
					if (const auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME)) {
						cursor = menu->depthPriority;
					}
				}
				if (map >= 0) {
					depth = (std::max)(depth, map + 1);
				}
				if (cursor > 0) {
					depth = (std::min)(depth, cursor - 1);
				}
				logs::info("Panel: menu depth {} (map {}, cursor {})", depth, map, cursor);
				return static_cast<std::int8_t>(depth);
			}

			RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
			{
				using Message = RE::UI_MESSAGE_TYPE;
				try {
					switch (*a_message.type) {
					case Message::kShow:
						Build();
						break;
					case Message::kScaleformEvent:
						if (HandleKey(a_message)) {
							return RE::UI_MESSAGE_RESULTS::kHandled;
						}
						break;
					case Message::kUserEvent:
						if (HandleUserEvent(a_message)) {
							return RE::UI_MESSAGE_RESULTS::kHandled;
						}
						break;
					default:
						break;
					}
				} catch (const std::exception& e) {
					logs::error("Panel: {}", e.what());
				}
				return RE::IMenu::ProcessMessage(a_message);
			}

			void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override
			{
				try {
					if (m_dragging >= 0) {
						FollowDrag();
					}
					if (m_dirty && !m_submitted) {
						m_dirty = false;
						Layout();
					}
				} catch (const std::exception& e) {
					logs::error("Panel: {}", e.what());
				}
				RE::IMenu::AdvanceMovie(a_interval, a_currentTime);
			}

			void OnEvent(std::uint32_t a_code)
			{
				if (m_submitted) {
					return;
				}
				const auto kind = static_cast<Kind>(a_code >> 16);
				const auto index = static_cast<std::size_t>((a_code >> 4) & 0xFFF);
				const auto event = static_cast<Event>(a_code & 0xF);
				switch (kind) {
				case kToggle:
					OnToggle(index, event);
					break;
				case kButton:
					OnButton(index, event);
					break;
				case kTrack:
					if (event == kRelease) {
						OnTrack(index);
					}
					break;
				case kKnob:
					OnKnob(index, event);
					break;
				default:
					break;
				}
			}

		private:
			// ---- Building

			void Build()
			{
				if (!uiMovie) {
					logs::error("Panel: no movie; closing (treated as Cancel)");
					Close();
					return;
				}
				m_data = Planner::GetPanelData();
				if (!m_data.open) {
					logs::info("Panel: opened without a trip; closing");
					Close();
					return;
				}
				m_look = ReadLook();
				m_openedAt = std::chrono::steady_clock::now();
				m_timing = Timing::kNormal;
				m_hasEstimate = m_data.trip.valid && m_data.trip.hours > 0.0;
				m_range = MakeRange(m_data.nowHour, m_hasEstimate ? m_data.trip.hours : 0.0);
				m_laterDay = m_range.dayMin;
				m_laterHour = m_range.HourMin(m_laterDay);
				m_top.reset();
				m_inputLogged = 0;
				m_inputSeen.clear();
				logs::info("Panel: title {}; labels {}; values {}", Describe(m_look.title), Describe(m_look.label), Describe(m_look.value));
				if (m_look.title.fromFlyout) {
					logs::info("Panel: rows from {}; background {}", m_look.rowsFrom,
						m_look.background ? std::format("#{:06X} at {:.0f}%, corners {:g}", m_look.backgroundColor, m_look.backgroundAlpha, m_look.radius) : "off"s);
				} else {
					logs::warn("Panel: the map's flyout could not be read: default look");
				}
				Layout();
			}

			RE::GFxValue NewClip(RE::GFxValue& a_parent, const std::string& a_name, int a_depth)
			{
				std::array<RE::GFxValue, 2> args{ RE::GFxValue(a_name.c_str()), RE::GFxValue(static_cast<double>(a_depth)) };
				RE::GFxValue                clip;
				a_parent.Invoke("createEmptyMovieClip", &clip, args.data(), args.size());
				if (!clip.IsDisplayObject()) {
					Get(a_parent, a_name.c_str(), clip);
				}
				return clip;
			}

			RE::GFxValue NewText(RE::GFxValue& a_parent, const std::string& a_name, int a_depth, const TextStyle& a_style, const std::string& a_text,
				double a_x, double a_y, std::optional<std::uint32_t> a_color = std::nullopt, double a_wrapWidth = 0.0)
			{
				std::array<RE::GFxValue, 6> args{ RE::GFxValue(a_name.c_str()), RE::GFxValue(static_cast<double>(a_depth)), RE::GFxValue(a_x),
					RE::GFxValue(a_y), RE::GFxValue(a_wrapWidth > 0.0 ? a_wrapWidth : 8.0), RE::GFxValue(8.0) };
				a_parent.Invoke("createTextField", nullptr, args.data(), args.size());
				RE::GFxValue field;
				if (!Get(a_parent, a_name.c_str(), field) || !field.IsDisplayObject()) {
					return field;
				}
				Set(field, "embedFonts", true);
				Set(field, "selectable", false);
				Set(field, "html", false);
				if (a_wrapWidth > 0.0) {
					Set(field, "multiline", true);
					Set(field, "wordWrap", true);
				}
				Set(field, "autoSize", std::string("left"));
				for (const auto& [name, value] : a_style.properties) {
					std::visit([&](const auto& v) {
						using T = std::decay_t<decltype(v)>;
						if constexpr (std::is_same_v<T, std::string>) {
							Set(field, name.c_str(), v);
						} else {
							Set(field, name.c_str(), v);
						}
					}, value);
				}
				if (m_systemFont) {
					Set(field, "embedFonts", false);
				}
				if (a_style.filter) {
					RE::GFxValue filter;
					uiMovie->CreateObject(&filter, a_style.filter->className.c_str());
					RE::GFxValue list;
					uiMovie->CreateArray(&list);
					if (filter.IsObject() && list.IsArray()) {
						for (const auto& [name, value] : a_style.filter->members) {
							std::visit([&](const auto& v) {
								using T = std::decay_t<decltype(v)>;
								if constexpr (!std::is_same_v<T, std::string>) {
									Set(filter, name.c_str(), v);
								}
							}, value);
						}
						list.PushBack(filter);
						field.SetMember("filters", list);
					} else if (!m_warnedFilter) {
						m_warnedFilter = true;
						logs::warn("Panel: could not make a {} for the text", a_style.filter->className);
					}
				}
				RE::GFxValue format;
				uiMovie->CreateObject(&format, "TextFormat");
				Set(format, "font", a_style.font);
				Set(format, "size", a_style.size);
				Set(format, "color", static_cast<double>(a_color.value_or(a_style.color)));
				Set(format, "bold", a_style.bold);
				Set(format, "italic", a_style.italic);
				if (a_style.letterSpacing) {
					Set(format, "letterSpacing", *a_style.letterSpacing);
				}
				field.Invoke("setNewTextFormat", nullptr, &format, 1);
				Set(field, "text", a_text);
				field.Invoke("setTextFormat", nullptr, &format, 1);
				if (!a_text.empty() && !m_systemFont && Number(field, "textWidth").value_or(0.0) <= 0.0) {
					// The font is not in the game's font library: the system's font, for every field from here on.
					m_systemFont = true;
					logs::warn("Panel: the font \"{}\" drew nothing; using the system font", a_style.font);
					Set(field, "embedFonts", false);
				}
				return field;
			}

			void SetText(RE::GFxValue& a_field, const std::string& a_text)
			{
				if (a_field.IsDisplayObject()) {
					Set(a_field, "text", a_text);
				}
			}

			static double Width(const RE::GFxValue& a_object) { return Number(a_object, "_width").value_or(0.0); }
			static double Height(const RE::GFxValue& a_object) { return Number(a_object, "_height").value_or(0.0); }

			void Bind(RE::GFxValue& a_clip, Kind a_kind, std::uint32_t a_index)
			{
				for (const auto& [event, name] : EVENT_NAMES) {
					RE::GFxValue fn;
					uiMovie->CreateFunction(&fn, EventHandler(), reinterpret_cast<void*>(static_cast<std::uintptr_t>(Code(a_kind, a_index, event))));
					a_clip.SetMember(name, fn);
				}
				Set(a_clip, "tabEnabled", false);
				Set(a_clip, "useHandCursor", false);
			}

			double Row() const { return m_look.value.size; }

			// The flyout's diamond bullet, with the text's shadow under it.
			void Diamond(RE::GFxValue& a_deco, double a_cx, double a_cy)
			{
				const double w = Row() * 0.17;
				const double h = Row() * 0.24;
				const auto   draw = [&](double a_x, double a_y, std::uint32_t a_color, double a_alpha) {
					Call(a_deco, "beginFill", { static_cast<double>(a_color), a_alpha });
					Call(a_deco, "moveTo", { a_x, a_y - h });
					Call(a_deco, "lineTo", { a_x + w, a_y });
					Call(a_deco, "lineTo", { a_x, a_y + h });
					Call(a_deco, "lineTo", { a_x - w, a_y });
					Call(a_deco, "lineTo", { a_x, a_y - h });
					a_deco.Invoke("endFill");
				};
				if (m_look.shadowDistance && *m_look.shadowDistance > 0.0 && m_look.shadowAlpha > 0.0) {
					const double radians = m_look.shadowAngle * 3.14159265358979 / 180.0;
					const double alpha = m_look.shadowAlpha <= 1.0 ? m_look.shadowAlpha * 100.0 : m_look.shadowAlpha;
					draw(a_cx + std::cos(radians) * *m_look.shadowDistance, a_cy + std::sin(radians) * *m_look.shadowDistance, m_look.shadowColor, alpha);
				}
				draw(a_cx, a_cy, m_look.title.color, 100.0);
			}

			// "Travel time:  2 h 55 min", as the flyout shows it. Returns the row's height.
			double TextRow(RE::GFxValue& a_panel, RE::GFxValue& a_deco, const std::string& a_name, double a_x, double a_y,
				const std::string& a_label, const std::string& a_value, RE::GFxValue* a_valueField = nullptr)
			{
				auto label = NewText(a_panel, a_name + "L", NextDepth(), m_look.label, a_label, a_x + Row() * 0.6, a_y);
				const double height = Height(label);
				Diamond(a_deco, a_x + Row() * 0.2, a_y + height * 0.5);
				auto value = NewText(a_panel, a_name + "V", NextDepth(), m_look.value, a_value, a_x + Row() * 0.6 + Width(label) + Row() * 0.3, a_y);
				m_right = (std::max)(m_right, a_x + Row() * 0.6 + Width(label) + Row() * 0.3 + Width(value));
				if (a_valueField) {
					*a_valueField = value;
				}
				return (std::max)(height, Height(value));
			}

			int NextDepth() { return ++m_depth; }

			double ArrivalHours() const
			{
				const auto& trip = m_data.trip;
				switch (m_timing) {
				case Timing::kInstant:
					return 0.0;
				case Timing::kThird:
					return trip.hours / 3.0;
				case Timing::kHalf:
					return trip.hours / 2.0;
				case Timing::kLater:
					return LaterHours();
				default:
					return m_hasEstimate ? trip.hours : 0.0;
				}
			}

			double LaterHours() const { return m_range.HoursFromNow(m_laterDay, m_laterHour, m_data.nowHour); }

			std::string ArrivalText() const
			{
				return m_hasEstimate || m_timing == Timing::kInstant ? Link::FormatArrival(ArrivalHours()) : std::string("as the game decides");
			}

			std::string DayText() const
			{
				return m_laterDay == 0 ? std::string("Same day as the normal arrival") :
				                         std::format("+{} day{}", m_laterDay, m_laterDay == 1 ? "" : "s");
			}

			std::string HourText() const { return std::format("{:02d}:00", m_laterHour); }

			void Layout()
			{
				RE::GFxValue root;
				if (!uiMovie || !uiMovie->GetVariable(&root, "_root")) {
					return;
				}
				if (m_dragging >= 0) {
					if (auto& knob = m_sliders[static_cast<std::size_t>(m_dragging)].knob; knob.IsDisplayObject()) {
						knob.Invoke("stopDrag");
					}
					m_dragging = -1;
				}
				m_depth = 0;
				m_right = 0.0;
				m_toggles.clear();
				m_buttons.clear();
				m_sliders = {};
				m_arrivalValue = RE::GFxValue();

				const auto& trip = m_data.trip;
				const double row = Row();
				const double padX = row * 0.8;
				const double padY = row * 0.6;
				const double gap = row * 0.3;

				m_panel = NewClip(root, "panel", 1);  // replaces the previous layout
				auto deco = NewClip(m_panel, "deco", NextDepth());
				std::vector<double> separators;

				double y = padY;
				auto   heading = NewText(m_panel, "heading", NextDepth(), m_look.title, std::format("Travel to {}", m_data.destination), padX, y);
				m_right = (std::max)(m_right, padX + Width(heading));
				y += Height(heading) + gap * 0.5;
				separators.push_back(y);
				y += gap;

				// The normal trip
				const bool fasterAllowed = m_hasEstimate && (m_data.minHoursForFaster <= 0.0f || trip.hours >= m_data.minHoursForFaster);
				if (m_hasEstimate) {
					y += TextRow(m_panel, deco, "time", padX, y, "Travel time:", Link::FormatDuration(trip.hours, !trip.calibrated)) + gap * 0.5;
					y += TextRow(m_panel, deco, "normal", padX, y, "Normal arrival:", Link::FormatArrival(trip.hours)) + gap;
				} else {
					auto note = NewText(m_panel, "note", NextDepth(), m_look.value,
						trip.otherWorldspace ? "No estimate for a trip into this area. Only a normal trip is available." :
						                       "No estimate for this trip. Only a normal trip is available.",
						padX, y);
					m_right = (std::max)(m_right, padX + Width(note));
					y += Height(note) + gap;
				}

				// Timing choices
				auto section = NewText(m_panel, "section", NextDepth(), m_look.label, "Arrival", padX, y);
				const double sectionY = y + Height(section) * 0.55;
				const double sectionX = padX + Width(section) + gap;
				y += Height(section) + gap * 0.5;

				if (!m_hasEstimate || (!fasterAllowed && m_timing < Timing::kNormal)) {
					m_timing = m_hasEstimate ? m_timing : Timing::kNormal;
					if (!fasterAllowed && m_timing < Timing::kNormal) {
						m_timing = Timing::kNormal;
					}
				}
				m_toggles = {
					Toggle{ "Instant", Timing::kInstant, fasterAllowed },
					Toggle{ "1/3", Timing::kThird, fasterAllowed },
					Toggle{ "1/2", Timing::kHalf, fasterAllowed },
					Toggle{ "Normal ETA", Timing::kNormal, true },
					Toggle{ "Later", Timing::kLater, m_hasEstimate },
				};
				double x = padX - row * 0.4;
				double toggleHeight = 0.0;
				for (std::size_t i = 0; i < m_toggles.size(); ++i) {
					auto& toggle = m_toggles[i];
					toggle.clip = NewClip(m_panel, std::format("toggle{}", i), NextDepth());
					Set(toggle.clip, "_x", x);
					Set(toggle.clip, "_y", y);
					const bool selected = toggle.timing == m_timing;
					auto       text = NewText(toggle.clip, "label", 1, m_look.label, toggle.text, row * 0.4, 0.0,
                        selected ? m_look.title.color : m_look.label.color);
					if (!toggle.enabled) {
						Set(text, "_alpha", 40.0);
					}
					toggle.w = Width(text) + row * 0.8;
					toggle.h = Height(text);
					toggleHeight = (std::max)(toggleHeight, toggle.h);
					DrawToggle(i, false);
					Bind(toggle.clip, kToggle, static_cast<std::uint32_t>(i));
					x += toggle.w + row * 0.1;
				}
				m_right = (std::max)(m_right, x);
				y += toggleHeight + gap * 0.5;
				if (m_hasEstimate) {
					// As wide as the Later sliders make it, so choosing Later does not move the choices.
					auto probe = NewText(m_panel, "probe", NextDepth(), m_look.value, "Same day as the normal arrival", 0.0, 0.0);
					m_right = (std::max)(m_right, padX + row * 15.8 + Width(probe));
					Set(probe, "_visible", false);
				}

				// Why the faster choices are greyed out (without an estimate, the note above says why).
				if (m_hasEstimate && !fasterAllowed) {
					auto hint = NewText(m_panel, "hint", NextDepth(), m_look.label,
						std::format("Instant, 1/3 and 1/2 need a travel time of at least {}.", Link::FormatDuration(m_data.minHoursForFaster, false)), padX, y);
					Set(hint, "_alpha", 75.0);
					m_right = (std::max)(m_right, padX + Width(hint));
					y += Height(hint) + gap * 0.5;
				}

				// Later: day and hour
				if (m_timing == Timing::kLater) {
					y += gap * 0.5;
					y += SliderRow(0, "Day", padX, y) + gap * 0.5;
					y += SliderRow(1, "Hour", padX, y) + gap;
				}

				separators.push_back(y);
				y += gap;

				// The result
				y += TextRow(m_panel, deco, "arrival", padX, y, "Arrival:", ArrivalText(), &m_arrivalValue) + gap * 0.5;
				const double contentWidth = (std::max)(m_right - padX, row * 20.0);
				std::vector<std::string> notes;
				if (m_hasEstimate && m_timing == Timing::kLater && LaterHours() - trip.hours > 24.0) {
					notes.emplace_back("More than a day after the normal arrival. Time passes as if you waited: timed effects end, "
									   "vampirism and survival needs advance, and followers you left waiting may leave.");
				}
				if (m_hasEstimate && !trip.calibrated && m_timing != Timing::kNormal) {
					notes.emplace_back("The travel time is approximate, so the arrival may be off.");
				}
				for (std::size_t i = 0; i < notes.size(); ++i) {
					auto note = NewText(m_panel, std::format("warning{}", i), NextDepth(), m_look.label, notes[i], padX, y, std::nullopt, contentWidth);
					Set(note, "_alpha", 80.0);
					y += Height(note) + gap * 0.5;
				}
				y += gap * 0.5;

				// Buttons
				m_buttons.clear();
				m_buttons.push_back(Button{ "Proceed", Planner::Choice::Action::kProceed });
				for (const auto& [index, text] : m_data.extraButtons) {
					m_buttons.push_back(Button{ text, Planner::Choice::Action::kOther, index });
				}
				m_buttons.push_back(Button{ "Cancel", Planner::Choice::Action::kCancel });
				x = padX;
				double buttonHeight = 0.0;
				for (std::size_t i = 0; i < m_buttons.size(); ++i) {
					auto& button = m_buttons[i];
					button.clip = NewClip(m_panel, std::format("button{}", i), NextDepth());
					Set(button.clip, "_x", x);
					Set(button.clip, "_y", y);
					auto text = NewText(button.clip, "label", 1, m_look.value, button.text, row * 0.5, row * 0.15);
					button.w = Width(text) + row;
					button.h = Height(text) + row * 0.3;
					buttonHeight = (std::max)(buttonHeight, button.h);
					DrawButton(i);
					Bind(button.clip, kButton, static_cast<std::uint32_t>(i));
					x += button.w + row * 0.3;
				}
				m_right = (std::max)(m_right, x - row * 0.3);
				y += buttonHeight + padY;

				// Lines and background, now that the size is known
				const double width = (std::max)(m_right, padX + contentWidth) + padX;
				Call(deco, "lineStyle", { 1.0, static_cast<double>(m_look.label.color), 40.0 });
				for (const auto line : separators) {
					Call(deco, "moveTo", { padX, line });
					Call(deco, "lineTo", { width - padX, line });
				}
				Call(deco, "moveTo", { sectionX, sectionY });
				Call(deco, "lineTo", { width - padX, sectionY });
				deco.Invoke("lineStyle");
				Call(m_panel, "clear");
				if (m_look.background && m_look.backgroundAlpha > 0.0) {
					FillRect(m_panel, m_look.backgroundColor, m_look.backgroundAlpha, 0.0, 0.0, width, y, m_look.radius);
				}
				// Centred when it opens; afterwards the top stays put and the panel grows downwards.
				if (!m_top) {
					m_top = std::round((STAGE_HEIGHT - y) / 2.0);
				}
				Set(m_panel, "_x", std::round((STAGE_WIDTH - width) / 2.0));
				Set(m_panel, "_y", std::clamp(*m_top, 0.0, (std::max)(0.0, STAGE_HEIGHT - y)));
			}

			double SliderRow(std::size_t a_index, const char* a_label, double a_x, double a_y)
			{
				const double row = Row();
				auto&        slider = m_sliders[a_index];
				auto         label = NewText(m_panel, std::format("sliderLabel{}", a_index), NextDepth(), m_look.label, a_label, a_x, a_y);
				const double height = (std::max)(Height(label), row);
				const double trackX = a_x + row * 3.0;  // 3 + 12 + 0.8 rows before the value: see the width kept in Layout
				slider.length = row * 12.0;
				slider.y = a_y + height * 0.5;
				if (a_index == 0) {
					slider.minimum = m_range.dayMin;
					slider.maximum = m_range.dayMax;
				} else {
					slider.minimum = m_range.HourMin(m_laterDay);
					slider.maximum = (std::max)(slider.minimum, m_range.HourMax(m_laterDay));
				}
				slider.x0 = trackX;

				// The track: a line, with an invisible strip around it to click on.
				auto track = NewClip(m_panel, std::format("track{}", a_index), NextDepth());
				FillRect(track, m_look.label.color, 1.0, trackX - row * 0.3, a_y, slider.length + row * 0.6, height, 0.0);
				Call(track, "lineStyle", { 2.0, static_cast<double>(m_look.label.color), 60.0 });
				Call(track, "moveTo", { trackX, slider.y });
				Call(track, "lineTo", { trackX + slider.length, slider.y });
				Bind(track, kTrack, static_cast<std::uint32_t>(a_index));

				// The knob, dragged along the track.
				slider.knob = NewClip(m_panel, std::format("knob{}", a_index), NextDepth());
				const double radius = row * 0.32;
				FillRect(slider.knob, m_look.title.color, 100.0, -radius, -radius, radius * 2.0, radius * 2.0, radius);
				Set(slider.knob, "_y", slider.y);
				Set(slider.knob, "_x", KnobX(slider, a_index == 0 ? m_laterDay : m_laterHour));
				Bind(slider.knob, kKnob, static_cast<std::uint32_t>(a_index));

				// Room for the longest value, so the panel need not grow while a knob is dragged.
				slider.valueText = NewText(m_panel, std::format("sliderValue{}", a_index), NextDepth(), m_look.value,
					a_index == 0 ? "Same day as the normal arrival" : "00:00", trackX + slider.length + row * 0.8, a_y);
				const double valueWidth = Width(slider.valueText);
				SetText(slider.valueText, a_index == 0 ? DayText() : HourText());
				m_right = (std::max)(m_right, trackX + slider.length + row * 0.8 + valueWidth);
				return height;
			}

			static double KnobX(const Slider& a_slider, int a_value)
			{
				if (a_slider.maximum <= a_slider.minimum) {
					return a_slider.x0;
				}
				return a_slider.x0 + a_slider.length * (a_value - a_slider.minimum) / static_cast<double>(a_slider.maximum - a_slider.minimum);
			}

			static int ValueAt(const Slider& a_slider, double a_x)
			{
				if (a_slider.maximum <= a_slider.minimum || a_slider.length <= 0.0) {
					return a_slider.minimum;
				}
				const double t = std::clamp((a_x - a_slider.x0) / a_slider.length, 0.0, 1.0);
				return a_slider.minimum + static_cast<int>(std::lround(t * (a_slider.maximum - a_slider.minimum)));
			}

			// ---- Drawing states

			void DrawToggle(std::size_t a_index, bool a_hovered)
			{
				auto& toggle = m_toggles[a_index];
				Call(toggle.clip, "clear");
				const bool selected = toggle.timing == m_timing;
				// A fill of alpha 1 still catches the mouse while looking empty.
				const double alpha = selected ? 25.0 : (a_hovered && toggle.enabled ? 14.0 : 1.0);
				FillRect(toggle.clip, m_look.label.color, alpha, 0.0, 0.0, toggle.w, toggle.h, m_look.radius * 0.66);
			}

			void DrawButton(std::size_t a_index)
			{
				auto& button = m_buttons[a_index];
				Call(button.clip, "clear");
				const double alpha = button.pressed ? 45.0 : (button.hovered ? 32.0 : 18.0);
				FillRect(button.clip, m_look.label.color, alpha, 0.0, 0.0, button.w, button.h, m_look.radius * 0.66);
			}

			// ---- Events

			bool Ready(const char* a_what) const
			{
				const auto age = std::chrono::steady_clock::now() - m_openedAt;
				if (age >= INPUT_GUARD) {
					return true;
				}
				logs::info("Panel: {} ignored, {} ms after the panel opened", a_what,
					std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
				return false;
			}

			void OnToggle(std::size_t a_index, Event a_event)
			{
				if (a_index >= m_toggles.size()) {
					return;
				}
				auto& toggle = m_toggles[a_index];
				switch (a_event) {
				case kRollOver:
					DrawToggle(a_index, true);
					break;
				case kRollOut:
				case kReleaseOutside:
					DrawToggle(a_index, false);
					break;
				case kRelease:
					if (toggle.enabled && toggle.timing != m_timing) {
						m_timing = toggle.timing;
						m_dirty = true;
					}
					break;
				default:
					break;
				}
			}

			void OnButton(std::size_t a_index, Event a_event)
			{
				if (a_index >= m_buttons.size()) {
					return;
				}
				auto& button = m_buttons[a_index];
				switch (a_event) {
				case kRollOver:
					button.hovered = true;
					break;
				case kRollOut:
					button.hovered = false;
					button.pressed = false;
					break;
				case kPress:
					button.pressed = true;
					break;
				case kReleaseOutside:
					button.pressed = false;
					button.hovered = false;
					break;
				case kRelease:
					button.pressed = false;
					if (Ready(button.text.c_str())) {
						DrawButton(a_index);
						Choose(button.action, button.otherIndex, std::format("the {} button", button.text));
						return;
					}
					break;
				default:
					break;
				}
				DrawButton(a_index);
			}

			void OnTrack(std::size_t a_index)
			{
				if (a_index >= m_sliders.size()) {
					return;
				}
				auto& slider = m_sliders[a_index];
				if (const auto mouse = Number(m_panel, "_xmouse")) {
					SetSlider(a_index, ValueAt(slider, *mouse));
					m_dirty = true;
				}
			}

			void OnKnob(std::size_t a_index, Event a_event)
			{
				if (a_index >= m_sliders.size()) {
					return;
				}
				auto& slider = m_sliders[a_index];
				if (a_event == kPress) {
					std::array<RE::GFxValue, 5> args{ RE::GFxValue(false), RE::GFxValue(slider.x0), RE::GFxValue(slider.y),
						RE::GFxValue(slider.x0 + slider.length), RE::GFxValue(slider.y) };
					slider.knob.Invoke("startDrag", nullptr, args.data(), args.size());
					m_dragging = static_cast<int>(a_index);
				} else if ((a_event == kRelease || a_event == kReleaseOutside) && m_dragging == static_cast<int>(a_index)) {
					slider.knob.Invoke("stopDrag");
					FollowDrag();
					m_dragging = -1;
					m_dirty = true;  // snaps the knob; the warning may come or go
				}
			}

			// While a knob is dragged: the value under it, and the texts that depend on it.
			void FollowDrag()
			{
				if (m_dragging < 0 || static_cast<std::size_t>(m_dragging) >= m_sliders.size()) {
					return;
				}
				const auto& slider = m_sliders[static_cast<std::size_t>(m_dragging)];
				if (const auto x = Number(slider.knob, "_x")) {
					SetSlider(static_cast<std::size_t>(m_dragging), ValueAt(slider, *x));
				}
			}

			void SetSlider(std::size_t a_index, int a_value)
			{
				const int before = a_index == 0 ? m_laterDay : m_laterHour;
				if (a_index == 0) {
					m_laterDay = std::clamp(a_value, m_range.dayMin, m_range.dayMax);
					const int hourMin = m_range.HourMin(m_laterDay);
					m_laterHour = std::clamp(m_laterHour, hourMin, (std::max)(hourMin, m_range.HourMax(m_laterDay)));
				} else {
					const int hourMin = m_range.HourMin(m_laterDay);
					m_laterHour = std::clamp(a_value, hourMin, (std::max)(hourMin, m_range.HourMax(m_laterDay)));
				}
				if ((a_index == 0 ? m_laterDay : m_laterHour) != before) {
					SetText(m_sliders[0].valueText, DayText());
					SetText(m_sliders[1].valueText, HourText());
					SetText(m_arrivalValue, ArrivalText());
				}
			}

			void Choose(Planner::Choice::Action a_action, int a_otherIndex, const std::string& a_how)
			{
				if (m_submitted) {
					return;
				}
				m_submitted = true;
				if (m_dragging >= 0) {
					m_sliders[static_cast<std::size_t>(m_dragging)].knob.Invoke("stopDrag");
					m_dragging = -1;
				}
				logs::info("Panel: {} with {}", a_action == Planner::Choice::Action::kProceed ? "proceed" :
				                                     a_action == Planner::Choice::Action::kCancel  ? "cancelled" :
				                                                                                     "other button",
					a_how);
				Planner::Choice choice;
				choice.action = a_action;
				choice.otherIndex = a_otherIndex;
				choice.timing = m_timing;
				choice.laterHours = m_timing == Timing::kLater ? LaterHours() : 0.0;
				choice.serial = m_data.serial;
				Planner::Submit(choice);
			}

			// What a key or one of the game's inputs asks for.
			enum class Command
			{
				kNone,
				kCancel,
				kProceed,
				kPrevious,  // timing choice to the left
				kNext,
				kHourUp,
				kHourDown,
				kDayUp,
				kDayDown,
			};

			static const char* CommandName(Command a_command)
			{
				switch (a_command) {
				case Command::kCancel:
					return "Cancel";
				case Command::kProceed:
					return "Proceed";
				case Command::kPrevious:
					return "previous timing";
				case Command::kNext:
					return "next timing";
				case Command::kHourUp:
					return "hour up";
				case Command::kHourDown:
					return "hour down";
				case Command::kDayUp:
					return "day up";
				case Command::kDayDown:
					return "day down";
				default:
					return "nothing";
				}
			}

			// Each input name is logged once per panel, keys up to a limit: enough to learn what
			// a keyboard and a controller send without filling the log with mouse clicks.
			void LogInput(const std::string& a_what, Command a_command)
			{
				if (m_inputLogged >= 24 || std::ranges::find(m_inputSeen, a_what) != m_inputSeen.end()) {
					return;
				}
				++m_inputLogged;
				m_inputSeen.push_back(a_what);
				logs::info("Panel: {} -> {}", a_what, CommandName(a_command));
			}

			// Keys, as the movie receives them.
			bool HandleKey(RE::UIMessage& a_message)
			{
				const auto data = static_cast<RE::BSUIScaleformData*>(a_message.data);
				const auto event = data ? data->scaleformEvent : nullptr;
				if (!event || event->type != RE::GFxEvent::EventType::kKeyDown) {
					return false;
				}
				const auto key = static_cast<RE::GFxKeyEvent*>(event)->keyCode;
				Command    command = Command::kNone;
				switch (key) {
				case RE::GFxKey::kEscape:
				case RE::GFxKey::kTab:  // backs out of the game's menus too
					command = Command::kCancel;
					break;
				case RE::GFxKey::kReturn:
				case RE::GFxKey::kKP_Enter:
					command = Command::kProceed;
					break;
				case RE::GFxKey::kLeft:
					command = Command::kPrevious;
					break;
				case RE::GFxKey::kRight:
					command = Command::kNext;
					break;
				case RE::GFxKey::kUp:
					command = Command::kHourUp;
					break;
				case RE::GFxKey::kDown:
					command = Command::kHourDown;
					break;
				case RE::GFxKey::kPageUp:
					command = Command::kDayUp;
					break;
				case RE::GFxKey::kPageDown:
					command = Command::kDayDown;
					break;
				default:
					break;
				}
				const auto what = std::format("key {}", static_cast<int>(key));
				LogInput(what, command);
				return command != Command::kNone && Run(command, false, what);
			}

			// The game's own inputs ("Cancel", "Accept", "Up"...), which follow the player's key
			// bindings and the controller.
			bool HandleUserEvent(RE::UIMessage& a_message)
			{
				const auto        data = static_cast<RE::BSUIMessageData*>(a_message.data);
				const std::string name = data && data->fixedStr.c_str() ? data->fixedStr.c_str() : "";
				static const std::array<std::pair<std::string_view, Command>, 10> inputs{ {
					{ "Cancel", Command::kCancel },
					{ "Accept", Command::kProceed },
					{ "Left", Command::kPrevious },
					{ "Right", Command::kNext },
					{ "Up", Command::kHourUp },
					{ "Down", Command::kHourDown },
					{ "PageUp", Command::kDayUp },
					{ "PageDown", Command::kDayDown },
					{ "NextPage", Command::kDayUp },
					{ "PrevPage", Command::kDayDown },
				} };
				Command command = Command::kNone;
				for (const auto& [input, what] : inputs) {
					if (name == input) {
						command = what;
						break;
					}
				}
				const auto what = std::format("input \"{}\"", name);
				LogInput(what, command);
				return command != Command::kNone && Run(command, true, std::format("the game's \"{}\" input", name));
			}

			bool Run(Command a_command, bool a_fromInput, const std::string& a_how)
			{
				if (m_submitted) {
					return true;
				}
				// One press often arrives twice, as a key and as the game's input (Tab: "Cancel"
				// and key 9 in the same frame). Act once.
				const auto now = std::chrono::steady_clock::now();
				if (a_command == m_lastCommand && a_fromInput != m_lastFromInput && now - m_lastCommandAt < std::chrono::milliseconds(100)) {
					m_lastCommand = Command::kNone;
					return true;
				}
				m_lastCommand = a_command;
				m_lastFromInput = a_fromInput;
				m_lastCommandAt = now;

				switch (a_command) {
				case Command::kCancel:
					if (Ready(a_how.c_str())) {
						Choose(Planner::Choice::Action::kCancel, -1, a_how);
					}
					return true;
				case Command::kProceed:
					if (Ready(a_how.c_str())) {
						Choose(Planner::Choice::Action::kProceed, -1, a_how);
					}
					return true;
				case Command::kPrevious:
				case Command::kNext:
					{
						// The next choice that is available, to the left or the right.
						const int step = a_command == Command::kPrevious ? -1 : 1;
						int       index = static_cast<int>(m_timing);
						for (int tries = 0; tries < 5; ++tries) {
							index += step;
							if (index < 0 || index >= static_cast<int>(m_toggles.size())) {
								break;
							}
							if (m_toggles[static_cast<std::size_t>(index)].enabled) {
								m_timing = m_toggles[static_cast<std::size_t>(index)].timing;
								m_dirty = true;
								break;
							}
						}
						return true;
					}
				case Command::kHourUp:
				case Command::kHourDown:
					if (m_timing == Timing::kLater) {
						SetSlider(1, m_laterHour + (a_command == Command::kHourUp ? 1 : -1));
						m_dirty = true;
					}
					return true;
				case Command::kDayUp:
				case Command::kDayDown:
					if (m_timing == Timing::kLater) {
						SetSlider(0, m_laterDay + (a_command == Command::kDayUp ? 1 : -1));
						m_dirty = true;
					}
					return true;
				default:
					return false;
				}
			}

			Planner::PanelData                       m_data;
			Look                                     m_look;
			std::chrono::steady_clock::time_point    m_openedAt{};
			Timing                                   m_timing{ Timing::kNormal };
			bool                                     m_hasEstimate{ false };
			LaterRange                               m_range;
			int                                      m_laterDay{ 0 };
			int                                      m_laterHour{ 0 };
			RE::GFxValue                             m_panel;
			RE::GFxValue                             m_arrivalValue;
			std::vector<Toggle>                      m_toggles;
			std::vector<Button>                      m_buttons;
			std::array<Slider, 2>                    m_sliders{};
			int                                      m_depth{ 0 };
			double                                   m_right{ 0.0 };
			int                                      m_dragging{ -1 };
			bool                                     m_dirty{ false };
			bool                                     m_submitted{ false };
			bool                                     m_systemFont{ false };
			bool                                     m_warnedFilter{ false };
			std::optional<double>                    m_top;
			int                                      m_inputLogged{ 0 };
			std::vector<std::string>                 m_inputSeen;
			Command                                  m_lastCommand{ Command::kNone };
			bool                                     m_lastFromInput{ false };
			std::chrono::steady_clock::time_point    m_lastCommandAt{};
		};

		void Handler::Call(Params& a_params)
		{
			try {
				if (g_menu) {
					g_menu->OnEvent(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_params.userData)));
				}
			} catch (const std::exception& e) {
				logs::error("Panel: {}", e.what());
			} catch (...) {
				logs::error("Panel: unknown error in a button event");
			}
		}

		// Knows when the game has really closed the menu.
		class MenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuWatcher* GetSingleton()
			{
				static MenuWatcher singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == MENU_NAME && !a_event->opening) {
					// Also when the game closed it without a button: the planner then sees it closed.
					g_requested = false;
					g_alive = false;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool Register()
	{
		if (REL::Module::IsVR()) {
			return false;
		}
		{
			RE::BSResourceNiBinaryStream file{ "Interface/FastTravelArrivalPlanner.swf" };
			if (!file.good()) {
				logs::error("Interface\\FastTravelArrivalPlanner.swf not found: the arrival panel is unavailable and the game's own box is used. Reinstall the mod.");
				return false;
			}
		}
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		ui->Register(MENU_NAME, ArrivalMenu::Create);
		ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::GetSingleton());
		g_available = true;
		logs::info("Arrival panel ready");
		return true;
	}

	bool IsAvailable()
	{
		return g_available.load();
	}

	void Open()
	{
		if (!g_available) {
			return;
		}
		g_requestedAt = std::chrono::steady_clock::now().time_since_epoch().count();
		g_requested = true;
		if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		}
	}

	void Close()
	{
		const bool wasRequested = g_requested.exchange(false);
		if (!wasRequested && !g_alive) {
			return;
		}
		if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		}
	}

	bool IsOpen()
	{
		if (g_requested && !g_alive) {
			// Asked for, but the game never made it: give up, so the planner does not wait forever.
			const auto asked = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_requestedAt.load()));
			if (std::chrono::steady_clock::now() - asked > OPEN_TIMEOUT && g_requested.exchange(false)) {
				logs::error("Panel: the game did not open the panel within {} s", OPEN_TIMEOUT.count());
			}
		}
		return g_requested.load() || g_alive.load();
	}

	bool IsReleased()
	{
		return !g_requested.load() && !g_alive.load();
	}
}
