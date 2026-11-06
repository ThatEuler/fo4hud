#include "game_control.h"
#include "control_memory.h"
#include "game_entities.h"
#include "game_combat.h"
#include "game_ui.h"
#include "wine_input_hooks.h"
#include "logging.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace {
using namespace fo4control;
using Clock=std::chrono::steady_clock;
enum class Stage { Pending, Running, Completed, Cancelled };
struct Command {
    uint64_t id=0,generation=0;
    Clock::time_point deadline;
    std::string op;
    Json args;
    std::atomic<Stage> stage{Stage::Pending};
    std::string result;
};
std::mutex command_mutex, lifecycle_mutex;
std::deque<std::shared_ptr<Command>> pending;
std::unordered_map<uint64_t,std::shared_ptr<Command>> commands;
std::shared_ptr<Command> active_capture;
std::atomic<uint64_t> generation{1},next_id{1};
std::atomic<bool> running{false};
std::atomic<uintptr_t> game_image{0};
std::thread server_thread;
std::string socket_path;
int listener=-1;

Json array3(const float* values) {
    Json a(json_object_new_array());for(int i=0;i<3;++i)json_object_array_add(a.get(),std::isfinite(values[i])?json_object_new_double(values[i]):nullptr);return a;
}
void checked_read(uintptr_t a,void* p,size_t n) { if(!read(a,p,n))throw std::runtime_error("game memory became unavailable"); }
template<class T>T value(uintptr_t a){T v{};checked_read(a,&v,sizeof(v));return v;}
std::string pooled(uintptr_t field) {
    auto entry=value<uintptr_t>(field);
    for(int i=0;entry && i<8;++i){
        auto flags=value<uint16_t>(entry+8);
        if(flags&0x4000){entry=value<uintptr_t>(entry+0x10);continue;}
        if(flags&0x8000)return {};
        auto length=value<uint32_t>(entry+0x10);if(length>512)return {};
        std::string s(length,'\0');if(length)checked_read(entry+0x18,s.data(),length);return s;
    }return {};
}
void check_function(uintptr_t image,uintptr_t rva,const std::vector<unsigned char>& expected) {
    std::vector<unsigned char> actual(expected.size());
    if(!read(image+rva,actual.data(),actual.size())||actual!=expected)throw std::runtime_error("native function bytes do not match supported game build");
}
Json player_state(uintptr_t image,uintptr_t player) {
    auto j=object();set(j.get(),"address",hex(player));set(j.get(),"form_id",hex(value<uint32_t>(player+0x14)));
    float position[3]{},rotation[3]{};checked_read(player+0xD0,position,sizeof(position));checked_read(player+0xC0,rotation,sizeof(rotation));
    set(j.get(),"position",array3(position).release());set(j.get(),"rotation_radians",array3(rotation).release());
    auto cell=value<uintptr_t>(player+0xB8);set(j.get(),"cell",hex(cell));
    if(cell){set(j.get(),"cell_form_id",hex(value<uint32_t>(cell+0x14)));set(j.get(),"interior",bool(value<uint16_t>(cell+0x40)&1));}
    set(j.get(),"menu_cursor",fo4hud_game_menu_cursor_active());
    uintptr_t owner=player+0x58,vtable=value<uintptr_t>(owner),getter=value<uintptr_t>(vtable+8);
    if(vtable!=image+0x256D450 || getter!=image+0xCC93D0)throw std::runtime_error("player actor-value layout mismatch");
    using Getter=float(__attribute__((ms_abi)) *)(void*,void*);
    const struct {const char* name;uintptr_t offset;} avs[]={{"health",0xD8},{"rads",0x1D0},{"action_points",0x08},{"carry_weight",0x80}};
    for(auto av:avs){auto info=value<uintptr_t>(image+0x30E7460+av.offset);if(info)set(j.get(),av.name,double(reinterpret_cast<Getter>(getter)(reinterpret_cast<void*>(owner),reinterpret_cast<void*>(info))));}
    return j;
}
Json observe(uintptr_t image,uintptr_t player,json_object* args) {
    auto snapshot=fo4hud_read_current_cell();auto j=object();set(j.get(),"snapshot_status",snapshot.status);
    set(j.get(),"player",player_state(image,player).release());
    double radius=number(args,"radius",5000);auto limit=integer(args,"limit",100);
    if(radius<1||radius>100000||limit<1||limit>500)throw std::runtime_error("radius must be 1..100000 and limit 1..500");
    std::string filter=string(args,"filter");
    struct Candidate {const Fo4Entity* entity;bool actor;float p[3];double distance;};std::vector<Candidate> list;
    auto add=[&](const Fo4Entity&e,bool actor){
        if(e.address==player)return;
        if(actor&&!fo4hud_actor_in_player_space(snapshot,e))return;
        if(!actor&&!fo4hud_world_reference_visible(e))return;
        if(!filter.empty() && e.name.find(filter)==std::string::npos)return;
        Candidate c{&e,actor,{e.position[0],e.position[1],e.position[2]},0};
        if(actor&&!fo4hud_read_actor_position(e,c.p))return;
        for(int k=0;k<3;++k){double d=c.p[k]-snapshot.player_position[k];c.distance+=d*d;}
        if(std::isfinite(c.distance)&&c.distance<=radius*radius){c.distance=std::sqrt(c.distance);list.push_back(c);}
    };
    for(auto&e:snapshot.entities)if(e.form_type!=65)add(e,false);
    for(auto&e:snapshot.actors)add(e,true);
    std::sort(list.begin(),list.end(),[](const Candidate&a,const Candidate&b){return a.distance<b.distance;});
    auto* entities=json_object_new_array();
    for(size_t i=0;i<list.size()&&i<size_t(limit);++i){
        const auto&c=list[i];const auto&e=*c.entity;auto v=object();
        set(v.get(),"address",hex(e.address));set(v.get(),"form_id",hex(e.form_id));set(v.get(),"name",e.name);
        set(v.get(),"kind",c.actor?"actor":e.radar_category==5?"plant":e.radar_category==2?"container":e.radar_category==1?"entrance":"item");
        set(v.get(),"position",array3(c.p).release());set(v.get(),"distance",c.distance);
        if(c.actor){uint8_t state=0;if(fo4hud_read_actor_life_state(e,state))set(v.get(),"life_state",int64_t(state));}
        if(e.radar_category==5)set(v.get(),"ready_to_harvest",fo4hud_flora_harvestable(e.address));
        json_object_array_add(entities,v.release());
    }
    set(j.get(),"entities",entities);set(j.get(),"matched",int64_t(list.size()));return j;
}
Json inventory(uintptr_t player,json_object* args) {
    uintptr_t source=member(args,"address")?address(args,"address"):player;
    auto inv=value<uintptr_t>(source+0xF8);auto j=object();auto*items=json_object_new_array();set(j.get(),"address",hex(source));set(j.get(),"items",items);
    if(!inv){set(j.get(),"status","inventory list not initialized");return j;}
    auto data=value<uintptr_t>(inv+0x58);auto size=value<uint32_t>(inv+0x68);auto capacity=value<uint32_t>(inv+0x60);
    if(size>capacity||size>10000)throw std::runtime_error("inventory layout mismatch");
    auto limit=integer(args,"limit",200);if(limit<1||limit>1000)throw std::runtime_error("limit must be 1..1000");
    for(uint32_t i=0;i<size&&json_object_array_length(items)<size_t(limit);++i){
        auto object_address=value<uintptr_t>(data+i*16),stack=value<uintptr_t>(data+i*16+8);if(!object_address)continue;
        uint64_t count=0;int depth=0;
        while(stack&&depth++<10000){count+=value<uint32_t>(stack+0x20);auto next=value<uintptr_t>(stack+0x10);if(next==stack)throw std::runtime_error("cyclic inventory stack");stack=next;}
        if(stack)throw std::runtime_error("inventory stack limit exceeded");if(!count)continue;
        auto v=object();set(v.get(),"address",hex(object_address));set(v.get(),"form_id",hex(value<uint32_t>(object_address+0x14)));
        set(v.get(),"form_type",int64_t(value<uint8_t>(object_address+0x1A)));set(v.get(),"count",int64_t(count));
        const auto type=value<uint8_t>(object_address+0x1A);uintptr_t name_offset=type==0x32?0xC0:0x70;
        try{set(v.get(),"name",pooled(object_address+name_offset));}catch(...){set(v.get(),"name","");}
        json_object_array_add(items,v.release());
    }
    if(value<uintptr_t>(inv+0x58)!=data||value<uint32_t>(inv+0x68)!=size)throw std::runtime_error("inventory changed during observation; retry");
    set(j.get(),"status","ready");return j;
}
Json dispatch_game(const std::string&op,json_object*args,uintptr_t image,void* player_ptr) {
    if(op=="write_memory")return write_memory(args);
    uintptr_t player=reinterpret_cast<uintptr_t>(player_ptr);
    if(!player||value<uint32_t>(player+0x14)!=0x14||value<uint8_t>(player+0x1A)!=65)throw std::runtime_error("player not loaded");
    if(op=="observe")return observe(image,player,args);
    if(op=="inventory")return inventory(player,args);
    if(op=="activate"){
        auto target=address(args,"address");auto form=value<uint32_t>(target+0x14);auto type=value<uint8_t>(target+0x1A);
        if(type!=64&&type!=65)throw std::runtime_error("target must be a world reference or actor");
        if(member(args,"form_id") && form!=address(args,"form_id"))throw std::runtime_error("target form ID changed");
        if(value<uint32_t>(target+0x10)&((1u<<5)|(1u<<11)))throw std::runtime_error("target is deleted or disabled");
        if(fo4hud_game_menu_cursor_active())throw std::runtime_error("close the active menu before activating a world reference");
        auto target_cell=value<uintptr_t>(target+0xB8),player_cell=value<uintptr_t>(player+0xB8);
        if(!target_cell||!player_cell)throw std::runtime_error("target cell unavailable");
        if(target_cell!=player_cell && ((value<uint16_t>(target_cell+0x40)&1)||(value<uint16_t>(player_cell+0x40)&1)||
           !value<uintptr_t>(target_cell+0xC8)||value<uintptr_t>(target_cell+0xC8)!=value<uintptr_t>(player_cell+0xC8)))throw std::runtime_error("target is in another space");
        float p[3]{},t[3]{};checked_read(player+0xD0,p,sizeof(p));checked_read(target+0xD0,t,sizeof(t));
        double max=number(args,"max_distance",300);if(max<1||max>5000)throw std::runtime_error("max_distance must be 1..5000");
        double distance=0;for(int i=0;i<3;++i){double d=p[i]-t[i];distance+=d*d;}
        if(!std::isfinite(distance)||distance>max*max)throw std::runtime_error("target is outside activation range");
        check_function(image,0x5144B0,{0x48,0x89,0x5C,0x24,0x08,0x44,0x89,0x4C,0x24,0x20});
        using Fn=bool(__attribute__((ms_abi)) *)(void*,void*,void*,int32_t,bool,int16_t,bool);
        bool accepted=reinterpret_cast<Fn>(image+0x5144B0)(reinterpret_cast<void*>(target),player_ptr,nullptr,1,false,1,false);
        auto j=object();set(j.get(),"activation_dispatched",true);set(j.get(),"native_default_accepted",accepted);set(j.get(),"form_id",hex(form));return j;
    }
    if(op=="set_transform"){
        if(fo4hud_game_menu_cursor_active())throw std::runtime_error("close the menu before moving the player");
        float position[3]{},rotation[3]{};checked_read(player+0xD0,position,sizeof(position));checked_read(player+0xC0,rotation,sizeof(rotation));
        for(int i=0;i<3;++i){const char*key=i==0?"x":i==1?"y":"z";double n=number(args,key,position[i]);if(std::abs(n)>10000000)throw std::runtime_error("coordinate out of range");position[i]=float(n);}
        if(member(args,"yaw_degrees"))rotation[2]=float(std::remainder(number(args,"yaw_degrees",0),360)*0.0174532925199433);
        if(member(args,"pitch_degrees")){double pitch=number(args,"pitch_degrees",0);if(pitch< -89||pitch>89)throw std::runtime_error("pitch must be -89..89");rotation[0]=float(pitch*0.0174532925199433);}
        auto cell=value<uintptr_t>(player+0xB8);if(!cell)throw std::runtime_error("current cell unavailable");
        auto world=(value<uint16_t>(cell+0x40)&1)?0:value<uintptr_t>(cell+0xC8);
        uint32_t invalid=value<uint32_t>(image+0x30E5280);
        check_function(image,0x1181020,{0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x54,0x24,0x10});
        using Fn=void(__attribute__((ms_abi)) *)(void*,uint32_t*,void*,void*,const float*,const float*);
        reinterpret_cast<Fn>(image+0x1181020)(player_ptr,&invalid,reinterpret_cast<void*>(cell),reinterpret_cast<void*>(world),position,rotation);
        auto j=object();set(j.get(),"transform_requested",true);set(j.get(),"position",array3(position).release());set(j.get(),"rotation_radians",array3(rotation).release());return j;
    }
    throw std::runtime_error("unknown game operation");
}
void complete(const std::shared_ptr<Command>& command,Json result) {
    command->result=dump(result.get());command->stage.store(Stage::Completed,std::memory_order_release);
}
std::shared_ptr<Command> submit(const std::string&op,json_object*args) {
    auto c=std::make_shared<Command>();c->id=next_id.fetch_add(1);c->generation=generation.load();c->op=op;
    c->args=Json(json_object_get(args));
    auto timeout=integer(args,"queue_timeout_ms",2000);if(timeout<100||timeout>10000)throw std::runtime_error("queue_timeout_ms must be 100..10000");
    c->deadline=Clock::now()+std::chrono::milliseconds(timeout);
    std::lock_guard<std::mutex> lock(command_mutex);
    // Remove finished receipts first; never evict a running command.
    if(commands.size()>=256){for(auto it=commands.begin();it!=commands.end();){auto s=it->second->stage.load();if(s==Stage::Completed||s==Stage::Cancelled)it=commands.erase(it);else ++it;}}
    if(commands.size()>=256||pending.size()>=32)throw std::runtime_error("control queue full");
    commands[c->id]=c;pending.push_back(c);return c;
}
Json dispatch(const std::string&op,json_object*args) {
    if(op=="status"){
        auto j=object();set(j.get(),"pid",int64_t(getpid()));set(j.get(),"image_base",hex(game_image.load()));
        set(j.get(),"build","Fallout4.exe 1.11.240");set(j.get(),"game_hook_active",fo4hud_combat_active());
        set(j.get(),"input_ready",fo4hud_wine_control_ready());set(j.get(),"input_busy",fo4hud_wine_control_busy());
        set(j.get(),"generation",int64_t(generation.load()));return j;
    }
    if(op=="ui_state")return fo4hud_ui_state(game_image.load());
    if(op=="list_saves")return fo4hud_ui_saves();
    if(op=="stop"){fo4hud_control_cancel();auto j=object();set(j.get(),"cancelled",true);set(j.get(),"generation",int64_t(generation.load()));return j;}
    if(op=="memory_maps")return memory_maps();
    if(op=="read_memory")return read_memory(args);
    if(op=="input"){
        std::vector<uint32_t>keys;auto*array=member(args,"keys");
        if(array){if(!json_object_is_type(array,json_type_array)||json_object_array_length(array)>8)throw std::runtime_error("keys must be an array with at most 8 entries");
            for(size_t i=0;i<json_object_array_length(array);++i){auto*v=json_object_array_get_idx(array,i);if(!json_object_is_type(v,json_type_int))throw std::runtime_error("key code must be integer");auto k=json_object_get_int64(v);if(k<1||k>511)throw std::runtime_error("key code out of range");keys.push_back(uint32_t(k));}}
        std::string error;auto duration=integer(args,"duration_ms",100);if(duration<1||duration>5000)throw std::runtime_error("duration_ms must be 1..5000");
        if(!fo4hud_wine_control_input(keys,int(duration),float(number(args,"look_x",0)),float(number(args,"look_y",0)),error))throw std::runtime_error(error);
        auto j=object();set(j.get(),"status","scheduled_on_wine_queue");set(j.get(),"generation",int64_t(generation.load()));return j;
    }
    if(op=="command_result"||op=="cancel_command"){
        auto id=integer(args,"command_id",0);std::shared_ptr<Command>c;
        {std::lock_guard<std::mutex> lock(command_mutex);auto it=commands.find(id);if(it==commands.end())throw std::runtime_error("unknown command ID (receipt expired or wrong game process)");c=it->second;}
        if(op=="cancel_command"){auto expected=Stage::Pending;c->stage.compare_exchange_strong(expected,Stage::Cancelled);}
        auto stage=c->stage.load(std::memory_order_acquire);auto j=object();set(j.get(),"command_id",int64_t(c->id));
        const char* status=stage==Stage::Pending?"queued":stage==Stage::Running?"running":stage==Stage::Completed?"completed":"cancelled";set(j.get(),"status",status);
        if(stage==Stage::Completed)set(j.get(),"result",parse(c->result).release());return j;
    }
    if(op=="ui_action"&&!fo4hud_ui_active())throw std::runtime_error("Menu control hooks unavailable; restart with the updated fo4hud build");
    if(op!="observe"&&op!="inventory"&&op!="activate"&&op!="set_transform"&&op!="write_memory"&&op!="screenshot"&&op!="ui_action")throw std::runtime_error("unknown operation");
    auto c=submit(op,args);auto j=object();set(j.get(),"status","queued");set(j.get(),"command_id",int64_t(c->id));return j;
}

bool transfer(int fd,std::string&data,bool send_data) {
    size_t position=0;auto deadline=Clock::now()+std::chrono::milliseconds(250);
    while(Clock::now()<deadline && running.load()){
        pollfd p{fd,short(send_data?POLLOUT:POLLIN),0};if(poll(&p,1,20)<=0)continue;
        if(send_data){auto n=send(fd,data.data()+position,data.size()-position,MSG_NOSIGNAL);if(n>0){position+=n;if(position==data.size())return true;}else if(errno!=EAGAIN&&errno!=EINTR)return false;}
        else{char b[4096];auto n=recv(fd,b,sizeof(b),0);if(n>0){data.append(b,n);if(data.size()>300000)return false;auto newline=data.find('\n');if(newline!=std::string::npos){if(newline+1!=data.size())return false;return true;}}else if(n==0||(errno!=EAGAIN&&errno!=EINTR))return false;}
    }return false;
}
void loop() {
    while(running.load(std::memory_order_acquire)){
        fo4hud_wine_control_pulse();
        {std::lock_guard<std::mutex> lock(command_mutex);for(auto&entry:commands){auto&c=entry.second;if(c->generation!=generation.load()||Clock::now()>c->deadline){auto s=Stage::Pending;c->stage.compare_exchange_strong(s,Stage::Cancelled);}}
         pending.erase(std::remove_if(pending.begin(),pending.end(),[](const auto& c){return c->stage.load()==Stage::Cancelled;}),pending.end());}
        pollfd p{listener,POLLIN,0};if(poll(&p,1,10)<=0)continue;
        int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);if(fd<0)continue;
        ucred peer{};socklen_t length=sizeof(peer);
        if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length)!=0||peer.uid!=getuid()){close(fd);continue;}
        std::string request;
        if(transfer(fd,request,false)){
            Json response;
            try{auto j=parse(request);auto op=string(j.get(),"op");auto*args=member(j.get(),"args");auto empty=object();if(!args)args=empty.get();if(!json_object_is_type(args,json_type_object))throw std::runtime_error("args must be an object");response=dispatch(op,args);}
            catch(const std::exception&e){response=failure(e.what());}
            catch(...){response=failure("unexpected control error");}
            auto output=dump(response.get())+"\n";transfer(fd,output,true);
        }close(fd);
    }
}
std::string runtime_directory() {
    const char*runtime=std::getenv("XDG_RUNTIME_DIR");
    std::string dir=runtime&&runtime[0]=='/'?std::string(runtime)+"/fo4hud":"/tmp/fo4hud-control-"+std::to_string(getuid());
    if(mkdir(dir.c_str(),0700)<0 && errno!=EEXIST)throw std::runtime_error("cannot create control directory");
    struct stat st{};if(lstat(dir.c_str(),&st)!=0||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077))throw std::runtime_error("control directory must be owned by this user with mode 0700");return dir;
}
void chunk(std::vector<unsigned char>&out,const char*type,const std::vector<unsigned char>&data){
    auto word=[&](uint32_t n){for(int s=24;s>=0;s-=8)out.push_back((n>>s)&255);};word(data.size());auto start=out.size();out.insert(out.end(),type,type+4);out.insert(out.end(),data.begin(),data.end());word(crc32(0,out.data()+start,out.size()-start));
}
}

void fo4hud_control_cancel(){generation.fetch_add(1,std::memory_order_acq_rel);fo4hud_wine_control_cancel();}
void fo4hud_control_tick(uintptr_t image,void*player){
    for(int i=0;i<4;++i){std::shared_ptr<Command>c;
        {std::lock_guard<std::mutex>lock(command_mutex);auto it=std::find_if(pending.begin(),pending.end(),[](const auto&p){return p->op!="screenshot"&&p->op!="ui_action";});if(it==pending.end())return;c=*it;pending.erase(it);}
        if(c->generation!=generation.load()||Clock::now()>c->deadline){auto s=Stage::Pending;c->stage.compare_exchange_strong(s,Stage::Cancelled);continue;}
        auto expected=Stage::Pending;if(!c->stage.compare_exchange_strong(expected,Stage::Running))continue;
        try{complete(c,dispatch_game(c->op,c->args.get(),image,player));}
        catch(const std::exception&e){complete(c,failure(e.what()));}
        catch(...){complete(c,failure("unexpected game command error"));}
    }
}
void fo4hud_control_ui_tick(uintptr_t image,uintptr_t menu){
    // One transition per menu update; later queued actions are revalidated.
    std::shared_ptr<Command> c;
    {std::lock_guard<std::mutex> lock(command_mutex);auto it=std::find_if(pending.begin(),pending.end(),[](const auto&p){return p->op=="ui_action";});if(it==pending.end())return;c=*it;pending.erase(it);}
    if(c->generation!=generation.load()||Clock::now()>c->deadline){auto s=Stage::Pending;c->stage.compare_exchange_strong(s,Stage::Cancelled);return;}
    auto expected=Stage::Pending;if(!c->stage.compare_exchange_strong(expected,Stage::Running))return;
    try{complete(c,fo4hud_ui_action(image,menu,c->args.get()));}
    catch(const std::exception&e){complete(c,failure(e.what()));}
    catch(...){complete(c,failure("unexpected UI command error"));}
}
bool fo4hud_control_start(uintptr_t image){
    std::lock_guard<std::mutex>lock(lifecycle_mutex);if(running.load())return true;
    if(!image)return false;const char*enabled=std::getenv("FO4HUD_CONTROL");if(enabled&&std::strcmp(enabled,"0")==0)return false;
    try{socket_path=runtime_directory()+"/game-"+std::to_string(getpid())+".sock";}
    catch(const std::exception&e){fo4hud_log("control","%s",e.what());return false;}
    if(socket_path.size()>=sizeof(sockaddr_un::sun_path))return false;
    listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(listener<0)return false;
    sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,socket_path.c_str(),socket_path.size()+1);
    if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){close(listener);listener=-1;return false;}
    if(chmod(socket_path.c_str(),0600)!=0||listen(listener,8)!=0){close(listener);listener=-1;unlink(socket_path.c_str());return false;}
    game_image.store(image);running.store(true);fo4hud_wine_control_set_cancel_callback(fo4hud_control_cancel);
    if(!fo4hud_ui_install(image))fo4hud_log("control","menu control unavailable: vtable changed or unsupported image");
    fo4hud_combat_set_control_tick(fo4hud_control_tick);server_thread=std::thread(loop);
    fo4hud_log("control","local MCP bridge listening at %s",socket_path.c_str());return true;
}
void fo4hud_control_stop(){
    std::lock_guard<std::mutex>lock(lifecycle_mutex);if(!running.exchange(false))return;
    fo4hud_ui_uninstall();
    fo4hud_combat_set_control_tick(nullptr);fo4hud_wine_control_set_cancel_callback(nullptr);fo4hud_control_cancel();
    if(server_thread.joinable())server_thread.join();if(listener>=0){close(listener);listener=-1;}unlink(socket_path.c_str());
    std::lock_guard<std::mutex>commands_lock(command_mutex);pending.clear();commands.clear();active_capture.reset();game_image.store(0);
}
std::string fo4hud_control_socket_path(){std::lock_guard<std::mutex>lock(lifecycle_mutex);return running.load()?socket_path:"";}
bool fo4hud_control_begin_capture(){
    std::lock_guard<std::mutex>lock(command_mutex);if(active_capture)return false;
    for(auto it=pending.begin();it!=pending.end();){auto c=*it;
        if(c->op!="screenshot"){++it;continue;}it=pending.erase(it);
        if(c->generation!=generation.load()||Clock::now()>c->deadline){auto s=Stage::Pending;c->stage.compare_exchange_strong(s,Stage::Cancelled);continue;}
        auto expected=Stage::Pending;if(!c->stage.compare_exchange_strong(expected,Stage::Running))continue;active_capture=c;return true;
    }return false;
}
void fo4hud_control_finish_capture(int width,int height,bool bgra,const unsigned char*pixels,const std::string&error){
    std::shared_ptr<Command>c;{std::lock_guard<std::mutex>lock(command_mutex);c=std::move(active_capture);}if(!c)return;
    if(!error.empty()||!pixels||width<1||height<1||width>16384||height>16384){complete(c,failure(error.empty()?"invalid screenshot":error));return;}
    try{
        size_t pitch=size_t(width)*4;std::vector<unsigned char>raw((pitch+1)*height);
        for(int y=0;y<height;++y){auto*dst=raw.data()+y*(pitch+1);dst[0]=0;std::memcpy(dst+1,pixels+size_t(y)*pitch,pitch);if(bgra)for(int x=0;x<width;++x)std::swap(dst[1+x*4],dst[3+x*4]);}
        std::vector<unsigned char>compressed(compressBound(raw.size()));uLongf n=compressed.size();if(compress2(compressed.data(),&n,raw.data(),raw.size(),1)!=Z_OK)throw std::runtime_error("PNG compression failed");compressed.resize(n);
        std::vector<unsigned char>png={137,80,78,71,13,10,26,10},header;
        for(uint32_t v:{uint32_t(width),uint32_t(height)})for(int s=24;s>=0;s-=8)header.push_back((v>>s)&255);
        header.insert(header.end(),{8,6,0,0,0});chunk(png,"IHDR",header);chunk(png,"IDAT",compressed);chunk(png,"IEND",{});
        std::string path=socket_path+".capture-"+std::to_string(c->id)+".png";
        int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)throw std::runtime_error("cannot create screenshot file");
        size_t done=0;while(done<png.size()){ssize_t w=write(fd,png.data()+done,png.size()-done);if(w<0&&errno==EINTR)continue;if(w<=0)break;done+=w;}close(fd);
        if(done!=png.size()){unlink(path.c_str());throw std::runtime_error("screenshot write failed");}
        auto j=object();set(j.get(),"path",path);set(j.get(),"width",int64_t(width));set(j.get(),"height",int64_t(height));complete(c,std::move(j));
    }catch(const std::exception&e){complete(c,failure(e.what()));}
}
