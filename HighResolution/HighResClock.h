#pragma once
#include <chrono>

namespace RW{
    namespace CORE{
        struct HighResClock
        {
            typedef long long                               rep;
            typedef std::nano                               period;
            typedef std::chrono::duration<rep, period>      duration;
            typedef std::chrono::time_point<HighResClock>   time_point;
            static const bool is_steady = true;

            static time_point now();
            static std::chrono::milliseconds diffMilli(time_point start, time_point end);
            static std::chrono::nanoseconds diffNano(time_point start, time_point end);
        };
    }

}

