// SPDX-License-Identifier: GPL-3.0-or-later
// Fork mkw60: measurement probes (CB hazard, prep cost, cache guard, flush, UBO, mutation) are only
// compiled in with the CMake option MKW60_PROBES=ON. Without it their Enabled() checks are constant
// false, so the compiler drops the probe code from the hot paths entirely (8.11.2026: the always-
// present checks were part of a ~1 ms per frame regression against B0 on the Peach Stadium).
#pragma once

#ifndef MKW60_PROBES
#define MKW60_PROBES 0
#endif

namespace Common {
inline constexpr bool kNx2Probes = MKW60_PROBES != 0;
} // namespace Common
