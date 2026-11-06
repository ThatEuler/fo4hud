#include "game_ui.h"
#include "game_control.h"
#include "control_memory.h"
#include "logging.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace fo4control;
using Advance = uintptr_t (__attribute__((ms_abi)) *)(uintptr_t, float, uintptr_t);
constexpr uintptr_t main_table=0x25B1258, pause_table=0x253B0F8;
std::atomic<uintptr_t> ui_image{0};
std::atomic<bool> installed{false};
std::mutex hook_mutex;
Advance main_advance=nullptr, pause_advance=nullptr;
template<class T>T value(uintptr_t a) { T v{}; if(!read(a,&v,sizeof(v)))throw std::runtime_error("UI memory unavailable");return v; }
std::string pooled(uintptr_t field) {
    auto entry=value<uintptr_t>(field);
    for(int i=0;entry && i<8;++i){
        auto flags=value<uint16_t>(entry+8);
        if(flags&0x4000){entry=value<uintptr_t>(entry+0x10);continue;}
        if(flags&0x8000)return {};
        auto n=value<uint32_t>(entry+0x10);if(n>256)throw std::runtime_error("Invalid menu name");
        std::string s(n,'\0');if(n&&!read(entry+0x18,s.data(),n))throw std::runtime_error("Menu name unavailable");return s;
    }return {};
}
void signature(uintptr_t image,uintptr_t rva,std::initializer_list<unsigned char> bytes) {
    std::vector<unsigned char> actual(bytes.size());
    if(!read(image+rva,actual.data(),actual.size())||!std::equal(actual.begin(),actual.end(),bytes.begin()))
        throw std::runtime_error("UI function signature mismatch for Fallout 4 1.11.240");
}
struct Menu { uintptr_t pointer, table; std::string name; };
std::vector<Menu> menus(uintptr_t image) {
    const auto ui=value<uintptr_t>(image+0x30E8930);
    if(!ui)return {};
    const auto data=value<uintptr_t>(ui+0x190);const auto n=value<uint32_t>(ui+0x1A0);
    if(n>64 || (n&&!data))throw std::runtime_error("UI menu stack mismatch");
    std::vector<Menu> result;
    for(uint32_t i=0;i<n;++i){auto m=value<uintptr_t>(data+i*8);if(m)result.push_back({m,value<uintptr_t>(m),pooled(m+0x50)});}
    if(data!=value<uintptr_t>(ui+0x190)||n!=value<uint32_t>(ui+0x1A0))throw std::runtime_error("UI changed during observation; retry");
    return result;
}
std::filesystem::path save_directory() {
    if(auto* p=std::getenv("FO4HUD_SAVE_DIR"))return p;
    std::filesystem::path prefix;
    if(auto* p=std::getenv("STEAM_COMPAT_DATA_PATH"))prefix=std::filesystem::path(p)/"pfx";
    else {auto* home=std::getenv("HOME");if(!home)throw std::runtime_error("Save directory unavailable");prefix=std::filesystem::path(home)/".local/share/Steam/steamapps/compatdata/377160/pfx";}
    return prefix/"drive_c/users/steamuser/Documents/My Games/Fallout4/Saves";
}
bool valid_save_name(const std::string& s) {
    return !s.empty() && s.size()<=240 && s!="." && s!=".." &&
        s.find_first_of("/\\:\r\n") == std::string::npos && s.find('\0')==std::string::npos &&
        std::filesystem::path(s).extension()==".fos";
}
void validate_save(const std::string& s) {
    if(!valid_save_name(s))throw std::runtime_error("save_name must be a .fos filename from list_saves, without directories");
    auto path=save_directory()/s;auto st=std::filesystem::symlink_status(path);
    if(!std::filesystem::is_regular_file(st))throw std::runtime_error("Save file missing or not a regular file");
    std::ifstream f(path,std::ios::binary);char magic[12]{};f.read(magic,sizeof(magic));
    if(f.gcount()!=12 || std::memcmp(magic,"FO4_SAVEGAME",11))throw std::runtime_error("File is not a Fallout 4 save");
}
bool exchange(uintptr_t slot,uintptr_t expected,uintptr_t replacement) {
    const size_t page=sysconf(_SC_PAGESIZE);void* start=reinterpret_cast<void*>(slot&~uintptr_t(page-1));
    // These slots live in the game's read-only vtable section.
    if(mprotect(start,page,PROT_READ|PROT_WRITE))return false;
    bool okay=__atomic_compare_exchange_n(reinterpret_cast<uintptr_t*>(slot),&expected,replacement,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    return mprotect(start,page,PROT_READ)==0 && okay;
}
uintptr_t advance(Advance original,uintptr_t menu,float delta,uintptr_t time) {
    auto result=original?original(menu,delta,time):0;
    static thread_local bool in_action=false;
    if(installed.load(std::memory_order_acquire)&&!in_action){
        in_action=true;fo4hud_control_ui_tick(ui_image.load(),menu);in_action=false;
    }
    return result;
}
uintptr_t __attribute__((ms_abi)) main_hook(uintptr_t m,float d,uintptr_t t){return advance(main_advance,m,d,t);}
uintptr_t __attribute__((ms_abi)) pause_hook(uintptr_t m,float d,uintptr_t t){return advance(pause_advance,m,d,t);}
void invoke(uintptr_t image,uintptr_t menu,bool main,uint32_t action,int32_t argument=-1) {
    // IMenu::Call's GFxFunctionHandler::Params: argument count +40, userData +48.
    alignas(8) unsigned char params[56]{},result[32]{},arg[32]{};
    uintptr_t result_ptr=reinterpret_cast<uintptr_t>(result);std::memcpy(params,&result_ptr,8);
    std::memcpy(params+48,&action,4);
    if(argument>=0){
        const uint32_t type=3,count=1;uintptr_t ptr=reinterpret_cast<uintptr_t>(arg);
        std::memcpy(arg+8,&type,4);std::memcpy(arg+16,&argument,4);
        std::memcpy(params+32,&ptr,8);std::memcpy(params+40,&count,4);
    }
    using Invoke=void(__attribute__((ms_abi)) *)(uintptr_t,void*);
    const auto rva=main?0x1070050:0xAD27B0;
    if(main)signature(image,rva,{0x40,0x55,0x53,0x41,0x54,0x41,0x56,0x48});
    else signature(image,rva,{0x40,0x55,0x53,0x56,0x48,0x8B,0xEC,0x48});
    reinterpret_cast<Invoke>(image+rva)(menu,params);
}
}

bool fo4hud_ui_install(uintptr_t image) {
    std::lock_guard<std::mutex> lock(hook_mutex);
    if(installed.load())return ui_image.load()==image;
    if(!image)return false;
    try {
        auto m=value<uintptr_t>(image+main_table+32),p=value<uintptr_t>(image+pause_table+32);
        if(m!=image+0x106FA20||p!=image+0xAD2630)return false;
        main_advance=reinterpret_cast<Advance>(m);pause_advance=reinterpret_cast<Advance>(p);ui_image.store(image);
        if(!exchange(image+main_table+32,m,reinterpret_cast<uintptr_t>(main_hook)))return false;
        if(!exchange(image+pause_table+32,p,reinterpret_cast<uintptr_t>(pause_hook))){exchange(image+main_table+32,reinterpret_cast<uintptr_t>(main_hook),m);return false;}
        installed.store(true,std::memory_order_release);fo4hud_log("control","main/pause menu control hooks installed");return true;
    }catch(...){return false;}
}
void fo4hud_ui_uninstall() {
    std::lock_guard<std::mutex> lock(hook_mutex);if(!installed.exchange(false))return;
    auto image=ui_image.load();
    exchange(image+main_table+32,reinterpret_cast<uintptr_t>(main_hook),reinterpret_cast<uintptr_t>(main_advance));
    exchange(image+pause_table+32,reinterpret_cast<uintptr_t>(pause_hook),reinterpret_cast<uintptr_t>(pause_advance));
}
bool fo4hud_ui_active(){
    if(!installed.load(std::memory_order_acquire))return false;
    auto image=ui_image.load();uintptr_t main=0,pause=0;
    return read(image+main_table+32,&main,sizeof(main)) &&
        read(image+pause_table+32,&pause,sizeof(pause)) &&
        main==reinterpret_cast<uintptr_t>(main_hook) && pause==reinterpret_cast<uintptr_t>(pause_hook);
}
Json fo4hud_ui_state(uintptr_t image) {
    auto j=object();set(j.get(),"ui_hook_active",fo4hud_ui_active());
    auto* list=json_object_new_array();set(j.get(),"menus",list);
    bool main=false,pause=false,loading=false;
    if(image)for(const auto& m:menus(image)){
        json_object_array_add(list,json_object_new_string(m.name.c_str()));
        main|=m.name=="MainMenu";pause|=m.name=="PauseMenu";loading|=m.name=="LoadingMenu";
    }
    set(j.get(),"main_menu",main);set(j.get(),"pause_menu",pause);set(j.get(),"loading",loading);return j;
}
Json fo4hud_ui_saves() {
    struct Save {std::string name;uintmax_t size;std::filesystem::file_time_type modified;};std::vector<Save> saves;
    auto directory=save_directory();
    if(std::filesystem::exists(directory))for(const auto& entry:std::filesystem::directory_iterator(directory)){
        if(!valid_save_name(entry.path().filename().string())||!std::filesystem::is_regular_file(entry.symlink_status()))continue;
        saves.push_back({entry.path().filename().string(),entry.file_size(),entry.last_write_time()});
        if(saves.size()>10000)throw std::runtime_error("More than 10000 saves; listing refused");
    }
    std::sort(saves.begin(),saves.end(),[](auto& a,auto& b){return a.modified!=b.modified?a.modified>b.modified:a.name<b.name;});
    auto j=object();auto* list=json_object_new_array();set(j.get(),"saves",list);set(j.get(),"directory",directory.string());
    for(const auto& s:saves){auto row=object();set(row.get(),"save_name",s.name);set(row.get(),"size_bytes",int64_t(s.size));json_object_array_add(list,row.release());}
    set(j.get(),"count",int64_t(saves.size()));set(j.get(),"order","newest filesystem modification first; Continue uses the game's current character");return j;
}
Json fo4hud_ui_action(uintptr_t image,uintptr_t menu,json_object* args) {
    const auto action=string(args,"action");auto stack=menus(image);
    auto found=std::find_if(stack.begin(),stack.end(),[&](const Menu&m){return m.pointer==menu;});
    if(found==stack.end())throw std::runtime_error("Menu changed before action; retry");
    const bool main=found->table==image+main_table,pause=found->table==image+pause_table;
    if(!main&&!pause)throw std::runtime_error("Open the main or pause menu first");
    for(auto& m:stack)if(m.name=="MessageBoxMenu"||m.name=="LoadingMenu"||m.name=="Console")throw std::runtime_error("Dismiss the confirmation/console or wait for loading before another menu action");
    auto j=object();set(j.get(),"action",action);set(j.get(),"menu",main?"MainMenu":"PauseMenu");
    if(action=="continue"){
        if(!main)throw std::runtime_error("Continue is only available at the main menu");
        if(!value<uint8_t>(image+0x32DD0FB))throw std::runtime_error("Main menu initialization is not finished");
        if(json_object_array_length(member(fo4hud_ui_saves().get(),"saves"))==0)throw std::runtime_error("No saves available");
        invoke(image,menu,true,2);set(j.get(),"status","continue_requested");
    }else if(action=="load"){
        const auto name=string(args,"save_name");validate_save(name);
        signature(image,0xBFBBB0,{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41});
        const auto manager=value<uintptr_t>(image+0x32A8618);if(!manager)throw std::runtime_error("Save manager unavailable");
        // Populate the engine-owned list, including all characters. This is the
        // same synchronous directory enumeration used by PopulateLoadList.
        // FinishLoadGame queues loading for the engine's next main update;
        // the console's blocking LoadGame routine must not run in AdvanceMovie.
        using Enumerate=void(__attribute__((ms_abi)) *)(uintptr_t,uintptr_t);
        reinterpret_cast<Enumerate>(image+0xBFBBB0)(manager,0);
        const auto n=value<uint32_t>(manager+24);const auto data=value<uintptr_t>(manager+8);
        if(n>10000 || (n&&!data))throw std::runtime_error("Engine save list mismatch");
        int index=-1;const auto wanted=std::filesystem::path(name).stem().string();
        for(uint32_t i=0;i<n;++i){
            const auto record=value<uintptr_t>(data+8*i);if(!record)continue;
            const auto filename=value<uintptr_t>(record);if(!filename)continue;
            std::string text;
            for(unsigned j=0;j<512;++j){char c=value<char>(filename+j);if(!c)break;text+=c;}
            if(std::filesystem::path(text).stem().string()==wanted){index=int(n-i-1);break;}
        }
        if(index<0)throw std::runtime_error("Save exists on disk but is absent from the engine's load list");
        invoke(image,menu,main,11,index);
        set(j.get(),"status","load_requested");set(j.get(),"save_name",name);
    }else if(action=="resume"){
        if(!pause)throw std::runtime_error("Resume requires the pause menu");invoke(image,menu,false,0);set(j.get(),"status","resume_requested");
    }else if(action=="quit_desktop"||action=="quit_main_menu"){
        if(main&&action=="quit_main_menu")throw std::runtime_error("Already at the main menu");
        invoke(image,menu,main,main?15:action=="quit_desktop"?61:15);
        set(j.get(),"status","quit_requested");set(j.get(),"may_require_confirmation",pause);
    }else throw std::runtime_error("Unknown UI action");
    return j;
}
