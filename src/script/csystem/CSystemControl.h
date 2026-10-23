#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>
#include "CSystemSnapshot.h"

// A separate display RPC: source records and the reader's page stream stay read-only.
// Each producer has its own seqlock. 64-bit identities are copied only inside a
// stable snapshot, including on x86; mode changes revoke every outstanding reply.
namespace csystemcontrol {
constexpr DWORD Magic = 0x43535232, Version = 3, Capacity = 4096, DisplayCapacity = 8192;
struct Mode { DWORD helper, enabled; uint64_t helperCreated, gameCreated, epoch, generation; DWORD snapshotBytes; std::array<BYTE,32> snapshotHash; };
struct Acknowledgement { uint64_t epoch, helperCreated, generation; DWORD status; };
struct Request { uint64_t id, epoch, helperCreated, page, part; DWORD length; wchar_t text[Capacity]; };
struct Reply { uint64_t id, epoch, helperCreated; DWORD length; wchar_t text[Capacity]; };
struct Result { uint64_t id, epoch, helperCreated; DWORD status; };
struct ResultSlot { volatile LONG sequence; Result result; };
struct Display { uint64_t id, epoch, helperCreated, generation, page, part; DWORD status, length, translatedLength; wchar_t text[DisplayCapacity], translation[DisplayCapacity]; };
struct alignas(8) Shared {
    DWORD magic, version, size, reserved;
    volatile LONG modeSequence; Mode mode;
    volatile LONG requestSequence; Request request;
    volatile LONG replySequence; Reply reply;
    ResultSlot results[32];
    volatile LONG ackSequence; Acknowledgement ack;
    volatile LONG displaySequence; Display display;
};
static_assert(sizeof(wchar_t)==2,"UTF16 protocol");

inline uint64_t created(HANDLE process) {
    FILETIME c{},e{},k{},u{};
    if(!process||!GetProcessTimes(process,&c,&e,&k,&u))return 0;
    return uint64_t(c.dwHighDateTime)<<32|c.dwLowDateTime;
}
inline std::wstring name(DWORD pid,uint64_t birth) { return L"Local\\Dango.CSystem.Display."+std::to_wstring(pid)+L"."+std::to_wstring(birth); }
// snapshotName 把映射绑定到游戏及Helper创建时间和模式代次
inline std::wstring snapshotName(DWORD pid,const Mode &m) {return name(pid,m.gameCreated)+L".Snapshot."+std::to_wstring(m.helper)+L"."+std::to_wstring(m.helperCreated)+L"."+std::to_wstring(m.epoch);}
template<class T> inline bool read(volatile LONG &sequence,const T &from,T &to) {
    for(unsigned attempt=0;attempt<3;attempt++) {
        LONG a=InterlockedCompareExchange(&sequence,0,0);if(a&1)continue;
        MemoryBarrier();memcpy(&to,&from,sizeof(T));MemoryBarrier();
        if(a==InterlockedCompareExchange(&sequence,0,0))return true;
    }
    return false;
}
template<class T> inline void write(volatile LONG &sequence,T &to,const T &from) {
    InterlockedIncrement(&sequence);MemoryBarrier();memcpy(&to,&from,sizeof(T));MemoryBarrier();InterlockedIncrement(&sequence);
}
inline bool valid(const Shared *p) { return p&&p->magic==Magic&&p->version==Version&&p->size==sizeof(Shared)&&p->reserved==0; }
inline bool same(const Mode &a,const Mode &b) { return a.helper==b.helper&&a.helperCreated==b.helperCreated&&a.gameCreated==b.gameCreated&&a.epoch==b.epoch&&a.enabled==b.enabled&&a.generation==b.generation&&a.snapshotBytes==b.snapshotBytes&&a.snapshotHash==b.snapshotHash; }
inline bool alive(const Mode &m) {
    HANDLE p=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,m.helper);
    bool ok=p&&created(p)==m.helperCreated&&WaitForSingleObject(p,0)==WAIT_TIMEOUT;
    if(p)CloseHandle(p);return ok;
}

class Host {
    HANDLE mapping=nullptr,snapshotMapping=nullptr;Shared *shared=nullptr;Mode mode{};DWORD gamePID=0;uint64_t seenAck=0;
    std::mutex mutex;uint64_t seenRequest=0,seenDisplay=0;uint64_t seenResults[32]{};
public:
    ~Host(){close();}
    bool open(DWORD pid,HANDLE process) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto birth=created(process);if(!birth||shared)return false;
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name(pid,birth).c_str());
        if(!mapping)return false;
        shared=static_cast<Shared *>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        if(!shared){CloseHandle(mapping);mapping=nullptr;return false;}
        Mode previous{};
        if(valid(shared)) { if(!read(shared->modeSequence,shared->mode,previous)||alive(previous)){UnmapViewOfFile(shared);CloseHandle(mapping);shared=nullptr;mapping=nullptr;return false;} }
        // A dead Host can leave a view held by the DLL. Mode identity, rather than
        // clearing another producer's counters, safely takes over that channel.
        mode={GetCurrentProcessId(),0,created(GetCurrentProcess()),birth,previous.epoch+1};
        gamePID=pid;
        shared->magic=Magic;shared->version=Version;shared->size=sizeof(Shared);shared->reserved=0;
        write(shared->modeSequence,shared->mode,mode);
        Request r{};
        if(read(shared->requestSequence,shared->request,r))seenRequest=r.id;
        for(unsigned i=0;i<32;i++){Result result{};if(read(shared->results[i].sequence,shared->results[i].result,result))seenResults[i]=result.id;}
        return true;
    }
    void close(){std::lock_guard<std::mutex> lock(mutex);if(shared){mode.enabled=0;mode.epoch++;write(shared->modeSequence,shared->mode,mode);UnmapViewOfFile(shared);}if(mapping)CloseHandle(mapping);if(snapshotMapping)CloseHandle(snapshotMapping);shared=nullptr;mapping=nullptr;snapshotMapping=nullptr;}
    bool enable(bool enabled){std::lock_guard<std::mutex> lock(mutex);if(!shared)return false;mode.enabled=enabled?1:0;mode.epoch++;mode.generation=0;mode.snapshotBytes=0;mode.snapshotHash={};write(shared->modeSequence,shared->mode,mode);if(snapshotMapping)CloseHandle(snapshotMapping);snapshotMapping=nullptr;return true;}
    // backfill 发布已核对字节，只有原生确认后Go才公布就绪
    bool backfill(uint64_t generation,const std::wstring &path,const std::array<BYTE,32> &hash) {
        std::vector<BYTE> bytes;if(!generation||!csystemsnapshot::load(path,hash,bytes))return false;
        std::lock_guard<std::mutex> lock(mutex);if(!shared)return false;
        Mode next=mode;next.epoch++;next.enabled=2;next.generation=generation;next.snapshotBytes=DWORD(bytes.size());next.snapshotHash=hash;
        HANDLE map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,next.snapshotBytes,snapshotName(gamePID,next).c_str());
        if(!map)return false;if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(map);return false;}
        void *view=MapViewOfFile(map,FILE_MAP_WRITE,0,0,next.snapshotBytes);if(!view){CloseHandle(map);return false;}
        memcpy(view,bytes.data(),bytes.size());UnmapViewOfFile(view);
        mode=next;write(shared->modeSequence,shared->mode,mode);if(snapshotMapping)CloseHandle(snapshotMapping);snapshotMapping=map;return true;
    }
    // acknowledgement 只接纳当前快照的加载确认
    bool acknowledgement(Acknowledgement &ack) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid(shared)||mode.enabled!=2||!read(shared->ackSequence,shared->ack,ack)||ack.epoch!=mode.epoch||ack.helperCreated!=mode.helperCreated||ack.generation!=mode.generation||ack.epoch==seenAck||ack.status<1||ack.status>2)return false;
        seenAck=ack.epoch;return true;
    }
    bool request(Request &r) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid(shared)||mode.enabled!=1||!read(shared->requestSequence,shared->request,r)||!r.id||r.id==seenRequest)return false;
        seenRequest=r.id;
        return r.epoch==mode.epoch&&r.helperCreated==mode.helperCreated&&r.page&&r.part&&r.length>0&&r.length<Capacity&&r.text[r.length]==0;
    }
    bool reply(uint64_t id,const std::wstring &text) {
        std::lock_guard<std::mutex> lock(mutex);Request r{};
        if(!valid(shared)||!mode.enabled||text.size()>=Capacity||!read(shared->requestSequence,shared->request,r)||r.id!=id||r.epoch!=mode.epoch||r.helperCreated!=mode.helperCreated)return false;
        Reply reply{};reply.id=id;reply.epoch=mode.epoch;reply.helperCreated=mode.helperCreated;reply.length=DWORD(text.size());
        memcpy(reply.text,text.data(),text.size()*2);write(shared->replySequence,shared->reply,reply);return true;
    }
    bool result(Result &r) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid(shared))return false;
        for(unsigned i=0;i<32;i++) {
            if(!read(shared->results[i].sequence,shared->results[i].result,r)||!r.id||r.id==seenResults[i])continue;
            seenResults[i]=r.id;if(r.helperCreated==mode.helperCreated&&r.status>=1&&r.status<=2)return true;
        }
        return false;
    }
    // display 只读取当前已加载代次的最新完整页面，旧模式通知不能恢复显示
    bool display(Display &value) {
        std::lock_guard<std::mutex> lock(mutex);
        Acknowledgement ack{};
        if(!valid(shared)||mode.enabled!=2||!read(shared->ackSequence,shared->ack,ack)||ack.status!=1||ack.epoch!=mode.epoch||ack.helperCreated!=mode.helperCreated||ack.generation!=mode.generation)return false;
        if(!read(shared->displaySequence,shared->display,value)||!value.id||value.id==seenDisplay)return false;
        seenDisplay=value.id;
        return value.epoch==mode.epoch&&value.helperCreated==mode.helperCreated&&value.generation==mode.generation&&value.page&&value.status>=1&&value.status<=3&&
            value.length<DisplayCapacity&&value.translatedLength<DisplayCapacity&&value.text[value.length]==0&&value.translation[value.translatedLength]==0&&
            (value.status==3?(value.length==0&&value.translatedLength==0):(value.part&&value.length>0&&(value.status==1?value.translatedLength>0:value.translatedLength==0)));
    }
};

class Client {
    HANDLE mapping=nullptr;Shared *shared=nullptr;uint64_t serial=0,displaySerial=0;
    std::mutex resultMutex,openMutex;
public:
    ~Client(){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);}
    bool current(const Mode &expected) {
        Mode m{};return valid(shared)&&read(shared->modeSequence,shared->mode,m)&&m.enabled==1&&same(m,expected)&&alive(m);
    }
    bool enabled(Mode &m) {
        if(!mode(m))return false;return m.enabled==1;
    }
    // mode 安全共享首次映射，快照加载和正文线程不得竞态初始化
    bool mode(Mode &m) {
        std::lock_guard<std::mutex> lock(openMutex);
        if(!shared) {
            mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name(GetCurrentProcessId(),created(GetCurrentProcess())).c_str());
            if(!mapping)return false;shared=static_cast<Shared *>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
            if(!shared){CloseHandle(mapping);mapping=nullptr;return false;}
            Request previous{};if(valid(shared)&&read(shared->requestSequence,shared->request,previous))serial=previous.id;
            Display previousDisplay{};if(valid(shared)&&read(shared->displaySequence,shared->display,previousDisplay))displaySerial=previousDisplay.id;
        }
        return valid(shared)&&read(shared->modeSequence,shared->mode,m)&&(m.enabled==1||m.enabled==2)&&m.gameCreated==created(GetCurrentProcess())&&alive(m);
    }
    // currentBackfill 复核快照模式身份，Helper死亡或换代立即失效
    bool currentBackfill(const Mode &expected) {Mode m{};return mode(m)&&m.enabled==2&&same(m,expected);}
    // loadSnapshot 只在专用后台线程复制并复查不可变映射
    bool loadSnapshot(const Mode &m,csystemsnapshot::Texts &texts) {
        if(m.snapshotBytes<24||m.snapshotBytes>csystemsnapshot::MaximumBytes||!currentBackfill(m))return false;
        HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,snapshotName(GetCurrentProcessId(),m).c_str());if(!map)return false;
        const auto view=static_cast<const BYTE *>(MapViewOfFile(map,FILE_MAP_READ,0,0,m.snapshotBytes));if(!view){CloseHandle(map);return false;}
        std::vector<BYTE> bytes(view,view+m.snapshotBytes);UnmapViewOfFile(view);CloseHandle(map);
        std::array<BYTE,32> hash{};
        return csystemsnapshot::digest(bytes.data(),DWORD(bytes.size()),hash)&&hash==m.snapshotHash&&csystemsnapshot::parse(bytes.data(),DWORD(bytes.size()),texts)&&currentBackfill(m);
    }
    // acknowledge 只有当前已加载或明确失败的代次才发送确认
    void acknowledge(const Mode &m,bool ok) {
        if(!currentBackfill(m))return;Acknowledgement ack{m.epoch,m.helperCreated,m.generation,ok?1u:2u};write(shared->ackSequence,shared->ack,ack);
    }
    // display 发布显示副本，不等待Host或Qt；调用方负责串行化单一生产者
    void display(const Mode &m,uint64_t page,uint64_t part,const std::wstring &text,const std::wstring &translation,DWORD status) {
        if(!page||text.size()>=DisplayCapacity||translation.size()>=DisplayCapacity||!currentBackfill(m))return;
        Display value{};value.id=++displaySerial;value.epoch=m.epoch;value.helperCreated=m.helperCreated;value.generation=m.generation;value.page=page;value.part=part;value.status=status;
        value.length=DWORD(text.size());value.translatedLength=DWORD(translation.size());memcpy(value.text,text.data(),text.size()*2);memcpy(value.translation,translation.data(),translation.size()*2);
        write(shared->displaySequence,shared->display,value);
    }
    // One caller at a time is enforced by the engine adapter, never a blocking lock
    // held across rendering or the translation service.
    bool wait(const Mode &m,uint64_t page,uint64_t part,const std::wstring &text,uint64_t &id,std::wstring &translated,DWORD timeout=3000) {
        if(text.empty()||text.size()>=Capacity||!current(m))return false;
        Request request{};request.id=id=++serial;request.epoch=m.epoch;request.helperCreated=m.helperCreated;request.page=page;request.part=part;request.length=DWORD(text.size());
        memcpy(request.text,text.data(),text.size()*2);write(shared->requestSequence,shared->request,request);
        const auto start=GetTickCount64();
        while(GetTickCount64()-start<timeout) {
            if(!current(m)||(GetAsyncKeyState(VK_CONTROL)&0x8000))return false;
            Reply reply{};
            if(read(shared->replySequence,shared->reply,reply)&&reply.id==id&&reply.epoch==m.epoch&&reply.helperCreated==m.helperCreated) {
                if(reply.length>=Capacity||reply.text[reply.length]!=0)return false;
                translated.assign(reply.text,reply.length);return !translated.empty();
            }
            Sleep(10);
        }
        return false;
    }
    void result(const Mode &m,uint64_t id,bool applied) {
        std::lock_guard<std::mutex> lock(resultMutex);
        if(!valid(shared)||!id)return;Result r{id,m.epoch,m.helperCreated,applied?1u:2u};auto &slot=shared->results[id%32];write(slot.sequence,slot.result,r);
    }
};
}
