#include "app/WindowIcon.h"

#include <cstdio>

int main() {
    auto *icon = scrctl::app::load_window_icon();
    if (!icon) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    bool transparent = false, visible = false;
    for (int y = 0; y < icon->h; ++y) {
        const auto *row = reinterpret_cast<const Uint32 *>(
            static_cast<const Uint8 *>(icon->pixels) + y * icon->pitch);
        for (int x = 0; x < icon->w; ++x) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(row[x], icon->format, &r, &g, &b, &a);
            transparent |= a == 0;
            visible |= a > 0 && (r > 0 || g > 0 || b > 0);
        }
    }
    const bool ok = icon->w == 1024 && icon->h == 1024 && transparent && visible;
    SDL_FreeSurface(icon);
    return ok ? 0 : 1;
}
