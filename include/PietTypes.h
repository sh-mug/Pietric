#ifndef PIET_TYPES_H
#define PIET_TYPES_H

#include <string>

// The 20 Piet colors (plus a catch–all Undefined)
enum class PietColor {
    LightRed, LightYellow, LightGreen, LightCyan, LightBlue, LightMagenta,
    Red, Yellow, Green, Cyan, Blue, Magenta,
    DarkRed, DarkYellow, DarkGreen, DarkCyan, DarkBlue, DarkMagenta,
    White, Black,
    Undefined
};

// Direction pointer values (DP)
enum class Direction {
    Right, Down, Left, Up
};

// Codel chooser states (CC)
enum class CodelChooser {
    Left, Right
};

// Piet command set
enum class Command {
    None,       // No command (or no operation)
    Push, 
    Pop, 
    Add, 
    Subtract, 
    Multiply, 
    Divide, 
    Modulo, 
    Not, 
    Greater, 
    Pointer, 
    Switch,
    Duplicate, 
    Roll, 
    InputNum, 
    InputChar, 
    OutputNum, 
    OutputChar
};

/// Convert a hex string (e.g. "FFC0C0") to a PietColor.
/// (The implementation is provided in Utils.cpp.)
PietColor hexToPietColor(const std::string &hex);

#endif // PIET_TYPES_H
