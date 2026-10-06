#pragma once

#include <SDL.h>

#include "app/ViewGeom.h"

namespace scrctl::app {

/// 将裁剪区域旋转后绘制到视口。独立函数用于四角着色帧的无窗口回读测试。
/// SDL_RenderCopyEx 使用顺时针角度，与 Crop 的转正角度一致。
/// dst 是旋转前的矩形，应与裁剪区域同尺寸、中心位于视口中心；90/270 度
/// 旋转后的包围盒才能铺满视口。直接用视口尺寸作为 dst 会造成缩窄和黑边。
inline void draw_rotated(SDL_Renderer *renderer, SDL_Texture *texture, const Crop &crop,
                         int degrees) {
    const SDL_Rect src { crop.x, crop.y, crop.w, crop.h };
    int vw = 0, vh = 0;
    viewport_size(crop, degrees, vw, vh);
    const SDL_Rect dst { (vw - crop.w) / 2, (vh - crop.h) / 2, crop.w, crop.h };
    SDL_RenderCopyEx(renderer, texture, &src, &dst, static_cast<double>(degrees), nullptr,
                     SDL_FLIP_NONE);
}

}  // namespace scrctl::app
