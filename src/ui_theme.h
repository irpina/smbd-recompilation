#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace smbd::theme {
inline std::array<std::uint8_t,4> wood_pixel(unsigned x,unsigned y) {
    const float curve=y+3*std::sin(x*.013f)+2*std::sin(x*.031f+y*.025f);
    const float grain=5*std::sin(curve*.65f)+3*std::sin(curve*2.7f)+8*std::sin(curve*.083f);
    const auto noise=((x*1664525U+y*1013904223U)^(x*y*2654435761U))>>28;
    return {static_cast<std::uint8_t>(107+grain+noise*.2f),static_cast<std::uint8_t>(70+grain*.72f+noise*.15f),
        static_cast<std::uint8_t>(35+grain*.38f),255};
}
}
