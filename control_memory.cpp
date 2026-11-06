#include "control_memory.h"
#include <sys/uio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fstream>
#include <limits>
#include <cstring>
#include <algorithm>

namespace fo4control {
bool read(uintptr_t a, void* data, size_t size) {
    if (!a || size>65536 || a>UINTPTR_MAX-size) return false;
    iovec local{data,size}, remote{reinterpret_cast<void*>(a),size};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==ssize_t(size);
}
struct Region { uintptr_t first,last; int protection; std::string name; };
static std::vector<Region> regions() {
    std::ifstream f("/proc/self/maps"); std::string line; std::vector<Region> out;
    while (std::getline(f,line)) {
        unsigned long first=0,last=0; char perms[5]{}; int n=0;
        if (std::sscanf(line.c_str(),"%lx-%lx %4s %*s %*s %*s %n",&first,&last,perms,&n)<3) continue;
        int p=(perms[0]=='r'?PROT_READ:0)|(perms[1]=='w'?PROT_WRITE:0)|(perms[2]=='x'?PROT_EXEC:0);
        out.push_back({first,last,p,n>0?line.substr(n):""});
    }
    return out;
}
Json memory_maps() {
    auto j=object(); auto* array=json_object_new_array();
    for (const auto& r:regions()) {
        auto v=object();set(v.get(),"start",hex(r.first));set(v.get(),"end",hex(r.last));
        set(v.get(),"read",bool(r.protection&PROT_READ));set(v.get(),"write",bool(r.protection&PROT_WRITE));
        set(v.get(),"execute",bool(r.protection&PROT_EXEC));set(v.get(),"path",r.name);
        json_object_array_add(array,v.release());
    }
    set(j.get(),"regions",array);return j;
}
static std::string encode(const std::vector<unsigned char>& bytes) {
    static const char* digits="0123456789abcdef";std::string s; s.reserve(bytes.size()*2);
    for(auto b:bytes){s+=digits[b>>4];s+=digits[b&15];}return s;
}
static std::vector<unsigned char> decode(const std::string& s) {
    if (s.empty() || s.size()%2 || s.size()>131072) throw std::runtime_error("hex data must contain 1..65536 bytes");
    auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    std::vector<unsigned char> b; b.reserve(s.size()/2);
    for(size_t i=0;i<s.size();i+=2){int a=digit(s[i]),v=digit(s[i+1]);if(a<0||v<0)throw std::runtime_error("invalid hex data");b.push_back((a<<4)|v);}return b;
}
Json read_memory(json_object* args) {
    auto a=address(args,"address");auto size=integer(args,"size",64);
    if(size<1||size>65536)throw std::runtime_error("size must be 1..65536");
    std::vector<unsigned char>b(size);if(!read(a,b.data(),b.size()))throw std::runtime_error("address is not fully readable");
    auto j=object();set(j.get(),"address",hex(a));set(j.get(),"size",size);set(j.get(),"data_hex",encode(b));return j;
}
Json write_memory(json_object* args) {
    auto a=address(args,"address");auto data=decode(string(args,"data_hex"));
    std::vector<unsigned char>before(data.size()),after(data.size());
    if(!read(a,before.data(),before.size()))throw std::runtime_error("target is not fully readable");
    if(member(args,"expected_hex") && decode(string(args,"expected_hex"))!=before)throw std::runtime_error("expected bytes do not match; no write performed");
    const bool force=boolean(args,"make_writable");
    auto mappings=regions();std::vector<Region> changed;
    uintptr_t cursor=a,end=a+data.size();
    for(const auto&r:mappings){
        if(r.last<=cursor||r.first>cursor)continue;
        if(!(r.protection&PROT_READ)||(!(r.protection&PROT_WRITE)&&!force))break;
        if(!(r.protection&PROT_WRITE))changed.push_back(r);
        cursor=std::min(end,r.last);if(cursor==end)break;
    }
    if(cursor!=end)throw std::runtime_error("target is unmapped or not writable; no write performed");
    const long page=sysconf(_SC_PAGESIZE);if(page<=0)throw std::runtime_error("page size unavailable");
    std::vector<Region> modified;
    bool protection_ok=true,restored=true;
    for(const auto&r:changed){
        auto first=std::max(a,r.first)&~uintptr_t(page-1);
        auto last=(std::min(end,r.last)+page-1)&~uintptr_t(page-1);
        if(mprotect(reinterpret_cast<void*>(first),last-first,r.protection|PROT_WRITE)!=0){protection_ok=false;break;}
        modified.push_back({first,last,r.protection,{}});
    }
    ssize_t written=-1;
    if(protection_ok){iovec l{data.data(),data.size()},r{reinterpret_cast<void*>(a),data.size()};written=process_vm_writev(getpid(),&l,1,&r,1,0);}
    for(const auto&r:modified)if(mprotect(reinterpret_cast<void*>(r.first),r.last-r.first,r.protection)!=0)restored=false;
    auto j=object();set(j.get(),"address",hex(a));set(j.get(),"before_hex",encode(before));
    set(j.get(),"bytes_written",int64_t(std::max<ssize_t>(0,written)));set(j.get(),"protections_restored",restored);
    bool readable=read(a,after.data(),after.size());if(readable)set(j.get(),"after_hex",encode(after));
    bool verified=protection_ok&&written==ssize_t(data.size())&&readable&&after==data;
    set(j.get(),"verified",verified);
    if(!verified||!restored)set(j.get(),"error","memory write or protection restoration was incomplete; inspect receipt");
    return j;
}
}
