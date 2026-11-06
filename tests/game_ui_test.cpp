#include "../game_ui.cpp"
#include <cassert>
#include <iostream>
#include <vector>

void fo4hud_control_ui_tick(uintptr_t,uintptr_t) {}
int main() {
    char directory[]="/tmp/fo4hud-ui-test-XXXXXX";assert(mkdtemp(directory));
    assert(setenv("FO4HUD_SAVE_DIR",directory,1)==0);
    const auto root=std::filesystem::path(directory);
    {std::ofstream f(root/"first.fos",std::ios::binary);f.write("FO4_SAVEGAME\0",12);}
    {std::ofstream f(root/"second.fos",std::ios::binary);f.write("FO4_SAVEGAME\0",12);}
    {std::ofstream f(root/"invalid.fos");f<<"not a save";}
    std::filesystem::last_write_time(root/"first.fos",std::filesystem::file_time_type::clock::now()-std::chrono::hours(1));
    std::filesystem::create_symlink(root/"first.fos",root/"symlink.fos");
    std::filesystem::create_directory(root/"directory.fos");
    auto listed=fo4hud_ui_saves();assert(integer(listed.get(),"count",0)==3);
    validate_save("first.fos");
    for(auto name:{"../first.fos","..\\first.fos","first.fos\n","/tmp/first.fos","first.txt","symlink.fos","invalid.fos","directory.fos","missing.fos"}){
        bool rejected=false;try{validate_save(name);}catch(...){rejected=true;}assert(rejected);
    }
    // Construct an active MainMenu/MessageBox stack to test guards without calling game code.
    std::vector<unsigned char> image(0x3300000),ui(0x1A8),main(0x300),box(0x80),name(64),boxname(64);
    uintptr_t base=reinterpret_cast<uintptr_t>(image.data()),ui_addr=reinterpret_cast<uintptr_t>(ui.data());
    auto put=[](void* p,size_t offset,auto v){std::memcpy(static_cast<char*>(p)+offset,&v,sizeof(v));};
    put(image.data(),0x30E8930,ui_addr);
    put(main.data(),0,base+main_table);put(main.data(),0x50,reinterpret_cast<uintptr_t>(name.data()));
    put(name.data(),0x10,uint32_t(8));std::memcpy(name.data()+0x18,"MainMenu",8);
    put(box.data(),0x50,reinterpret_cast<uintptr_t>(boxname.data()));
    put(boxname.data(),0x10,uint32_t(14));std::memcpy(boxname.data()+0x18,"MessageBoxMenu",14);
    uintptr_t pointers[]={reinterpret_cast<uintptr_t>(main.data()),reinterpret_cast<uintptr_t>(box.data())};
    put(ui.data(),0x190,reinterpret_cast<uintptr_t>(pointers));put(ui.data(),0x1A0,uint32_t(2));
    auto state=fo4hud_ui_state(base);assert(boolean(state.get(),"main_menu"));
    auto args=parse("{\"action\":\"continue\"}");
    bool blocked=false;try{fo4hud_ui_action(base,pointers[0],args.get());}catch(const std::exception&e){blocked=std::strstr(e.what(),"confirmation");}assert(blocked);
    put(ui.data(),0x1A0,uint32_t(1));
    args=parse("{\"action\":\"resume\"}");blocked=false;
    try{fo4hud_ui_action(base,pointers[0],args.get());}catch(const std::exception&e){blocked=std::strstr(e.what(),"pause menu");}assert(blocked);
    args=parse("{\"action\":\"load\",\"save_name\":\"../first.fos\"}");blocked=false;
    try{fo4hud_ui_action(base,pointers[0],args.get());}catch(...){blocked=true;}assert(blocked);
    put(ui.data(),0x1A0,uint32_t(65));blocked=false;
    try{fo4hud_ui_state(base);}catch(...){blocked=true;}assert(blocked);
    assert(!fo4hud_ui_install(1));
    std::filesystem::remove_all(root);std::cout<<"UI save validation and menu guards passed\n";
}
