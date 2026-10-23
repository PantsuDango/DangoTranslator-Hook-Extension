#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>
#include "NScripterSnapshot.h"

namespace nscriptercontrol {
constexpr DWORD Magic=0x4e534431,Version=2,Capacity=4096;
struct Mode { DWORD helper,enabled; uint64_t helperCreated,gameCreated,epoch; wchar_t font[LF_FACESIZE]; uint64_t generation; DWORD snapshotBytes; std::array<BYTE,32> snapshotHash; };
struct Acknowledgement { uint64_t epoch,helperCreated,generation; DWORD status; };
struct Request { uint64_t id,epoch,helperCreated,page,part; DWORD length; wchar_t text[Capacity]; };
struct Reply { uint64_t id,epoch,helperCreated; DWORD length; wchar_t text[Capacity]; };
struct Result { uint64_t id,epoch,helperCreated; DWORD status; };
struct ResultSlot { volatile LONG sequence; Result value; };
struct alignas(8) Shared {
    DWORD magic,version,size,reserved;
    volatile LONG modeSequence; Mode mode;
    volatile LONG requestSequence; Request request;
    volatile LONG replySequence; Reply reply;
    ResultSlot results[32];
    volatile LONG ackSequence; Acknowledgement ack;
};
static_assert(sizeof(wchar_t)==2,"UTF16 protocol");

// created 取得进程创建身份，避免PID复用污染请求
inline uint64_t created(HANDLE process) {
    FILETIME c{},e{},k{},u{};
    return process&&GetProcessTimes(process,&c,&e,&k,&u)?uint64_t(c.dwHighDateTime)<<32|c.dwLowDateTime:0;
}
// name 将共享通道绑定到游戏实际创建时间
inline std::wstring name(DWORD pid,uint64_t birth) {return L"Local\\Dango.NScripter.Display."+std::to_wstring(pid)+L"."+std::to_wstring(birth);}
// snapshotName 将只读快照绑定到游戏、Helper身份及加载代次
inline std::wstring snapshotName(DWORD pid,const Mode &m){return name(pid,m.gameCreated)+L".Snapshot."+std::to_wstring(m.helper)+L"."+std::to_wstring(m.helperCreated)+L"."+std::to_wstring(m.epoch);}
// read 在x86中用序列锁复制完整身份和文字
template<class T> inline bool read(volatile LONG &sequence,const T &from,T &to) {
    for(unsigned attempt=0;attempt<3;attempt++) {
        auto a=InterlockedCompareExchange(&sequence,0,0);if(a&1)continue;
        MemoryBarrier();memcpy(&to,&from,sizeof(T));MemoryBarrier();
        if(a==InterlockedCompareExchange(&sequence,0,0))return true;
    }
    return false;
}
// write 发布单一生产者的完整有界快照
template<class T> inline void write(volatile LONG &sequence,T &to,const T &from) {
    InterlockedIncrement(&sequence);MemoryBarrier();memcpy(&to,&from,sizeof(T));MemoryBarrier();InterlockedIncrement(&sequence);
}
// valid 核对专用读取及绘字协议，不接纳其他引擎通道
inline bool valid(const Shared *p) {return p&&p->magic==Magic&&p->version==Version&&p->size==sizeof(Shared)&&p->reserved==0;}
// same 核对Helper身份及模式代次，字体切换也淘汰旧请求
inline bool same(const Mode &a,const Mode &b) {return a.helper==b.helper&&a.helperCreated==b.helperCreated&&a.gameCreated==b.gameCreated&&a.epoch==b.epoch&&a.enabled==b.enabled&&a.generation==b.generation&&a.snapshotBytes==b.snapshotBytes&&a.snapshotHash==b.snapshotHash&&memcmp(a.font,b.font,sizeof(a.font))==0;}
// alive 核对Helper没有退出或被同PID的新进程替换
inline bool alive(const Mode &m) {
    HANDLE p=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,m.helper);
    bool ok=p&&created(p)==m.helperCreated&&WaitForSingleObject(p,0)==WAIT_TIMEOUT;
    if(p)CloseHandle(p);return ok;
}
// advanceKeys 读取推进按键的高位状态，不把当前页触发点击误当作下一次推进
inline DWORD advanceKeys() {return ((GetAsyncKeyState(VK_LBUTTON)&0x8000)?1u:0u)|((GetAsyncKeyState(VK_RETURN)&0x8000)?2u:0u)|((GetAsyncKeyState(VK_SPACE)&0x8000)?4u:0u);}
class Host {
    HANDLE mapping=nullptr,snapshotMapping=nullptr;Shared *shared=nullptr;Mode mode{};DWORD gamePID=0;uint64_t seenAck=0;
    std::mutex mutex;uint64_t seenRequest=0,seenResults[32]{};
public:
    // ~Host 关闭当前模式并释放Host持有的通道
    ~Host(){close();}
    // open 建立专用通道，仅接替已经死亡的旧Helper
    bool open(DWORD pid,HANDLE process) {
        std::lock_guard<std::mutex> lock(mutex);auto birth=created(process);if(!birth||shared)return false;
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name(pid,birth).c_str());if(!mapping)return false;
        shared=static_cast<Shared *>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        if(!shared){CloseHandle(mapping);mapping=nullptr;return false;}
        Mode previous{};
        if(valid(shared)&&(!read(shared->modeSequence,shared->mode,previous)||alive(previous))) {UnmapViewOfFile(shared);CloseHandle(mapping);mapping=nullptr;shared=nullptr;return false;}
        mode={GetCurrentProcessId(),0,created(GetCurrentProcess()),birth,previous.epoch+1};
        wcscpy_s(mode.font,L"Microsoft YaHei");
        gamePID=pid;
        shared->magic=Magic;shared->version=Version;shared->size=sizeof(Shared);shared->reserved=0;
        write(shared->modeSequence,shared->mode,mode);
        Request r{};if(read(shared->requestSequence,shared->request,r))seenRequest=r.id;
        for(unsigned i=0;i<32;i++){Result previousResult{};if(read(shared->results[i].sequence,shared->results[i].value,previousResult))seenResults[i]=previousResult.id;}
        return true;
    }
    // close 使所有等待回复失效后释放通道
    void close(){std::lock_guard<std::mutex> lock(mutex);if(shared){mode.enabled=0;mode.epoch++;write(shared->modeSequence,shared->mode,mode);UnmapViewOfFile(shared);}if(mapping)CloseHandle(mapping);if(snapshotMapping)CloseHandle(snapshotMapping);snapshotMapping=nullptr;mapping=nullptr;shared=nullptr;}
    // enable 每次开关产生新代次，不让迟到译文进入下一页
    bool enable(bool value){std::lock_guard<std::mutex> lock(mutex);if(!shared)return false;mode.enabled=value?1:0;mode.epoch++;mode.generation=0;mode.snapshotBytes=0;mode.snapshotHash={};write(shared->modeSequence,shared->mode,mode);if(snapshotMapping)CloseHandle(snapshotMapping);snapshotMapping=nullptr;return true;}
    // backfill 加载并发布整份不可变快照，不进入逐句请求通道
    bool backfill(uint64_t generation,const std::wstring &path,const std::array<BYTE,32> &hash) {
        std::vector<BYTE> bytes;if(!generation||!nscriptersnapshot::load(path,hash,bytes))return false;
        std::lock_guard<std::mutex> lock(mutex);if(!shared)return false;
        Mode next=mode;next.enabled=2;next.epoch++;next.generation=generation;next.snapshotBytes=DWORD(bytes.size());next.snapshotHash=hash;
        wcscpy_s(next.font,L"Microsoft YaHei");
        auto map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,next.snapshotBytes,snapshotName(gamePID,next).c_str());
        if(!map)return false;if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(map);return false;}
        auto view=MapViewOfFile(map,FILE_MAP_WRITE,0,0,next.snapshotBytes);if(!view){CloseHandle(map);return false;}
        memcpy(view,bytes.data(),bytes.size());UnmapViewOfFile(view);mode=next;write(shared->modeSequence,shared->mode,mode);if(snapshotMapping)CloseHandle(snapshotMapping);snapshotMapping=map;return true;
    }
    // acknowledgement 只有当前快照的原生加载确认可以完成重启
    bool acknowledgement(Acknowledgement &ack) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid(shared)||mode.enabled!=2||!read(shared->ackSequence,shared->ack,ack)||ack.epoch!=mode.epoch||ack.helperCreated!=mode.helperCreated||ack.generation!=mode.generation||ack.epoch==seenAck||ack.status<1||ack.status>2)return false;
        seenAck=ack.epoch;return true;
    }
    // font 保存有界字体名称并取消当前旧字体请求
    void font(const std::wstring &value){std::lock_guard<std::mutex> lock(mutex);if(!shared||mode.enabled==2)return;memset(mode.font,0,sizeof(mode.font));auto family=value.empty()?L"Microsoft YaHei":value.c_str();wcsncpy_s(mode.font,family,_TRUNCATE);mode.epoch++;write(shared->modeSequence,shared->mode,mode);}
    // request 只交付当前模式的单一完整正文段
    bool request(Request &r) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid(shared)||mode.enabled!=1||!read(shared->requestSequence,shared->request,r)||!r.id||r.id==seenRequest)return false;
        seenRequest=r.id;return r.epoch==mode.epoch&&r.helperCreated==mode.helperCreated&&r.page&&r.part&&r.length>0&&r.length<Capacity&&r.text[r.length]==0;
    }
    // reply 精确绑定请求及代次，空串表示保留原文
    bool reply(uint64_t id,const std::wstring &text) {
        std::lock_guard<std::mutex> lock(mutex);Request r{};
        if(!valid(shared)||mode.enabled!=1||text.size()>=Capacity||!read(shared->requestSequence,shared->request,r)||r.id!=id||r.epoch!=mode.epoch||r.helperCreated!=mode.helperCreated)return false;
        Reply v{};v.id=id;v.epoch=mode.epoch;v.helperCreated=mode.helperCreated;v.length=DWORD(text.size());memcpy(v.text,text.data(),text.size()*2);write(shared->replySequence,shared->reply,v);return true;
    }
    // result 读取真实绘字及返回后的确认，拒绝旧模式结果
    bool result(Result &r) {
        std::lock_guard<std::mutex> lock(mutex);if(!valid(shared))return false;
        for(unsigned i=0;i<32;i++) {
            if(!read(shared->results[i].sequence,shared->results[i].value,r)||!r.id||r.id==seenResults[i])continue;
            seenResults[i]=r.id;if(r.epoch==mode.epoch&&r.helperCreated==mode.helperCreated&&r.status>=1&&r.status<=2)return true;
        }
        return false;
    }
};
class Client {
    HANDLE mapping=nullptr,helperHandle=nullptr;Shared *shared=nullptr;uint64_t serial=0,gameBirth=0,helperBirth=0;DWORD helperPID=0;
    std::mutex openMutex,resultMutex;
public:
    // ~Client 在排版退出后释放原生通道引用
    ~Client(){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(helperHandle)CloseHandle(helperHandle);}
    // mode 只打开当前进程和仍存活Helper的专用通道
    bool mode(Mode &m) {
        std::lock_guard<std::mutex> lock(openMutex);
        if(!shared) {
            gameBirth=created(GetCurrentProcess());mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name(GetCurrentProcessId(),gameBirth).c_str());if(!mapping)return false;
            shared=static_cast<Shared *>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
            if(!shared){CloseHandle(mapping);mapping=nullptr;return false;}
            Request r{};if(valid(shared)&&read(shared->requestSequence,shared->request,r))serial=r.id;
        }
        if(!valid(shared)||!read(shared->modeSequence,shared->mode,m)||(m.enabled!=1&&m.enabled!=2)||m.font[LF_FACESIZE-1]!=0||m.gameCreated!=gameBirth)return false;
        if(helperPID!=m.helper||helperBirth!=m.helperCreated||!helperHandle){if(helperHandle)CloseHandle(helperHandle);helperHandle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,m.helper);helperPID=m.helper;helperBirth=m.helperCreated;if(!helperHandle||created(helperHandle)!=helperBirth){if(helperHandle)CloseHandle(helperHandle);helperHandle=nullptr;return false;}}
        return WaitForSingleObject(helperHandle,0)==WAIT_TIMEOUT;
    }
    // current 取消开关变化、字体变化和Helper死亡后的旧等待
    bool current(const Mode &expected){Mode m{};return mode(m)&&m.enabled==1&&same(m,expected);}
    // currentBackfill 在每次绘字前撤销关闭、换Helper及字体变化后的旧快照
    bool currentBackfill(const Mode &expected){Mode m{};return mode(m)&&m.enabled==2&&same(m,expected);}
    // loadSnapshot 在独立线程只读复制映射，复查摘要及当前身份
    bool loadSnapshot(const Mode &m,nscriptersnapshot::Texts &texts) {
        if(m.snapshotBytes<24||m.snapshotBytes>nscriptersnapshot::MaximumBytes||!currentBackfill(m))return false;
        auto map=OpenFileMappingW(FILE_MAP_READ,FALSE,snapshotName(GetCurrentProcessId(),m).c_str());if(!map)return false;
        auto view=static_cast<const BYTE *>(MapViewOfFile(map,FILE_MAP_READ,0,0,m.snapshotBytes));if(!view){CloseHandle(map);return false;}
        std::vector<BYTE> bytes(view,view+m.snapshotBytes);UnmapViewOfFile(view);CloseHandle(map);std::array<BYTE,32> hash{};
        return nscriptersnapshot::digest(bytes.data(),DWORD(bytes.size()),hash)&&hash==m.snapshotHash&&nscriptersnapshot::parse(bytes.data(),DWORD(bytes.size()),texts)&&currentBackfill(m);
    }
    // acknowledge 只有当前加载完成或明确失败的代次可以发确认
    void acknowledge(const Mode &m,bool ok){if(!currentBackfill(m))return;Acknowledgement ack{m.epoch,m.helperCreated,m.generation,ok?1u:2u};write(shared->ackSequence,shared->ack,ack);}
    // wait 在有界期限内等待精确段译文，Ctrl及模式失效立即回退
    bool wait(const Mode &m,uint64_t page,uint64_t part,const std::wstring &text,uint64_t &id,std::wstring &translated) {
        if(text.empty()||text.size()>=Capacity||!current(m))return false;
        Request r{};r.id=id=++serial;r.epoch=m.epoch;r.helperCreated=m.helperCreated;r.page=page;r.part=part;r.length=DWORD(text.size());memcpy(r.text,text.data(),text.size()*2);write(shared->requestSequence,shared->request,r);
        auto start=GetTickCount64();auto previousKeys=advanceKeys();
        while(GetTickCount64()-start<3000) {
            auto keys=advanceKeys();bool advanced=(keys&~previousKeys)!=0;previousKeys=keys;
            if(!current(m)||(GetAsyncKeyState(VK_CONTROL)&0x8000)||(GetAsyncKeyState(VK_ESCAPE)&0x8000)||advanced)return false;
            Reply reply{};
            if(read(shared->replySequence,shared->reply,reply)&&reply.id==id&&reply.epoch==m.epoch&&reply.helperCreated==m.helperCreated) {
                if(reply.length>=Capacity||reply.text[reply.length]!=0)return false;translated.assign(reply.text,reply.length);return !translated.empty();
            }
            Sleep(10);
        }
        return false;
    }
    // result 只由正文返回点公布实际应用或回退
    void result(const Mode &m,uint64_t id,bool applied){std::lock_guard<std::mutex> lock(resultMutex);if(!valid(shared)||!id)return;Result r{id,m.epoch,m.helperCreated,applied?1u:2u};auto &slot=shared->results[id%32];write(slot.sequence,slot.value,r);}
};
}
