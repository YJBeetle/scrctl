#pragma once

#include <SDL.h>

#include "app/ViewGeom.h"

namespace scrctl::app {

/// 把面板上那一块画进视口，旋转也在这儿发生。
///
/// 单独拆成一个函数只为了让它能被离线自检：转屏画歪这件事在真机上有一个额外的
/// 变量（画面内容一直在动），而 `SDL_RenderCopyEx` 的角度方向、dst 该取视口尺寸
/// 还是裁剪尺寸这两件事，用一张四角染色的假帧在无头模式下就能一次判死。
/// 留在 Presenter 里就只能靠开窗口肉眼看。
///
/// 两条容易搞错的约定：
/// - `SDL_RenderCopyEx` 的 angle 是**顺时针**度数，而 `Crop` 的角度本来就是
///   "要转正需要顺时针转多少"（真机对出来的），所以直接传，不取反。
/// - **dst 是"没转之前"那块图像所在的矩形，不是视口**。SDL 绕 dst 的中心做旋转，
///   转完的图像会溢出 dst：拿视口尺寸当 dst，90°/270° 会画成"中间竖着一条、
///   四角全黑"（这条是无头回读实测出来的，不是推的）。正确写法是给一个与裁剪框
///   同尺寸、中心落在视口中心的矩形——转 90 之后它的包围盒恰好铺满视口。
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
