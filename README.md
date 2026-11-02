# Fast Travel Arrival Planner (prototype)

An SKSE plugin for Skyrim Special Edition / Anniversary Edition and an add-on for **Fast Travel Time Info**. When you click a destination on the world map, an arrival panel opens in place of the game's "Fast travel to ...?" box. There you choose **when you arrive**: instantly, in 1/3 or 1/2 of the normal travel time, at the normal ETA, or later, at a whole hour up to 30 days after the normal arrival.

Time only ever moves forward. Proceed with untouched settings is exactly a normal fast travel.

**This is a prototype.** It writes every step to its log for testing.

## Requirements

- SKSE64 matching your game version
- Address Library for SKSE Plugins (All in one)
- **Fast Travel Time Info 0.1.4 or newer** (0.2.0 or newer recommended: the panel then matches the map's flyout)
- **SKSE Menu Framework**

Without Fast Travel Time Info 0.1.4+ or SKSE Menu Framework, the planner stays inactive and the game's own box is used.

## How it works

- **Faster arrivals (Instant, 1/3, 1/2).** Just before the trip, the planner changes the game setting `fFastTravelSpeedMult`, so the game's own fast travel takes the chosen share of its normal time. The setting is put back when the trip ends. If no fast travel follows, it is put back the next time the map opens, or when a save is loaded. Changed game settings are never stored in save files, so a crash cannot leave it changed.
- **Later arrivals.** The trip itself is normal. When the loading screen closes, the clock moves forward to the chosen hour: hour, day, month, year and the game's day counter move together, as when time passes naturally. Never backwards.
- **Calibration.** Faster trips are not used for Fast Travel Time Info's calibration.
- **Other ways of travelling.** The planner only acts on the confirmation box the world map shows. Carriages and other travel are untouched. A map opened from a conversation is left alone too (`bSkipAfterConversation`).

## Prototype tests

For each trip, the **status page** shows the last planned trip: SKSE Menu Framework, then *Fast Travel Arrival Planner*, then *Status*. Full details are in `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelArrivalPlanner.log`.

Already confirmed: all timing options arrive on time (Instant at 0 minutes, 1/3 and 1/2 within a few minutes, Later to the minute, including into a new year); the two mods talk to each other; the panel replaces the box, and Proceed, Cancel and Place Marker work; Solstheim works; trips to and from the Forgotten Vale and Fort Dawnguard work, with the panel on and off; loading times are normal.

Still open:

1. **Does the planner stay out of other travel?**
   - Take a carriage: no panel should appear.
   - If you use a mod that opens the map from a conversation, check that no panel appears there either. The log says *Map opened from a conversation*.
2. **Panel details.**
   - Does the game's own box ever flash briefly before the panel?
   - Does Escape close the panel and return to the map?
   - If you have a controller, can you use the panel with it?
3. **Fort Dawnguard with Fast Travel Time Info 0.1.9.** Trips into and out of it now have an approximate estimate, so the panel offers every option there. Try 1/3 or Later once.

Please send back both log files: `FastTravelArrivalPlanner.log` and `FastTravelTimeInfo.log`.

## Getting the DLL

Exactly as for Fast Travel Time Info: double-click **`build.bat`** (option A in Fast Travel Time Info's README), or upload the project to GitHub and let the **Build plugin** action run (option B). Both produce `FastTravelArrivalPlanner-mod.zip`; install it with Vortex.

## Settings

Your settings live in `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelArrivalPlanner.ini`, so updates never replace them. The plugin creates that file from the defaults (`Data\SKSE\Plugins\FastTravelArrivalPlanner.ini`) the first time, and copies in any option a newer version adds. In this prototype, the page in SKSE Menu Framework is a status page with a *Reload INI* button.

- **`bShowPanel = 0`** brings back the game's own box, in case another interface mod conflicts.
- **`fMinHoursForFaster`** sets the 1-hour threshold for Instant, 1/3 and 1/2. 0 removes the limit.
- **`bMatchMapStyle`** (on by default): the panel looks like the map's location flyout, whatever interface mod draws it: colours, font, text sizes and text shadow are read from the flyout while the game runs, and the background is exactly Fast Travel Time Info's: same opacity and corners, or none when its background is off. The font is used only if SKSE Menu Framework has it. 0 uses SKSE Menu Framework's own theme.
- **`sFont`**: a font by the name SKSE Menu Framework lists in its own settings. Empty means the flyout's font (if the framework has it), otherwise the framework's font.

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (GPL-3.0-or-later with plugin exceptions)
- [spdlog](https://github.com/gabime/spdlog) (MIT), used through CommonLibSSE-NG for the log file
- [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by Thiago099 / QTR-Modding: its API header is included in `extern/SKSEMenuFramework` (GPL-3.0)

## Changelog

- **0.0.14**: The panel uses the flyout's font only if SKSE Menu Framework really hands back that font (some versions hand back their own instead), and the log names the font actually used.
- **0.0.13**: With Fast Travel Time Info 0.3.2 the panel's text matches the flyout's size (the heading as large as the location name). When the flyout's shadow cannot be read, the panel draws one like the vanilla and Dear Diary flyouts: black, 2 map pixels down and to the right, with a soft edge.
- **0.0.12**: With Fast Travel Time Info 0.3.1: text sizes that cannot be right are ignored instead of shrinking the panel, the text shadow is only left out when the flyout really has none, and the log line *Panel sizes* shows how the sizes were worked out.
- **0.0.11**: The panel's background is exactly Fast Travel Time Info's, including none when its background is off (0.0.8 to 0.0.10 kept at least half opacity, and 75% without one). Only tooltips keep a background.
- **0.0.10**: The panel follows the flyout's font, text sizes (the heading as large as the location name, the rest as large as the flyout's rows) and text shadow, as well as its colours, so it matches whatever interface mod draws the map (needs Fast Travel Time Info 0.3.0 or newer). The log line *Panel look:* shows what was used.
- **0.0.9**: The old Later method (`bLaterByClock = 0`, a stretched trip that could miss the hour by many hours) is gone: Later is always a normal trip, then the clock moves forward to the exact hour. The option is removed from your settings file automatically, with its comment lines.
- **0.0.8**:
  - The panel matches the map's location flyout: no title bar, the flyout's colours and diamond bullets, Fast Travel Time Info's background and corners, and the timing choices as words that light up instead of round buttons. The heading reads "Travel to ..." (with Fast Travel Time Info 0.2.0 or newer; with older versions it uses plain colours).
  - Settings move to your own file in `Documents\My Games\Skyrim Special Edition\SKSE`, which updates never replace. **Updating from 0.0.7:** if you changed anything in `Data\SKSE\Plugins\FastTravelArrivalPlanner.ini` (such as `bShowPanel`), set it again in the new file after the first start.
  - New options `bMatchMapStyle` and `sFont`.
  - Clicks and keys are ignored for the first 0.3 seconds after the panel opens, so the second click of a double-click cannot press a button. The log names what cancelled the panel (button, Escape or controller).
- **0.0.7**: Wording only, for Fast Travel Time Info 0.1.9, which estimates trips into and out of areas such as Fort Dawnguard from their map positions. Such estimates are approximate, so the panel now says "The travel time is approximate" instead of naming FTTI's starting guess. The log no longer reports a "difference" for trips without an estimate.
- **0.0.6**: The weather options are removed; the planner is about arrival time only. (A weather of choice is better served by tools made for it, and doing it well would need each destination's weather list before travelling.)
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
