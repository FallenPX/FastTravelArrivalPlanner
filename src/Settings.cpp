#include "pch.h"

#include "Settings.h"

namespace FTAP
{
	namespace
	{
		constexpr auto INI_PATH = "Data/SKSE/Plugins/FastTravelArrivalPlanner.ini"sv;

		std::mutex                      g_lock;
		std::shared_ptr<const Settings> g_current = std::make_shared<const Settings>();

		std::string Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n\"");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r\n\"");
			return std::string(a_str.substr(first, last - first + 1));
		}

		std::string Lower(std::string a_str)
		{
			std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_str;
		}

		void ReadBool(const std::string& a_value, bool& a_out)
		{
			const auto v = Lower(a_value);
			if (v == "1" || v == "true" || v == "yes" || v == "on") {
				a_out = true;
			} else if (v == "0" || v == "false" || v == "no" || v == "off") {
				a_out = false;
			}
		}

		void ReadFloat(const std::string& a_value, float& a_out, float a_min, float a_max)
		{
			try {
				const float v = std::stof(a_value);
				if (std::isfinite(v)) {
					a_out = std::clamp(v, a_min, a_max);
				}
			} catch (...) {
			}
		}

		Settings LoadFromIni()
		{
			Settings      s;
			std::ifstream file{ std::string(INI_PATH) };
			if (!file) {
				logs::info("{} not found; using defaults", INI_PATH);
				return s;
			}
			std::string line;
			while (std::getline(file, line)) {
				if (const auto comment = line.find_first_of(";#"); comment != std::string::npos) {
					line.resize(comment);
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;  // section header, blank line or junk
				}
				const auto key = Lower(Trim(std::string_view(line).substr(0, eq)));
				const auto value = Trim(std::string_view(line).substr(eq + 1));
				if (key == "benabled") {
					ReadBool(value, s.enabled);
				} else if (key == "bshowpanel") {
					ReadBool(value, s.showPanel);
				} else if (key == "bweatheroptions") {
					ReadBool(value, s.weatherOptions);
				} else if (key == "fminhoursforfaster") {
					ReadFloat(value, s.minHoursForFaster, 0.0f, 48.0f);
				} else if (key == "blaterbyclock") {
					ReadBool(value, s.laterByClock);
				} else if (key == "bskipafterconversation") {
					ReadBool(value, s.skipAfterConversation);
				} else if (key == "fconversationseconds") {
					ReadFloat(value, s.conversationSeconds, 0.0f, 30.0f);
				} else if (key == "blogweathers") {
					ReadBool(value, s.logWeathers);
				}
			}
			return s;
		}
	}

	std::shared_ptr<const Settings> Settings::Current()
	{
		std::lock_guard guard{ g_lock };
		return g_current;
	}

	void Settings::Reload()
	{
		auto loaded = std::make_shared<const Settings>(LoadFromIni());
		logs::info("Settings: enabled={} panel={} weather={} minHoursForFaster={:.2f} laterByClock={} skipAfterConversation={} ({:.1f} s) logWeathers={}",
			loaded->enabled, loaded->showPanel, loaded->weatherOptions, loaded->minHoursForFaster, loaded->laterByClock,
			loaded->skipAfterConversation, loaded->conversationSeconds, loaded->logWeathers);
		std::lock_guard guard{ g_lock };
		g_current = std::move(loaded);
	}
}
