#pragma once

#include "Planner.h"

namespace FTAP
{
	// Later arrivals, in whole hours. Times are counted from today's midnight; the day is
	// counted from the day of the normal arrival.
	struct LaterRange
	{
		double etaAbs{ 0.0 };   // normal arrival, hours since today's midnight
		int    etaDay{ 0 };     // days after today
		int    etaHour{ 0 };    // whole hour of the day of the normal arrival
		int    dayMin{ 0 };
		int    dayMax{ Planner::MAX_LATER_DAYS };

		[[nodiscard]] int HourMin(int a_day) const { return a_day == 0 ? etaHour + 1 : 0; }
		[[nodiscard]] int HourMax(int a_day) const { return a_day == Planner::MAX_LATER_DAYS ? etaHour : 23; }
		// Arrival in hours from now.
		[[nodiscard]] double HoursFromNow(int a_day, int a_hour, double a_nowHour) const
		{
			return (etaDay + a_day) * 24.0 + a_hour - a_nowHour;
		}
	};

	inline LaterRange MakeRange(double a_nowHour, double a_eta)
	{
		LaterRange r;
		r.etaAbs = a_nowHour + a_eta;
		r.etaDay = static_cast<int>(std::floor(r.etaAbs / 24.0));
		r.etaHour = static_cast<int>(std::floor(r.etaAbs - r.etaDay * 24.0));
		r.etaHour = std::clamp(r.etaHour, 0, 23);
		r.dayMin = r.etaHour >= 23 ? 1 : 0;
		return r;
	}
}
