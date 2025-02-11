#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>
#include "PietTypes.h"

class Parser {
public:
    Parser();
    // Parses an input file.
    // If the filename ends with .bmp, .png, or .gif, it is interpreted as an image.
    // Otherwise, it is parsed as a text file with whitespace-separated hex color codes.
    bool parseFile(const std::string &filename);
    // Returns the parsed grid (rows of codels)
    const std::vector<std::vector<PietColor>>& getGrid() const;

private:
    std::vector<std::vector<PietColor>> grid;
};

#endif // PARSER_H
