#include "Parser.h"
#include "Utils.h"
#include "ImageLoader.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

Parser::Parser() {
}

static std::string getExtension(const std::string &filename) {
    size_t pos = filename.find_last_of('.');
    if (pos == std::string::npos)
        return "";
    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool Parser::parseFile(const std::string &filename) {
    std::string ext = getExtension(filename);
    if (ext == "bmp" || ext == "png" || ext == "gif") {
        // --- IMAGE FILE HANDLING ---
        Image image;
        if (!loadImage(filename, image)) {
            std::cerr << "Failed to load image: " << filename << "\n";
            return false;
        }
        // Determine the maximum candidate codel size N such that
        // when dividing the image into non–overlapping N×N blocks, each block is uniform.
        int maxCandidate = 1;
        int maxN = std::min(image.width, image.height);
        for (int n = 1; n <= maxN; ++n) {
            if (image.width % n != 0 || image.height % n != 0)
                continue; // Only consider divisors.
            bool valid = true;
            int numBlocksWidth = image.width / n;
            int numBlocksHeight = image.height / n;
            // For each block, check that all pixels match the top–left pixel.
            for (int by = 0; by < numBlocksHeight && valid; ++by) {
                for (int bx = 0; bx < numBlocksWidth && valid; ++bx) {
                    int baseIndex = ((by * n) * image.width + (bx * n)) * 3;
                    uint8_t r = image.data[baseIndex];
                    uint8_t g = image.data[baseIndex + 1];
                    uint8_t b = image.data[baseIndex + 2];
                    for (int y = 0; y < n && valid; ++y) {
                        for (int x = 0; x < n; ++x) {
                            int index = (((by * n) + y) * image.width + (bx * n + x)) * 3;
                            if (image.data[index] != r || image.data[index+1] != g || image.data[index+2] != b) {
                                valid = false;
                                break;
                            }
                        }
                    }
                }
            }
            if (valid) {
                maxCandidate = n; // update candidate if this block size works.
            }
        }
        int codelSize = maxCandidate;
        std::cout << "Determined codel size: " << codelSize << "\n";

        // Build the grid: each cell represents one codel.
        int rows = image.height / codelSize;
        int cols = image.width / codelSize;
        grid.clear();
        grid.resize(rows, std::vector<PietColor>(cols, PietColor::Undefined));
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                // Use the top–left pixel of the block as its color.
                int index = ((r * codelSize) * image.width + (c * codelSize)) * 3;
                uint8_t red   = image.data[index];
                uint8_t green = image.data[index + 1];
                uint8_t blue  = image.data[index + 2];
                std::string hex = rgbToHex(red, green, blue);
                grid[r][c] = hexToPietColor(hex);
            }
        }
        return true;
    } else {
        // --- TEXT FILE HANDLING (whitespace–separated hex codes) ---
        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Error: Cannot open file " << filename << "\n";
            return false;
        }
        grid.clear();
        std::string line;
        while (std::getline(infile, line)) {
            if (line.empty())
                continue;
            std::istringstream iss(line);
            std::vector<PietColor> row;
            std::string token;
            while (iss >> token) {
                row.push_back(hexToPietColor(token));
            }
            grid.push_back(row);
        }
        infile.close();
        return true;
    }
}

const std::vector<std::vector<PietColor>>& Parser::getGrid() const {
    return grid;
}
