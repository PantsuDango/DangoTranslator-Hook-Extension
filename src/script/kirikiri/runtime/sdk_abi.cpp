#include "sdk_api.h"

namespace dango_krkr {
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
static bool executableAddress(void* p){
    MEMORY_BASIC_INFORMATION info={};
    if(!p || !VirtualQuery(p,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    return (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
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
}
