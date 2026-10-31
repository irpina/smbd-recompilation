#pragma once
#include <immintrin.h>
#include <cstdint>

namespace smbd {
// Units are original 1/60-second steps. Only explicitly verified time-bearing
// operations use this context; geometry arithmetic and queue counts do not.
struct FrameStep {
    float delta=1,phase=0;
    bool logic=true;
};
inline thread_local FrameStep frame_step;

// Keep the original integer clock visible to game code while sampling its
// motion between rule ticks. External resets and endpoint clamps resync it.
struct FrameCounter {
    double value=0;
    std::uint32_t previous=0;
    bool initialized=false;
    double sample(std::uint32_t count,int speed) {
        if(!initialized || count!=previous+(frame_step.logic?speed:0)) value=static_cast<std::int32_t>(count);
        else value+=double(frame_step.delta)*speed;
        previous=count;initialized=true;
        return value;
    }
};

inline std::int32_t frame_integer_increment(std::int32_t speed) {
    if(frame_step.delta==1) return speed;
    const auto tick=frame_step.logic?1:0;
    const auto phase=[&](float value){return _mm_cvttss_si32(_mm_set_ss(value*speed));};
    return tick*speed+phase(frame_step.phase)-phase(frame_step.phase-frame_step.delta+tick);
}

inline __m128 frame_increment(__m128 value) {
    return frame_step.delta==1?value:_mm_mul_ss(value,_mm_set_ss(frame_step.delta));
}
inline __m128 frame_position(__m128 left,__m128 right,__m128 acceleration,bool velocity_left) {
    if(frame_step.delta==1) return _mm_add_ss(left,right);
    const auto position=velocity_left?right:left,velocity=velocity_left?left:right;
    const auto dt=_mm_set_ss(frame_step.delta);
    // The original uses semi-implicit Euler: v += a; p += v. This continuous
    // form composes to that exact one-step displacement under constant force,
    // instead of changing gravity's displacement when a step is subdivided.
    const auto correction=_mm_mul_ss(acceleration,_mm_set_ss(frame_step.delta*(1-frame_step.delta)*0.5f));
    const auto result=_mm_add_ss(position,_mm_add_ss(_mm_mul_ss(velocity,dt),correction));
    return _mm_move_ss(left,result);
}
}
