#include "../game_control.h"
#include "../control_json.h"
#include <poll.h>
#include <unistd.h>
#include <iostream>
#include <array>
#include <thread>

int main(){
    std::array<unsigned char,32> bytes{};bytes[0]=1;bytes[1]=2;
    if(!fo4hud_control_start(1))return 2;
    auto info=fo4control::object();fo4control::set(info.get(),"socket",fo4hud_control_socket_path());fo4control::set(info.get(),"address",fo4control::hex(reinterpret_cast<uintptr_t>(bytes.data())));
    std::cout<<fo4control::dump(info.get())<<std::endl;
    bool paused=false;
    while(true){
        pollfd in{STDIN_FILENO,POLLIN,0};if(poll(&in,1,2)>0){std::string line;if(!std::getline(std::cin,line)||line=="quit")break;paused=line=="pause";std::cout<<(paused?"paused":"resumed")<<std::endl;}
        if(!paused)fo4hud_control_tick(0,nullptr);
        if(fo4hud_control_begin_capture()){
            const unsigned char bgra[]={0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,255};
            fo4hud_control_finish_capture(2,2,true,bgra);
        }
    }
    fo4hud_control_stop();
}
