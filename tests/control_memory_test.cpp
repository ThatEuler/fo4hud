#include "../control_memory.h"
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cassert>
#include <cstring>

using namespace fo4control;
int main(){
    unsigned char bytes[]={1,2,3,4};auto args=object();set(args.get(),"address",hex(reinterpret_cast<uintptr_t>(bytes)));set(args.get(),"size",int64_t(4));
    auto r=read_memory(args.get());assert(string(r.get(),"data_hex")=="01020304");
    set(args.get(),"data_hex","aabbccdd");set(args.get(),"expected_hex","01020304");
    auto w=write_memory(args.get());assert(boolean(w.get(),"verified")&&boolean(w.get(),"protections_restored"));assert(bytes[0]==0xAA&&bytes[3]==0xDD);
    bool mismatch=false;try{write_memory(args.get());}catch(const std::exception&){mismatch=true;}assert(mismatch&&bytes[0]==0xAA);
    set(args.get(),"address","0x1");bool unreadable=false;try{read_memory(args.get());}catch(const std::exception&){unreadable=true;}assert(unreadable);
    const auto page=sysconf(_SC_PAGESIZE);
    void* memory=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);
    auto*target=static_cast<unsigned char*>(memory)+page-2;std::memcpy(target,"abcd",4);
    assert(mprotect(memory,page,PROT_READ)==0);
    auto a=object();set(a.get(),"address",hex(reinterpret_cast<uintptr_t>(target)));set(a.get(),"data_hex","01020304");
    bool protected_write=false;try{write_memory(a.get());}catch(const std::exception&){protected_write=true;}assert(protected_write&&target[0]=='a');
    set(a.get(),"make_writable",true);auto changed=write_memory(a.get());assert(boolean(changed.get(),"verified")&&boolean(changed.get(),"protections_restored"));
    assert(target[0]==1&&target[3]==4);
    unsigned char v=9;iovec local{&v,1},remote{memory,1};assert(process_vm_writev(getpid(),&local,1,&remote,1,0)==-1); // First page restored read-only.
    remote.iov_base=static_cast<unsigned char*>(memory)+page;assert(process_vm_writev(getpid(),&local,1,&remote,1,0)==1); // Second page stayed writable.
    set(a.get(),"data_hex","zz");bool invalid=false;try{write_memory(a.get());}catch(const std::exception&){invalid=true;}assert(invalid);
    assert(memory_maps());munmap(memory,page*2);
}
