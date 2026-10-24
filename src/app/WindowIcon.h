#pragma once

#include <SDL.h>

namespace scrctl::app {
// 调用者用 SDL_FreeSurface 释放；像素随 surface 一起释放。
SDL_Surface *load_window_icon();
void set_window_icon(SDL_Window *window);
}
