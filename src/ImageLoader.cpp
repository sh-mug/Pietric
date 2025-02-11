#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "ImageLoader.h"
#include <iostream>

bool loadImage(const std::string &filename, Image &image) {
    int w, h, channels;
    // Force 3 channels (RGB) regardless of image type.
    unsigned char *data = stbi_load(filename.c_str(), &w, &h, &channels, 3);
    if (!data) {
        std::cerr << "Error: Cannot load image: " << filename << "\n";
        return false;
    }
    image.width = w;
    image.height = h;
    image.channels = 3;
    image.data.assign(data, data + w * h * 3);
    stbi_image_free(data);
    return true;
}
