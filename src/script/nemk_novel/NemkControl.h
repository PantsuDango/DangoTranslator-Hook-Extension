#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>
#pragma comment(lib,"bcrypt.lib")

// 资源授权与异步正文分槽，游戏主线程不等待网络或Host
namespace nemkcontrol {
constexpr size_t Capacity=65536;
struct Command { uint64_t generation=0; uint32_t enabled=0; wchar_t path[2048]{},font[2048]{}; char digest[65]{},fingerprint[65]{}; };
struct Result { uint64_t generation=0; uint32_t status=0; char digest[65]{}; };
struct LiveCommand {uint64_t epoch=0;uint32_t enabled=0;wchar_t font[2048]{};};
struct LiveReply {uint64_t epoch=0,generation=0,sequence=0,owner=0;wchar_t text[4097]{};};
struct LiveResult {uint64_t epoch=0,sequence=0;uint32_t status=0;};
struct Shared { char magic[8]; DWORD target,host; uint64_t targetTime,hostTime; BYTE token[16]; volatile LONG commandStamp,resultStamp; Command command; Result result; volatile LONG liveStamp,replyStamp,liveResultStamp; LiveCommand live;LiveReply reply;LiveResult liveResult; };
static_assert(sizeof(Shared)<Capacity,"Nemk control budget");
inline uint64_t creation(HANDLE process) { FILETIME a{},b{},c{},d{};return GetProcessTimes(process,&a,&b,&c,&d)?uint64_t(a.dwHighDateTime)<<32|a.dwLowDateTime:0; }
inline std::wstring name(DWORD pid,uint64_t time) { return L"Local\\Dango.NemkNovel."+std::to_wstring(pid)+L"."+std::to_wstring(time); }
struct Host {
    HANDLE mapping=nullptr,owner=nullptr; Shared *shared=nullptr; std::mutex mutex; uint64_t delivered=0; uint32_t lastStatus=0;LONG liveDelivered=0;
    ~Host(){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(owner){ReleaseMutex(owner);CloseHandle(owner);}}
    bool open(DWORD pid,HANDLE process) {
        auto time=creation(process);if(!time)return false;
        owner=CreateMutexW(nullptr,FALSE,(name(pid,time)+L".owner").c_str());
        if(!owner)return false;auto wait=WaitForSingleObject(owner,0);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(owner);owner=nullptr;return false;}
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(Capacity),name(pid,time).c_str());
        if(!mapping)return false;
        shared=static_cast<Shared *>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,Capacity));if(!shared)return false;
        memset(shared,0,Capacity);memcpy(shared->magic,"DNMC0002",8);shared->target=pid;shared->targetTime=time;shared->host=GetCurrentProcessId();shared->hostTime=creation(GetCurrentProcess());
        return BCryptGenRandom(nullptr,shared->token,16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0;
    }
    bool write(uint64_t generation,uint32_t enabled,const std::wstring &path={},const std::string &digest={},const std::string &fingerprint={},const std::wstring &font={}) {
        std::lock_guard<std::mutex> lock(mutex);if(!shared||!generation||generation<=shared->command.generation||path.size()>2047||font.size()>2047||path.find(L'\0')!=path.npos||font.find(L'\0')!=font.npos||(enabled&&(digest.size()!=64||fingerprint.size()!=64)))return false;
        if(enabled>2)return false;
        Command c;c.generation=generation;c.enabled=enabled;memcpy(c.path,path.data(),path.size()*2);memcpy(c.font,font.data(),font.size()*2);memcpy(c.digest,digest.data(),digest.size());memcpy(c.fingerprint,fingerprint.data(),fingerprint.size());
        InterlockedIncrement(&shared->commandStamp);memcpy(&shared->command,&c,sizeof(c));MemoryBarrier();InterlockedIncrement(&shared->commandStamp);return true;
    }
    bool result(Result &r) {if(!shared)return false;LONG stamp=InterlockedCompareExchange(&shared->resultStamp,0,0);if(stamp&1)return false;memcpy(&r,&shared->result,sizeof(r));MemoryBarrier();if(stamp!=InterlockedCompareExchange(&shared->resultStamp,0,0)||!r.generation||r.generation<delivered||(r.generation==delivered&&r.status==lastStatus)||r.status>3)return false;delivered=r.generation;lastStatus=r.status;return true;}
    // 实时开关使用独立递增代次，不改变资源授权和加载确认
    bool live(uint64_t epoch,bool enabled,const std::wstring &font={}) {
        std::lock_guard<std::mutex> lock(mutex);if(!shared||!epoch||epoch<=shared->live.epoch||font.size()>2047||font.find(L'\0')!=font.npos||(enabled&&font.empty()))return false;
        LiveCommand c;c.epoch=epoch;c.enabled=enabled;memcpy(c.font,font.data(),font.size()*2);
        InterlockedIncrement(&shared->liveStamp);memcpy(&shared->live,&c,sizeof(c));MemoryBarrier();InterlockedIncrement(&shared->liveStamp);return true;
    }
    // 回复只占一个有界槽，正文身份由原生再次核对
    bool reply(uint64_t epoch,uint64_t generation,uint64_t sequence,uint64_t pageOwner,const std::wstring &text) {
        std::lock_guard<std::mutex> lock(mutex);if(!shared||!epoch||shared->live.epoch!=epoch||!shared->live.enabled||!sequence||!pageOwner||text.size()>4096||text.find(L'\0')!=text.npos)return false;
        LiveReply r;r.epoch=epoch;r.generation=generation;r.sequence=sequence;r.owner=pageOwner;memcpy(r.text,text.data(),text.size()*2);
        InterlockedIncrement(&shared->replyStamp);memcpy(&shared->reply,&r,sizeof(r));MemoryBarrier();InterlockedIncrement(&shared->replyStamp);return true;
    }
    // 主线程真实写回后才发布成功，Host仅转发稳定快照
    bool liveResult(LiveResult &r) {
        if(!shared)return false;LONG stamp=InterlockedCompareExchange(&shared->liveResultStamp,0,0);if(!stamp||(stamp&1)||stamp==liveDelivered)return false;
        memcpy(&r,&shared->liveResult,sizeof(r));MemoryBarrier();if(stamp!=InterlockedCompareExchange(&shared->liveResultStamp,0,0)||!r.epoch||r.status<1||r.status>5)return false;liveDelivered=stamp;return true;
    }
};
}
