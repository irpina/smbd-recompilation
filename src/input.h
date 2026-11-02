#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>

struct SDL_Gamepad;
union SDL_Event;
namespace smbd {
struct GamepadState {
    std::uint32_t packet=0;
    std::uint16_t buttons=0;
    std::array<std::uint8_t,8> analog{};
    std::array<std::int16_t,4> axes{};
    bool operator==(const GamepadState&) const = default;
};
struct ControlAction {std::string_view id,label;};
inline constexpr std::array<ControlAction,24> control_actions{{
    {"up","D-pad up"},{"down","D-pad down"},{"left","D-pad left"},{"right","D-pad right"},
    {"start","Start / Pause"},{"back","Back"},{"left_click","Left stick click"},{"right_click","Right stick click"},
    {"a","A / Confirm"},{"b","B / Cancel"},{"x","X"},{"y","Y"},{"black","Black"},{"white","White"},
    {"left_trigger","Left trigger"},{"right_trigger","Right trigger"},
    {"left_stick_left","Left stick left"},{"left_stick_right","Left stick right"},{"left_stick_down","Left stick down"},{"left_stick_up","Left stick up"},
    {"right_stick_left","Right stick left"},{"right_stick_right","Right stick right"},{"right_stick_down","Right stick down"},{"right_stick_up","Right stick up"}}};
// Zero is unbound. Positive pad codes select a button; negative codes select
// an axis and direction. These are stable SDL gamepad inputs, not device IDs.
struct ControlBinding {int keyboard=0,gamepad=0;bool operator==(const ControlBinding&) const = default;};
using ControlBindings=std::array<ControlBinding,control_actions.size()>;
ControlBindings default_bindings();
bool valid_key_binding(int);
bool valid_pad_binding(int);
int pad_axis_binding(unsigned axis,bool positive);
std::string binding_name(int code,bool gamepad);
void apply_keyboard(GamepadState&,const ControlBindings&,std::span<const bool> keys);
void apply_gamepad(GamepadState&,const ControlBindings&,std::span<const bool> buttons,std::span<const std::int16_t> axes);
class Input {
public:
    explicit Input(bool keyboard=true,bool pump_events=true,std::uint32_t device_id=0);
    ~Input();
    Input(const Input&)=delete;
    Input& operator=(const Input&)=delete;
    void event(const SDL_Event&);
    void update();
    void keyboard(bool enabled) {keyboard_=enabled;updated_=0;}
    void capture_keyboard(bool captured) {capture_keyboard_=captured;}
    void capture(bool captured) {if(capture_all_!=captured) {capture_all_=captured;updated_=0;}}
    void bindings(const ControlBindings& value) {if(bindings_!=value) {bindings_=value;updated_=0;}}
    std::uint32_t connected() const;
    const GamepadState& state(unsigned port) const;
    bool has_rumble(unsigned port) const;
    std::uint32_t rumble(unsigned port,std::uint16_t low,std::uint16_t high);
    bool quit_requested=false;
private:
    void refresh();
    bool keyboard_,pump_events_;
    bool capture_keyboard_=false;
    bool capture_all_=false;
    std::array<SDL_Gamepad*,4> devices_{};
    struct Rumble {std::uint16_t low=0,high=0;std::uint64_t renew=0;};
    std::array<Rumble,4> rumble_{};
    std::array<GamepadState,4> states_{};
    std::uint64_t updated_=0;
    std::uint32_t device_id_=0;
    ControlBindings bindings_=default_bindings();
};
std::string check_input();
}
