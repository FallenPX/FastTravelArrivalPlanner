#include "pch.h"

#include "Weather.h"

namespace FTAP::Weather
{
	namespace
	{
		using Flag = RE::TESWeather::WeatherDataFlag;

		std::optional<Flag> FlagFor(Choice a_choice)
		{
			switch (a_choice) {
			case Choice::kClear:
				return Flag::kPleasant;
			case Choice::kCloudy:
				return Flag::kCloudy;
			case Choice::kRain:
				return Flag::kRainy;
			case Choice::kSnow:
				return Flag::kSnow;
			default:
				return std::nullopt;
			}
		}

		std::string FormText(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "none";
			}
			const auto file = a_form->GetFile(0);
			return std::format("{:08X} ({})", a_form->GetFormID(), file ? file->GetFilename() : "created in game"sv);
		}

		// How dense the weather's rain or snow particles are (0 = none). Bethesda classifies some
		// mostly clear weathers with a few wind-blown flakes as "snow", so this tells them apart.
		float PrecipitationDensity(const RE::TESWeather* a_weather)
		{
			const auto particles = a_weather ? a_weather->precipitationData : nullptr;
			if (!particles || REL::Module::IsVR()) {
				return 0.0f;
			}
			const auto& data = particles->GetRuntimeData().data;
			const auto  index = static_cast<std::uint32_t>(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity);
			if (index >= data.size()) {
				return 0.0f;
			}
			const float density = data[index].f;
			return std::isfinite(density) && density > 0.0f ? density : 0.0f;
		}

		std::string ClassText(const RE::TESWeather* a_weather)
		{
			if (!a_weather) {
				return {};
			}
			std::string out;
			const auto  add = [&](Flag a_flag, const char* a_name) {
                if (a_weather->data.flags.any(a_flag)) {
                    if (!out.empty()) {
                        out += '+';
                    }
                    out += a_name;
                }
			};
			add(Flag::kPleasant, "pleasant");
			add(Flag::kCloudy, "cloudy");
			add(Flag::kRainy, "rainy");
			add(Flag::kSnow, "snow");
			return out.empty() ? "unclassified" : out;
		}

		struct RegionWeathers
		{
			RE::TESRegion*                 region{ nullptr };
			int                            priority{ -1 };
			bool                           overrides{ false };
			std::vector<RE::WeatherType*>  weathers;
		};

		RE::TESObjectCELL* PlayerCell()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player ? player->GetParentCell() : nullptr;
		}

		// Every region with a weather list that covers the player's cell.
		std::vector<RegionWeathers> RegionsHere(RE::TESObjectCELL* a_cell)
		{
			std::vector<RegionWeathers> out;
			if (!a_cell) {
				return out;
			}
			const auto list = a_cell->GetRegionList(false);
			if (!list) {
				return out;
			}
			for (const auto region : *list) {
				if (!region || !region->dataList) {
					continue;
				}
				for (const auto data : region->dataList->regionDataList) {
					if (!data || data->GetType() != RE::TESRegionData::Type::kWeather) {
						continue;
					}
					RegionWeathers entry;
					entry.region = region;
					entry.priority = data->dataHeader.priority;
					entry.overrides = data->dataHeader.flags.any(RE::TESRegionData::DataHeader::Flag::kOverride);
					for (const auto type : static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes) {
						if (type && type->weather) {
							entry.weathers.push_back(type);
						}
					}
					if (!entry.weathers.empty()) {
						out.push_back(std::move(entry));
					}
				}
			}
			// The game lets the highest-priority region decide the weather.
			std::ranges::stable_sort(out, [](const RegionWeathers& a_l, const RegionWeathers& a_r) {
				return a_l.priority > a_r.priority;
			});
			return out;
		}

		std::vector<RE::WeatherType*> ClimateWeathers()
		{
			std::vector<RE::WeatherType*> out;
			RE::TESClimate*               climate = nullptr;
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				if (const auto world = player->GetWorldspace()) {
					climate = world->climate;
				}
			}
			if (!climate) {
				if (const auto sky = RE::Sky::GetSingleton()) {
					climate = sky->currentClimate;
				}
			}
			if (climate) {
				for (const auto type : climate->weatherList) {
					if (type && type->weather) {
						out.push_back(type);
					}
				}
			}
			return out;
		}

		RE::TESWeather* PickWeighted(const std::vector<RE::WeatherType*>& a_candidates)
		{
			if (a_candidates.empty()) {
				return nullptr;
			}
			std::uint64_t total = 0;
			for (const auto type : a_candidates) {
				total += type->chance;
			}
			if (total == 0) {
				return a_candidates.front()->weather;
			}
			static std::mt19937_64                  rng{ std::random_device{}() };
			std::uniform_int_distribution<std::uint64_t> dist{ 0, total - 1 };
			auto                                    roll = dist(rng);
			for (const auto type : a_candidates) {
				if (roll < type->chance) {
					return type->weather;
				}
				roll -= type->chance;
			}
			return a_candidates.back()->weather;
		}

		// For Rain and Snow, keep only weathers whose precipitation is at least half as dense as
		// the heaviest one on offer, so the choice means visible rain or snow, not a few flakes.
		void KeepVisiblePrecipitation(Choice a_choice, std::vector<RE::WeatherType*>& a_candidates)
		{
			if (a_choice != Choice::kRain && a_choice != Choice::kSnow) {
				return;
			}
			float heaviest = 0.0f;
			for (const auto type : a_candidates) {
				heaviest = (std::max)(heaviest, PrecipitationDensity(type->weather));
			}
			if (heaviest <= 0.0f) {
				return;  // no density information: keep them all
			}
			std::erase_if(a_candidates, [&](const RE::WeatherType* a_type) {
				return PrecipitationDensity(a_type->weather) < heaviest * 0.5f;
			});
		}

		std::mutex              g_loggedLock;
		std::set<RE::FormID>    g_loggedRegions;
	}

	const char* Name(Choice a_choice)
	{
		switch (a_choice) {
		case Choice::kClear:
			return "Clear";
		case Choice::kCloudy:
			return "Cloudy";
		case Choice::kRain:
			return "Rain";
		case Choice::kSnow:
			return "Snow";
		default:
			return "Natural";
		}
	}

	Pick PickHere(Choice a_choice)
	{
		Pick       result;
		const auto flag = FlagFor(a_choice);
		if (!flag) {
			result.reason = "natural weather chosen";
			return result;
		}
		const auto cell = PlayerCell();
		if (!cell) {
			result.reason = "the player is not in a cell yet";
			return result;
		}
		if (cell->IsInteriorCell()) {
			result.reason = "arrived indoors";
			return result;
		}
		if (const auto sky = RE::Sky::GetSingleton(); sky && sky->overrideWeather) {
			result.reason = std::format("the weather here is held by a quest or script ({})", FormText(sky->overrideWeather));
			return result;
		}

		const auto matches = [&](const RE::WeatherType* a_type) { return a_type->weather->data.flags.any(*flag); };

		const auto regions = RegionsHere(cell);
		if (!regions.empty()) {
			const auto& top = regions.front();
			std::vector<RE::WeatherType*> candidates;
			std::ranges::copy_if(top.weathers, std::back_inserter(candidates), matches);
			if (candidates.empty()) {
				result.reason = std::format("region {} has no {} weather", FormText(top.region), Name(a_choice));
				return result;
			}
			const auto all = candidates.size();
			KeepVisiblePrecipitation(a_choice, candidates);
			result.weather = PickWeighted(candidates);
			result.reason = std::format("from region {} ({} of {} candidate(s))", FormText(top.region), candidates.size(), all);
			return result;
		}

		const auto climate = ClimateWeathers();
		std::vector<RE::WeatherType*> candidates;
		std::ranges::copy_if(climate, std::back_inserter(candidates), matches);
		if (candidates.empty()) {
			result.reason = std::format("no region weather here and the climate has no {} weather", Name(a_choice));
			return result;
		}
		KeepVisiblePrecipitation(a_choice, candidates);
		result.weather = PickWeighted(candidates);
		result.reason = std::format("from the worldspace climate ({} candidate(s))", candidates.size());
		return result;
	}

	void Apply(RE::TESWeather* a_weather)
	{
		if (const auto sky = RE::Sky::GetSingleton(); sky && a_weather) {
			sky->ForceWeather(a_weather, false);
		}
	}

	RE::TESWeather* Current()
	{
		const auto sky = RE::Sky::GetSingleton();
		return sky ? sky->currentWeather : nullptr;
	}

	std::string Describe(const RE::TESWeather* a_weather)
	{
		if (!a_weather) {
			return "none";
		}
		const float density = PrecipitationDensity(a_weather);
		return density > 0.0f ? std::format("{} [{}, precipitation density {:.1f}]", FormText(a_weather), ClassText(a_weather), density) :
		                        std::format("{} [{}]", FormText(a_weather), ClassText(a_weather));
	}

	void LogRegionsHere()
	{
		const auto cell = PlayerCell();
		if (!cell || cell->IsInteriorCell()) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto world = player ? player->GetWorldspace() : nullptr;

		const auto regions = RegionsHere(cell);
		if (regions.empty()) {
			logs::info("[Weathers] No region weather lists here (worldspace {}); climate list:", FormText(world));
			for (const auto type : ClimateWeathers()) {
				logs::info("[Weathers]   {} chance {}", Describe(type->weather), type->chance);
			}
			return;
		}
		std::lock_guard guard{ g_loggedLock };
		for (const auto& entry : regions) {
			if (!g_loggedRegions.insert(entry.region->GetFormID()).second) {
				continue;
			}
			logs::info("[Weathers] Region {} in worldspace {}: priority {}{}, {} weather(s)",
				FormText(entry.region), FormText(world), entry.priority, entry.overrides ? ", override" : "", entry.weathers.size());
			for (const auto type : entry.weathers) {
				logs::info("[Weathers]   {} chance {}", Describe(type->weather), type->chance);
			}
		}
	}
}
