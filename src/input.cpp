#include "input.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <stdexcept>
#include <format>

namespace smbd {
int pad_axis_binding(unsigned axis,bool positive) {return -int(1+axis*2+unsigned(positive));}
ControlBindings default_bindings() {
    constexpr int keys[]={SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_RETURN,SDL_SCANCODE_BACKSPACE,SDL_SCANCODE_LCTRL,SDL_SCANCODE_RCTRL,
        SDL_SCANCODE_SPACE,SDL_SCANCODE_LSHIFT,SDL_SCANCODE_E,SDL_SCANCODE_Q,SDL_SCANCODE_R,SDL_SCANCODE_F,SDL_SCANCODE_Z,SDL_SCANCODE_C,
        SDL_SCANCODE_A,SDL_SCANCODE_D,SDL_SCANCODE_S,SDL_SCANCODE_W,0,0,0,0};
    constexpr SDL_GamepadButton buttons[]={SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_LEFT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    ControlBindings result{};
    for(unsigned i=0;i<result.size();++i) result[i].keyboard=keys[i];
    for(unsigned i=0;i<std::size(buttons);++i) result[i].gamepad=int(buttons[i])+1;
    result[14].gamepad=pad_axis_binding(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,true);result[15].gamepad=pad_axis_binding(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,true);
    for(unsigned i=0;i<4;++i) {
        result[16+i*2].gamepad=pad_axis_binding(i,(i&1)!=0);
        result[17+i*2].gamepad=pad_axis_binding(i,(i&1)==0);
    }
    return result;
}
bool valid_key_binding(int key) {
    return key>=0 && key<SDL_SCANCODE_COUNT && key!=SDL_SCANCODE_ESCAPE && key!=SDL_SCANCODE_F1 && key!=SDL_SCANCODE_F8 && key!=SDL_SCANCODE_F9;
}
bool valid_pad_binding(int code) {
    if(code>=0) return code<=SDL_GAMEPAD_BUTTON_COUNT;
    if(code < -SDL_GAMEPAD_AXIS_COUNT*2) return false;
    const auto axis=(-code-1)/2;
    return axis<SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ((-code-1)&1)!=0;
}
std::string binding_name(int code,bool pad) {
    if(!code) return "Unbound";
    if(!pad) return valid_key_binding(code)?SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(code),SDL_KMOD_NONE,false)):"Unbound";
    if(!valid_pad_binding(code)) return "Unbound";
    if(code>0) {
        constexpr const char* names[]={"A / South","B / East","X / West","Y / North","Back","Guide","Start","Left stick click","Right stick click","Left shoulder","Right shoulder","D-pad up","D-pad down","D-pad left","D-pad right"};
        if(unsigned(code)<=std::size(names)) return names[code-1];
        const auto* name=SDL_GetGamepadStringForButton(SDL_GamepadButton(code-1));return name?name:"Unknown button";
    }
    constexpr const char* names[]={"Left stick left","Left stick right","Left stick up","Left stick down","Right stick left","Right stick right","Right stick up","Right stick down","Left trigger","Left trigger","Right trigger","Right trigger"};
    return names[-code-1];
}
void apply_keyboard(GamepadState& state,const ControlBindings& bindings,std::span<const bool> keys) {
    const auto held=[&](unsigned action) {const auto key=bindings[action].keyboard;return key>0 && std::size_t(key)<keys.size() && keys[key];};
    for(unsigned i=0;i<8;++i) {if(held(i)) state.buttons|=1U<<i;if(held(i+8)) state.analog[i]=255;}
    for(unsigned axis=0;axis<4;++axis) {
        const auto negative=held(16+axis*2),positive=held(17+axis*2);
        if(negative || positive) state.axes[axis]=static_cast<std::int16_t>((positive?32767:0)-(negative?32767:0));
    }
}
void apply_gamepad(GamepadState& state,const ControlBindings& bindings,std::span<const bool> buttons,std::span<const std::int16_t> axes) {
    std::array<int,4> sticks{};
    for(unsigned action=0;action<bindings.size();++action) {
        const auto code=bindings[action].gamepad;int strength=0;
        if(code>0 && std::size_t(code)<=buttons.size()) strength=buttons[code-1]?32767:0;
        else if(code<0 && valid_pad_binding(code)) {
            const auto index=unsigned(-code-1),axis=index/2;
            if(axis<axes.size()) strength=std::max(0,(index&1)?int(axes[axis]):-int(axes[axis]));
        }
        if(action<8) {if(strength>=16384) state.buttons|=1U<<action;}
        else if(action<16) state.analog[action-8]=static_cast<std::uint8_t>(std::min(255,strength*255/32767));
        else sticks[(action-16)/2]+=((action-16)&1)?strength:-strength;
    }
    for(unsigned axis=0;axis<4;++axis) state.axes[axis]=static_cast<std::int16_t>(std::clamp(sticks[axis],-32768,32767));
}
Input::Input(bool keyboard,bool pump_events,std::uint32_t device_id):keyboard_(keyboard),pump_events_(pump_events),device_id_(device_id) {
    if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());
    try {refresh();update();} catch(...) {
        for(auto device:devices_) if(device) SDL_CloseGamepad(device);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);throw;
    }
}
Input::~Input() {
    for(auto device:devices_) if(device) SDL_CloseGamepad(device);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
void Input::refresh() {
    for(unsigned port=0;port<devices_.size();++port) if(auto& device=devices_[port];device && !SDL_GamepadConnected(device)) {
        SDL_CloseGamepad(device);device=nullptr;rumble_[port]={};
    }
    int count=0;auto* ids=SDL_GetGamepads(&count);
    if(!ids) throw std::runtime_error(SDL_GetError());
    for(int i=0;i<count;++i) {
        if(device_id_ && ids[i]!=device_id_) continue;
        if(std::ranges::any_of(devices_,[&](auto device){return device && SDL_GetGamepadID(device)==ids[i];})) continue;
        const auto slot=std::ranges::find(devices_,nullptr);
        if(slot==devices_.end()) break;
        *slot=SDL_OpenGamepad(ids[i]);
        if(!*slot) {SDL_free(ids);throw std::runtime_error(SDL_GetError());}
    }
    SDL_free(ids);
}
void Input::event(const SDL_Event& event) {
    if(event.type==SDL_EVENT_GAMEPAD_ADDED || event.type==SDL_EVENT_GAMEPAD_REMOVED) refresh();
    if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit_requested=true;
}
void Input::update() {
    const auto now=SDL_GetTicksNS();
    if(updated_ && now-updated_<8000000) return;
    updated_=now;
    if(pump_events_) {SDL_Event value;while(SDL_PollEvent(&value)) event(value);}
    int key_count=0;const auto* keys=SDL_GetKeyboardState(&key_count);
    for(unsigned port=0;port<4;++port) {
        GamepadState next;next.packet=states_[port].packet;
        if(auto* device=devices_[port];device && SDL_GamepadConnected(device)) {
            // Xbox feedback has no duration. Renew SDL's finite effect until
            // the game replaces it or explicitly sets both motors to zero.
            auto& effect=rumble_[port];
            if(effect.renew && now>=effect.renew) {
                if(!SDL_RumbleGamepad(device,effect.low,effect.high,1000)) throw std::runtime_error(SDL_GetError());
                effect.renew=now+500000000;
            }
            std::array<bool,SDL_GAMEPAD_BUTTON_COUNT> buttons{};std::array<std::int16_t,SDL_GAMEPAD_AXIS_COUNT> axes{};
            for(unsigned i=0;i<buttons.size();++i) buttons[i]=SDL_GetGamepadButton(device,SDL_GamepadButton(i));
            for(unsigned i=0;i<axes.size();++i) axes[i]=SDL_GetGamepadAxis(device,SDL_GamepadAxis(i));
            apply_gamepad(next,bindings_,buttons,axes);
        }
        if(port==0 && keyboard_ && !capture_keyboard_) {
            apply_keyboard(next,bindings_,{keys,static_cast<std::size_t>(key_count)});
        }
        if(capture_all_) {next={};next.packet=states_[port].packet;}
        if(next!=states_[port]) ++next.packet;
        states_[port]=next;
    }
}
std::uint32_t Input::connected() const {
    std::uint32_t mask=keyboard_?1:0;
    for(unsigned port=0;port<4;++port) if(devices_[port] && SDL_GamepadConnected(devices_[port])) mask|=1U<<port;
    return mask;
}
const GamepadState& Input::state(unsigned port) const {return states_.at(port);}
bool Input::has_rumble(unsigned port) const {
    return port<4 && devices_[port] && SDL_GetBooleanProperty(SDL_GetGamepadProperties(devices_[port]),
        SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false);
}
std::uint32_t Input::rumble(unsigned port,std::uint16_t low,std::uint16_t high) {
    if(port>=4 || !(connected()&(1U<<port))) return 1167;
    if(!has_rumble(port)) return 50;
    if(!SDL_RumbleGamepad(devices_[port],low,high,1000)) return 31;
    rumble_[port]={low,high,(low || high)?SDL_GetTicksNS()+500000000:0};return 0;
}
std::string check_input() {
    struct VirtualDevice {
        SDL_JoystickID id=0;SDL_Joystick* joystick=nullptr;
        std::uint16_t low=0,high=0;
        VirtualDevice() {
            if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());
            SDL_VirtualJoystickDesc description{};SDL_INIT_INTERFACE(&description);
            description.type=SDL_JOYSTICK_TYPE_GAMEPAD;description.naxes=SDL_GAMEPAD_AXIS_COUNT;
            description.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;description.axis_mask=(1U<<SDL_GAMEPAD_AXIS_COUNT)-1;
            description.button_mask=(1U<<SDL_GAMEPAD_BUTTON_COUNT)-1;description.name="Super Monkey Ball Deluxe offline input diagnostic";
            description.userdata=this;
            description.Rumble=[](void* context,Uint16 low,Uint16 high) {
                auto& device=*static_cast<VirtualDevice*>(context);device.low=low;device.high=high;return true;
            };
            id=SDL_AttachVirtualJoystick(&description);
            if(id) joystick=SDL_OpenJoystick(id);
            if(!joystick) {
                const std::string error=SDL_GetError();if(id) SDL_DetachVirtualJoystick(id);
                SDL_QuitSubSystem(SDL_INIT_GAMEPAD);throw std::runtime_error(error);
            }
        }
        void detach() {if(joystick) {SDL_CloseJoystick(joystick);joystick=nullptr;}if(id) {SDL_DetachVirtualJoystick(id);id=0;}}
        ~VirtualDevice() {detach();SDL_QuitSubSystem(SDL_INIT_GAMEPAD);}
        void axis(unsigned index,Sint16 value) {if(!SDL_SetJoystickVirtualAxis(joystick,index,value)) throw std::runtime_error(SDL_GetError());}
        void button(SDL_GamepadButton button,bool value) {if(!SDL_SetJoystickVirtualButton(joystick,button,value)) throw std::runtime_error(SDL_GetError());}
    } device;
    device.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);device.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    Input input(false,true,device.id);
    unsigned cases=0;
    const auto expect=[&](bool valid,std::string_view name) {if(!valid) throw std::runtime_error("SDL3 input mismatch: "+std::string(name));++cases;};
    const auto sample=[&]() -> const GamepadState& {SDL_Delay(9);input.update();return input.state(0);};
    expect(input.connected()==1,"virtual device connection");
    constexpr SDL_GamepadButton digital[]={SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_LEFT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    constexpr SDL_GamepadButton analog[]={SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for(unsigned i=0;i<8;++i) {
        device.button(digital[i],true);expect(sample().buttons==(1U<<i),"digital button");
        device.button(digital[i],false);expect(sample().buttons==0,"digital release");
    }
    for(unsigned i=0;i<6;++i) {
        device.button(analog[i],true);expect(sample().analog[i]==255,"analog button");
        device.button(analog[i],false);expect(sample().analog[i]==0,"analog release");
    }
    for(unsigned i=0;i<4;++i) for(const Sint16 value:{Sint16(-32768),Sint16(32767)}) {
        device.axis(i,value);const auto expected=i&1?std::clamp(-int(value),-32768,32767):int(value);
        expect(sample().axes[i]==expected,"stick direction and range");
        device.axis(i,0);sample();
    }
    for(unsigned i=0;i<2;++i) {
        device.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER+i,32767);expect(sample().analog[i+6]==255,"trigger range");
        device.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER+i,-32768);expect(sample().analog[i+6]==0,"trigger release");
    }
    const auto packet=input.state(0).packet;expect(sample().packet==packet,"unchanged packet number");
    device.button(SDL_GAMEPAD_BUTTON_SOUTH,true);expect(sample().packet==packet+1,"changed packet number");
    auto bindings=default_bindings();bindings[8].gamepad=SDL_GAMEPAD_BUTTON_NORTH+1;input.bindings(bindings);
    expect(sample().analog[0]==0,"old controller binding released");
    device.button(SDL_GAMEPAD_BUTTON_NORTH,true);expect(sample().analog[0]==255,"controller button remapping");
    device.button(SDL_GAMEPAD_BUTTON_NORTH,false);device.button(SDL_GAMEPAD_BUTTON_SOUTH,false);
    bindings[17].gamepad=pad_axis_binding(SDL_GAMEPAD_AXIS_RIGHTX,false);input.bindings(bindings);
    device.axis(SDL_GAMEPAD_AXIS_RIGHTX,-24576);expect(sample().axes[0]==24576,"controller axis remapping");
    device.axis(SDL_GAMEPAD_AXIS_RIGHTX,0);bindings[8].gamepad=0;input.bindings(bindings);
    device.button(SDL_GAMEPAD_BUTTON_NORTH,true);expect(sample().analog[0]==0,"unbound controller action");
    std::array<bool,SDL_SCANCODE_COUNT> keys{};bindings[8].keyboard=SDL_SCANCODE_J;keys[SDL_SCANCODE_SPACE]=true;
    GamepadState keyboard{};apply_keyboard(keyboard,bindings,keys);expect(keyboard.analog[0]==0,"old keyboard binding released");
    keys[SDL_SCANCODE_J]=true;apply_keyboard(keyboard,bindings,keys);expect(keyboard.analog[0]==255,"keyboard remapping");
    keys[SDL_SCANCODE_A]=keys[SDL_SCANCODE_D]=true;keyboard.axes[0]=1234;apply_keyboard(keyboard,bindings,keys);
    expect(keyboard.axes[0]==0,"opposite keyboard directions cancel");
    expect(!valid_key_binding(SDL_SCANCODE_ESCAPE) && !valid_key_binding(SDL_SCANCODE_F1) && !valid_key_binding(SDL_SCANCODE_F8) && !valid_key_binding(SDL_SCANCODE_F9),"reserved shortcuts");
    expect(!valid_pad_binding(-1000) && !valid_pad_binding(pad_axis_binding(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,false)),"invalid controller bindings");
    expect(input.has_rumble(0),"rumble capability");
    expect(input.rumble(0,12345,54321)==0 && device.low==12345 && device.high==54321,"rumble delivery");
    const auto rumble_until=SDL_GetTicks()+1200;
    while(SDL_GetTicks()<rumble_until) sample();
    expect(device.low==12345 && device.high==54321,"rumble persists until replaced");
    expect(input.rumble(0,0,0)==0 && device.low==0 && device.high==0,"explicit rumble stop");
    device.detach();sample();expect(input.connected()==0,"hot removal");
    expect(input.rumble(0,0,0)==1167,"disconnected rumble status");
    return std::format("{{\"format\":\"smbd-input-check-v1\",\"backend\":\"SDL3\",\"passed_cases\":{}}}",cases);
}
}
