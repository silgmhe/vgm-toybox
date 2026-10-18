#pragma once

// The tray menu glyphs, as 16 x 16 SVG sources. TrayMenuIcons.cpp parses them once and
// recolours the black ink at draw time; the tick badge is added there as a vector, because
// it has to follow the menu's ink colour rather than the asset.
namespace toybox::standalone::tray_svg
{

inline constexpr const char* show = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="1.6" y="1.6" width="12.8" height="12.8" rx="3.3" fill="none" stroke="#000" stroke-width="1.4"/><rect x="4.875" y="8.25" width="1.55" height="3.5" rx="0.775" fill="#000"/><rect x="7.225" y="6.05" width="1.55" height="5.7" rx="0.775" fill="#000"/><rect x="9.575" y="7.45" width="1.55" height="4.3" rx="0.775" fill="#000"/></svg>)svg";

inline constexpr const char* play = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><path d="M5.0 3.1 L12.6 8 L5.0 12.9 Z" fill="#000" stroke="#000" stroke-width="1.5" stroke-linejoin="round"/></svg>)svg";

inline constexpr const char* pause = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="4.55" y="3.05" width="2.2" height="9.9" rx="1.1" fill="#000"/><rect x="9.25" y="3.05" width="2.2" height="9.9" rx="1.1" fill="#000"/></svg>)svg";

inline constexpr const char* stop = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="3.9" y="3.9" width="8.2" height="8.2" rx="1.8" fill="#000"/></svg>)svg";

inline constexpr const char* back = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="2.55" y="3.2" width="1.7" height="9.6" rx="0.85" fill="#000"/><path d="M12.9 3.15 L5.5 8 L12.9 12.85 Z" fill="#000" stroke="#000" stroke-width="1.2" stroke-linejoin="round"/></svg>)svg";

inline constexpr const char* forward = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="11.75" y="3.2" width="1.7" height="9.6" rx="0.85" fill="#000"/><path d="M3.1 3.15 L10.5 8 L3.1 12.85 Z" fill="#000" stroke="#000" stroke-width="1.2" stroke-linejoin="round"/></svg>)svg";

inline constexpr const char* loop = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><path d="M 10.753 4.068 A 4.8 4.8 0 1 1 4.606 4.606" fill="none" stroke="#000" stroke-width="1.45" stroke-linecap="butt" stroke-linejoin="round"/><path d="M 6.444 2.767 L 3.510 4.005 L 5.207 5.702 Z" fill="#000"/></svg>)svg";

inline constexpr const char* forever = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><path d="M 7.9 7.6 C 5.15 4.6075 2.6500000000000004 5.319999999999999 2.6500000000000004 7.6 C 2.6500000000000004 9.879999999999999 5.15 10.5925 7.9 7.6 C 10.65 4.6075 13.15 5.319999999999999 13.15 7.6 C 13.15 9.879999999999999 10.65 10.5925 7.9 7.6 Z" fill="none" stroke="#000" stroke-width="1.45" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";

inline constexpr const char* fade = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="1.9" y="3.2" width="2.2" height="9.4" rx="0.7" fill="#000"/><rect x="5.1" y="5.4" width="2.2" height="7.2" rx="0.7" fill="#000"/><rect x="8.3" y="7.6" width="2.2" height="5.0" rx="0.7" fill="#000"/><rect x="11.5" y="9.8" width="2.2" height="2.8" rx="0.7" fill="#000"/></svg>)svg";

inline constexpr const char* fadeOff = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect x="1.9" y="6.2" width="2.1" height="3.6" rx="0.55" fill="#000"/><path d="M4.0 7.6 L6.5 5.3 L6.5 10.7 L4.0 8.4 Z" fill="#000"/><path d="M 4.9 4.6 L 11.5 10.4" fill="none" stroke="#000" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";

inline constexpr const char* quit = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><path d="M 10.029 3.850 A 4.8 4.8 0 1 1 5.971 3.850" fill="none" stroke="#000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M 8.0 2.45 L 8.0 7.6" fill="none" stroke="#000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";

} // namespace toybox::standalone::tray_svg
