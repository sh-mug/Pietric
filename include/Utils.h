#ifndef UTILS_H
#define UTILS_H

#include <string>
#include "PietTypes.h"

// Converts a hex string to a PietColor. Recognizes the 20 colors and returns Undefined for unknown codes.
PietColor hexToPietColor(const std::string &hex);

// Converts an RGB triple (each component in 0–255) to a hexadecimal string (e.g. "FFC0C0").
std::string rgbToHex(unsigned char r, unsigned char g, unsigned char b);

#endif // UTILS_H
