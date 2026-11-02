# Fast Travel Arrival Planner

An SKSE plugin for Skyrim Special Edition / Anniversary Edition and an add-on for **Fast Travel Time Info**. When you click a destination on the world map, an arrival panel opens in place of the game's "Fast travel to ...?" box. There you choose **when you arrive**: instantly, in 1/3 or 1/2 of the normal travel time, at the normal ETA, or later, at a whole hour up to 30 days after the normal arrival.

Time only ever moves forward. Proceed with untouched settings is exactly a normal fast travel.

## Requirements

- SKSE64 matching your game version
- Address Library for SKSE Plugins (All in one)
- **Fast Travel Time Info**, preferably the latest version (0.1.4 is the oldest that works; from 0.2.0 the panel also follows its background settings)
- SKSE Menu Framework: optional. It adds an in-game settings page.

Without Fast Travel Time Info 0.1.4+, the planner stays inactive and the game's own box is used.

## The panel

The game's own interface draws the panel, as it draws the map: the panel takes the font, text sizes, colours and text shadow of the map's location flyout, from whatever interface mod draws that flyout, and Fast Travel Time Info's background settings, including none when its background is off. The cursor is the game's, so an interface mod's cursor stays. The panel's movie, `Interface\FastTravelArrivalPlanner.swf`, is an empty stage that ships with the mod; the plugin builds the panel on it when it opens.

- Mouse: click a timing choice, drag the Day and Hour sliders (or click the track), then Proceed, Place Marker or Cancel.
- Keyboard: Left and Right choose the timing, Up and Down the hour, Page Up and Page Down the day; Enter proceeds; Escape or Tab cancels.
- Controller: the panel also follows the game's own inputs (Accept, Cancel, Up, Down, Left, Right and the page inputs), so a controller should work, but this has not been tested yet.
- Instant, 1/3 and 1/2 need a travel time of at least `fMinHoursForFaster`; a line under the choices says so when they are greyed out.

## How it works

- **Faster arrivals (Instant, 1/3, 1/2).** Just before the trip, the planner changes the game setting `fFastTravelSpeedMult`, so the game's own fast travel takes the chosen share of its normal time. The setting is put back when the trip ends. If no fast travel follows, it is put back the next time the map opens, or when a save is loaded. Changed game settings are never stored in save files, so a crash cannot leave it changed.
- **Later arrivals.** The trip itself is normal. When the loading screen closes, the clock moves forward to the chosen hour: hour, day, month, year and the game's day counter move together, as when time passes naturally. Never backwards.
- **Calibration.** Faster trips are not used for Fast Travel Time Info's calibration.
- **Other ways of travelling.** The planner only acts on the confirmation box the world map shows. Carriages and other travel are untouched. A map opened from a conversation is left alone too (`bSkipAfterConversation`).

## Reporting a problem

Please include both log files from `Documents\My Games\Skyrim Special Edition\SKSE`: `FastTravelArrivalPlanner.log` and `FastTravelTimeInfo.log`. By default the planner's log keeps a short summary of each trip. For a problem with the panel or a trip, switch on `bDetailedLog` (or *Detailed log* on the settings page) first and reproduce the problem: the log then records every step, and the keys and inputs the panel receives.

With SKSE Menu Framework installed, its *Fast Travel Arrival Planner* page also shows the last planned trip.

Tested on Skyrim Anniversary Edition 1.6.1170 with Dear Diary Dark Mode: all timing options (Instant at 0 minutes, 1/3 and 1/2 within a few minutes, Later to the minute, including into a new year), Proceed, Cancel and Place Marker, the mouse and the keyboard, Solstheim, the Forgotten Vale, Fort Dawnguard, and carriages (left alone). Not tested yet: a controller, and mods that open the map from a conversation.

## Getting the DLL

Exactly as for Fast Travel Time Info: double-click **`build.bat`** (option A in Fast Travel Time Info's README), or upload the project to GitHub and let the **Build plugin** action run (option B). Both produce `FastTravelArrivalPlanner-mod.zip` (the DLL, the default INI and `Interface\FastTravelArrivalPlanner.swf`); install it with Vortex. On GitHub the download is the build's *FastTravelArrivalPlanner-mod* artifact.

## Settings

Your settings live in `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelArrivalPlanner.ini`, so updates never replace them. The plugin creates that file from the defaults (`Data\SKSE\Plugins\FastTravelArrivalPlanner.ini`) the first time. After an update it copies in any option the new version adds, with its explanation, and takes out any option the new version no longer uses; everything else in the file stays as you set it, including your comments.

With SKSE Menu Framework installed, its *Fast Travel Arrival Planner* page changes the settings in the game. Each change takes effect at once and is saved to that same file. *Reload INI* reads the file again after you edit it by hand.

- **`bShowPanel`** (on by default): clicking a destination on the world map opens the arrival panel. 0 brings back the game's own box, in case another interface mod conflicts.
- **`fMinHoursForFaster`** (1 hour by default): Instant, 1/3 and 1/2 are offered only when the normal travel time is at least this many game hours, from 1 to 3. Shorter trips are quick enough as they are; a player who wants to walk a little more can raise it to 3. Travel times grow with the timescale: at the default timescale of 20, a trip takes 2.5 times as many game hours as at 8. The settings page slides from 1 to 3 hours in half hours. A value outside 1 to 3 in your settings file is replaced by the nearest one allowed, and the log says so.
- **`bSkipAfterConversation`** (on by default): a map opened from a conversation is left to the game. **`fConversationSeconds`** (INI only) sets how soon after a conversation a map still counts as opened from it.
- **`bDetailedLog`** (off by default): the log records every step of each trip as well; see *Reporting a problem*.

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (GPL-3.0-or-later with plugin exceptions)
- [spdlog](https://github.com/gabime/spdlog) (MIT), used through CommonLibSSE-NG for the log file
- [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by Thiago099 / QTR-Modding: its API header is included in `extern/SKSEMenuFramework` (GPL-3.0)

## Changelog

- **0.3.0**: First public release. The log is a short summary of each trip by default: the confirmation box, the choice, the target, what the game reported, and the clock move for Later. The new `bDetailedLog` (also on the settings page) brings back every step, the panel's inputs and its layout details for bug reports. The panel's look is logged once, and again only when it changes. Loading screens that do not belong to a planned trip (a carriage, a door) are no longer logged as part of the previous trip.
- **0.2.2**: The minimum travel time for Instant, 1/3 and 1/2 (`fMinHoursForFaster`) now ranges from 1 to 3 game hours, in half hours on the settings page; it used to range from no minimum to 48 hours. A value outside the range in your settings file is replaced by the nearest allowed one, written back to the file, and noted in the log. Generally, a setting that cannot be used as written is now corrected in the file, so the file always says what the game does.
- **0.2.1**: Tooltips on the settings page wrap at half the page's width, so the long one on the minimum travel time no longer runs off the screen. Settings file: the `[Panel]` section that 0.2.0 left behind in files from 0.0.10 or earlier, with two comment lines about the font, is now removed too. 0.2.0 had mistaken one of those comments (*Empty = the framework's font*) for an option and kept them; a comment now only counts as an option when it names a real one. Generally, a section no option of this version belongs to, and that holds no option any more, is removed with its leftover comments.
- **0.2.0**: The panel in the game's own interface is now the only panel: the SKSE Menu Framework panel is gone, with its settings `bGameInterface`, `bMatchMapStyle` and `sFont`. The master switch `bEnabled` is merged into `bShowPanel`, which did the same; if you had switched the planner off with it, the panel stays off. These options are taken out of your settings file automatically, with their comments and the then empty `[Panel]` section; everything else stays as you set it. SKSE Menu Framework's page is now a settings page: the panel on or off, the minimum travel time for Instant, 1/3 and 1/2, and maps opened from a conversation, each saved to your settings file at once; the last planned trip and *Reload INI* stay.
- **0.1.1**: Keyboard and controller: the panel also follows the game's own inputs (Accept, Cancel, Up, Down, Left, Right and the page inputs), and a press that arrives both as a key and as an input counts once. Numpad Enter proceeds. When the flyout has no rows to copy (the Forgotten Vale, for one), the panel's rows keep the look of an earlier flyout instead of the title's font. The log lists each key and input once per panel with what it did.
- **0.1.0**: The panel is now drawn by the game's own interface, like the map: sharp text in the flyout's own font, sizes, colours and text shadow (copied from the flyout as the game draws it), Fast Travel Time Info's background, and the game's cursor, so an interface mod's cursor stays. Keyboard: Left/Right for the timing, Up/Down for the hour, Page Up/Page Down for the day, Enter to proceed, Escape or Tab to cancel. When Instant, 1/3 and 1/2 are greyed out, a line under them says why. SKSE Menu Framework is now optional: `bGameInterface = 0` (new, in `[Panel]`) brings back its panel, and it still provides the status page. The mod now includes `Interface\FastTravelArrivalPlanner.swf`.
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
