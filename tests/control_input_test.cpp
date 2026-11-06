#include "../wine_input_hooks.cpp"
#include <cassert>
#include <tuple>

namespace {
std::vector<std::pair<uint32_t,uint32_t>> delivered;
float look_dx=0,look_dy=0;
void fake_key(void*,wl_keyboard*,uint32_t,uint32_t,uint32_t key,uint32_t state){delivered.emplace_back(key,state);}
void fake_button(void*,wl_pointer*,uint32_t,uint32_t,uint32_t key,uint32_t state){delivered.emplace_back(key,state);}
void fake_relative(void*,zwp_relative_pointer_v1*,uint32_t,uint32_t,wl_fixed_t dx,wl_fixed_t dy,wl_fixed_t,wl_fixed_t){look_dx=wl_fixed_to_double(dx);look_dy=wl_fixed_to_double(dy);}
}
int main(){
    installed=true;control_focused=true;control_keyboard=reinterpret_cast<wl_keyboard*>(1);control_display=reinterpret_cast<wl_display*>(1);control_pointer=reinterpret_cast<wl_pointer*>(1);
    original_key=fake_key;original_button=fake_button;original_relative=fake_relative;
    std::string error;auto now=std::chrono::steady_clock::now();
    assert(fo4hud_wine_control_input({KEY_W},100,0,0,error));
    assert(fo4hud_wine_control_busy());assert(delivered.empty());
    // Duration begins when Wine dispatches the press, not when the socket request arrived.
    dispatch_control_input(now+std::chrono::milliseconds(50));
    assert(delivered.size()==1&&delivered.back()==std::make_pair(uint32_t(KEY_W),1u));
    dispatch_control_input(now+std::chrono::milliseconds(120));assert(delivered.size()==1);
    dispatch_control_input(now+std::chrono::milliseconds(151));assert(delivered.size()==2&&delivered.back().second==0);
    assert(!fo4hud_wine_control_busy());
    assert(fo4hud_wine_control_input({KEY_A},500,0,0,error));dispatch_control_input(std::chrono::steady_clock::now());
    fo4hud_wine_control_cancel();dispatch_control_input(std::chrono::steady_clock::now());assert(delivered.back()==std::make_pair(uint32_t(KEY_A),0u));
    assert(fo4hud_wine_control_input({KEY_D},500,0,0,error));dispatch_control_input(std::chrono::steady_clock::now());
    physical_keys[KEY_D]=true;auto count=delivered.size();fo4hud_wine_control_cancel();dispatch_control_input(std::chrono::steady_clock::now());
    assert(delivered.size()==count); // Never release a key the user is physically holding.
    physical_keys[KEY_D]=false;
    assert(fo4hud_wine_control_input({},100,12,-7,error));dispatch_control_input(std::chrono::steady_clock::now());assert(look_dx==12&&look_dy==-7);
    assert(!fo4hud_wine_control_input({KEY_W},5001,0,0,error));assert(!fo4hud_wine_control_input({511},100,0,0,error));
    capture_keyboard=true;assert(!fo4hud_wine_control_input({KEY_W},100,0,0,error));capture_keyboard=false;
    assert(fo4hud_wine_control_input({KEY_W},100,0,0,error));dispatch_control_input(std::chrono::steady_clock::now()+std::chrono::seconds(3));
    assert(!fo4hud_wine_control_busy()); // A key press whose Wine queue stalled is discarded.
    control_focused=false;assert(!fo4hud_wine_control_input({KEY_W},100,0,0,error));
    installed=false;
}
