// 官方SDK类型、字符串与导入指针在各自ABI命名空间内独立管理
#include "sdk/tp_stub.h"
#include "sdk/tp_stub.cpp"

static constexpr size_t resourceLimit=8*1024*1024;
static std::wstring callbackName;
static iTJSDispatch2* globalObject=nullptr;
static ttstr* resourceName=nullptr;
static IStream* resourceStream=nullptr;
static Reply currentReply;
static Action currentAction=Action::Inspect;
static bool objectLayoutChecked=false;
static std::wstring sessionNonce;
static std::unique_ptr<DisplayPlan> displayPlan;
static uint64_t lastDisplayGeneration=0;
static iTJSDispatch2* displayState=nullptr;
static bool displayActive=false;
static void** nativeFontVtable=nullptr;
struct DisplayEvent {uint64_t tick,serial;uint32_t replaced;std::string original,display;};
static std::vector<DisplayEvent> displayEvents;
static size_t displayEventBytes=0;
static bool readDisplayActive(){
    if(displayActive && displayState){
        tTJSVariant active;
        if(TJS_FAILED(displayState->PropGet(0,L"active",nullptr,&active,displayState)) || active.Type()!=tvtInteger || active.AsInteger()!=1)displayActive=false;
    }
    return displayActive;
}
static bool displayUtf8(const wchar_t* value,std::string& result){
    size_t length=wcslen(value);if(length>32768)return false;
    if(!length){result.clear();return true;}
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(length),nullptr,0,nullptr,nullptr);
    if(size<=0 || size>98304)return false;
    result.resize(size);
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(length),result.data(),size,nullptr,nullptr)==size;
}
static const wchar_t* displayBootstrap=
#include "display_script.inc"
;
static const wchar_t* displayInspect=
#include "display_inspect.inc"
;
static const wchar_t* displayEntry=
#include "display_entry.inc"
;

// sdkObjectFits 使用官方SDK构造对象并验证写入边界，不猜测内部字段布局
template<typename T> static bool sdkObjectFits(){
    alignas(T) uint8_t storage[4096];
    memset(storage,0xa5,sizeof(storage));
    T* object=::new(storage) T();
    bool bounded=true;
    for(size_t i=sizeof(T);i<sizeof(storage);i++)if(storage[i]!=0xa5){bounded=false;break;}
    object->~T();
    return bounded;
}

// ReadCallback 在引擎自己的TJS异常边界内读取原生资源，不改写原始资源
// NativeDisplayProxy 保留原生调用的参数个数与上下文，只替换本次测量或绘制的文字参数
class NativeDisplayProxy final:public tTJSDispatch {
    tTJSVariant original,enabled,source,display;
    iTJSDispatch2* scope;
    tjs_int textIndex;
    std::vector<tTJSVariant*> forwarded;
public:
    NativeDisplayProxy(const tTJSVariant& function,iTJSDispatch2* state,tjs_int index):scope(state),textIndex(index){
        original=function;scope->AddRef();
    }
    ~NativeDisplayProxy(){scope->Release();}
    tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 flags,const tjs_char* member,tjs_uint32* hint,tTJSVariant* result,tjs_int count,tTJSVariant** args,iTJSDispatch2* object) override {
        auto& function=original.AsObjectClosureNoAddRef();
        if(member || count<=textIndex || count>64 || args[textIndex]->Type()!=tvtString)
            return function.FuncCall(flags,member,hint,result,count,args,object);
        if(TJS_FAILED(scope->PropGet(0,L"enabled",nullptr,&enabled,scope)) || enabled.Type()!=tvtInteger || enabled.AsInteger()!=1 ||
           TJS_FAILED(scope->PropGet(0,L"source",nullptr,&source,scope)) || source.Type()!=tvtString || !source.DiscernCompare(*args[textIndex]) ||
           TJS_FAILED(scope->PropGet(0,L"display",nullptr,&display,scope)) || display.Type()!=tvtString)
            return function.FuncCall(flags,member,hint,result,count,args,object);
        forwarded.assign(args,args+count);forwarded[textIndex]=&display;
        // 可能跨编译器抛出异常的原生调用不持有栈上SDK对象，外层固定TJS代理恢复字体
        return function.FuncCall(flags,member,hint,result,count,forwarded.data(),object);
    }
};

// ReadCallback 只提供资源读取、位置查询和固定显示代理构造
class ReadCallback final:public tTJSDispatch {
public:
    tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32,const tjs_char* member,tjs_uint32*,tTJSVariant* result,tjs_int count,tTJSVariant** args,iTJSDispatch2*) override {
        if(member)return TJS_E_INVALIDPARAM;
        if(count==2){
            if(!result || args[0]->Type()!=tvtObject || args[1]->Type()!=tvtInteger)return TJS_E_INVALIDPARAM;
            auto kind=args[1]->AsInteger();
            const wchar_t* name=kind==0||kind==3?L"Layer":kind==1?L"Font":kind==2?L"KAGParser":nullptr;
            if(!name)return TJS_E_INVALIDPARAM;
            auto object=args[0]->AsObjectNoAddRef();
            auto classID=TJSFindNativeClassID(name);iTJSNativeInstance* instance=nullptr;
            bool native=object && classID>=0 && TJS_SUCCEEDED(object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,classID,&instance)) && instance;
            if(kind==3 && native){
                tTJSVariant font;
                native=TJS_SUCCEEDED(object->PropGet(0,L"font",nullptr,&font,object)) && font.Type()==tvtObject;
                auto fontObject=native?font.AsObjectNoAddRef():nullptr;
                native=fontObject && fontObject->IsInstanceOf(0,nullptr,nullptr,L"Font",fontObject)==TJS_S_TRUE;
                nativeFontVtable=native?*reinterpret_cast<void***>(fontObject):nullptr;
            }else if(kind==1 && classID<0){
                // 旧版Font为直接派发对象，没有独立的原生类登记；只接收原生Layer参照的同一接口布局
                native=object && nativeFontVtable && *reinterpret_cast<void***>(object)==nativeFontVtable && object->IsInstanceOf(0,nullptr,nullptr,L"Font",object)==TJS_S_TRUE;
            }
            *result=tTJSVariant(tjs_int(native));
            return TJS_S_OK;
        }
        if(count==4){
            if(!displayActive || !displayPlan || args[0]->Type()!=tvtString || args[1]->Type()!=tvtString || args[2]->Type()!=tvtInteger || args[3]->Type()!=tvtInteger)return TJS_S_OK;
            auto replaced=args[2]->AsInteger();auto serial=args[3]->AsInteger();
            if((replaced!=0 && replaced!=1) || serial<0 || serial>tjs_int64(1)<<53)return TJS_S_OK;
            try {
                DisplayEvent event{GetTickCount64(),uint64_t(serial),uint32_t(replaced),{}, {}};
                if(!displayUtf8(ttstr(*args[0]).c_str(),event.original) || !displayUtf8(ttstr(*args[1]).c_str(),event.display) || event.original.empty())return TJS_S_OK;
                size_t bytes=event.original.size()+event.display.size()+28;
                while(!displayEvents.empty() && (displayEvents.size()>=64 || displayEventBytes+bytes>2*1024*1024)){
                    displayEventBytes-=displayEvents.front().original.size()+displayEvents.front().display.size()+28;
                    displayEvents.erase(displayEvents.begin());
                }
                displayEvents.push_back(std::move(event));displayEventBytes+=bytes;
            }catch(...){displayEvents.clear();displayEventBytes=0;}
            return TJS_S_OK;
        }
        if(count==3){
            if(!result || args[0]->Type()!=tvtObject || args[1]->Type()!=tvtObject || args[2]->Type()!=tvtInteger)return TJS_E_INVALIDPARAM;
            tjs_int64 index=args[2]->AsInteger();
            if(index!=0 && index!=2)return TJS_E_INVALIDPARAM;
            auto function=new NativeDisplayProxy(*args[0],args[1]->AsObjectNoAddRef(),tjs_int(index));
            *result=tTJSVariant(static_cast<iTJSDispatch2*>(function));function->Release();
            return TJS_S_OK;
        }
        if(count==5){
            if(result)result->Clear();
            if(!displayPlan || !result || args[0]->Type()!=tvtString || args[1]->Type()!=tvtInteger || args[2]->Type()!=tvtInteger || args[3]->Type()!=tvtString || args[4]->Type()!=tvtString)return TJS_S_OK;
            tjs_int64 line=args[1]->AsInteger(),position=args[2]->AsInteger();
            if(line<0 || line>1000000 || position<=0 || position>32768)return TJS_S_OK;
            std::wstring display;
            if(displayPlan->lookup(ttstr(*args[0]).c_str(),uint32_t(line),uint32_t(position),ttstr(*args[3]).c_str(),ttstr(*args[4]).c_str(),display))*result=tTJSVariant(display.c_str());
            return TJS_S_OK;
        }
        if(count!=0 || currentAction!=Action::ReadResource || !resourceName)return TJS_E_INVALIDPARAM;
        // 会跨编译器抛出的引擎操作仅使用长期保存的对象，不依赖本栈帧析构
        resourceStream=TVPCreateIStream(*resourceName,TJS_BS_READ);
        if(!resourceStream){currentReply.status=2;return TJS_S_OK;}
        char buffer[65536];
        for(;;){
            ULONG read=0;HRESULT status=resourceStream->Read(buffer,sizeof(buffer),&read);
            if(FAILED(status) || read>sizeof(buffer) || currentReply.data.size()+read>resourceLimit){currentReply.status=2;break;}
            try {currentReply.data.insert(currentReply.data.end(),buffer,buffer+read);}catch(...){currentReply.status=2;break;}
            if(!read || status==S_FALSE)break;
        }
        if(result)*result=tTJSVariant(tjs_int(currentReply.status==0));
        return TJS_S_OK;
    }
};
static ReadCallback* readCallback=nullptr;

// utf8Reply 只将固定诊断和引擎异常描述编码为通信结果
static Reply utf8Reply(uint32_t status,const std::wstring& text){
    Reply reply;reply.status=status;
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.c_str(),int(text.size()),nullptr,0,nullptr,nullptr);
    if(size<=0 || size>65536){reply.status=2;return reply;}
    reply.data.resize(size);
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.c_str(),int(text.size()),reinterpret_cast<char*>(reply.data.data()),size,nullptr,nullptr);
    return reply;
}

static bool initialize(void* value,const std::wstring& nonce){
    if(globalObject || nonce.size()!=32)return false;
    for(auto ch:nonce)if(!((ch>=L'0'&&ch<=L'9')||(ch>=L'a'&&ch<=L'f')))return false;
    auto exporter=static_cast<iTVPFunctionExporter*>(value);
    const char* required[]={
        "void ::TVPExecuteExpression(const ttstr &,tTJSVariant *)",
        "HWND ::TVPGetApplicationWindowHandle()",
        "tTJSVariantString * ::TJSAllocVariantString(const tjs_char *)",
        "iTJSDispatch2 * ::TVPGetScriptDispatch()",
        "IStream * ::TVPCreateIStream(const ttstr &,tjs_uint32)",
        "tTJSVariant::tTJSVariant()",
        "tTJSVariant::~ tTJSVariant()",
        "tTJSVariantType tTJSVariant::Type()",
        "tTJSVariant::tTJSVariant(iTJSDispatch2 *)",
        "tTJSVariant::tTJSVariant(tjs_int32)",
        "tTJSVariant & tTJSVariant::operator =(const tTJSVariant &)",
        "iTJSDispatch2 * tTJSVariant::AsObjectNoAddRef() const",
        "tTJSString::tTJSString()",
        "tTJSString::tTJSString(const tjs_char *)",
        "tTJSString::tTJSString(const tTJSVariant &)",
        "tTJSString::~ tTJSString()",
        "const tjs_char * tTJSString::c_str() const"
        ,"void tTJSVariant::Clear()"
        ,"tTVInteger tTJSVariant::AsInteger() const"
        ,"tTJSVariant::tTJSVariant(const tjs_char *)"
        ,"tTJSVariantClosure & tTJSVariant::AsObjectClosureNoAddRef() const"
        ,"bool tTJSVariant::DiscernCompare(const tTJSVariant &) const"
        ,"tjs_int32 ::TJSFindNativeClassID(const tjs_char *)"
    };
    void* functions[sizeof(required)/sizeof(required[0])]={};
    if(!exporter->QueryFunctionsByNarrowString(required,functions,unsigned(sizeof(functions)/sizeof(functions[0]))))return false;
    for(auto pointer:functions)if(!pointer)return false;
    if(!TVPInitImportStub(exporter))return false;
    // 初始化字符串与全局派发对象必须由GUI线程后续首次任务完成
    callbackName=L"__dangoKrkrNative_"+nonce;
    sessionNonce=nonce;
    lastDisplayGeneration=0;
    return true;
}
static HWND applicationWindow(){return TVPGetApplicationWindowHandle();}

// attachCallback 只注册本会话的随机名称，已有同名成员时拒绝接管
static bool attachCallback(){
    if(globalObject)return true;
    auto object=TVPGetScriptDispatch();
    if(!object)return false;
    tTJSVariant existing;
    if(TJS_SUCCEEDED(object->PropGet(0,callbackName.c_str(),nullptr,&existing,object))){object->Release();return false;}
    auto callback=new ReadCallback();
    tTJSVariant value(static_cast<iTJSDispatch2*>(callback));
    if(TJS_FAILED(object->PropSet(TJS_MEMBERENSURE,callbackName.c_str(),nullptr,&value,object))){callback->Release();object->Release();return false;}
    globalObject=object;readCallback=callback;
    return true;
}

// perform 只执行固定操作，所有资源参数通过SDK字符串传入而不拼成TJS源码
static Reply perform(Action action,const std::wstring& argument,const std::vector<uint8_t>& payload){
    if(!objectLayoutChecked){
        if(!sdkObjectFits<tTJSVariant>() || !sdkObjectFits<ttstr>())return utf8Reply(2,L"KiriKiri SDK object boundary mismatch");
        objectLayoutChecked=true;
    }
    if(action==Action::Detach){
        if(displayState){
            std::wstring source=L"(function(){try{global['"+callbackName+L"_display'].detach();return 1;}catch(e){return 'detach-error:'+e.message;}})()";
            tTJSVariant result;TVPExecuteExpression(ttstr(source.c_str()),&result);
            if(result.Type()==tvtString)return utf8Reply(2,ttstr(result).c_str());
            tTJSVariant value;
            if(TJS_SUCCEEDED(globalObject->PropGet(0,(callbackName+L"_display").c_str(),nullptr,&value,globalObject)) && value.Type()==tvtObject && value.AsObjectNoAddRef()==displayState)
                globalObject->DeleteMember(0,(callbackName+L"_display").c_str(),nullptr,globalObject);
            displayState->Release();displayState=nullptr;
        }
        nativeFontVtable=nullptr;displayEvents.clear();displayEventBytes=0;displayActive=false;displayPlan.reset();
        if(globalObject){
            tTJSVariant value;
            if(TJS_SUCCEEDED(globalObject->PropGet(0,callbackName.c_str(),nullptr,&value,globalObject)) && value.Type()==tvtObject && value.AsObjectNoAddRef()==readCallback)
                globalObject->DeleteMember(0,callbackName.c_str(),nullptr,globalObject);
            globalObject->Release();globalObject=nullptr;
        }
        if(readCallback){readCallback->Release();readCallback=nullptr;}
        ::delete resourceName;resourceName=nullptr;
        return {};
    }
    if(action==Action::StageDisplay){
        if(displayState)return utf8Reply(2,L"KiriKiri display snapshot already prepared");
        auto next=parseDisplayPlan(payload,sessionNonce,lastDisplayGeneration,displayPlan?displayPlan->memoryUsage:0);
        if(!next)return utf8Reply(2,L"KiriKiri display snapshot validation failed");
        displayPlan=std::move(next);
        lastDisplayGeneration=displayPlan->generation;
        return utf8Reply(0,L"staged="+std::to_wstring(displayPlan->generation)+L";slots="+std::to_wstring(displayPlan->slotCount));
    }
    if(action==Action::DisplayStatus){
        readDisplayActive();
        std::wstring diagnostic;
        if(displayState){
            std::wstring source=L"global['"+callbackName+L"_display'].report()";
            tTJSVariant result;TVPExecuteExpression(ttstr(source.c_str()),&result);
            if(result.Type()==tvtString)diagnostic=L";"+std::wstring(ttstr(result).c_str());
        }
        return utf8Reply(0,L"staged="+std::to_wstring(displayPlan?displayPlan->generation:0)+L";prepared="+std::to_wstring(displayState?1:0)+L";active="+std::to_wstring(displayActive?1:0)+diagnostic);
    }
    if(action==Action::PollDisplay){
        Reply reply;
        auto append=[&](const void* data,size_t size){auto bytes=static_cast<const uint8_t*>(data);reply.data.insert(reply.data.end(),bytes,bytes+size);};
        uint64_t generation=displayPlan?displayPlan->generation:0;
        uint32_t active=readDisplayActive()?1:0,count=active?uint32_t(displayEvents.size()):0;
        append(&generation,8);append(&active,4);append(&count,4);
        if(active)for(auto& event:displayEvents){
            uint32_t original=uint32_t(event.original.size()),display=uint32_t(event.display.size());
            append(&event.tick,8);append(&event.serial,8);append(&event.replaced,4);append(&original,4);append(&display,4);
            append(event.original.data(),original);append(event.display.data(),display);
        }
        displayEvents.clear();displayEventBytes=0;
        return reply;
    }
    if(action==Action::ActivateDisplay){
        if(!displayState || !displayPlan || displayActive)return utf8Reply(2,L"KiriKiri display not prepared");
        std::wstring source=L"(function(){try{return global['"+callbackName+L"_display'].activate();}catch(e){return 'activate-error:'+e.message;}})()";
        tTJSVariant result;TVPExecuteExpression(ttstr(source.c_str()),&result);
        if(result.Type()!=tvtInteger || result.AsInteger()!=1)return utf8Reply(2,ttstr(result).c_str());
        displayActive=true;
        return utf8Reply(0,L"active="+std::to_wstring(displayPlan->generation));
    }
    if(action==Action::DeactivateDisplay){
        if(displayState){
            std::wstring source=L"(function(){try{global['"+callbackName+L"_display'].detach();return 1;}catch(e){return 'deactivate-error:'+e.message;}})()";
            tTJSVariant result;TVPExecuteExpression(ttstr(source.c_str()),&result);
            if(result.Type()!=tvtInteger || result.AsInteger()!=1)return utf8Reply(2,ttstr(result).c_str());
            globalObject->DeleteMember(0,(callbackName+L"_display").c_str(),nullptr,globalObject);
            displayState->Release();displayState=nullptr;
        }
        displayEvents.clear();displayEventBytes=0;displayActive=false;displayPlan.reset();
        return utf8Reply(0,L"active=0");
    }
    if(action==Action::PrepareDisplay){
        if(!displayPlan || displayState || !attachCallback())return utf8Reply(2,L"KiriKiri display snapshot unavailable");
        tTJSVariant existing;
        std::wstring stateName=callbackName+L"_display";
        if(TJS_SUCCEEDED(globalObject->PropGet(0,stateName.c_str(),nullptr,&existing,globalObject)))return utf8Reply(2,L"KiriKiri display state name collision");
        // 固定源码没有外部脚本参数，创建状态并不接管游戏显示对象
        std::wstring source=L"(function(){var cfg=void;try{cfg="+std::wstring(displayBootstrap)+L";cfg.resolve="+std::wstring(displayEntry)+L";cfg.native=global['"+callbackName+L"'];if(!cfg.native(cfg.reference,3))throw new global.Exception('native-font-reference');invalidate cfg.reference;cfg.reference=void;global['"+stateName+L"']=cfg;return cfg;}catch(e){if(cfg!==void && cfg.reference!==void)invalidate cfg.reference;return 'display-error:'+e.message;}})()";
        tTJSVariant result;TVPExecuteExpression(ttstr(source.c_str()),&result);
        if(result.Type()!=tvtObject)return utf8Reply(2,ttstr(result).c_str());
        displayState=result.AsObjectNoAddRef();displayState->AddRef();
        return utf8Reply(0,L"prepared=1;active=0");
    }
    if(action==Action::Inspect){
        tTJSVariant result;
        TVPExecuteExpression(ttstr(L"(function(){try{return 'sdk=1;parser='+(kag.conductor instanceof \'KAGParser\')+';messages='+kag.fore.messages.count+';back='+kag.back.messages.count+';storage='+kag.conductor.curStorage;}catch(e){return 'sdk=1;parser=0;error='+e.message;}})()"),&result);
        return utf8Reply(0,std::wstring(ttstr(result).c_str())+L";abi="+DANGO_KRKR_SDK_ABI);
    }
    if(action==Action::InspectLayout){
        if(!attachCallback())return utf8Reply(2,L"KiriKiri SDK callback unavailable");
        std::wstring source=displayInspect;
        auto marker=source.find(L"__DANGO_NATIVE_CALLBACK__");
        if(marker==std::wstring::npos)return utf8Reply(2,L"KiriKiri layout proof unavailable");
        source.replace(marker,wcslen(L"__DANGO_NATIVE_CALLBACK__"),callbackName);
        marker=source.find(L"__DANGO_ENTRY_RESOLVER__");
        if(marker==std::wstring::npos)return utf8Reply(2,L"KiriKiri display entry proof unavailable");
        source.replace(marker,wcslen(L"__DANGO_ENTRY_RESOLVER__"),displayEntry);
        tTJSVariant result;TVPExecuteExpression(ttstr(source.c_str()),&result);
        return utf8Reply(0,ttstr(result).c_str());
    }
    if(action!=Action::ReadResource || argument.empty() || argument.size()>1024 || !attachCallback())return utf8Reply(2,L"KiriKiri SDK action unavailable");
    currentAction=action;currentReply={};resourceName=::new ttstr(argument.c_str());
    // 名称只由initialize验证过的十六进制令牌组成，外部不能提交可执行脚本
    std::wstring source=L"(function(){try{return global['"+callbackName+L"']();}catch(e){return 'read-error:'+e.message;}})()";
    tTJSVariant result;
    TVPExecuteExpression(ttstr(source.c_str()),&result);
    if(resourceStream){resourceStream->Release();resourceStream=nullptr;}
    ::delete resourceName;resourceName=nullptr;
    currentAction=Action::Inspect;
    if(result.Type()==tvtString)return utf8Reply(2,ttstr(result).c_str());
    if(currentReply.status!=0)currentReply.data.clear();
    return std::move(currentReply);
}
static const SdkApi api={initialize,applicationWindow,perform,DANGO_KRKR_SDK_ABI};
