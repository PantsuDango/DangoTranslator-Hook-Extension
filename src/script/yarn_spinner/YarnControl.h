#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <string>
#include <mutex>
#include <cstring>
#include <atomic>
#pragma comment(lib, "bcrypt.lib")

// yarncontrol 用目标进程创建时间隔离共享通道，游戏线程只读快照且不等待Host
namespace yarncontrol
{
    constexpr size_t size = 131072, resultOffset = 26152, queryOffset = 32768, cacheOffset = 65536;
    struct Host
    {
        HANDLE mapping = nullptr;
        BYTE *data = nullptr;
        std::mutex mutex;
        uint64_t revision = 0, epoch = 0, delivered = 0;
        uint32_t deliveredStatus = 0;
        uint64_t queryDelivered = 0;
        std::atomic<uint64_t> modeStart{0};
        ~Host() { if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping); }
        bool create(DWORD pid, HANDLE target)
        {
            FILETIME created{}, ended{}, kernel{}, user{}, hostCreated{};
            if(!GetProcessTimes(target,&created,&ended,&kernel,&user)||!GetProcessTimes(GetCurrentProcess(),&hostCreated,&ended,&kernel,&user))return false;
            uint64_t targetTime=uint64_t(created.dwHighDateTime)<<32|created.dwLowDateTime;
            uint64_t hostTime=uint64_t(hostCreated.dwHighDateTime)<<32|hostCreated.dwLowDateTime;
            auto name=L"Local\\Dango.YarnSpinner."+std::to_wstring(pid)+L"."+std::to_wstring(targetTime);
            mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(size),name.c_str());
            if(!mapping||GetLastError()==ERROR_ALREADY_EXISTS)return false;
            data=static_cast<BYTE *>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,size));if(!data)return false;
            memset(data,0,size);memcpy(data,"DYRT0002",8);
            auto host=GetCurrentProcessId();memcpy(data+8,&pid,4);memcpy(data+12,&host,4);memcpy(data+16,&targetTime,8);memcpy(data+24,&hostTime,8);
            if(BCryptGenRandom(nullptr,data+32,16,BCRYPT_USE_SYSTEM_PREFERRED_RNG))return false;
            write(false,0,L"",L"",true);return true;
        }
        bool write(bool enabled,uint64_t page,const std::wstring &font,const std::wstring &text,bool mode=false,bool chinese=false)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!data||font.size()>1023||text.size()>12000||font.find(L'\0')!=std::wstring::npos||text.find(L'\0')!=std::wstring::npos)return false;
            if(!mode&&(!*reinterpret_cast<uint32_t *>(data+80)||!page))return false;
            InterlockedIncrement(reinterpret_cast<volatile LONG *>(data+48));
            ++revision;if(mode){++epoch;modeStart.store(revision);}
            memcpy(data+56,&revision,8);memcpy(data+64,&epoch,8);memcpy(data+72,&page,8);
            uint32_t active=enabled?1:0;memcpy(data+80,&active,4);
            if(mode){uint32_t language=chinese?1:0;memcpy(data+92,&language,4);uint32_t length=uint32_t(font.size());memcpy(data+84,&length,4);memset(data+96,0,2048);memcpy(data+96,font.data(),font.size()*2);}
            uint32_t length=uint32_t(text.size());memcpy(data+88,&length,4);memset(data+2144,0,24002);memcpy(data+2144,text.data(),text.size()*2);
            MemoryBarrier();InterlockedIncrement(reinterpret_cast<volatile LONG *>(data+48));return true;
        }
        bool result(uint64_t &generation,uint64_t &page,uint32_t &status)
        {
            if(!data)return false;
            LONG before=InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(data+52),0,0);if(before&1)return false;
            BYTE copy[24];memcpy(copy,data+resultOffset,sizeof(copy));MemoryBarrier();
            if(before!=InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(data+52),0,0))return false;
            memcpy(&generation,copy,8);memcpy(&page,copy+8,8);memcpy(&status,copy+16,4);
            if(!generation||generation<modeStart.load()||generation<delivered||(generation==delivered&&status==deliveredStatus)||status>5)return false;
            delivered=generation;deliveredStatus=status;return true;
        }

        // query 独立消费显示缓存查询，不进入实时正文页或翻译请求
        bool query(uint64_t &id,uint64_t &mode,std::wstring &text,uint32_t *kind=nullptr)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!data||!*reinterpret_cast<uint32_t *>(data+80))return false;
            auto stamp=reinterpret_cast<volatile LONG *>(data+queryOffset);
            LONG before=InterlockedCompareExchange(stamp,0,0);if(before&1)return false;
            BYTE copy[24036];memcpy(copy,data+queryOffset,sizeof(copy));MemoryBarrier();
            if(before!=InterlockedCompareExchange(stamp,0,0))return false;
            uint32_t length=0,source=0;memcpy(&length,copy+4,4);memcpy(&id,copy+8,8);memcpy(&mode,copy+16,8);memcpy(&source,copy+24,4);
            if(!id||id<=queryDelivered||mode!=epoch||!length||length>12000||source>1)return false;
            text.assign(reinterpret_cast<const wchar_t *>(copy+32),length);
            if(text.find(L'\0')!=std::wstring::npos)return false;
            if(kind)*kind=source;queryDelivered=id;return true;
        }

        // cacheResult 只回复当前查询与模式，过期结果不修改控制快照
        bool cacheResult(uint64_t id,uint64_t mode,const std::wstring &text)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!data||!id||mode!=epoch||!*reinterpret_cast<uint32_t *>(data+80)||text.size()>12000||text.find(L'\0')!=std::wstring::npos)return false;
            uint64_t current=0;memcpy(&current,data+queryOffset+8,8);if(current!=id)return false;
            auto stamp=reinterpret_cast<volatile LONG *>(data+cacheOffset);
            InterlockedIncrement(stamp);
            uint32_t length=uint32_t(text.size());memcpy(data+cacheOffset+4,&length,4);memcpy(data+cacheOffset+8,&id,8);memcpy(data+cacheOffset+16,&mode,8);
            memset(data+cacheOffset+32,0,24002);memcpy(data+cacheOffset+32,text.data(),text.size()*2);
            MemoryBarrier();InterlockedIncrement(stamp);return true;
        }
    };
}
