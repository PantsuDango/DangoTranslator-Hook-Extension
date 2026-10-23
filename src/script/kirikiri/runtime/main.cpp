#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "sdk_api.h"
#include <array>
#include <atomic>
#include <memory>

namespace dango_krkr {
static constexpr uint32_t protocolMagic=0x314b5244;
static constexpr uint32_t packetLimit=32*1024*1024+4096;
struct Endpoint {uint32_t magic,version,target,owner,port;char nonce[32];};
static_assert(sizeof(Endpoint)==52,"endpoint layout");
struct Job {Action action;std::wstring argument;Reply reply;std::vector<uint8_t> payload;};
static HMODULE module;
static const SdkApi* sdk=nullptr;
static HHOOK messageHook=nullptr;
static HANDLE completed=nullptr;
static UINT taskMessage=0;
static HWND application=nullptr;
static std::atomic<Job*> pending{nullptr};
static DWORD guiThread=0;

// onMessage 只在原生应用GUI线程消费经过会话认证的固定动作
static LRESULT CALLBACK onMessage(int code,WPARAM w,LPARAM l){
    if(code>=0 && w==PM_REMOVE && reinterpret_cast<MSG*>(l)->message==taskMessage){
        reinterpret_cast<MSG*>(l)->message=WM_NULL;
        if(Job* job=pending.exchange(nullptr)){
            job->reply=sdk->perform(job->action,job->argument,job->payload);
            SetEvent(completed);
        }
    }
    return CallNextHookEx(messageHook,code,w,l);
}

// executeGui 保持安装线程存活，超时后不销毁仍可能执行的任务对象
static bool executeGui(Job& job,HANDLE owner){
    ResetEvent(completed);
    pending.store(&job);
    if(!PostMessageW(application,taskMessage,0,0)){pending.store(nullptr);return false;}
    HANDLE handles[]={completed,owner};
    DWORD wait=WaitForMultipleObjects(2,handles,FALSE,10000);
    if(wait==WAIT_OBJECT_0)return true;
    if(pending.exchange(nullptr)==&job)return false;
    // 已进入GUI调用时必须等它完成，不能留下指向已销毁栈帧的回调
    return WaitForSingleObject(completed,INFINITE)==WAIT_OBJECT_0;
}

// readEndpoint 每次重新打开当前会话映射，PID相同也必须验证新的随机令牌
static bool readEndpoint(Endpoint& endpoint){
    std::wstring name=L"Local\\DangoKrkrSdk-v1-"+std::to_wstring(GetCurrentProcessId());
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
    if(!mapping)return false;
    void* view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(endpoint));
    if(view){memcpy(&endpoint,view,sizeof(endpoint));UnmapViewOfFile(view);}
    CloseHandle(mapping);
    if(!view || endpoint.magic!=protocolMagic || endpoint.version!=1 || endpoint.target!=GetCurrentProcessId() || !endpoint.owner || !endpoint.port || endpoint.port>65535)return false;
    for(char ch:endpoint.nonce)if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))return false;
    return true;
}

// serverOwnedBy 用操作系统TCP拥有者表核对服务器进程，不能相信连接自报PID
static bool serverOwnedBy(SOCKET socket,const Endpoint& endpoint){
    sockaddr_in local={};int size=sizeof(local);
    if(getsockname(socket,reinterpret_cast<sockaddr*>(&local),&size)!=0)return false;
    DWORD bytes=0;
    GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0);
    if(bytes<sizeof(DWORD) || bytes>16*1024*1024)return false;
    std::vector<uint8_t> storage(bytes);
    if(GetExtendedTcpTable(storage.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)!=NO_ERROR)return false;
    auto table=reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(storage.data());
    if(table->dwNumEntries>(storage.size()-sizeof(DWORD))/sizeof(MIB_TCPROW_OWNER_PID))return false;
    for(DWORD i=0;i<table->dwNumEntries;i++){
        auto& row=table->table[i];
        if(row.dwLocalAddr==htonl(INADDR_LOOPBACK) && row.dwRemoteAddr==htonl(INADDR_LOOPBACK) && row.dwLocalPort==htons(uint16_t(endpoint.port)) && row.dwRemotePort==local.sin_port && row.dwOwningPid==endpoint.owner)return true;
    }
    return false;
}
static bool transfer(SOCKET socket,void* data,size_t size,bool writing){
    char* bytes=static_cast<char*>(data);
    while(size){int done=writing?send(socket,bytes,int(size),0):recv(socket,bytes,int(size),0);if(done<=0)return false;bytes+=done;size-=done;}
    return true;
}
static bool sendPacket(SOCKET socket,const std::vector<uint8_t>& data){
    uint32_t size=uint32_t(data.size());
    return size<=packetLimit && transfer(socket,&size,4,true) && transfer(socket,const_cast<uint8_t*>(data.data()),size,true);
}
static bool readPacket(SOCKET socket,std::vector<uint8_t>& data){
    uint32_t size=0;if(!transfer(socket,&size,4,false) || size<8 || size>packetLimit)return false;
    data.resize(size);return transfer(socket,data.data(),size,false);
}

// resourceArgument 仅接受规范的相对资源路径，拒绝磁盘路径与命令文本
static bool resourceArgument(const uint8_t* data,size_t size,std::wstring& value){
    if(!size || size>3072)return false;
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(data),int(size),nullptr,0);
    if(count<=0 || count>1024)return false;
    value.resize(count);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(data),int(size),value.data(),count);
    if(value.front()==L'/' || value.back()==L'/')return false;
    size_t start=0;
    for(size_t i=0;i<=value.size();i++){
        if(i<value.size() && value[i]!=L'/'){
            if(value[i]<32 || value[i]==127 || value[i]==L'\\' || value[i]==L':' || value[i]==L'>')return false;
            continue;
        }
        auto part=value.substr(start,i-start);
        if(part.empty() || part==L"." || part==L"..")return false;
        start=i+1;
    }
    return true;
}

static void serve(SOCKET socket,const Endpoint& endpoint,HANDLE owner){
    std::vector<uint8_t> hello(40);memcpy(hello.data(),&protocolMagic,4);uint32_t pid=GetCurrentProcessId();memcpy(hello.data()+4,&pid,4);memcpy(hello.data()+8,endpoint.nonce,32);
    if(!sendPacket(socket,hello))return;
    std::vector<uint8_t> packet;
    while(WaitForSingleObject(owner,0)==WAIT_TIMEOUT && readPacket(socket,packet)){
        uint32_t serial=0,rawAction=0;memcpy(&serial,packet.data(),4);memcpy(&rawAction,packet.data()+4,4);
        if(!serial || rawAction<1 || rawAction>10)return;
        Job job{static_cast<Action>(rawAction),{},Reply{}};
        if(job.action==Action::StageDisplay){
            if(packet.size()<104 || packet.size()>32*1024*1024+8)return;
            job.payload.assign(packet.begin()+8,packet.end());
        }else if((job.action==Action::ReadResource && !resourceArgument(packet.data()+8,packet.size()-8,job.argument)) || (job.action!=Action::ReadResource && packet.size()!=8))return;
        if(!executeGui(job,owner))return;
        std::vector<uint8_t> response(8);memcpy(response.data(),&serial,4);memcpy(response.data()+4,&job.reply.status,4);
        response.insert(response.end(),job.reply.data.begin(),job.reply.data.end());
        if(!sendPacket(socket,response) || job.action==Action::Detach)return;
    }
}

// worker 会话断开后等待新令牌，支持同一游戏进程中的客户端重连
static DWORD WINAPI worker(void*){
    WSADATA wsa={};if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 0;
    auto get=reinterpret_cast<void*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(nullptr),"TVPGetFunctionExporter"));
    if(!get){WSACleanup();return 0;}
    void* exporter=get();sdk=negotiateSdk(exporter);if(!sdk){WSACleanup();return 0;}
    std::array<char,32> previous={};
    for(;;){
        Endpoint endpoint={};
        if(!readEndpoint(endpoint) || memcmp(previous.data(),endpoint.nonce,32)==0){Sleep(500);continue;}
        HANDLE owner=OpenProcess(SYNCHRONIZE,FALSE,endpoint.owner);
        if(!owner || WaitForSingleObject(owner,0)!=WAIT_TIMEOUT){if(owner)CloseHandle(owner);Sleep(500);continue;}
        std::wstring nonce(endpoint.nonce,endpoint.nonce+32);
        if(!sdk->initialize(exporter,nonce)){CloseHandle(owner);break;}
        application=sdk->applicationWindow();DWORD pid=0;guiThread=GetWindowThreadProcessId(application,&pid);
        if(!guiThread || pid!=GetCurrentProcessId()){CloseHandle(owner);break;}
        completed=CreateEventW(nullptr,TRUE,FALSE,nullptr);taskMessage=RegisterWindowMessageW((L"DangoKrkrSdk-v1-"+nonce).c_str());
        messageHook=SetWindowsHookExW(WH_GETMESSAGE,onMessage,module,guiThread);
        if(!completed || !messageHook){if(completed)CloseHandle(completed);CloseHandle(owner);break;}
        SOCKET socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address={};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(uint16_t(endpoint.port));
        DWORD timeout=15000;setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        if(socket!=INVALID_SOCKET && connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && serverOwnedBy(socket,endpoint))serve(socket,endpoint,owner);
        if(socket!=INVALID_SOCKET)closesocket(socket);
        Job detach{Action::Detach,{},Reply{}};executeGui(detach,owner);
        UnhookWindowsHookEx(messageHook);messageHook=nullptr;CloseHandle(completed);completed=nullptr;CloseHandle(owner);
        memcpy(previous.data(),endpoint.nonce,32);
    }
    WSACleanup();return 0;
}
}
BOOL WINAPI DllMain(HINSTANCE handle,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){dango_krkr::module=handle;DisableThreadLibraryCalls(handle);HANDLE thread=CreateThread(nullptr,0,dango_krkr::worker,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
    return TRUE;
}
