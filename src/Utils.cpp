#include "Utils.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

// Helper: convert a string to uppercase.
static std::string toUpper(const std::string &str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

PietColor hexToPietColor(const std::string &hex) {
    std::string h = toUpper(hex);
    if (h == "FFC0C0") return PietColor::LightRed;
    if (h == "FFFFC0") return PietColor::LightYellow;
    if (h == "C0FFC0") return PietColor::LightGreen;
    if (h == "C0FFFF") return PietColor::LightCyan;
    if (h == "C0C0FF") return PietColor::LightBlue;
    if (h == "FFC0FF") return PietColor::LightMagenta;
    if (h == "FF0000") return PietColor::Red;
    if (h == "FFFF00") return PietColor::Yellow;
    if (h == "00FF00") return PietColor::Green;
    if (h == "00FFFF") return PietColor::Cyan;
    if (h == "0000FF") return PietColor::Blue;
    if (h == "FF00FF") return PietColor::Magenta;
    if (h == "C00000") return PietColor::DarkRed;
    if (h == "C0C000") return PietColor::DarkYellow;
    if (h == "00C000") return PietColor::DarkGreen;
    if (h == "00C0C0") return PietColor::DarkCyan;
    if (h == "0000C0") return PietColor::DarkBlue;
    if (h == "C000C0") return PietColor::DarkMagenta;
    if (h == "FFFFFF") return PietColor::White;
    if (h == "000000") return PietColor::Black;
    return PietColor::Undefined;
}

std::string rgbToHex(unsigned char r, unsigned char g, unsigned char b) {
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0')
       << std::setw(2) << static_cast<int>(r)
       << std::setw(2) << static_cast<int>(g)
       << std::setw(2) << static_cast<int>(b);
    return ss.str();
}
