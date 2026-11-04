#include "cpu.h"
#include "call_trace.h"
#include "file.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <unordered_map>

namespace smbd {
[[noreturn,gnu::noinline,gnu::cold]] void Memory::range_error(std::uint32_t address,std::size_t size) {
    throw std::runtime_error(std::format("Guest memory access at 0x{:08X}, {} bytes, outside the mapped regions",address,size));
}
std::byte* Memory::mapped_pointer(std::uint32_t address,std::size_t size) const noexcept {
    for(unsigned i=0;i<region_count_;++i) {
        const auto& region=regions_[i];
        const auto offset=static_cast<std::uint64_t>(address)-region.base;
        if(size<=region.bytes.size() && offset<=region.bytes.size()-size) return region.bytes.data()+offset;
    }
    auto found=dynamic_regions_.upper_bound(address);
    if(found!=dynamic_regions_.begin()) {
        --found;const auto offset=std::uint64_t(address)-found->first;
        if(size<=found->second.size() && offset<=found->second.size()-size) return found->second.data()+offset;
    }
    return nullptr;
}
void Memory::map_dynamic(std::uint32_t base,std::span<std::byte> bytes) {
    const auto end=std::uint64_t(base)+bytes.size();
    if(bytes.empty() || end>0x100000000ULL || dynamic_regions_.size()>=32768)
        throw std::runtime_error("Invalid or exhausted dynamic guest mappings");
    const auto overlaps=[&](std::uint32_t other,std::size_t size) {return base<std::uint64_t(other)+size && other<end;};
    if(overlaps(base_,bytes_.size())) throw std::runtime_error("Dynamic guest mapping overlaps RAM");
    for(unsigned i=0;i<region_count_;++i) if(overlaps(regions_[i].base,regions_[i].bytes.size()))
        throw std::runtime_error("Dynamic guest mapping overlaps a static mapping");
    auto next=dynamic_regions_.lower_bound(base);
    if(next!=dynamic_regions_.end() && next->first<end) throw std::runtime_error("Dynamic guest mappings overlap");
    if(next!=dynamic_regions_.begin()) {
        const auto previous=std::prev(next);
        if(std::uint64_t(previous->first)+previous->second.size()>base) throw std::runtime_error("Dynamic guest mappings overlap");
        if(std::uint64_t(previous->first)+previous->second.size()==base && previous->second.data()+previous->second.size()==bytes.data()) {
            base=previous->first;bytes={previous->second.data(),previous->second.size()+bytes.size()};dynamic_regions_.erase(previous);
        }
    }
    if(next!=dynamic_regions_.end() && end==next->first && bytes.data()+bytes.size()==next->second.data()) {
        bytes={bytes.data(),bytes.size()+next->second.size()};dynamic_regions_.erase(next);
    }
    dynamic_regions_.emplace(base,bytes);
}
void Memory::unmap_dynamic(std::uint32_t base,std::uint32_t size) {
    const auto end=std::uint64_t(base)+size;
    auto found=dynamic_regions_.upper_bound(base);
    if(found!=dynamic_regions_.begin()) --found;
    while(found!=dynamic_regions_.end() && found->first<end) {
        const auto start=found->first;const auto bytes=found->second;
        const auto limit=std::uint64_t(start)+bytes.size();
        if(limit<=base) {++found;continue;}
        found=dynamic_regions_.erase(found);
        if(start<base) dynamic_regions_.emplace(start,bytes.first(base-start));
        if(limit>end) {dynamic_regions_.emplace(static_cast<std::uint32_t>(end),bytes.subspan(end-start));break;}
    }
}
double frame_gain_interval(FloatingState& state,double gain,float interval) {
    if(interval==1) return gain;
    HostFloatingScope floating(state);
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Invalid continuous response coefficient");
    if(interval==0) return 0;
    return gain==1?1:-std::expm1(std::log1p(-gain)*interval);
}
double frame_gain_double(FloatingState& state,double gain) {
    return frame_gain_interval(state,gain,frame_step.delta);
}
__m128 frame_gain_interval(FloatingState& state,__m128 gain,float interval) {
    if(interval==1) return gain;
    return _mm_move_ss(gain,_mm_set_ss(static_cast<float>(frame_gain_interval(state,_mm_cvtss_f32(gain),interval))));
}
__m128 frame_gain(FloatingState& state,__m128 gain) {
    if(frame_step.delta==1) return gain;
    return _mm_move_ss(gain,_mm_set_ss(static_cast<float>(frame_gain_double(state,_mm_cvtss_f32(gain)))));
}
namespace {
struct WorldResponse {double value=0;std::uint16_t previous=0;bool initialized=false;unsigned kind=0;};
// A8060's four WORLD records at 73E930 have two signed 16-bit tilt angles.
thread_local std::array<WorldResponse,8> world_responses;
struct ClipStep {
    std::uint64_t rule;
    std::uint32_t after, bounds;
    float before, increment;
};
thread_local std::unordered_map<std::uint32_t,ClipStep> clip_steps;
thread_local std::uint64_t clip_pruned_rule=0;
// 2454B0 walks 0x9C-byte text boxes. 6BB50 passes the inner record to drawing.
constexpr std::uint32_t text_box_size=0x9C, text_box_inner=0x10;
constexpr std::uint32_t text_pose_fields[]={0x20,0x24,0x28,0x40,0x44,0x48,0x4C,0x50};
struct TextPose {
    std::array<std::uint32_t,std::size(text_pose_fields)> position;
    std::uint16_t roll;
    bool operator==(const TextPose&) const=default;
};
struct TextPoseSample {
    std::uint32_t box,descriptor;
    TextPose committed,draw;
};
thread_local std::vector<TextPoseSample> text_poses;
thread_local std::uint64_t text_pose_tick=0;
thread_local float text_pose_phase=0;

TextPose read_text_pose(Memory& memory,std::uint32_t box) {
    TextPose pose;
    for(unsigned i=0;i<std::size(text_pose_fields);++i)
        pose.position[i]=memory.load<std::uint32_t>(box+text_pose_fields[i]);
    pose.roll=memory.load<std::uint16_t>(box+0x60);
    return pose;
}
bool sample_text_pose() {
    return frame_step.phase>0 && frame_step.delta==frame_step.interval;
}
WorldResponse& world_response(std::uint32_t address) {
    if(address<0x73E930 || address>=0x73EA20 || (address-0x73E930)%0x3C>2 || (address&1))
        throw std::runtime_error("World response is outside the original tilt array");
    return world_responses[(address-0x73E930)/0x3C*2+((address-0x73E930)%0x3C)/2];
}
}
void frame_reset_world() {world_responses={};}
void reset_frame_history() {frame_reset_world();clip_steps.clear();text_poses.clear();}

void frame_text_box_update(Cpu& cpu,Memory& memory,void (*update)(Cpu&,Memory&,bool,bool)) {
    const bool fractional=sample_text_pose();
    if(!fractional || (memory.load<std::uint8_t>(0x892114)&0xA)) {
        update(cpu,memory,true,frame_step.logic);
        return;
    }
    const auto stack=cpu.registers[esp];
    const auto box=memory.load<std::uint32_t>(stack+4);
    update(cpu,memory,true,frame_step.logic);
    const auto descriptor=memory.load<std::uint32_t>(box+0x94);
    if(!descriptor) return;

    // Evaluate the original next placement on a private guest-stack record.
    // The real box's spring always compares two consecutive rule poses.
    const auto preview_stack=stack-0xB0;
    const auto shadow=preview_stack+0x10;
    std::memcpy(memory.access(shadow,text_box_size),memory.access(box,text_box_size),text_box_size);
    memory.store<std::uint32_t>(preview_stack,cpu.eip);
    memory.store<std::uint32_t>(preview_stack+4,shadow);
    struct MatrixRestore {
        void* address;
        std::array<std::byte,48> saved;
        ~MatrixRestore() {std::memcpy(address,saved.data(),saved.size());}
    } matrix{memory.access(0x6DD5A0,48),{}};
    std::memcpy(matrix.saved.data(),matrix.address,matrix.saved.size());
    HostFloatingScope host(cpu.floating);
    Cpu preview=cpu;
    preview.registers[esp]=preview_stack;
    preview.preempt=nullptr;
    preview.diagnostic=nullptr;
    {
        FloatingScope floating(preview.floating);
        update(preview,memory,false,true);
    }
    const auto from=read_text_pose(memory,box),to=read_text_pose(memory,shadow);
    TextPose draw=from;
    for(unsigned i=0;i<std::size(text_pose_fields);++i) {
        const auto start=std::bit_cast<float>(from.position[i]),end=std::bit_cast<float>(to.position[i]);
        draw.position[i]=std::bit_cast<std::uint32_t>(std::lerp(start,end,frame_step.phase));
    }
    const auto turn=static_cast<std::int16_t>(to.roll-from.roll);
    draw.roll=static_cast<std::uint16_t>(from.roll+static_cast<int>(turn*frame_step.phase));
    if(text_pose_tick!=frame_rule_serial || text_pose_phase!=frame_step.phase) {
        text_poses.clear();
        text_pose_tick=frame_rule_serial;
        text_pose_phase=frame_step.phase;
    }
    const auto entry=std::ranges::find(text_poses,box,&TextPoseSample::box);
    const TextPoseSample sample{box,descriptor,from,draw};
    if(entry==text_poses.end()) text_poses.push_back(sample);
    else *entry=sample;
}

void frame_text_box_draw(Cpu& cpu,Memory& memory,void (*draw)(Cpu&,Memory&)) {
    const auto inner=cpu.registers[esi];
    const auto box=inner-text_box_inner;
    if(!sample_text_pose() || text_pose_tick!=frame_rule_serial || text_pose_phase!=frame_step.phase ||
       (memory.load<std::uint8_t>(0x892114)&0xA)) {
        draw(cpu,memory);
        return;
    }
    const auto sample=std::ranges::find(text_poses,box,&TextPoseSample::box);
    if(sample==text_poses.end() || sample->descriptor!=memory.load<std::uint32_t>(box+0x94) ||
       sample->committed!=read_text_pose(memory,box)) {
        draw(cpu,memory);
        return;
    }
    const auto stack=cpu.registers[esp];
    const auto draw_stack=stack-0xB0,shadow=draw_stack+0x10;
    std::memcpy(memory.access(shadow,text_box_size),memory.access(box,text_box_size),text_box_size);
    for(unsigned i=0;i<std::size(text_pose_fields);++i)
        memory.store<std::uint32_t>(shadow+text_pose_fields[i],sample->draw.position[i]);
    memory.store<std::uint16_t>(shadow+0x60,sample->draw.roll);
    memory.store<std::uint32_t>(draw_stack,memory.load<std::uint32_t>(stack));
    cpu.registers[esp]=draw_stack;
    cpu.registers[esi]=shadow+text_box_inner;
    struct DrawStackRestore {
        Cpu& cpu;
        std::uint32_t inner;
        ~DrawStackRestore() {cpu.registers[esp]+=0xB0;cpu.registers[esi]=inner;}
    } restore{cpu,inner};
    draw(cpu,memory);
}

void frame_clip_reset(std::uint32_t clip) {clip_steps.erase(clip);}
void frame_clip_copy(std::uint32_t model) {
    const auto primary=clip_steps.find(model+0x40);
    if(primary==clip_steps.end()) frame_clip_reset(model+0x58);
    else {
        const auto step=primary->second;
        clip_steps.insert_or_assign(model+0x58,step);
    }
}
void frame_clip_commit(Memory& memory,std::uint32_t clip,std::uint32_t bounds,float before,float increment) {
    if(clip_pruned_rule!=frame_rule_serial) {
        // Keep the previous rule's entries available for reuse by live clips.
        std::erase_if(clip_steps,[](const auto& entry){return frame_rule_serial-entry.second.rule>1;});
        clip_pruned_rule=frame_rule_serial;
    }
    clip_steps.insert_or_assign(clip,ClipStep{frame_rule_serial,memory.load<std::uint32_t>(clip+0xC),bounds,before,increment});
}
std::uint32_t frame_clip_sample(Memory& memory,std::uint32_t model,bool secondary) {
    const auto clip=model+(secondary?0x58U:0x40U);
    const auto current=memory.load<std::uint32_t>(clip+0xC);
    if(frame_step.phase==0 || frame_step.delta!=frame_step.interval) return current;

    float before,increment;
    std::uint32_t bounds;
    if(frame_step.logic) {
        if(memory.load<std::uint16_t>(clip)&2) return current;
        before=std::bit_cast<float>(current);
        increment=std::bit_cast<float>(memory.load<std::uint32_t>(clip+8))*
                  std::bit_cast<float>(memory.load<std::uint32_t>(model+4));
        bounds=memory.load<std::uint32_t>(clip+2);
    } else {
        const auto step=clip_steps.find(clip);
        if(step==clip_steps.end() || step->second.rule!=frame_rule_serial || step->second.after!=current) return current;
        before=step->second.before;
        increment=step->second.increment;
        bounds=step->second.bounds;
    }
    // The pose is drawn before the rule advances the playhead. Sample that
    // same interval after advancement, holding its endpoint until a wrap.
    const auto sample=before+increment*frame_step.phase;
    const auto time=increment>0?std::min(sample,std::max(before,float(bounds>>16))):
                    std::max(sample,std::min(before,float(bounds&0xFFFF)));
    return std::bit_cast<std::uint32_t>(time);
}

void frame_reset_world(std::uint32_t address) {
    if((address-0x73E930)%0x3C) throw std::runtime_error("World initializer is not at a record boundary");
    world_response(address)={};world_response(address+2)={};
}
std::uint32_t frame_world_response(FloatingState& floating,Memory& memory,std::uint32_t address,std::uint32_t difference,bool replay) {
    auto& state=world_response(address);
    const auto delta=static_cast<std::int16_t>(difference);
    if(frame_step.delta==1 && !state.initialized) return static_cast<std::uint32_t>(delta>>2);
    HostFloatingScope host(floating);
    const auto current=memory.load<std::uint16_t>(address);
    if(!state.initialized || state.previous!=current || state.kind!=unsigned(replay))
        state.value=static_cast<std::int16_t>(current);
    const auto fraction=std::remainder(state.value-static_cast<std::int16_t>(current),65536.0);
    const auto gain=frame_step.delta==1?0.25:-std::expm1(std::log(0.75)*frame_step.delta);
    state.value=std::remainder(state.value+(double(delta)-fraction)*gain,65536.0);
    // Carry sub-unit angle precision between scenes. Truncating each small
    // response separately changes the resting angle as refresh rate increases.
    state.previous=static_cast<std::uint16_t>(static_cast<std::int32_t>(std::floor(state.value)));
    state.initialized=true;state.kind=unsigned(replay);
    return static_cast<std::uint16_t>(state.previous-current);
}
std::uint32_t frame_world_steering(FloatingState& floating,Memory& memory,std::uint32_t address,std::uint32_t difference,std::uint32_t original) {
    auto& state=world_response(address);
    if(frame_step.delta==1 && !state.initialized) return original;
    HostFloatingScope host(floating);
    const auto current=memory.load<std::uint16_t>(address);
    if(!state.initialized || state.previous!=current || state.kind!=2)
        state.value=static_cast<std::int16_t>(current);
    // A7F20 first approaches the input by .2, then damps the result by .8.
    // Fractionally iterating these separately changes the equilibrium. Compose
    // the original affine map first; retain sub-unit precision until the store.
    constexpr double gain=0.2f,decay=0.8f,retention=decay*(1-gain);
    const double target=static_cast<std::int16_t>(current)-static_cast<std::int16_t>(difference);
    const auto equilibrium=decay*gain*target/(1-retention);
    state.value=equilibrium+(state.value-equilibrium)*std::pow(retention,frame_step.delta);
    state.previous=static_cast<std::uint16_t>(static_cast<std::int32_t>(state.value));
    state.initialized=true;state.kind=2;
    return state.previous;
}
std::uint32_t frame_world_decay(FloatingState& floating,Memory& memory,std::uint32_t address,std::uint32_t original) {
    auto& state=world_response(address);
    if(frame_step.delta==1 && !state.initialized) return original;
    HostFloatingScope host(floating);
    const auto current=memory.load<std::uint16_t>(address);
    if(!state.initialized || state.previous!=current || state.kind!=3)
        state.value=static_cast<std::int16_t>(current);
    // A7D05's gain had already been scaled, but rounding the resulting angle
    // every scene still lost time-dependent precision. Carry it at the store.
    state.value*=std::pow(1-double(0.2f),frame_step.delta);
    state.previous=static_cast<std::uint16_t>(static_cast<std::int32_t>(state.value));
    state.initialized=true;state.kind=3;
    return state.previous;
}
__m128 frame_decay(FloatingState& state,__m128 retention) {
    if(frame_step.delta==1) return retention;
    HostFloatingScope floating(state);
    const auto r=_mm_cvtss_f32(retention);
    if(!std::isfinite(r) || r<0 || r>1) throw std::runtime_error("Invalid continuous damping coefficient");
    return _mm_move_ss(retention,_mm_set_ss(std::pow(r,frame_step.delta)));
}
__m128 frame_rotation_threshold(FloatingState& state,__m128 cosine,float interval) {
    if(interval==1) return cosine;
    HostFloatingScope floating(state);
    return _mm_move_ss(cosine,_mm_set_ss(static_cast<float>(std::cos(std::acos(_mm_cvtss_f32(cosine))*interval))));
}
void frame_quaternion_step(FloatingState& state,Memory& memory,std::uint32_t address,float interval,bool whole_tick) {
    if(interval==1) return;
    HostFloatingScope floating(state);
    std::array<double,4> q{};
    for(unsigned i=0;i<4;++i) q[i]=std::bit_cast<float>(memory.load<std::uint32_t>(address+4*i));
    const auto length=std::hypot(q[0],q[1],q[2]);
    if(!length) return;
    const auto scale=whole_tick?(interval>0?1.0/interval:0):interval;
    const auto angle=std::atan2(length,q[3])*scale;
    const auto factor=std::sin(angle)/length;
    for(unsigned i=0;i<3;++i) q[i]*=factor;
    q[3]=std::cos(angle);
    for(unsigned i=0;i<4;++i) memory.store<std::uint32_t>(address+4*i,std::bit_cast<std::uint32_t>(static_cast<float>(q[i])));
}
__m128 frame_damped_increment(FloatingState& state,__m128 velocity,float retention) {
    if(frame_step.delta==1) return velocity;
    HostFloatingScope floating(state);
    const double r=retention;
    if(!std::isfinite(r) || r<0 || r>1) throw std::runtime_error("Invalid continuous damping coefficient");
    // Fractional iterate of v *= r; p += v, using the already-decayed velocity.
    const double scale=r==1?frame_step.delta:r==0?0:r*std::expm1(-std::log(r)*frame_step.delta)/(1-r);
    return _mm_mul_ss(velocity,_mm_set_ss(static_cast<float>(scale)));
}
void frame_record_sample(FloatingState& state,Memory& memory,std::array<std::uint32_t,24>& saved,bool restore) {
    HostFloatingScope floating(state);
    const double back=frame_step.delta>0?std::clamp(double(frame_step.phase)/frame_step.delta,0.0,1.0):0;
    constexpr std::array offsets{4U,8U,12U,0x28U,0x2CU};
    for(unsigned player=0;player<4;++player) {
        const auto ball=0x738DF0U+player*0x1B0,world=0x73E930U+player*0x3C;
        for(unsigned i=0;i<offsets.size();++i) {
            if(restore) memory.store<std::uint32_t>(ball+offsets[i],saved[player*6+i]);
            else saved[player*6+i]=memory.load<std::uint32_t>(ball+offsets[i]);
        }
        if(restore) {memory.store<std::uint32_t>(world,saved[player*6+5]);continue;}
        saved[player*6+5]=memory.load<std::uint32_t>(world);
        const auto type=memory.load<std::uint8_t>(ball+3);
        if(!memory.load<std::uint8_t>(ball) || (type!=4 && type!=6 && type!=20 && type!=28 && type!=32)) continue;
        // The recorder writes uniform 60 Hz samples. A rule tick can fall inside
        // the current scene interval; sample that instant, then restore live poses.
        for(unsigned axis=0;axis<3;++axis) {
            const double now=std::bit_cast<float>(saved[player*6+axis]);
            const double previous=std::bit_cast<float>(memory.load<std::uint32_t>(ball+0x10+axis*4));
            memory.store<std::uint32_t>(ball+4+axis*4,std::bit_cast<std::uint32_t>(static_cast<float>(now+(previous-now)*back)));
        }
        const auto angle=[&](std::uint32_t address,std::int16_t movement) {
            const auto now=memory.load<std::uint16_t>(address);
            memory.store<std::uint16_t>(address,static_cast<std::uint16_t>(now-std::lround(movement*back)));
        };
        for(unsigned axis=0;axis<3;++axis)
            angle(ball+0x28+axis*2,std::bit_cast<std::int16_t>(memory.load<std::uint16_t>(ball+0x60+axis*2)));
        for(unsigned axis=0;axis<2;++axis)
            angle(world+axis*2,std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(memory.load<std::uint16_t>(world+axis*2)-memory.load<std::uint16_t>(world+4+axis*2))));
    }
}
void frame_ball_step(FloatingState& state,Memory& memory,std::uint32_t ball,std::array<std::uint32_t,3>& displacement) {
    const double dt=frame_step.delta;
    if(dt==1) return;
    HostFloatingScope floating(state);
    const auto value=[&](std::uint32_t address){return double(std::bit_cast<float>(memory.load<std::uint32_t>(address)));};
    std::array<double,3> velocity{},acceleration{},normal{};
    double normal_velocity=0,normal_acceleration=0,length=0;
    for(unsigned axis=0;axis<3;++axis) {
        velocity[axis]=std::bit_cast<float>(displacement[axis]);
        acceleration[axis]=dt?(value(ball+0x1C+axis*4)-velocity[axis])/dt:0;
        normal[axis]=value(ball+0x11C+axis*4);length+=normal[axis]*normal[axis];
        normal_velocity+=velocity[axis]*normal[axis];normal_acceleration+=acceleration[axis]*normal[axis];
    }
    double position_velocity=dt,position_force=dt*(dt+1)*0.5,velocity_force=dt;
    const bool grounded=(memory.load<std::uint32_t>(ball+0x128)&1) && length>0 && std::isfinite(length);
    if(grounded) {
        const auto loss=value(memory.load<std::uint32_t>(0x6C7EA8)==60?0x347BC8:0x3FAE78);
        // Fractional iterate of the original contact step:
        // p += v+a; v = (v+a)*(1-loss). This also preserves acceleration's
        // interaction with drag, rather than scaling the two independently.
        const auto gain=-std::expm1(std::log1p(-loss)*dt);
        position_velocity=gain/loss;
        position_force=(dt-(1-loss)*position_velocity)/loss;
        velocity_force=(1-loss)*position_velocity/(1-gain);
    }
    for(unsigned axis=0;axis<3;++axis) {
        const auto vn=grounded?normal[axis]*normal_velocity/length:velocity[axis];
        const auto an=grounded?normal[axis]*normal_acceleration/length:acceleration[axis];
        const auto vt=velocity[axis]-vn,at=acceleration[axis]-an;
        const auto distance=vn*dt+an*(dt*(dt+1)*0.5)+vt*position_velocity+at*position_force;
        displacement[axis]=std::bit_cast<std::uint32_t>(static_cast<float>(distance));
        if(grounded) memory.store<std::uint32_t>(ball+0x1C+axis*4,std::bit_cast<std::uint32_t>(static_cast<float>(velocity[axis]+an*dt+at*velocity_force)));
    }
}
// The original immediate frontend (31200..31462) supplies plain XYZ quads,
// sometimes with packed colour and UVs. Complete their shared visibility and
// screen-edge contracts before 313B0 submits them, including font batches.
void complete_screen_primitives(Memory& memory,bool ultrawide) {
    if(memory.load<std::uint32_t>(0x6DEF70U)!=8 || memory.load<std::uint32_t>(0x6DEF7CU)) return;
    const auto format=memory.load<std::uint32_t>(0x6DE3E0U);
    unsigned stride;
    switch(format) {
    case 0x200:stride=12;break;
    case 0xA00:stride=16;break;
    case 0x2200:stride=20;break;
    case 0x2A00:stride=24;break;
    case 0x6200:stride=28;break;
    default:return; // Normal/skinning layouts are scene geometry.
    }
    // Four vertices occupy stride DWORDs. Process each quad in a batch with
    // fixed scratch storage; there is no packet scan or deferred state.
    const auto words=memory.load<std::uint32_t>(0x6DEF6CU);
    if(!words || words%stride || words/stride>8192) return;
    struct Restore {unsigned status=_mm_getcsr();~Restore(){_mm_setcsr(status);}} restore;
    const auto begin=memory.load<std::uint32_t>(0x6DEF60U)-4*words;
    const bool deferred=memory.load<std::uint32_t>(0x6DEF74U)!=0;
    const auto total=deferred?memory.load<std::uint32_t>(0x6DEF78U):0;
    const bool clip=ultrawide && memory.load<std::uint8_t>(0x6C934CU) &&
        memory.load<std::uint32_t>(0x7196ECU) && memory.load<std::uint32_t>(0x6DE0D4U)==1 &&
        (deferred?total>=words:words<=0x7FF && memory.load<std::uint32_t>(begin-4)==(0x40001818U|(words<<18)));
    const float anchor=clip?static_cast<std::int16_t>(memory.load<std::uint16_t>(0x71B310U)):0;
    const float native_width=clip?memory.load<std::uint16_t>(memory.load<std::uint32_t>(0x6DD520U)+4):0;
    const auto visible=[&](std::uint32_t vertices) {
        // 74E70's original 16:9 transform scales around the UI anchor. Preserve
        // its visibility before widening, using every rotated corner. Vertex
        // layouts and batched outlines share this contract with single sprites.
        float minimum=0,maximum=0;
        for(unsigned i=0;i<4;++i) {
            const auto x=std::bit_cast<float>(memory.load<std::uint32_t>(vertices+i*stride));
            if(!std::isfinite(x)) return true;
            const float native=anchor+(x-anchor)*0.75f;
            if(!i) minimum=maximum=native;
            else {minimum=std::min(minimum,native);maximum=std::max(maximum,native);}
        }
        return maximum>=0 && minimum<=native_width;
    };
    const auto complete=[&](std::uint32_t begin) {
        std::array<std::array<std::uint32_t,7>,4> vertices{};
        for(unsigned i=0;i<4;++i) std::memcpy(vertices[i].data(),memory.access(begin+i*stride,stride),stride);
        const auto value=[&](unsigned i,unsigned component) {return std::bit_cast<float>(vertices[i][component]);};
        const float z=value(0,2);
        std::array<float,2> minimum{value(0,0),value(0,1)},maximum=minimum;
        for(unsigned i=0;i<4;++i) {
            if(value(i,2)!=z || !std::isfinite(z)) return;
            for(unsigned axis=0;axis<2;++axis) {
                const float v=value(i,axis);if(!std::isfinite(v)) return;
                minimum[axis]=std::min(minimum[axis],v);maximum[axis]=std::max(maximum[axis],v);
            }
        }
        if(minimum[0]==maximum[0] || minimum[1]==maximum[1]) return;
        // 2F790 uploads projection * the current 3x4 model/view to k96..99.
        // Use that same matrix and the SDK viewport; never infer camera dimensions
        // from a particular menu, sprite number or render-target allocation.
        std::array<std::array<float,4>,4> projection{},matrix{};
        std::array<std::array<float,4>,3> model{};
        std::memcpy(projection.data(),memory.access(0x6DDF28U,sizeof(projection)),sizeof(projection));
        std::memcpy(model.data(),memory.access(memory.load<std::uint32_t>(0x6DDEC4U),sizeof(model)),sizeof(model));
        bool identity=true;
        for(unsigned row=0;row<3;++row) for(unsigned col=0;col<4;++col) identity&=model[row][col]==float(row==col);
        for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col) {
            matrix[row][col]=projection[row][0]*model[0][col]+projection[row][1]*model[1][col]+projection[row][2]*model[2][col];
            if(col==3) matrix[row][col]+=projection[row][3];
        }
        if(matrix[0][1]!=0 || matrix[1][0]!=0 || matrix[2][0]!=0 || matrix[2][1]!=0 || matrix[3][0]!=0 || matrix[3][1]!=0) return;
        const float w=matrix[3][2]*z+matrix[3][3];if(!std::isfinite(w) || w<=0) return;
        const auto device=memory.load<std::uint32_t>(0x32B6A8U);
        std::array<std::uint32_t,4> viewport{};
        std::memcpy(viewport.data(),memory.access(device+0xEE0,sizeof(viewport)),sizeof(viewport));
        if(!viewport[2] || !viewport[3]) return;
        const auto screen=memory.load<std::uint32_t>(0x6DD520U);
        const float width=memory.load<std::uint16_t>(screen+4),height=memory.load<std::uint16_t>(screen+6);
        // SDK render-state mirrors: ZERO + destination ONE with ADD preserves
        // every colour/alpha channel. A plain quad in that state is a depth mask,
        // including masks smaller than their projection viewport.
        const bool depth_only=format==0x200 && memory.load<std::uint32_t>(0x32B4FCU)!=0 &&
            memory.load<std::uint32_t>(0x32B508U)==0 && memory.load<std::uint32_t>(0x32B50CU)==1 &&
            memory.load<std::uint32_t>(0x32B538U)==0x8006;
        const bool canvas=identity && memory.load<std::uint32_t>(0x6DE0D4U)==1 && viewport[0]==0 && viewport[1]==0 &&
            viewport[2]==width && viewport[3]==height && std::round(minimum[0])<=0 && std::round(maximum[0])>=width &&
            std::round(minimum[1])<=0 && std::round(maximum[1])>=height;
        struct Edge {float nominal,position,raster;};
        struct Axis {float scale,offset;Edge low,high;};
        std::array<Axis,2> axes{};
        bool changed=false;
        for(unsigned axis=0;axis<2;++axis) {
            auto& a=axes[axis];const float half=float(viewport[axis+2])/2,sign=axis?-1.0f:1.0f;
            a.scale=sign*half*matrix[axis][axis]/w;
            a.offset=float(viewport[axis])+half+sign*half*(matrix[axis][2]*z+matrix[axis][3])/w;
            if(!std::isfinite(a.scale) || !std::isfinite(a.offset) || a.scale==0) return;
            const auto p0=a.scale*minimum[axis]+a.offset,p1=a.scale*maximum[axis]+a.offset;
            const float first=float(viewport[axis]),last=first+float(viewport[axis+2]);
            const auto low=std::min(p0,p1),high=std::max(p0,p1);
            if(!canvas && !depth_only && (std::round(low)>first || std::round(high)<last)) return;
            const auto edge=[&](float p,float boundary,bool upper) {
                const auto raster=std::trunc((p+0.53125f)*16)/16-0.03125f;
                const auto snapped=std::round(boundary*16)/16+(upper?0.0625f:0);
                const auto aligned=snapped-0.03125f;
                const bool fit=depth_only || std::round(p)==boundary || raster==aligned || (canvas && (upper?p<boundary:p>boundary));
                changed|=fit && raster!=aligned;
                // Invert NV2A's signed truncation at the centre of its 1/16 cell.
                // This gives the same outward coverage at zero and at positive
                // viewport origins; subtracting one fixed offset does not.
                const auto position=snapped+(snapped>0?0.03125f:snapped<0?-0.03125f:0)-0.53125f;
                return fit?Edge{boundary,position,aligned}:Edge{p,p,raster};
            };
            a.low=edge(low,depth_only?low:first,false);a.high=edge(high,depth_only?high:last,true);
        }
        if(!changed) return;
        std::array<unsigned,4> corners{},corner{};unsigned mask=0;
        for(unsigned i=0;i<4;++i) {
            if((value(i,0)!=minimum[0] && value(i,0)!=maximum[0]) || (value(i,1)!=minimum[1] && value(i,1)!=maximum[1])) return;
            corner[i]=unsigned((value(i,0)==maximum[0])==(axes[0].scale>0))|
                (unsigned((value(i,1)==maximum[1])==(axes[1].scale>0))<<1);
            if(mask&(1U<<corner[i])) return;
            mask|=1U<<corner[i];corners[corner[i]]=i;
        }
        for(unsigned i=0;i<4;++i) if((corner[i]^corner[(i+1)%4])==3) return;
        const auto original=vertices;
        const unsigned colour=(format&0x800)?1:0,uv=3+colour;
        for(unsigned i=0;i<4;++i) {
            std::array<float,2> ratio{};
            for(unsigned axis=0;axis<2;++axis) {
                const auto& a=axes[axis];const auto& e=(corner[i]&(1U<<axis))?a.high:a.low;
                vertices[i][axis]=std::bit_cast<std::uint32_t>((e.position-a.offset)/a.scale);
                ratio[axis]=(e.raster-a.low.nominal)/(a.high.nominal-a.low.nominal);
            }
            const auto interpolate=[&](const auto& component) {
                const auto low=component(corners[0])+(component(corners[1])-component(corners[0]))*ratio[0];
                const auto high=component(corners[2])+(component(corners[3])-component(corners[2]))*ratio[0];
                return low+(high-low)*ratio[1];
            };
            // Reparameterize every UV set with the actual raster edges. This is
            // necessary for a scene copy to keep its texels aligned, not stretched.
            for(unsigned component=uv;component<stride/4;++component) {
                const auto v=interpolate([&](unsigned n){return std::bit_cast<float>(original[n][component]);});
                if(!std::isfinite(v)) return;
                vertices[i][component]=std::bit_cast<std::uint32_t>(v);
            }
            if(colour) {
                vertices[i][3]=0;
                for(unsigned channel=0;channel<4;++channel) {
                    const auto v=interpolate([&](unsigned n){return float((original[n][3]>>(8*channel))&255);});
                    vertices[i][3]|=std::uint32_t(std::clamp(std::round(v),0.0f,255.0f))<<(8*channel);
                }
            }
        }
        for(unsigned i=0;i<4;++i) std::memcpy(memory.access(begin+i*stride,stride),vertices[i].data(),stride);
    };
    unsigned kept=0;
    for(unsigned quad=0;quad<words/stride;++quad) {
        const auto source=begin+quad*stride*4;
        if(clip && !visible(source)) continue;
        complete(source);
        if(kept!=quad) std::memcpy(memory.access(begin+kept*stride*4,stride*4),memory.access(source,stride*4),stride*4);
        ++kept;
    }
    const auto retained=kept*stride;
    if(retained==words) return;
    // Compact actual primitives, preserving every attribute and all surrounding
    // GPU state. 31170 emits deferred batches later; immediate batches already
    // have their inline-array header. Both allow an empty begin/end sequence.
    if(deferred) memory.store<std::uint32_t>(0x6DEF78U,total-words+retained);
    else memory.store<std::uint32_t>(begin-4,0x40001818U|(retained<<18));
    memory.store<std::uint32_t>(0x6DEF6CU,retained);
    memory.store<std::uint32_t>(0x6DEF60U,begin+retained*4);
}
namespace {
struct CallSample {
    std::uint64_t visits=0;
    std::array<std::uint32_t,8> registers{},stack{};
    std::array<std::array<std::uint32_t,4>,8> pointees{};
};
bool trace_calls=false;
std::array<std::uint64_t,2> ui_draw_calls{};
std::uint64_t dropped_calls=0;
std::map<std::pair<std::uint32_t,std::uint32_t>,CallSample> call_samples;
void trace_call(const Cpu& cpu,const Memory& memory,std::uint32_t target) {
    const auto key=std::pair{cpu.eip,target};
    auto found=call_samples.find(key);
    if(found==call_samples.end()) {
        if(call_samples.size()>=4096) {++dropped_calls;return;}
        found=call_samples.emplace(key,CallSample{}).first;
        auto& sample=found->second;sample.registers=cpu.registers;
        // Sample ordinary RAM only; tracing must never read native device ports.
        for(unsigned reg=0;reg<8;++reg) {
            const auto address=cpu.registers[reg];
            if(address>=0x10000 && address<=0x04000000-sizeof(sample.pointees[reg]))
                std::memcpy(sample.pointees[reg].data(),memory.access(address,sizeof(sample.pointees[reg])),sizeof(sample.pointees[reg]));
        }
        const auto stack=cpu.registers[esp];
        if(stack>=0x10000 && stack<=0x04000000-sizeof(sample.stack))
            std::memcpy(sample.stack.data(),memory.access(stack,sizeof(sample.stack)),sizeof(sample.stack));
    }
    ++found->second.visits;
}
}
void enable_call_trace() {call_samples.clear();dropped_calls=0;ui_draw_calls={};trace_calls=true;}
void trace_ui_draw(Cpu&,Memory&) {if(trace_calls) ++ui_draw_calls[frame_step.logic?1:0];}
std::string call_trace_report(std::string_view executable_sha256) {
    std::ostringstream out;
    out<<"{\"format\":\"smbd-call-trace-v1\",\"ui_draw_calls\":{\"intermediate\":"<<ui_draw_calls[0]<<",\"logic\":"<<ui_draw_calls[1]<<"},\"executable_sha256\":"<<json(executable_sha256)
        <<",\"exhaustive\":false,\"dropped_calls\":"<<dropped_calls<<",\"calls\":[";
    const auto words=[&](const auto& values) {
        out<<'[';bool first=true;
        for(auto value:values) {if(!first) out<<',';first=false;out<<json(hex32(value));}
        out<<']';
    };
    bool first=true;
    for(const auto& [key,sample]:call_samples) {
        if(!first) out<<',';first=false;
        out<<"{\"site\":"<<json(hex32(key.first))<<",\"target\":"<<json(hex32(key.second))<<",\"visits\":"<<sample.visits<<",\"registers\":";
        words(sample.registers);out<<",\"stack\":";words(sample.stack);out<<",\"pointees\":[";
        for(unsigned reg=0;reg<8;++reg) {if(reg) out<<',';words(sample.pointees[reg]);}
        out<<"]}";
    }
    return out.str()+"]}";
}
std::uint32_t read_port(Cpu& cpu,std::uint16_t port,unsigned width) {
    if(!cpu.kernel || !cpu.kernel->read_port) throw NativeBoundary(cpu.eip,std::format("unbound {}-bit input port 0x{:04X}",width,port));
    HostFloatingScope host(cpu.floating);
    return cpu.kernel->read_port(cpu.kernel->context,port,width);
}
void write_port(Cpu& cpu,std::uint16_t port,unsigned width,std::uint32_t value) {
    if(!cpu.kernel || !cpu.kernel->write_port) throw NativeBoundary(cpu.eip,std::format("unbound {}-bit output port 0x{:04X}",width,port));
    HostFloatingScope host(cpu.floating);
    cpu.kernel->write_port(cpu.kernel->context,port,width,value);
}
void invoke_native(Cpu& cpu, Memory& memory, std::uint32_t address) {
    if(trace_calls) trace_call(cpu,memory,address);
    cpu.eip = address;
    if(address>0xFFF00000U && address<0xFFF02000U && !(address&15U)) {
        const auto ordinal=(address-0xFFF00000U)/16;
        if(!cpu.kernel || !cpu.kernel->entries[ordinal]) throw NativeBoundary(address,"kernel function is unbound");
        HostFloatingScope host(cpu.floating);
        cpu.kernel->entries[ordinal](cpu.kernel->context,cpu,memory);
        return;
    }
    const auto found = std::ranges::lower_bound(compiled_batch, address, {}, &CompiledFunction::address);
    if (found == compiled_batch.end() || found->address != address)
        throw NativeBoundary(address, "function is absent from the fixed native table");
    found->run(cpu, memory);
}
}
