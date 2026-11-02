#include "pch.h"

#include "Settings.h"

// Settings are read from the player's own file, which lives outside the mod's folder so that
// installing an update never replaces it. The INI that comes with the mod holds the defaults
// and is the template for that file; options a newer version adds are merged in. The INI
// handling is the same as Fast Travel Time Info's.

namespace FTAP
{
	namespace
	{
		constexpr auto DEFAULTS_PATH = "Data/SKSE/Plugins/FastTravelArrivalPlanner.ini"sv;
		constexpr auto USER_FILE_NAME = "FastTravelArrivalPlanner.ini"sv;
		// Lines starting with this describe only the defaults file and are not copied.
		constexpr auto DEFAULTS_ONLY_MARK = ";!"sv;

		std::mutex                      g_lock;
		std::shared_ptr<const Settings> g_current = std::make_shared<const Settings>();
		std::atomic<std::uint32_t>      g_version{ 1 };

		std::string Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r\n");
			return std::string(a_str.substr(first, last - first + 1));
		}

		std::string Lower(std::string a_str)
		{
			std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_str;
		}

		// Values may be wrapped in double quotes so that leading/trailing spaces survive,
		// e.g. sMonthJoiner = " of ".
		std::string Unquote(std::string a_str)
		{
			if (a_str.size() >= 2 && a_str.front() == '"' && a_str.back() == '"') {
				return a_str.substr(1, a_str.size() - 2);
			}
			return a_str;
		}

		// Position of a ';' or '#' comment that is not inside double quotes, or npos.
		std::size_t FindComment(std::string_view a_line, std::size_t a_from = 0)
		{
			bool inQuotes = false;
			for (std::size_t i = a_from; i < a_line.size(); ++i) {
				if (a_line[i] == '"') {
					inQuotes = !inQuotes;
				} else if (!inQuotes && (a_line[i] == ';' || a_line[i] == '#')) {
					return i;
				}
			}
			return std::string_view::npos;
		}

		bool ToBool(const std::string& a_value, bool a_default)
		{
			const auto v = Lower(a_value);
			if (v == "1" || v == "true" || v == "yes" || v == "on") {
				return true;
			}
			if (v == "0" || v == "false" || v == "no" || v == "off") {
				return false;
			}
			return a_default;
		}

		float ToFloat(const std::string& a_value, float a_default)
		{
			try {
				return std::stof(a_value);
			} catch (...) {
				return a_default;
			}
		}

		// One INI key: where it lives, how to read it and how to write it.
		struct Field
		{
			std::string_view                                    name;     // as written in the INI
			std::string_view                                    section;  // without brackets
			std::function<void(Settings&, const std::string&)> parse;    // raw value, quotes already removed
			std::function<std::string(const Settings&)>        format;   // value as it should be written
			std::function<bool(const std::string&)>            fits;     // readable, and within the allowed range
		};

		Field MakeBool(std::string_view a_name, bool Settings::* a_member, std::string_view a_section = "General")
		{
			return Field{
				a_name, a_section,
				[a_member](Settings& a_s, const std::string& a_v) { a_s.*a_member = ToBool(a_v, a_s.*a_member); },
				[a_member](const Settings& a_s) { return std::string(a_s.*a_member ? "1" : "0"); },
				[](const std::string& a_v) { return ToBool(a_v, true) == ToBool(a_v, false); }
			};
		}

		// Number held within [a_min, a_max]; an unreadable value keeps the default.
		Field MakeFloat(std::string_view a_name, float Settings::* a_member, float a_min, float a_max, std::string_view a_section = "General")
		{
			return Field{
				a_name, a_section,
				[a_member, a_min, a_max](Settings& a_s, const std::string& a_v) {
					const auto v = ToFloat(a_v, a_s.*a_member);
					if (std::isfinite(v)) {
						a_s.*a_member = std::clamp(v, a_min, a_max);
					}
				},
				[a_member](const Settings& a_s) { return std::format("{:g}", a_s.*a_member); },
				[a_min, a_max](const std::string& a_v) {
					const auto v = ToFloat(a_v, std::numeric_limits<float>::quiet_NaN());
					return std::isfinite(v) && v >= a_min && v <= a_max;
				}
			};
		}

		const std::vector<Field>& Fields()
		{
			static const std::vector<Field> fields{
				MakeBool("bShowPanel", &Settings::showPanel),
				MakeFloat("fMinHoursForFaster", &Settings::minHoursForFaster, Settings::MIN_HOURS_FOR_FASTER, Settings::MAX_HOURS_FOR_FASTER),
				MakeBool("bSkipAfterConversation", &Settings::skipAfterConversation),
				MakeFloat("fConversationSeconds", &Settings::conversationSeconds, 0.0f, 30.0f),
			};
			return fields;
		}

		const Field* FindField(const std::string& a_lowerKey)
		{
			for (const auto& field : Fields()) {
				if (Lower(std::string(field.name)) == a_lowerKey) {
					return &field;
				}
			}
			return nullptr;
		}

		// An active "key = value" line, split into its parts. Positions refer to the original line.
		struct KeyLine
		{
			std::string key;  // lower case
			std::string value;
			std::size_t valueStart{ 0 };
			std::size_t valueEnd{ 0 };
		};

		std::optional<KeyLine> ParseKeyLine(const std::string& a_line)
		{
			const auto trimmed = Trim(a_line);
			if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' || trimmed.front() == '[') {
				return std::nullopt;
			}
			const auto eq = a_line.find('=');
			if (eq == std::string::npos) {
				return std::nullopt;
			}
			const auto comment = FindComment(a_line);
			if (comment != std::string::npos && comment < eq) {
				return std::nullopt;  // the '=' is inside a comment
			}

			KeyLine result;
			result.key = Lower(Trim(std::string_view(a_line).substr(0, eq)));
			if (result.key.empty()) {
				return std::nullopt;
			}
			const auto regionEnd = comment == std::string::npos ? a_line.size() : comment;
			auto       start = eq + 1;
			while (start < regionEnd && (a_line[start] == ' ' || a_line[start] == '\t')) {
				++start;
			}
			auto end = regionEnd;
			while (end > start && (a_line[end - 1] == ' ' || a_line[end - 1] == '\t' || a_line[end - 1] == '\r')) {
				--end;
			}
			result.valueStart = start;
			result.valueEnd = end;
			result.value = Unquote(a_line.substr(start, end - start));
			return result;
		}

		std::optional<std::string> SectionName(const std::string& a_line)
		{
			auto trimmed = Trim(a_line);
			if (const auto comment = FindComment(trimmed); comment != std::string::npos) {
				trimmed = Trim(std::string_view(trimmed).substr(0, comment));
			}
			if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
				return Lower(Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2)));
			}
			return std::nullopt;
		}

		struct IniFile
		{
			std::vector<std::string> lines;
			bool                     crlf{ true };
			bool                     exists{ false };
		};

		// Next to the log and the calibration file:
		// Documents\My Games\Skyrim Special Edition\SKSE\FastTravelArrivalPlanner.ini
		std::filesystem::path UserPath()
		{
			if (auto dir = logs::log_directory()) {
				return *dir / USER_FILE_NAME;
			}
			return std::filesystem::path(DEFAULTS_PATH);  // no Documents folder found: use the mod's INI as before
		}

		IniFile ReadIni(const std::filesystem::path& a_path)
		{
			IniFile       ini;
			std::ifstream file{ a_path, std::ios::binary };
			if (!file) {
				return ini;
			}
			ini.exists = true;
			bool sawLf = false;
			bool sawCrLf = false;
			for (std::string line; std::getline(file, line);) {
				sawLf = true;
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
					sawCrLf = true;
				}
				ini.lines.push_back(std::move(line));
			}
			ini.crlf = sawCrLf || !sawLf;
			return ini;
		}

		// Writes in place (not via a temporary file), so a mod manager's hard link to the file
		// stays intact.
		bool WriteIni(const std::filesystem::path& a_path, const IniFile& a_ini)
		{
			std::error_code ec;
			if (a_path.has_parent_path()) {
				std::filesystem::create_directories(a_path.parent_path(), ec);
			}
			std::ofstream file{ a_path, std::ios::binary | std::ios::trunc };
			if (!file) {
				logs::warn("Could not write {}", a_path.string());
				return false;
			}
			const std::string_view eol = a_ini.crlf ? "\r\n" : "\n";
			for (const auto& line : a_ini.lines) {
				file << line << eol;
			}
			file.flush();
			if (!file) {
				logs::warn("Writing {} failed", a_path.string());
				return false;
			}
			return true;
		}

		// "; sDayUnit = d": an option the player has commented out on purpose. Only names of
		// real options count, so ordinary comments that happen to contain '=' are ignored.
		std::optional<std::string> CommentedKey(const std::string& a_line)
		{
			const auto trimmed = Trim(a_line);
			if (trimmed.empty() || (trimmed.front() != ';' && trimmed.front() != '#')) {
				return std::nullopt;
			}
			const auto start = trimmed.find_first_not_of(";# \t");
			if (start == std::string::npos) {
				return std::nullopt;
			}
			if (const auto parsed = ParseKeyLine(trimmed.substr(start)); parsed && FindField(parsed->key)) {
				return parsed->key;
			}
			return std::nullopt;
		}

		// Options a newer version dropped (lower case). They are taken out of the player's file,
		// with the comment lines right above them that were copied from the defaults.
		constexpr std::array OBSOLETE_KEYS{
			"blaterbyclock"sv,   // 0.0.9: Later always moves the clock
			"benabled"sv,        // 0.2.0: merged into bShowPanel (see MigrateMasterSwitch)
			"bgameinterface"sv,  // 0.2.0: the panel is always drawn by the game's own interface
			"bmatchmapstyle"sv,  // 0.2.0: with the SKSE Menu Framework panel
			"sfont"sv,           // 0.2.0: with the SKSE Menu Framework panel
		};

		// Looks like an option name ("blaterbyclock"), not prose such as "1 = a normal trip".
		bool IsOptionName(const std::string& a_key)
		{
			return a_key.size() >= 2 && std::isalpha(static_cast<unsigned char>(a_key.front())) &&
			       std::ranges::all_of(a_key, [](unsigned char c) { return std::isalnum(c) || c == '_'; });
		}

		bool IsObsolete(std::string_view a_key)
		{
			return std::ranges::find(OBSOLETE_KEYS, a_key) != OBSOLETE_KEYS.end();
		}

		// The key of an option line: an active "key = value" line, or one commented out that
		// names an option of this version or an obsolete one. Explanations that happen to
		// contain " = " ("; Empty = the flyout's font ...") are not options.
		std::optional<std::string> AnyKey(const std::string& a_line)
		{
			if (const auto parsed = ParseKeyLine(a_line); parsed && IsOptionName(parsed->key)) {
				return parsed->key;
			}
			const auto trimmed = Trim(a_line);
			if (trimmed.empty() || (trimmed.front() != ';' && trimmed.front() != '#')) {
				return std::nullopt;
			}
			const auto start = trimmed.find_first_not_of(";# \t");
			if (start == std::string::npos) {
				return std::nullopt;
			}
			if (const auto parsed = ParseKeyLine(trimmed.substr(start)); parsed && (FindField(parsed->key) || IsObsolete(parsed->key))) {
				return parsed->key;
			}
			return std::nullopt;
		}

		std::vector<std::string> RemoveObsolete(IniFile& a_ini)
		{
			std::vector<std::string> removed;
			for (std::size_t i = 0; i < a_ini.lines.size();) {
				const auto key = AnyKey(a_ini.lines[i]);
				if (!key || !IsObsolete(*key)) {
					++i;
					continue;
				}
				// The comment block right above it, up to a blank line, a section or another option.
				std::size_t first = i;
				while (first > 0) {
					const auto above = Trim(a_ini.lines[first - 1]);
					if (above.empty() || (above.front() != ';' && above.front() != '#') || AnyKey(a_ini.lines[first - 1])) {
						break;
					}
					--first;
				}
				// Comments only count as its own when the option ends their block; otherwise they
				// explain the option that follows. One of the blank lines around it goes too, so
				// no double blank line is left.
				const bool  blankAfter = i + 1 >= a_ini.lines.size() || Trim(a_ini.lines[i + 1]).empty();
				std::size_t last = i;
				if (!blankAfter) {
					first = i;
				} else if (first > 0 && Trim(a_ini.lines[first - 1]).empty()) {
					--first;
				} else if (i + 1 < a_ini.lines.size() && (first == 0 || SectionName(a_ini.lines[first - 1]))) {
					++last;  // first in its section: the blank line after it goes instead
				}
				auto name = Trim(std::string_view(a_ini.lines[i]).substr(0, a_ini.lines[i].find('=')));
				name.erase(0, (std::min)(name.size(), name.find_first_not_of(";# \t")));
				removed.push_back(std::move(name));
				a_ini.lines.erase(a_ini.lines.begin() + static_cast<std::ptrdiff_t>(first), a_ini.lines.begin() + static_cast<std::ptrdiff_t>(last + 1));
				i = first;
			}
			return removed;
		}

		// 0.2.0 merged the master switch (bEnabled) into bShowPanel, which did the same. A player
		// who had switched the planner off with it keeps it off.
		bool MigrateMasterSwitch(IniFile& a_ini)
		{
			std::optional<std::size_t> enabledLine;
			std::optional<std::size_t> panelLine;
			bool                       off = false;
			for (std::size_t i = 0; i < a_ini.lines.size(); ++i) {
				const auto parsed = ParseKeyLine(a_ini.lines[i]);
				if (!parsed) {
					continue;
				}
				if (parsed->key == "benabled") {
					enabledLine = i;
					off = !ToBool(parsed->value, true);
				} else if (parsed->key == "bshowpanel") {
					panelLine = i;
				}
			}
			if (!off || !enabledLine) {
				return false;
			}
			if (panelLine) {
				auto&      line = a_ini.lines[*panelLine];
				const auto parsed = ParseKeyLine(line);
				line = line.substr(0, parsed->valueStart) + (parsed->valueStart == parsed->valueEnd && line.ends_with('=') ? " 0" : "0") +
				       line.substr(parsed->valueEnd);
			} else {
				a_ini.lines[*enabledLine] = "bShowPanel = 0";
			}
			return true;
		}

		// Sections that no option of this version belongs to and that hold no option any more
		// (all of theirs were dropped), with whatever comments are left in them and the blank
		// line that separated them. A section that still holds an option line is kept.
		std::vector<std::string> RemoveUnusedSections(IniFile& a_ini)
		{
			std::vector<std::string> removed;
			for (std::size_t i = 0; i < a_ini.lines.size();) {
				const auto name = SectionName(a_ini.lines[i]);
				if (!name) {
					++i;
					continue;
				}
				std::size_t end = i + 1;
				bool        holdsOption = false;
				while (end < a_ini.lines.size() && !SectionName(a_ini.lines[end])) {
					if (AnyKey(a_ini.lines[end])) {
						holdsOption = true;
					}
					++end;
				}
				const bool used = std::ranges::any_of(Fields(), [&](const Field& a_field) { return Lower(std::string(a_field.section)) == *name; });
				if (used || holdsOption) {
					i = end;
					continue;
				}
				std::size_t first = i;
				if (end >= a_ini.lines.size() && first > 0 && Trim(a_ini.lines[first - 1]).empty()) {
					--first;  // at the end of the file: the blank line above it goes too
				}
				removed.push_back(Trim(a_ini.lines[i]));
				a_ini.lines.erase(a_ini.lines.begin() + static_cast<std::ptrdiff_t>(first), a_ini.lines.begin() + static_cast<std::ptrdiff_t>(end));
				i = first;
			}
			return removed;
		}

		std::string JoinNames(const std::vector<std::string>& a_names)
		{
			std::string list;
			for (const auto& name : a_names) {
				list += (list.empty() ? "" : ", ") + name;
			}
			return list;
		}

		// Creates the player's settings file from the defaults the first time, and afterwards
		// adds any option that a newer version introduced. Nothing the player has set, changed
		// or commented out is ever touched.
		void PrepareUserFile()
		{
			const auto user = UserPath();
			const auto defaultsPath = std::filesystem::path(DEFAULTS_PATH);
			if (user == defaultsPath) {
				return;
			}
			const auto defaults = ReadIni(defaultsPath);
			auto       mine = ReadIni(user);

			if (!mine.exists) {
				IniFile created;
				created.crlf = defaults.exists ? defaults.crlf : true;
				created.lines = {
					"; Fast Travel Arrival Planner - your settings",
					"; This file is yours: installing an update never replaces it. Options that a new",
					"; version adds are copied in here automatically, with their default values.",
				};
				if (defaults.exists) {
					for (const auto& line : defaults.lines) {
						if (!Trim(line).starts_with(DEFAULTS_ONLY_MARK)) {
							created.lines.push_back(line);
						}
					}
				} else {
					// No defaults file either: write every option with its default value.
					const Settings           values;
					std::string              section;
					for (const auto& field : Fields()) {
						if (field.section != section) {
							section = field.section;
							created.lines.emplace_back();
							created.lines.push_back(std::format("[{}]", section));
						}
						created.lines.push_back(std::format("{} = {}", field.name, field.format(values)));
					}
				}
				if (WriteIni(user, created)) {
					logs::info("Created your settings file {} from the defaults", user.string());
				}
				return;
			}
			const bool migrated = MigrateMasterSwitch(mine);
			const auto removed = RemoveObsolete(mine);
			const auto emptied = RemoveUnusedSections(mine);
			const bool tidied = migrated || !removed.empty() || !emptied.empty();
			const auto logRemoved = [&]() {
				if (migrated) {
					logs::info("bEnabled = 0 from an earlier version: the arrival panel stays off (bShowPanel = 0)");
				}
				if (!removed.empty()) {
					logs::info("Removed option(s) this version no longer uses from your settings file: {}", JoinNames(removed));
				}
				if (!emptied.empty()) {
					logs::info("Removed section(s) this version no longer uses from your settings file: {}", JoinNames(emptied));
				}
			};
			if (!defaults.exists) {
				if (tidied && WriteIni(user, mine)) {
					logRemoved();
				}
				return;
			}

			// What the player's file already mentions: options set, or commented out on purpose.
			std::set<std::string>              known;
			std::map<std::string, std::size_t> sectionLastLine;  // lower-case section -> last non-blank line
			std::string                        section;
			for (std::size_t i = 0; i < mine.lines.size(); ++i) {
				const auto& line = mine.lines[i];
				if (const auto name = SectionName(line)) {
					section = *name;
					sectionLastLine[section] = i;
					continue;
				}
				if (!Trim(line).empty()) {
					sectionLastLine[section] = i;
				}
				if (const auto parsed = ParseKeyLine(line)) {
					known.insert(parsed->key);
				} else if (const auto key = CommentedKey(line)) {
					known.insert(*key);
				}
			}

			// Options in the defaults that the player's file lacks, each with the comment lines
			// that explain it.
			std::map<std::string, std::vector<std::string>> additions;  // section -> lines to add
			std::map<std::string, std::string>               sectionHeaders;
			std::vector<std::string>                         sectionOrder;
			std::vector<std::string>                         addedNames;
			std::vector<std::string>                         comments;
			section.clear();
			for (const auto& line : defaults.lines) {
				const auto trimmed = Trim(line);
				if (const auto name = SectionName(line)) {
					section = *name;
					sectionHeaders[section] = trimmed;
					comments.clear();
					continue;
				}
				if (trimmed.empty()) {
					comments.clear();
					continue;
				}
				if (trimmed.starts_with(DEFAULTS_ONLY_MARK)) {
					continue;
				}
				std::optional<std::string> key;
				std::string                displayName;
				if (const auto parsed = ParseKeyLine(line)) {
					key = parsed->key;
					displayName = Trim(std::string_view(line).substr(0, line.find('=')));
				} else {
					key = CommentedKey(line);
				}
				if (!key) {
					comments.push_back(line);  // explanation for the option that follows
					continue;
				}
				if (!known.contains(*key)) {
					known.insert(*key);
					if (!additions.contains(section)) {
						sectionOrder.push_back(section);
					}
					auto& block = additions[section];
					if (!comments.empty()) {
						block.emplace_back();  // a blank line before each explained option, as in the defaults
						block.insert(block.end(), comments.begin(), comments.end());
					}
					block.push_back(line);
					addedNames.push_back(displayName.empty() ? *key : displayName);
				}
				comments.clear();
			}
			if (additions.empty()) {
				if (tidied && WriteIni(user, mine)) {
					logRemoved();
				}
				return;
			}

			// Into existing sections, last first so earlier positions stay valid.
			std::vector<std::pair<std::size_t, std::string>> inserts;
			for (const auto& key : sectionOrder) {
				if (const auto it = sectionLastLine.find(key); it != sectionLastLine.end() && !key.empty()) {
					inserts.emplace_back(it->second, key);
				}
			}
			std::ranges::sort(inserts, std::greater{});
			for (const auto& [after, key] : inserts) {
				auto& lines = additions[key];
				if (lines.empty() || !lines.front().empty()) {
					lines.insert(lines.begin(), std::string{});  // keep a blank line after the existing options
				}
				mine.lines.insert(mine.lines.begin() + static_cast<std::ptrdiff_t>(after + 1), lines.begin(), lines.end());
				additions.erase(key);
			}
			// Sections the player's file does not have yet go at the end.
			for (const auto& key : sectionOrder) {
				const auto it = additions.find(key);
				if (it == additions.end()) {
					continue;
				}
				if (!mine.lines.empty() && !Trim(mine.lines.back()).empty()) {
					mine.lines.emplace_back();
				}
				if (!key.empty()) {
					mine.lines.push_back(sectionHeaders.contains(key) ? sectionHeaders[key] : std::format("[{}]", key));
				}
				auto lines = it->second;
				if (!lines.empty() && lines.front().empty()) {
					lines.erase(lines.begin());  // no blank line straight after the section header
				}
				mine.lines.insert(mine.lines.end(), lines.begin(), lines.end());
			}

			if (WriteIni(user, mine)) {
				logs::info("Added new option(s) to your settings file: {}", JoinNames(addedNames));
				logRemoved();
			}
		}
	}

	std::shared_ptr<const Settings> Settings::Current()
	{
		std::lock_guard guard{ g_lock };
		return g_current;
	}

	std::uint32_t Settings::Version()
	{
		return g_version.load();
	}

	void Settings::Set(Settings a_settings)
	{
		auto next = std::make_shared<const Settings>(std::move(a_settings));
		{
			std::lock_guard guard{ g_lock };
			g_current = std::move(next);
		}
		++g_version;
	}

	std::string Settings::FilePath()
	{
		return UserPath().string();
	}

	void Settings::Reload()
	{
		try {
			PrepareUserFile();
		} catch (const std::exception& e) {
			logs::warn("Could not prepare your settings file: {}", e.what());
		}

		Settings                 loaded;
		std::vector<std::string> misfits;  // values that cannot be used as written
		const auto               path = UserPath();
		const auto               ini = ReadIni(path);
		if (ini.exists) {
			logs::info("Settings file: {}", path.string());
			for (const auto& line : ini.lines) {
				if (const auto parsed = ParseKeyLine(line)) {
					if (const auto field = FindField(parsed->key)) {
						field->parse(loaded, parsed->value);
						if (!field->fits(parsed->value)) {
							misfits.push_back(std::format("{} = {}", field->name, parsed->value));
						}
					}
				}
			}
		} else {
			logs::info("No settings file at {}, using defaults", path.string());
		}
		logs::info("Settings: panel={} minHoursForFaster={:.2f} skipAfterConversation={} ({:.1f} s)",
			loaded.showPanel, loaded.minHoursForFaster, loaded.skipAfterConversation, loaded.conversationSeconds);
		Set(loaded);
		if (!misfits.empty()) {
			// Out of range or unreadable: the value in use is written back, so the file says
			// what the game does.
			logs::warn("Setting(s) outside what this version allows: {}. Using the nearest allowed value (or the default) instead, and writing it to your settings file.",
				JoinNames(misfits));
			SaveToIni(loaded);
		}
	}

	bool Settings::SaveToIni(const Settings& a_settings)
	{
		const auto     path = UserPath();
		auto           ini = ReadIni(path);
		const Settings defaults;
		const auto&    fields = Fields();

		std::vector<bool>                  present(fields.size(), false);
		std::map<std::string, std::size_t> sectionLastLine;  // lower-case section -> last line index
		std::string                        section;
		bool                               changed = false;

		for (std::size_t i = 0; i < ini.lines.size(); ++i) {
			auto& line = ini.lines[i];
			if (const auto name = SectionName(line)) {
				section = *name;
				sectionLastLine[section] = i;
				continue;
			}
			if (!Trim(line).empty()) {
				sectionLastLine[section] = i;
			}

			const auto parsed = ParseKeyLine(line);
			if (!parsed) {
				continue;
			}
			const auto field = FindField(parsed->key);
			if (!field) {
				continue;
			}
			present[static_cast<std::size_t>(field - fields.data())] = true;

			// Leave the line alone if it already means the same thing (e.g. "true" vs "1"), unless
			// it only does so after being brought into range.
			Settings existing = defaults;
			field->parse(existing, parsed->value);
			const auto wanted = field->format(a_settings);
			if (field->format(existing) == wanted && field->fits(parsed->value)) {
				continue;
			}
			std::string rebuilt = line.substr(0, parsed->valueStart);
			if (parsed->valueStart == parsed->valueEnd && !rebuilt.empty() && rebuilt.back() == '=') {
				rebuilt += ' ';
			}
			rebuilt += wanted;
			rebuilt += line.substr(parsed->valueEnd);
			line = std::move(rebuilt);
			changed = true;
		}

		// Keys that are not in the file yet: add them only if they differ from the default,
		// so a deliberately commented-out line keeps meaning "use the default".
		std::map<std::string, std::vector<std::string>> additions;  // section -> new lines
		std::vector<std::string>                        sectionOrder;
		for (std::size_t i = 0; i < fields.size(); ++i) {
			if (present[i]) {
				continue;
			}
			const auto value = fields[i].format(a_settings);
			if (value == fields[i].format(defaults)) {
				continue;
			}
			const auto key = Lower(std::string(fields[i].section));
			if (!additions.contains(key)) {
				sectionOrder.push_back(key);
			}
			additions[key].push_back(std::format("{} = {}", fields[i].name, value));
		}

		if (!additions.empty()) {
			changed = true;

			// Insert into existing sections, last section first so earlier indices stay valid.
			std::vector<std::pair<std::size_t, std::string>> inserts;
			for (const auto& key : sectionOrder) {
				if (const auto it = sectionLastLine.find(key); it != sectionLastLine.end() && !key.empty()) {
					inserts.emplace_back(it->second, key);
				}
			}
			std::ranges::sort(inserts, std::greater{});
			for (const auto& [after, key] : inserts) {
				auto& lines = additions[key];
				ini.lines.insert(ini.lines.begin() + static_cast<std::ptrdiff_t>(after + 1), lines.begin(), lines.end());
				additions.erase(key);
			}

			// Sections that do not exist yet go at the end.
			for (const auto& key : sectionOrder) {
				const auto it = additions.find(key);
				if (it == additions.end()) {
					continue;
				}
				const auto& field = *std::ranges::find_if(fields, [&](const Field& a_f) { return Lower(std::string(a_f.section)) == key; });
				if (!ini.lines.empty() && !Trim(ini.lines.back()).empty()) {
					ini.lines.emplace_back();
				}
				ini.lines.push_back(std::format("[{}]", field.section));
				ini.lines.insert(ini.lines.end(), it->second.begin(), it->second.end());
			}
		}

		if (!changed) {
			return true;
		}

		if (!WriteIni(path, ini)) {
			return false;
		}
		logs::info("Settings saved to {}", path.string());
		return true;
	}
}

