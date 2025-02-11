#ifndef IMAGE_LOADER_H
#define IMAGE_LOADER_H

#include <string>
#include <vector>
#include <cstdint>

// Structure holding image data.
struct Image {
    int width;
    int height;
    int channels;              // In our case, we force 3 channels (RGB).
    std::vector<uint8_t> data; // Pixel data in row–major order (RGB)
};

// Loads an image (bmp/png/gif) from file. Returns true on success.
bool loadImage(const std::string &filename, Image &image);

#endif // IMAGE_LOADER_H
