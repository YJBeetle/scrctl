#pragma once

#include <SDL.h>

#include "app/ViewGeom.h"

namespace scrctl::app {

/// 先在裁剪区域内水平翻转，再旋转并绘制到视口。
/// 翻转交给 SDL 的纹理坐标处理，不复制或修改源像素。
/// 独立函数用于四角着色帧的无窗口回读测试。
/// SDL_RenderCopyEx 使用顺时针角度，与 Crop 的转正角度一致。
/// dst 是旋转前的矩形，宽高方向与裁剪区域一致、中心位于内容矩形中心；90/270 度
/// 需交换目标宽高，旋转后的包围盒才铺满内容区域。未给 output 时按源像素绘制。
inline bool draw_rotated(SDL_Renderer *renderer, SDL_Texture *texture, const Crop &crop,
                         int degrees, bool horizontal_flip = false,
                         const ContentRect *output = nullptr) {
    const SDL_Rect src { crop.x, crop.y, crop.w, crop.h };
    int vw = 0, vh = 0;
    viewport_size(crop, degrees, vw, vh);
    // 宽高奇偶性不同的 90/270 度裁剪需要半像素中心。整数 dst 截断会造成
    // 整幅偏移一像素和一列黑边，使实际画面与触摸使用的视口定义不一致。
    const bool swapped = degrees == 90 || degrees == 270;
    const float width = static_cast<float>(output ? (swapped ? output->h : output->w) : crop.w);
    const float height = static_cast<float>(output ? (swapped ? output->w : output->h) : crop.h);
    const float center_x = output ? output->x + output->w / 2.0f : vw / 2.0f;
    const float center_y = output ? output->y + output->h / 2.0f : vh / 2.0f;
    const SDL_FRect dst { center_x - width / 2.0f, center_y - height / 2.0f, width, height };
    // SDL2 软件路径在 +270 度的浮点边界舍入上仍可能偏移奇偶不同的裁剪。
    // 使用等价的 -90 度绘制；逻辑角度与输入逆变换继续使用原始 270。
    const double angle = degrees == 270 ? -90.0 : static_cast<double>(degrees);
    return SDL_RenderCopyExF(renderer, texture, &src, &dst, angle, nullptr,
                             horizontal_flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE) == 0;
}

}  // namespace scrctl::app
