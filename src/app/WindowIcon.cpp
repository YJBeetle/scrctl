#include "app/WindowIcon.h"
#include "WindowIconData.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace scrctl::app {
SDL_Surface *load_window_icon() {
    int width = 0, height = 0, channels = 0;
    auto *pixels = stbi_load_from_memory(window_icon_png, sizeof(window_icon_png),
                                         &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        SDL_SetError("Cannot decode embedded icon: %s", stbi_failure_reason());
        return nullptr;
    }
    // SDL_CreateRGBSurfaceWithFormat 拥有像素；复制后即可释放解码器的缓冲区。
    auto *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface && SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4,
                                    surface->format->format, surface->pixels, surface->pitch) != 0) {
        SDL_FreeSurface(surface);
        surface = nullptr;
    }
    stbi_image_free(pixels);
    return surface;
}

void set_window_icon(SDL_Window *window) {
    if (auto *icon = load_window_icon()) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot load scrctl icon: %s", SDL_GetError());
    }
}
}
