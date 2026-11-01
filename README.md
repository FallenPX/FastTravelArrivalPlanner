# Fast Travel Arrival Planner (prototype)

An SKSE plugin for Skyrim Special Edition / Anniversary Edition and an add-on for **Fast Travel Time Info**. When you click a destination on the world map, an arrival panel opens in place of the game's "Fast travel to ...?" box. There you can choose:

- **When you arrive:** Instant, 1/3 or 1/2 of the normal travel time, the normal ETA, or later (whole hours, up to 30 days after the normal arrival).
- **The weather on arrival:** Natural, Clear, Cloudy, Rain or Snow.

Time only ever moves forward. Proceed with untouched settings is exactly a normal fast travel.

**This is a prototype.** It exists to answer the seven test questions below, and it writes every step to its log.

## Requirements

- SKSE64 matching your game version
- Address Library for SKSE Plugins (All in one)
- **Fast Travel Time Info 0.1.4 or newer**
- **SKSE Menu Framework**

Without Fast Travel Time Info 0.1.4+ or SKSE Menu Framework, the planner stays inactive and the game's own box is used.

## How it works

- **Faster arrivals (Instant, 1/3, 1/2).** Just before the trip, the planner changes the game setting `fFastTravelSpeedMult`, so the game's own fast travel takes the chosen share of its normal time. The setting is put back when the trip ends. If no fast travel follows, it is put back the next time the map opens, or when a save is loaded. Changed game settings are never stored in save files, so a crash cannot leave it changed.
- **Later arrivals.** The trip itself is normal. When the loading screen closes, the clock moves forward to the chosen hour: hour, day, month, year and the days-passed counter move together, as when time passes naturally. Never backwards. (`bLaterByClock = 0` uses the 0.0.2 method instead.)
- **Weather** is picked from the destination region's own weather list, by the classification each weather record carries (pleasant, cloudy, rainy, snow). Weathers from DLCs, Creation Club content and weather mods are included automatically. Unclassified weathers, such as ash storms, are never picked but can still occur under Natural. The weather is not locked, so it changes naturally afterwards. Weather held by a quest or script is left alone.
- **Calibration.** Planned trips are not used for Fast Travel Time Info's calibration.
- **Other ways of travelling.** The planner only acts on the confirmation box the world map shows. Carriages and other travel are untouched. A map opened from a conversation is left alone too (`bSkipAfterConversation`).

## Prototype tests

Install the updated Fast Travel Time Info (0.1.4), this plugin and SKSE Menu Framework, then make a save you can return to. For each test, the **status page** shows the last planned trip: SKSE Menu Framework, then *Fast Travel Arrival Planner*, then *Status*. Full details are in `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelArrivalPlanner.log`.

1. **Do the timing options arrive on time?**
   - **1/2**, **1/3**: worked in 0.0.2 (within a minute).
   - **Later** (0.0.3): pick a day and an hour, for example 07:00 three days later. Check the in-game clock on arrival; the status page shows *Arrival vs target after that*.
   - Because Later now moves the clock directly, also check afterwards: the date and time in the Wait menu, that time keeps running normally, and that a shop or inn still keeps its usual hours. The log records the clock before and after it moves, and on arrival (lines starting with `[State`).
2. **Does Instant reach zero hours?** Choose **Instant** and note *Game reported* on the status page.
3. **Does the chosen weather stay?**
   - In 0.0.2 the weather was always applied after loading: the step queued during loading only ran once the loading screen had closed. 0.0.3 therefore applies it there.
   - The 0.0.3 and 0.0.4 logs showed the game never replaced the chosen weather in the first 30 seconds, so it is simply applied once on arrival.
4. **Do the two mods talk to each other?**
   - The status page's first line should say *Connected to Fast Travel Time Info 0.1.4*.
   - After a planned trip, Fast Travel Time Info's log should say *Trip planned by Fast Travel Arrival Planner: not used for calibration*.
5. **Does the planner stay out of other travel?**
   - Take a carriage: no panel should appear.
   - If you use a mod that opens the map from a conversation, check that no panel appears there either. The log says *Map opened from a conversation*.
6. **Does the panel replace the box properly?**
   - Click a destination. The panel should open instead of the box. Note whether the game's box flashes briefly first.
   - Check that the map cannot be clicked behind the panel.
   - Check that Proceed travels and that Cancel (and Escape) return to the map.
   - If your box has a Place Marker button, check that it still works.
   - If you have a controller, try using the panel with it.
7. **DLC worlds.**
   - On Solstheim, check that Fast Travel Time Info's estimates and the panel look right, and travel once with a weather choice.
   - Fast travel to and from the Forgotten Vale. Try once with `bShowPanel = 0` as well, because the unmodded game is known to hang on a darkened map there sometimes.
   - The log lists each region's weathers on lines starting with `[Weathers]`.

Please send back both log files: `FastTravelArrivalPlanner.log` and `FastTravelTimeInfo.log`.

## Getting the DLL

Exactly as for Fast Travel Time Info: double-click **`build.bat`** (option A in Fast Travel Time Info's README), or upload the project to GitHub and let the **Build plugin** action run (option B). Both produce `FastTravelArrivalPlanner-mod.zip`; install it with Vortex.

## Settings

`Data\SKSE\Plugins\FastTravelArrivalPlanner.ini`. In this prototype, the page in SKSE Menu Framework is a status page with a *Reload INI* button.

- **`bShowPanel = 0`** brings back the game's own box, in case another interface mod conflicts.
- **`fMinHoursForFaster`** sets the 1-hour threshold for Instant, 1/3 and 1/2. 0 removes the limit.

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (GPL-3.0-or-later with plugin exceptions)
- [spdlog](https://github.com/gabime/spdlog) (MIT), used through CommonLibSSE-NG for the log file
- [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by Thiago099 / QTR-Modding: its API header is included in `extern/SKSEMenuFramework` (GPL-3.0)

## Changelog

- **0.0.5**: Fix: the blank screen after the loading screen, and the longer loading. The checks 3, 10 and 30 seconds after arrival (added in 0.0.3) used a game task that queued itself again; the game ran it over and over without pausing, which held up one of its worker threads for the full 30 seconds. The checks are removed (their logs had shown nothing changes after arrival), and nothing in the planner repeats itself through the game's task queue any more.
- **0.0.4**:
  - Fix: Later moved the date but not the game's day counter, so the weekday went wrong and day-based timers did not advance. The game rebuilds that counter from whole days every frame, and 0.0.4 moves those whole days too. **Saves made after a Later trip with 0.0.3 have this mismatch; load a save from before it.**
  - Arrival shows 6:00 instead of 5:59.
  - Rain and Snow prefer weathers with visible rain or snow over ones with only a few flakes. The log lists each weather's precipitation density.
  - A chosen trip starts only once SKSE Menu Framework has let go of the screen.
  - Every loading screen's length is logged, so trips with the panel on and off can be compared.
- **0.0.3**: Later arrivals are exact: a normal trip, then the clock moves forward to the chosen hour (0.0.2 stretched the trip, which missed the hour by up to a day and a half on long trips, because the game's travel time differs from the estimate by up to ~15% on some routes). The chosen weather is held for the first 15 seconds after arrival. Clock and weather are logged 3, 10 and 30 seconds after arrival.
- **0.0.2**: Fix: the chosen timing and weather had no effect. 0.0.1 put `fFastTravelSpeedMult` back as soon as the map closed, before the game had worked out the travel time, and the weather step (which runs when the trip ends) was skipped with it. The setting now stays changed until the trip ends.
- **0.0.1**: Prototype for testing.
