#pragma once
#include <immintrin.h>
#include <cstdint>

namespace smbd {
// Units are original 1/60-second steps. Only explicitly verified time-bearing
// operations use this context; geometry arithmetic and queue counts do not.
struct FrameStep {
    float delta=1,phase=0;
    bool logic=true;
    // A legacy controller may temporarily use delta=1. Presentation and
    // completion queries still need the actual scene interval.
    float interval=delta;
    bool capture_presentation=false;
    float previous_phase=0;
};
inline thread_local FrameStep frame_step;
inline thread_local std::uint64_t frame_rule_serial=0;

inline void advance_frame_step(float delta,float phase,bool logic) {
    const auto previous=frame_step.phase;
    frame_step={delta,phase,logic};
    frame_step.previous_phase=previous;
    if(logic) ++frame_rule_serial;
}

inline bool frame_continuous_ball(std::uint32_t type) {
    return type==4 || type==6 || type==8 || type==10 || type==20 || type==28 || type==32;
}
inline float frame_ball_interval(std::uint32_t type) {
    // Initialization and legacy state handlers still produce whole-tick
    // transforms, even when the surrounding scene interval is fractional.
    return frame_continuous_ball(type)?frame_step.delta:1.0f;
}

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
    // Quantize the SAME boundary at both ends of adjacent scenes. Rebuilding
    // the previous phase by subtracting rounded floats can cross an integer
    // threshold, adding or losing a turn unit on every subdivision.
    return tick*speed+phase(frame_step.phase)-phase(frame_step.previous_phase);
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
