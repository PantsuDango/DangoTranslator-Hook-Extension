#include "sdk_api.h"
#include <tlhelp32.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace dango_krkr {
static bool executableAddress(void* p){
    MEMORY_BASIC_INFORMATION info={};
    if(!p || !VirtualQuery(p,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    return (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
}
#if !defined(_WIN64)
// queryNeutral 恢复原始栈顶，兼容cdecl和stdcall的官方导出器接口
__declspec(naked) static DWORD __cdecl queryNeutral(void*,void*,const char**,void**,unsigned,unsigned*) {
    __asm {
        push ebp
        mov ebp,esp
        push esi
        push edi
        mov esi,esp
        push dword ptr [ebp+24]
        push dword ptr [ebp+20]
        push dword ptr [ebp+16]
        push dword ptr [ebp+12]
        call dword ptr [ebp+8]
        movzx eax,al
        mov edx,esi
        sub edx,esp
        mov edi,dword ptr [ebp+28]
        mov dword ptr [edi],edx
        mov esp,esi
        pop edi
        pop esi
        pop ebp
        ret
    }
}
static int queryAbi(void* exporter){
    __try {
        auto method=(*static_cast<void***>(exporter))[1];
        if(!executableAddress(method))return -1;
        const char* name="void ::TVPExecuteExpression(const ttstr &,tTJSVariant *)";
        void* pointer=nullptr;unsigned remaining=999;
        if(!queryNeutral(method,exporter,&name,&pointer,1,&remaining) || !executableAddress(pointer))return -1;
        return remaining==0?0:remaining==16?1:-1;
    } __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}
const SdkApi* negotiateSdk(void* exporter){
    int abi=queryAbi(exporter);
    return abi==0?legacySdk():abi==1?currentSdk():nullptr;
}
void* locateExporter(){
    auto get=reinterpret_cast<void*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(nullptr),"TVPGetFunctionExporter"));
    return get?get():nullptr;
}
#else
typedef bool (*NarrowQuery)(void*,const char**,void**,unsigned);

// mainImage 返回主程序映像范围，导出器与其方法必须属于引擎自身
static bool mainImage(uintptr_t& begin,uintptr_t& end){
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if(!base || dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    begin=uintptr_t(base);end=begin+nt->OptionalHeader.SizeOfImage;
    return true;
}

// queryExporter 在异常边界内按窄字符串签名查询单个导入函数
static bool queryExporter(void* exporter,const char* name,void** pointer){
    __try {
        auto method=reinterpret_cast<NarrowQuery>((*static_cast<void***>(exporter))[1]);
        *pointer=nullptr;
        return method(exporter,&name,pointer,1) && executableAddress(*pointer);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

// validExporter 核对候选对象虚表与方法均位于主程序内，并能解析固定的脚本入口
static bool validExporter(void* candidate){
    uintptr_t begin=0,end=0;
    if(!candidate || !mainImage(begin,end))return false;
    __try {
        auto table=*static_cast<void***>(candidate);
        if(uintptr_t(table)<begin || uintptr_t(table)+2*sizeof(void*)>end)return false;
        for(int i=0;i<2;i++)if(uintptr_t(table[i])<begin || uintptr_t(table[i])>=end || !executableAddress(table[i]))return false;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    void* pointer=nullptr;
    return queryExporter(candidate,"iTJSDispatch2 * ::TVPGetScriptDispatch()",&pointer) && uintptr_t(pointer)>=begin && uintptr_t(pointer)<end;
}

// collectStores 收集函数开头处写入RIP相对全局变量的地址，并跟随直接调用与跳转一层
static void collectStores(const uint8_t* code,int depth,std::vector<void**>& stores){
    __try {
        for(int i=0;i<96;i++){
            const uint8_t* p=code+i;
            // REX.W mov [rip+disp32],reg：插件初始化把导出器保存到存根全局变量
            if((p[0]==0x48 || p[0]==0x4c) && p[1]==0x89 && (p[2]&0xc7)==0x05){
                auto disp=*reinterpret_cast<const int32_t*>(p+3);
                stores.push_back(reinterpret_cast<void**>(const_cast<uint8_t*>(p+7+disp)));
                continue;
            }
            if(depth<2 && (p[0]==0xe8 || p[0]==0xe9)){
                auto target=p+5+*reinterpret_cast<const int32_t*>(p+1);
                if(executableAddress(const_cast<uint8_t*>(target)))collectStores(target,depth+1,stores);
                if(p[0]==0xe9 && i==0)return;
            }
            if(p[0]==0xc3 || p[0]==0xcc)return;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

// readSlot 在异常边界内读取候选全局变量，地址不可读时视为空
static void* readSlot(void** slot){
    __try {return *slot;} __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}

// locateExporter 主程序未导出接口时，从已链接插件保存的导出器中取得并校验
void* locateExporter(){
    auto get=reinterpret_cast<void*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(nullptr),"TVPGetFunctionExporter"));
    if(get){void* exporter=get();if(validExporter(exporter))return exporter;}
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(snapshot==INVALID_HANDLE_VALUE)return nullptr;
    MODULEENTRY32W entry={sizeof(entry)};
    void* found=nullptr;
    for(BOOL more=Module32FirstW(snapshot,&entry);more && !found;more=Module32NextW(snapshot,&entry)){
        auto link=reinterpret_cast<const uint8_t*>(GetProcAddress(entry.hModule,"V2Link"));
        if(!link || !executableAddress(const_cast<uint8_t*>(link)))continue;
        std::vector<void**> stores;collectStores(link,0,stores);
        for(auto slot:stores){
            void* candidate=readSlot(slot);
            if(validExporter(candidate)){found=candidate;break;}
        }
    }
    CloseHandle(snapshot);
    return found;
}

// importsAvailable 扫描本模块编入的全部导入签名，任一不可解析即放弃，避免存根找不到函数时终止游戏
static bool importsAvailable(void* exporter){
    auto base=reinterpret_cast<uint8_t*>(&__ImageBase);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto section=IMAGE_FIRST_SECTION(nt);
    size_t checked=0;
    for(WORD index=0;index<nt->FileHeader.NumberOfSections;index++,section++){
        if(section->Characteristics&IMAGE_SCN_MEM_EXECUTE)continue;
        auto data=reinterpret_cast<const char*>(base+section->VirtualAddress);
        size_t size=section->Misc.VirtualSize;
        for(size_t start=0;start<size;){
            size_t end=start;
            while(end<size && data[end]>=0x20 && data[end]<0x7f)end++;
            if(end<size && !data[end] && end-start>=8 && end-start<512){
                std::string value(data+start,end-start);
                auto colon=value.find("::"),open=value.find('(');
                bool signature=colon!=std::string::npos && open!=std::string::npos && colon<open &&
                    (value.back()==')' || value.size()>6 && value.compare(value.size()-6,6,") const")==0);
                if(signature){
                    void* pointer=nullptr;
                    if(!queryExporter(exporter,value.c_str(),&pointer))return false;
                    checked++;
                }
            }
            start=end+1;
        }
    }
    return checked!=0;
}
const SdkApi* negotiateSdk(void* exporter){
    return validExporter(exporter) && importsAvailable(exporter)?currentSdk():nullptr;
}
#endif
}
