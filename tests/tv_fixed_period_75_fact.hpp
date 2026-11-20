#pragma once

// Retained TradingView facts, read by eye; transcription awaits independent
// visual review. No engine output supplies any expected value or mapping.
// Source (all value, NA and timestamp assertions using this table cite it):
// Retained TradingView capture: decoded.json
// SHA256 9dd2b5b17e428532d1280c0151330d715949610304a74e849bbefd22f86ba12c
// A: chart30/request75, gaps_on/off, lookahead_off, records A.bars[0..95].
// B: chart60/request75, lookahead_on/off, gaps_off, B.bars[0..47].
// Minutes are relative to the capture's 2024-01-01 00:00 UTC origin.
// This header contains only retained input facts.
#include <cstdint>

namespace tv_fixed_period_75_fact {
constexpr std::int64_t kOriginMs = 1704067200000LL;
constexpr std::int64_t kMinuteMs = 60000;
struct RequestedBar { int open_minute; int close_minute; double value; };
inline constexpr RequestedBar kBars[] = {
    {0, 75, 0x1.4bc4a8f5c28f6p+15},
    {75, 150, 0x1.4c64051eb851fp+15},
    {150, 225, 0x1.4a7c6147ae148p+15},
    {225, 300, 0x1.4b3ffae147ae1p+15},
    {300, 375, 0x1.4a48051eb851fp+15},
    {375, 450, 0x1.4bef666666666p+15},
    {450, 525, 0x1.4bf9d1eb851ecp+15},
    {525, 600, 0x1.4d336147ae148p+15},
    {600, 675, 0x1.4d8e51eb851ecp+15},
    {675, 750, 0x1.4d784cccccccdp+15},
    {750, 825, 0x1.4de9851eb851fp+15},
    {825, 900, 0x1.4ce87ae147ae1p+15},
    {900, 975, 0x1.4eaf7ae147ae1p+15},
    {975, 1050, 0x1.4dd63851eb852p+15},
    {1050, 1125, 0x1.51203851eb852p+15},
    {1125, 1200, 0x1.53fbfae147ae1p+15},
    {1200, 1275, 0x1.55991eb851eb8p+15},
    {1275, 1350, 0x1.539e333333333p+15},
    {1350, 1425, 0x1.5869bd70a3d71p+15},
    {1425, 1440, 0x1.592719999999ap+15},
    {1440, 1515, 0x1.5e5270a3d70a4p+15},
    {1515, 1590, 0x1.6057fae147ae1p+15},
    {1590, 1665, 0x1.62d40f5c28f5cp+15},
    {1665, 1740, 0x1.60f799999999ap+15},
    {1740, 1815, 0x1.61700a3d70a3dp+15},
    {1815, 1890, 0x1.632a333333333p+15},
    {1890, 1965, 0x1.64c7570a3d70ap+15},
    {1965, 2040, 0x1.64d2f5c28f5c3p+15},
    {2040, 2115, 0x1.63f0000000000p+15},
    {2115, 2190, 0x1.6314000000000p+15},
    {2190, 2265, 0x1.64bcf5c28f5c3p+15},
    {2265, 2340, 0x1.619575c28f5c3p+15},
    {2340, 2415, 0x1.60322e147ae14p+15},
    {2415, 2490, 0x1.6130051eb851fp+15},
    {2490, 2565, 0x1.602bc28f5c28fp+15},
    {2565, 2640, 0x1.5fdefae147ae1p+15},
    {2640, 2715, 0x1.5e57fae147ae1p+15},
    {2715, 2790, 0x1.600aae147ae14p+15},
    {2790, 2865, 0x1.5e3f851eb851fp+15},
    {2865, 2880, 0x1.5f25d1eb851ecp+15},
};
constexpr int kBarCount = 40;
constexpr int kChart30Bars = 96;
inline constexpr int kChart30GapsOn[kChart30Bars] = {
    -1,-1,0,-1,1,-1,-1,2,-1,3,-1,-1,4,-1,5,-1,
    -1,6,-1,7,-1,-1,8,-1,9,-1,-1,10,-1,11,-1,-1,
    12,-1,13,-1,-1,14,-1,15,-1,-1,16,-1,17,-1,-1,19,
    -1,-1,20,-1,21,-1,-1,22,-1,23,-1,-1,24,-1,25,-1,
    -1,26,-1,27,-1,-1,28,-1,29,-1,-1,30,-1,31,-1,-1,
    32,-1,33,-1,-1,34,-1,35,-1,-1,36,-1,37,-1,-1,39,
};
inline constexpr int kChart30GapsOff[kChart30Bars] = {
    -1,-1,0,0,1,1,1,2,2,3,3,3,4,4,5,5,
    5,6,6,7,7,7,8,8,9,9,9,10,10,11,11,11,
    12,12,13,13,13,14,14,15,15,15,16,16,17,17,17,19,
    19,19,20,20,21,21,21,22,22,23,23,23,24,24,25,25,
    25,26,26,27,27,27,28,28,29,29,29,30,30,31,31,31,
    32,32,33,33,33,34,34,35,35,35,36,36,37,37,37,39,
};
constexpr int kChart60Bars = 48;
inline constexpr int kChart60LookaheadOn[kChart60Bars] = {
    0,0,1,2,3,4,4,5,6,7,8,8,9,10,11,12,
    12,13,14,15,16,16,17,18,20,20,21,22,23,24,24,25,
    26,27,28,28,29,30,31,32,32,33,34,35,36,36,37,38,
};
inline constexpr int kChart60LookaheadOff[kChart60Bars] = {
    -1,0,1,2,3,3,4,5,6,7,7,8,9,10,11,11,
    12,13,14,15,15,16,17,19,19,20,21,22,23,23,24,25,
    26,27,27,28,29,30,31,31,32,33,34,35,35,36,37,39,
};
} // namespace tv_fixed_period_75_fact
