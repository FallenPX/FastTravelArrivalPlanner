#include "pch.h"

#include "Planner.h"
#include "Link.h"
#include "Panel.h"
#include "Settings.h"

// How it works
// ------------
// 1. Clicking a world-map marker makes the game queue its own confirmation box, whose
//    callback is a FastTravelConfirmCallback. When that box opens, the planner takes the box
//    out of the queue (keeping its callback), closes it and opens the arrival panel instead.
// 2. Proceed, Cancel and any other button (such as Place Marker) are passed back to the
//    game's own callback, so the game itself carries out the choice, with its own checks.
// 3. For a timing other than Normal ETA, the game setting fFastTravelSpeedMult is changed
//    just before Proceed so that the game's own fast travel takes the chosen time, and it
//    is put back when the trip ends (or the map closes without travelling, or a save loads).
//    Time therefore only ever moves forward, and the game handles the calendar itself.
// 4. A chosen weather is applied when the player arrives, from the destination region's own
//    weather list.
//
// Prototype: every step writes to FastTravelArrivalPlanner.log so the tests can be read back.

namespace FTAP::Planner
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr double INSTANT_FACTOR = 100000.0;  // speed multiplier for Instant: ~0 hours
		constexpr float  MIN_MULT = 1.0e-6f;
		constexpr float  MAX_MULT = 1.0e7f;
		constexpr auto   TRIP_TIMEOUT = std::chrono::minutes(2);
		constexpr float  ARRIVAL_RADIUS = 16384.0f;  // four exterior cells

		// The game's confirmation box, taken over by the panel.
		struct Box
		{
			RE::BSTSmartPointer<RE::IMessageBoxCallback> callback;
			std::uint8_t                                 offset{ 0 };
			int                                          yes{ 0 };
			int                                          cancel{ 1 };
			std::vector<std::string>                     buttons;
			RE::NiPoint3                                 destinationPos;
			PanelData                                    panel;
		};

		// A trip the player has proceeded with.
		struct Trip
		{
			std::string     destination;
			Timing          timing{ Timing::kNormal };
			Weather::Choice weather{ Weather::Choice::kNatural };
			double          estimateHours{ 0.0 };
			bool            calibrated{ false };
			double          targetHours{ 0.0 };  // hours from departure
			bool            speedChanged{ false };
			float           originalMult{ 1.0f };
			float           setMult{ 1.0f };
			float           startDaysPassed{ 0.0f };
			RE::NiPoint3    originPos;
			RE::NiPoint3    destinationPos;
			Clock::time_point proceededAt;
			bool            confirmed{ false };   // the map closed with the fast travel confirmed
			bool            clockTopUp{ false };  // Later: normal trip, then the clock moves forward to the target
		};

		// What still has to happen once the player is in the world again (after the loading screen).
		struct ArrivalJob
		{
			Weather::Choice weather{ Weather::Choice::kNatural };
			double          clockHours{ 0.0 };  // move the clock forward by this much
			double          targetHours{ 0.0 };
			float           startDaysPassed{ 0.0f };
			RE::NiPoint3    originPos;
			RE::NiPoint3    destinationPos;
		};

		std::recursive_mutex      g_lock;
		std::optional<Box>        g_box;
		std::optional<Trip>       g_trip;
		std::optional<ArrivalJob> g_arrivalJob;
		std::vector<std::string>  g_lastTrip;
		std::uint64_t             g_serial{ 0 };

		// Map / conversation tracking (UI thread only).
		bool              g_mapOpen{ false };
		bool              g_mapFromConversation{ false };
		bool              g_dialogueOpen{ false };
		Clock::time_point g_dialogueClosedAt{};

		// Shared between the UI thread and the main thread.
		std::atomic<bool>      g_loadingOpen{ false };
		std::atomic<long long> g_lastArrivalTicks{ 0 };  // Clock ticks; 0 = none yet
		std::atomic<long long> g_timelineTicks{ 0 };     // when Proceed was pressed; 0 = none yet
		std::atomic<long long> g_loadingOpenedTicks{ 0 };

		long long NowTicks()
		{
			return Clock::now().time_since_epoch().count();
		}

		double SecondsSince(long long a_ticks)
		{
			return std::chrono::duration<double>(Clock::duration(NowTicks() - a_ticks)).count();
		}

		std::atomic<bool> g_actionQueued{ false };

		// ---- Small helpers

		const char* TimingName(Timing a_timing)
		{
			switch (a_timing) {
			case Timing::kInstant:
				return "Instant";
			case Timing::kThird:
				return "1/3";
			case Timing::kHalf:
				return "1/2";
			case Timing::kLater:
				return "Later";
			default:
				return "Normal ETA";
			}
		}

		std::string Hours(double a_hours)
		{
			const auto minutes = std::llround(a_hours * 60.0);
			const auto sign = minutes < 0 ? "-" : "";
			const auto total = std::llabs(minutes);
			return std::format("{}{} h {:02} min", sign, total / 60, total % 60);
		}

		// "+1234 ms" since Proceed, for the timeline in the log.
		std::string Stamp()
		{
			const auto start = g_timelineTicks.load();
			if (start == 0) {
				return {};
			}
			return std::format("[+{} ms] ", std::llround(SecondsSince(start) * 1000.0));
		}

		RE::Setting* SpeedSetting()
		{
			const auto gmst = RE::GameSettingCollection::GetSingleton();
			return gmst ? gmst->GetSetting("fFastTravelSpeedMult") : nullptr;
		}

		float DaysPassed()
		{
			const auto calendar = RE::Calendar::GetSingleton();
			return calendar ? calendar->GetDaysPassed() : 0.0f;
		}

		RE::NiPoint3 PlayerPos()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player ? player->GetPosition() : RE::NiPoint3{};
		}

		bool IsFastTravelConfirm(const RE::IMessageBoxCallback* a_callback)
		{
			if (!a_callback) {
				return false;
			}
			static REL::Relocation<std::uintptr_t> vtable{ RE::FastTravelConfirmCallback::VTABLE[0] };
			return *reinterpret_cast<const std::uintptr_t*>(a_callback) == vtable.address();
		}

		// Caller holds g_lock.
		void RestoreSpeed(Trip& a_trip, std::string_view a_why)
		{
			if (!a_trip.speedChanged) {
				return;
			}
			a_trip.speedChanged = false;
			const auto setting = SpeedSetting();
			if (!setting) {
				return;
			}
			const float current = setting->GetFloat();
			if (std::abs(current - a_trip.setMult) <= 1.0e-6f * (std::max)(1.0f, std::abs(a_trip.setMult))) {
				setting->data.f = a_trip.originalMult;
				logs::info("{}fFastTravelSpeedMult restored {} -> {} ({})", Stamp(), a_trip.setMult, a_trip.originalMult, a_why);
			} else {
				logs::warn("{}fFastTravelSpeedMult was changed by something else meanwhile (now {}); left as it is ({})", Stamp(), current, a_why);
			}
		}

		void RunButton(Box& a_box, int a_index, std::string_view a_label)
		{
			if (!a_box.callback || a_index < 0) {
				return;
			}
			const auto option = static_cast<std::uint8_t>(a_box.offset + a_index);
			logs::info("{}{}: passing button {} (\"{}\") to the game", Stamp(), a_label, a_index,
				a_index < static_cast<int>(a_box.buttons.size()) ? a_box.buttons[static_cast<std::size_t>(a_index)] : "?"s);
			a_box.callback->Run(option);
		}

		// ---- Taking over the game's confirmation box (UI thread)

		const RE::MapMenuMarker* FindMarker(const RE::MapMenu::RUNTIME_DATA2& a_data, std::string_view a_body)
		{
			const auto nameOf = [](const RE::MapMenuMarker& a_marker) -> std::string_view {
				if (a_marker.fullName) {
					if (const auto name = a_marker.fullName->GetFullName()) {
						return name;
					}
				}
				return {};
			};
			const auto& markers = a_data.mapMarkers;
			const auto  selected = a_data.selectedMarker;
			if (selected >= 0 && static_cast<std::uint32_t>(selected) < markers.size()) {
				const auto& marker = markers[static_cast<std::uint32_t>(selected)];
				const auto  name = nameOf(marker);
				if (!name.empty() && a_body.find(name) != std::string_view::npos) {
					return &marker;
				}
			}
			// Fall back to the one marker whose name the box mentions.
			const RE::MapMenuMarker* found = nullptr;
			std::size_t              foundLength = 0;
			for (const auto& marker : markers) {
				const auto name = nameOf(marker);
				if (name.empty() || a_body.find(name) == std::string_view::npos) {
					continue;
				}
				if (name.size() > foundLength) {  // "Riverwood Trader" beats "Riverwood"
					found = &marker;
					foundLength = name.size();
				} else if (name.size() == foundLength) {
					found = nullptr;  // ambiguous
				}
			}
			return found;
		}

		void TryTakeOver()
		{
			const auto settings = Settings::Current();
			auto*      data = RE::MessageBoxMenu::GetCurrentMessageBoxData();
			if (!data || !data->callback || !IsFastTravelConfirm(data->callback.get())) {
				return;  // some other message box
			}
			const std::string body = data->bodyText.c_str() ? data->bodyText.c_str() : "";
			logs::info("Fast-travel confirmation box opened: \"{}\"", body);

			if (!settings->enabled || !settings->showPanel) {
				logs::info("  Panel switched off in the INI: the game's box stays");
				return;
			}
			if (!g_mapOpen || g_mapFromConversation) {
				logs::info("  Map was opened from a conversation (or is not open): the game's box stays");
				return;
			}
			if (!Link::Get()) {
				logs::info("  {}: the game's box stays", Link::Describe());
				return;
			}
			if (!Panel::IsAvailable()) {
				logs::info("  SKSE Menu Framework is not available: the game's box stays");
				return;
			}

			const auto ui = RE::UI::GetSingleton();
			const auto map = ui ? ui->GetMenu<RE::MapMenu>() : RE::GPtr<RE::MapMenu>{};
			const auto mapData = map ? map->GetRuntimeData2() : nullptr;
			if (!mapData) {
				logs::info("  Map data not available: the game's box stays");
				return;
			}
			const auto marker = FindMarker(*mapData, body);
			const auto ref = marker ? RE::TESObjectREFR::LookupByHandle(marker->ref) : RE::NiPointer<RE::TESObjectREFR>{};
			if (!marker || !ref) {
				logs::warn("  Could not tell which marker the box is about: the game's box stays");
				return;
			}

			Box box;
			for (const auto& text : data->buttonText) {
				box.buttons.emplace_back(text.c_str() ? text.c_str() : "");
			}
			const int count = static_cast<int>(box.buttons.size());
			box.cancel = (data->cancelButtonIndex >= 0 && data->cancelButtonIndex < count) ? data->cancelButtonIndex : (count >= 2 ? 1 : -1);
			box.yes = box.cancel == 0 ? 1 : 0;
			box.offset = data->buttonPressOffset;
			std::string list;
			for (int i = 0; i < count; ++i) {
				list += std::format("{}[{}] \"{}\"", i == 0 ? "" : ", ", i, box.buttons[static_cast<std::size_t>(i)]);
			}
			logs::info("  Buttons: {} (cancel index {}, press offset {}, warning type {})", list, data->cancelButtonIndex, box.offset, data->warningType);
			if (count < 2 || box.cancel < 0 || box.yes >= count) {
				logs::warn("  Unexpected buttons: the game's box stays");
				return;
			}
			for (int i = 0; i < count; ++i) {
				if (i != box.yes && i != box.cancel) {
					box.panel.extraButtons.emplace_back(i, box.buttons[static_cast<std::size_t>(i)]);
				}
			}

			box.callback = data->callback;
			box.destinationPos = ref->GetPosition();
			box.panel.open = true;
			box.panel.destination = std::string(marker->fullName && marker->fullName->GetFullName() ? marker->fullName->GetFullName() : "");
			box.panel.weatherOptions = settings->weatherOptions;
			box.panel.minHoursForFaster = settings->minHoursForFaster;
			if (const auto calendar = RE::Calendar::GetSingleton()) {
				box.panel.nowHour = calendar->GetHour();
			}
			Link::Get()->GetTripTo(ref.get(), &box.panel.trip);
			const auto& trip = box.panel.trip;
			logs::info("  Destination \"{}\": estimate {} ({}){}", box.panel.destination,
				trip.valid ? Hours(trip.hours) : "none"s, trip.calibrated ? "calibrated" : "starting guess",
				trip.otherWorldspace ? ", in another worldspace" : "");

			// Close the game's box: take its data out of the queue (this destroys the data; the
			// callback lives on in box.callback) and hide the menu if nothing else is queued.
			RE::MessageBoxMenu::RemoveMessageFromQueue(data);
			data = nullptr;
			if (!RE::MessageBoxMenu::GetCurrentMessageBoxData()) {
				if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
					queue->AddMessage(RE::MessageBoxMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kHide, nullptr);
				}
			}

			{
				std::lock_guard guard{ g_lock };
				box.panel.serial = ++g_serial;
				g_box = std::move(box);
			}
			g_timelineTicks = 0;  // log timestamps start again at the next Proceed
			g_actionQueued = false;
			Panel::Open();
			logs::info("  Arrival panel opened in place of the game's box");
		}

		// ---- Carrying out the panel's choice (UI thread)

		void PrepareTrip(const Box& a_box, const Choice& a_choice)
		{
			const auto settings = Settings::Current();
			const auto& info = a_box.panel.trip;
			const double estimate = info.hours;

			Trip trip;
			trip.destination = a_box.panel.destination;
			trip.timing = a_choice.timing;
			trip.weather = settings->weatherOptions ? a_choice.weather : Weather::Choice::kNatural;
			trip.estimateHours = estimate;
			trip.calibrated = info.calibrated;
			trip.targetHours = estimate;
			trip.originPos = PlayerPos();
			trip.destinationPos = a_box.destinationPos;
			trip.startDaysPassed = DaysPassed();
			trip.proceededAt = Clock::now();

			const bool hasEstimate = info.valid && estimate > 0.0;
			const bool fasterAllowed = hasEstimate && (settings->minHoursForFaster <= 0.0f || estimate >= settings->minHoursForFaster);
			double     factor = 1.0;
			switch (trip.timing) {
			case Timing::kInstant:
				if (fasterAllowed) {
					factor = INSTANT_FACTOR;
					trip.targetHours = 0.0;
				}
				break;
			case Timing::kThird:
				if (fasterAllowed) {
					factor = 3.0;
					trip.targetHours = estimate / 3.0;
				}
				break;
			case Timing::kHalf:
				if (fasterAllowed) {
					factor = 2.0;
					trip.targetHours = estimate / 2.0;
				}
				break;
			case Timing::kLater:
				if (hasEstimate && a_choice.laterHours > estimate && a_choice.laterHours <= estimate + MAX_LATER_DAYS * 24.0 + 1.0) {
					trip.targetHours = a_choice.laterHours;
					if (settings->laterByClock) {
						// A normal trip, then the clock moves forward to the exact time. Prototype 0.0.2
						// stretched the trip itself, but the game's travel time differs from FTTI's
						// estimate by up to ~15% on some routes, which over days becomes many hours.
						trip.clockTopUp = true;
					} else {
						factor = estimate / a_choice.laterHours;
					}
				}
				break;
			default:
				break;
			}
			if (trip.timing != Timing::kNormal && factor == 1.0 && !trip.clockTopUp) {
				logs::warn("{}{} is not possible for this trip; travelling normally", Stamp(), TimingName(trip.timing));
				trip.timing = Timing::kNormal;
			}

			if (factor != 1.0) {
				if (const auto setting = SpeedSetting()) {
					trip.originalMult = setting->GetFloat();
					trip.setMult = std::clamp(static_cast<float>(trip.originalMult * factor), MIN_MULT, MAX_MULT);
					setting->data.f = trip.setMult;
					trip.speedChanged = true;
					if (const auto iface = Link::Get()) {
						iface->ExcludeNextTrip();  // FTTI must not learn from this trip
					}
				} else {
					logs::error("{}fFastTravelSpeedMult not found; travelling normally", Stamp());
					trip.timing = Timing::kNormal;
					trip.targetHours = estimate;
				}
			}

			logs::info("{}Proceed to \"{}\": timing {}, weather {}", Stamp(), trip.destination, TimingName(trip.timing), Weather::Name(trip.weather));
			logs::info("{}  FTTI estimate {} ({}), target {}{}{}", Stamp(), Hours(estimate), trip.calibrated ? "calibrated" : "starting guess",
				Hours(trip.targetHours), trip.speedChanged ? std::format(", fFastTravelSpeedMult {} -> {}", trip.originalMult, trip.setMult) : ""s,
				trip.clockTopUp ? ", normal trip then the clock moves forward" : "");

			std::lock_guard guard{ g_lock };
			if (g_trip) {
				RestoreSpeed(*g_trip, "replaced by a new trip");
			}
			g_trip = std::move(trip);
			g_arrivalJob.reset();
		}

		void Execute(const Choice& a_choice)
		{
			std::optional<Box> box;
			{
				std::lock_guard guard{ g_lock };
				if (!g_box || g_box->panel.serial != a_choice.serial) {
					logs::info("Panel choice ignored: the panel it came from is gone");
					return;
				}
				box = std::move(g_box);
				g_box.reset();
			}
			switch (a_choice.action) {
			case Choice::Action::kProceed:
				g_timelineTicks = NowTicks();
				PrepareTrip(*box, a_choice);
				RunButton(*box, box->yes, "Proceed");
				break;
			case Choice::Action::kOther:
				RunButton(*box, a_choice.otherIndex, "Other button");
				break;
			default:
				RunButton(*box, box->cancel, "Cancel");
				break;
			}
		}

		// A choice waiting for SKSE Menu Framework to let go of the screen (UI thread). Checked
		// once per map frame by the map hook below, never by a task that queues itself again.
		std::optional<Choice> g_pendingChoice;
		int                   g_pendingFrames{ 0 };
		constexpr int         MAX_PENDING_FRAMES = 120;  // about two seconds

		void ExecuteOrWait(const Choice& a_choice)
		{
			if (Panel::IsReleased()) {
				Execute(a_choice);
				return;
			}
			logs::info("Waiting for SKSE Menu Framework to let go of the screen before carrying out the choice");
			std::lock_guard guard{ g_lock };
			g_pendingChoice = a_choice;
			g_pendingFrames = 0;
		}

		void DropBox(std::string_view a_why)
		{
			std::optional<Box> box;
			{
				std::lock_guard guard{ g_lock };
				box = std::move(g_box);
				g_box.reset();
			}
			if (box) {
				logs::info("Arrival panel closed ({})", a_why);
			}
			Panel::Close();
		}

		// ---- Arrival (main thread / UI thread)

		bool Arrived(const RE::NiPoint3& a_origin, const RE::NiPoint3& a_destination)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetParentCell()) {
				return false;
			}
			const auto pos = player->GetPosition();
			const auto toDestination = pos.GetDistance(a_destination);
			return toDestination < ARRIVAL_RADIUS && toDestination <= pos.GetDistance(a_origin);
		}

		void LogState(std::string_view a_label)
		{
			const auto c = RE::Calendar::GetSingleton();
			if (!c || !c->gameHour || !c->gameDay || !c->gameMonth || !c->gameYear || !c->gameDaysPassed) {
				return;
			}
			std::array<char, 128> text{};
			c->GetTimeDateString(text.data(), static_cast<std::uint32_t>(text.size()), true);
			logs::info("[State: {}] {} | hour {:.3f}, day {}, month {}, year {}, days passed {:.5f} (raw {:.5f}, midnights {}) | weather {}",
				a_label, text.data(), c->gameHour->value, c->gameDay->value, c->gameMonth->value, c->gameYear->value,
				c->gameDaysPassed->value, c->rawDaysPassed, c->midnightsPassed, Weather::Describe(Weather::Current()));
		}

		// Moves the game clock forward, the way time passes naturally: hour, day, month and year
		// roll over together, and the day counter moves by the midnights crossed. Never backwards.
		//
		// What the game keeps (learned from prototype 0.0.3's logs): Calendar::rawDaysPassed holds
		// whole days only, and every frame the game sets GameDaysPassed = rawDaysPassed + GameHour/24.
		// 0.0.3 wrote GameDaysPassed alone, so the game put it straight back: the date moved but the
		// day counter (and the weekday) did not. 0.0.4 moves rawDaysPassed by the midnights crossed.
		std::string AdvanceClock(double a_hours)
		{
			const auto c = RE::Calendar::GetSingleton();
			if (!c || !c->gameHour || !c->gameDay || !c->gameMonth || !c->gameYear || !c->gameDaysPassed) {
				return "not moved: calendar not available";
			}
			if (!(a_hours > 0.0)) {
				return "not moved: nothing to add";
			}
			LogState("before moving the clock");

			const double daysPassed = c->gameDaysPassed->value;
			const double raw = c->rawDaysPassed;
			const bool   rawIsWholeDays = std::abs(raw - std::floor(daysPassed)) < 0.001;

			double        hour = static_cast<double>(c->gameHour->value) + a_hours;
			int           day = static_cast<int>(std::lround(c->gameDay->value));
			int           month = static_cast<int>(std::lround(c->gameMonth->value));
			int           year = static_cast<int>(std::lround(c->gameYear->value));
			std::uint32_t midnights = 0;
			while (hour >= 24.0) {
				hour -= 24.0;
				++midnights;
				if (++day > RE::Calendar::DAYS_IN_MONTH[((month % 12) + 12) % 12]) {
					day = 1;
					if (++month >= 12) {
						month = 0;
						++year;
					}
				}
			}
			// Later arrivals are whole hours: snap to the minute so the game shows 6:00, not 5:59.
			const double minutes = hour * 60.0;
			if (const double snapped = std::round(minutes); std::abs(minutes - snapped) < 0.6) {
				hour = (std::min)(snapped / 60.0 + 1.0e-5, 23.99999);
			}

			if (rawIsWholeDays) {
				c->rawDaysPassed = static_cast<float>(raw + midnights);
				c->gameDaysPassed->value = static_cast<float>(raw + midnights + hour / 24.0);
			} else {
				logs::warn("{}The game's day counter does not look as expected (raw {}, days passed {}); moving days passed only", Stamp(), raw, daysPassed);
				c->gameDaysPassed->value = static_cast<float>(daysPassed + (hour - c->gameHour->value) / 24.0 + midnights);
			}
			c->gameHour->value = static_cast<float>(hour);
			c->gameDay->value = static_cast<float>(day);
			c->gameMonth->value = static_cast<float>(month);
			c->gameYear->value = static_cast<float>(year);

			LogState("after moving the clock");
			return std::format("moved forward {} ({} midnight(s){})", Hours(a_hours), midnights,
				rawIsWholeDays ? "" : "; day counter not as expected");
		}

		// Runs once the loading screen has closed: moves the clock (Later) and sets the weather.
		void RunArrivalJob()
		{
			const auto                settings = Settings::Current();
			std::optional<ArrivalJob> job;
			{
				std::lock_guard guard{ g_lock };
				job = std::move(g_arrivalJob);
				g_arrivalJob.reset();
			}
			if (!job) {
				return;
			}
			if (!Arrived(job->originPos, job->destinationPos)) {
				logs::warn("{}The player does not seem to be at the destination; continuing anyway", Stamp());
			}

			std::lock_guard guard{ g_lock };
			if (job->clockHours > 0.0) {
				const auto result = AdvanceClock(job->clockHours);
				const auto finalDifference = (static_cast<double>(DaysPassed()) - job->startDaysPassed) * 24.0 - job->targetHours;
				logs::info("{}Clock {}; arrival now differs from the target by {}", Stamp(), result, Hours(finalDifference));
				g_lastTrip.push_back(std::format("Clock {}", result));
				g_lastTrip.push_back(std::format("Arrival vs target after that: {}", Hours(finalDifference)));
			}

			if (job->weather != Weather::Choice::kNatural) {
				if (settings->logWeathers) {
					Weather::LogRegionsHere();
				}
				const auto pick = Weather::PickHere(job->weather);
				if (!pick.weather) {
					logs::info("{}Weather {} not applied: {}", Stamp(), Weather::Name(job->weather), pick.reason);
					g_lastTrip.push_back(std::format("Weather {}: not applied - {}", Weather::Name(job->weather), pick.reason));
				} else {
					Weather::Apply(pick.weather);
					logs::info("{}Weather {} applied: {} {}; current weather now {}", Stamp(), Weather::Name(job->weather),
						Weather::Describe(pick.weather), pick.reason, Weather::Describe(Weather::Current()));
					g_lastTrip.push_back(std::format("Weather {}: applied - {}", Weather::Name(job->weather), Weather::Describe(pick.weather)));
				}
			}

			LogState("on arrival");

			// Prototypes 0.0.3 and 0.0.4 also checked the clock and weather again 3, 10 and 30 s
			// later, using a game task that queued itself again until then. The game ran that
			// task over and over without pausing, which held up one of its own worker threads
			// for 30 s: the blank screen after the loading screen. Those logs showed the clock
			// and weather never changed afterwards, so the checks are gone.
		}

		void OnFastTravelEnd(float a_reportedHours)
		{
			std::optional<Trip> ended;
			{
				std::lock_guard guard{ g_lock };
				if (!g_trip) {
					return;
				}
				ended = std::move(g_trip);
				g_trip.reset();
			}
			auto&        trip = *ended;
			const double calendarHours = (static_cast<double>(DaysPassed()) - trip.startDaysPassed) * 24.0;
			const double difference = calendarHours - trip.targetHours;

			std::lock_guard guard{ g_lock };
			RestoreSpeed(trip, "trip ended");
			logs::info("{}Fast travel ended: game reports {}, calendar moved {}; target {} -> difference {} ({})", Stamp(),
				Hours(a_reportedHours), Hours(calendarHours), Hours(trip.targetHours), Hours(difference), TimingName(trip.timing));

			g_lastTrip = {
				std::format("Destination: {}", trip.destination),
				std::format("Timing: {}, weather: {}", TimingName(trip.timing), Weather::Name(trip.weather)),
				std::format("FTTI estimate: {} ({})", Hours(trip.estimateHours), trip.calibrated ? "calibrated" : "starting guess"),
				std::format("Target: {}", Hours(trip.targetHours)),
				std::format("Game reported: {}; calendar moved: {}", Hours(a_reportedHours), Hours(calendarHours)),
				std::format("Difference from target after the trip itself: {}", Hours(difference)),
			};
			g_lastArrivalTicks = NowTicks();

			// Guard against a different fast travel (a carriage, say) ending a stale plan.
			const bool ours = trip.confirmed || Clock::now() - trip.proceededAt < TRIP_TIMEOUT;
			if (!ours) {
				logs::warn("{}This fast travel does not seem to be the planned one; clock and weather left alone", Stamp());
				g_lastTrip.push_back("Note: this fast travel did not seem to be the planned one");
				return;
			}
			ArrivalJob job;
			job.weather = trip.weather;
			job.targetHours = trip.targetHours;
			job.startDaysPassed = trip.startDaysPassed;
			job.originPos = trip.originPos;
			job.destinationPos = trip.destinationPos;
			if (trip.clockTopUp) {
				const double remaining = trip.targetHours - calendarHours;
				if (remaining > 1.0 / 60.0) {
					job.clockHours = remaining;
				} else {
					logs::info("{}Already at or past the target; the clock is not moved (never backwards)", Stamp());
					g_lastTrip.push_back("Clock: not moved - the trip itself already reached the target");
				}
			}
			if (job.weather == Weather::Choice::kNatural && job.clockHours <= 0.0) {
				return;
			}
			g_arrivalJob = job;
			if (!g_loadingOpen) {
				// The loading screen is already over (or never showed): do it now.
				SKSE::GetTaskInterface()->AddUITask([]() { RunArrivalJob(); });
			}
			// Otherwise it runs when the loading screen closes.
		}

		// ---- Event sinks

		class EventHandler final :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
			public RE::BSTEventSink<RE::TESFastTravelEndEvent>
		{
		public:
			static EventHandler* GetSingleton()
			{
				static EventHandler singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				try {
					const auto& name = a_event->menuName;
					if (name == RE::DialogueMenu::MENU_NAME) {
						g_dialogueOpen = a_event->opening;
						if (!a_event->opening) {
							g_dialogueClosedAt = Clock::now();
						}
					} else if (name == RE::MapMenu::MENU_NAME) {
						a_event->opening ? OnMapOpened() : OnMapClosed();
					} else if (name == RE::MessageBoxMenu::MENU_NAME) {
						if (a_event->opening && g_mapOpen) {
							// Let the box finish opening first, then take it over.
							SKSE::GetTaskInterface()->AddUITask([]() { TryTakeOver(); });
						}
					} else if (name == RE::LoadingMenu::MENU_NAME) {
						g_loadingOpen = a_event->opening;
						OnLoading(a_event->opening);
					}
				} catch (const std::exception& e) {
					logs::error("Menu event failed: {}", e.what());
				} catch (...) {
					logs::error("Menu event failed: unknown exception");
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent* a_event, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
			{
				if (a_event) {
					try {
						OnFastTravelEnd(a_event->fastTravelEndHours);
					} catch (const std::exception& e) {
						logs::error("Fast-travel end failed: {}", e.what());
					} catch (...) {
						logs::error("Fast-travel end failed: unknown exception");
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			static void OnMapOpened()
			{
				g_mapOpen = true;
				const auto settings = Settings::Current();
				const auto sinceDialogue = std::chrono::duration<double>(Clock::now() - g_dialogueClosedAt).count();
				g_mapFromConversation = settings->skipAfterConversation &&
				                        (g_dialogueOpen || sinceDialogue < static_cast<double>(settings->conversationSeconds));
				logs::info("Map opened{}", g_mapFromConversation ? " from a conversation: the planner stays out of it this time" : "");

				// A planned trip still waiting here means no fast travel followed it. Put the
				// setting back before Fast Travel Time Info shows any estimates on this map.
				std::lock_guard guard{ g_lock };
				if (g_trip) {
					RestoreSpeed(*g_trip, "no fast travel followed the last Proceed");
					g_trip.reset();
				}
			}

			static void OnMapClosed()
			{
				g_mapOpen = false;
				{
					std::lock_guard guard{ g_lock };
					g_pendingChoice.reset();
				}
				DropBox("the map closed");

				bool confirmed = false;
				if (const auto ui = RE::UI::GetSingleton()) {
					if (const auto map = ui->GetMenu<RE::MapMenu>()) {
						if (const auto data = map->GetRuntimeData2()) {
							confirmed = data->fastTravelConfirmed;
						}
					}
				}
				std::lock_guard guard{ g_lock };
				if (!g_trip) {
					return;
				}
				if (confirmed) {
					g_trip->confirmed = true;
				}
				// The game works out the travel time only after the map has closed, so the setting
				// must stay changed until the trip ends. Prototype 0.0.1 restored it here whenever
				// it could not read the confirmation at this moment, which undid every plan.
				// If no trip follows, the setting is restored the next time the map opens, or
				// when a game is loaded.
				logs::info("{}Map closed ({}); keeping fFastTravelSpeedMult until the trip ends", Stamp(),
					g_trip->confirmed ? "fast travel confirmed" : "confirmation not seen");
			}

			static void OnLoading(bool a_opening)
			{
				const auto arrived = g_lastArrivalTicks.load();
				const bool recent = arrived != 0 && SecondsSince(arrived) < 60.0;
				bool       travelling = false;
				{
					std::lock_guard guard{ g_lock };
					travelling = g_trip.has_value();
				}
				// Every loading screen's length is logged, planned trip or not, so trips with the
				// panel switched off (bShowPanel = 0) can be compared with planned ones.
				const auto stamp = g_timelineTicks.load() != 0 && (recent || travelling) ? Stamp() : std::string{};
				if (a_opening) {
					g_loadingOpenedTicks = NowTicks();
					logs::info("{}Loading screen opened", stamp);
				} else if (const auto opened = g_loadingOpenedTicks.exchange(0); opened != 0) {
					logs::info("{}Loading screen closed after {:.1f} s{}", stamp, SecondsSince(opened), travelling || recent ? "" : " (not a planned trip)");
				} else {
					logs::info("{}Loading screen closed", stamp);
				}
				if (a_opening) {
					return;
				}
				bool hasJob = false;
				{
					std::lock_guard guard{ g_lock };
					hasJob = g_arrivalJob.has_value();
				}
				if (hasJob) {
					RunArrivalJob();
				} else if (recent && Settings::Current()->logWeathers) {
					Weather::LogRegionsHere();  // prototype test 7: record each region's weathers
				}
			}
		};

		// ---- Map hook: notices a panel that was closed without one of its buttons (UI thread)

		struct AdvanceMovieHook
		{
			static void thunk(RE::MapMenu* a_this, float a_interval, std::uint32_t a_currentTime)
			{
				func(a_this, a_interval, a_currentTime);
				try {
					// Note the game's own confirmation while the map is still open (it is not
					// reliably readable any more once the map has started closing).
					if (const auto data = a_this ? a_this->GetRuntimeData2() : nullptr; data && data->fastTravelConfirmed) {
						std::lock_guard guard{ g_lock };
						if (g_trip && !g_trip->confirmed) {
							g_trip->confirmed = true;
							logs::info("{}The game confirmed the fast travel", Stamp());
						}
					}

					std::optional<Choice> ready;
					{
						std::lock_guard guard{ g_lock };
						if (g_pendingChoice && (Panel::IsReleased() || ++g_pendingFrames >= MAX_PENDING_FRAMES)) {
							ready = g_pendingChoice;
							g_pendingChoice.reset();
							logs::info("SKSE Menu Framework let go after {} map frame(s){}", g_pendingFrames,
								g_pendingFrames >= MAX_PENDING_FRAMES ? " (stopped waiting)" : "");
						}
					}
					if (ready) {
						Execute(*ready);
					}

					bool orphaned = false;
					std::uint64_t serial = 0;
					{
						std::lock_guard guard{ g_lock };
						orphaned = g_box && !g_actionQueued && !Panel::IsOpen();
						if (orphaned) {
							serial = g_box->panel.serial;
						}
					}
					if (orphaned) {
						logs::info("The panel was closed without a button: treated as Cancel");
						Choice cancel;
						cancel.action = Choice::Action::kCancel;
						cancel.serial = serial;
						Execute(cancel);
					}
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	PanelData GetPanelData()
	{
		std::lock_guard guard{ g_lock };
		return g_box ? g_box->panel : PanelData{};
	}

	void Submit(const Choice& a_choice)
	{
		if (g_actionQueued.exchange(true)) {
			return;  // one choice per panel
		}
		Panel::Close();
		SKSE::GetTaskInterface()->AddUITask([a_choice]() {
			try {
				ExecuteOrWait(a_choice);
			} catch (const std::exception& e) {
				logs::error("Carrying out the panel choice failed: {}", e.what());
			} catch (...) {
				logs::error("Carrying out the panel choice failed: unknown exception");
			}
		});
	}

	std::vector<std::string> LastTripLines()
	{
		std::lock_guard guard{ g_lock };
		return g_lastTrip;
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			return;
		}
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MapMenu[0] };
		AdvanceMovieHook::func = vtable.write_vfunc(0x5, AdvanceMovieHook::thunk);

		const auto handler = EventHandler::GetSingleton();
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(handler);
		}
		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			if (const auto source = holder->GetEventSource<RE::TESFastTravelEndEvent>()) {
				source->AddEventSink(handler);
			}
		}
		logs::info("Listening for the map, its confirmation box, loading screens and fast-travel ends");
	}

	void OnGameLoad()
	{
		DropBox("a game is loading");
		std::lock_guard guard{ g_lock };
		g_pendingChoice.reset();
		if (g_trip) {
			RestoreSpeed(*g_trip, "a game is loading");
			g_trip.reset();
		}
		g_arrivalJob.reset();
	}
}
