#include "CSystem.h"
#include "CSystemLayout.generated.h"
#include "CSystemDisplay.generated.h"
#include "CSystemControl.h"
#include "CSystemFont.h"
#include <atomic>
#include <array>
#include <memory>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
struct Range { uintptr_t first, last; };
std::vector<Range> ranges;
std::map<std::string,uintptr_t> sites;
std::atomic<bool> enabled{false};
std::atomic<bool> choicesEnabled{false};
std::atomic<bool> displayEnabled{false};
std::atomic<unsigned> activeRenders{0};
std::atomic_flag waiting = ATOMIC_FLAG_INIT;
struct WaitingGuard { ~WaitingGuard(){waiting.clear();} };
bool quiescing=false;
csystemcontrol::Client displayClient;
csystemfont::Resources displayFonts;
struct BackfillSnapshot { csystemcontrol::Mode mode{}; csystemsnapshot::Texts texts; };
std::shared_ptr<const BackfillSnapshot> backfillSnapshot;
std::thread snapshotThread;
std::atomic<bool> snapshotStopping{false},snapshotStopped{true};
uintptr_t fontDraw=0;
std::atomic<bool> unsafeReported{false},fontReported{false};
using Record=std::array<BYTE,0x1118>;
struct RenderFrame { std::shared_ptr<Record> record; std::shared_ptr<csystemfont::Bundle> fonts; std::shared_ptr<std::array<IDWriteTextFormat *,2>> formatPair; uint64_t request=0,displayPage=0,displayPart=0; uintptr_t displayOwner=0; std::wstring displayOriginal; csystemcontrol::Mode mode{}; };
thread_local std::vector<RenderFrame> renderFrames;
thread_local unsigned overflowFrames=0;
std::atomic<unsigned long long> serial{0};
std::mutex stateMutex;
struct State {
    DWORD renderThread=0;
    std::vector<BYTE> source;
    unsigned long long body=0, menu=0, revision=0, captured=0;
    uintptr_t capturedMenu=0;
    std::shared_ptr<Record> translated;
    std::shared_ptr<csystemfont::Bundle> fonts;
    Record original{};
    unsigned long long translatedRevision=0;
    ULONGLONG lastPageAt=0,lastRapidAt=0;
    unsigned rapid=0;
    uint64_t displayEpoch=0,displayPage=0,displayPart=0;
    unsigned displayLines=0;
    bool displayApplied=false,displayRejected=false;
    std::wstring displayOriginal,displayTranslation;
};
std::map<uintptr_t,State> states;

// loadBackfillSnapshots 在游戏排版线程之外加载并确认整份不可变快照
void loadBackfillSnapshots() {
    uint64_t epoch=0,helper=0;
    while(!snapshotStopping.load()) {
        csystemcontrol::Mode mode{};
        if(displayClient.mode(mode)&&mode.enabled==2) {
            if(mode.epoch!=epoch||mode.helperCreated!=helper) {
                epoch=mode.epoch;helper=mode.helperCreated;std::shared_ptr<BackfillSnapshot> next;
                bool ok=false;try {next=std::make_shared<BackfillSnapshot>();next->mode=mode;ok=displayClient.loadSnapshot(mode,next->texts);}catch(...){ok=false;}
                if(!snapshotStopping.load()&&displayClient.currentBackfill(mode)) {
                    std::shared_ptr<const BackfillSnapshot> value=ok?next:nullptr;std::atomic_store(&backfillSnapshot,value);
                    displayClient.acknowledge(mode,ok);
                }
            }
        } else {std::atomic_store(&backfillSnapshot,std::shared_ptr<const BackfillSnapshot>{});epoch=0;helper=0;}
        Sleep(20);
    }
    snapshotStopped.store(true);
}

// readable 只接受有界的已提交可读区域
bool readable(uintptr_t address,size_t n) {
    if(!address||!n||n>32*1024*1024||address>UINTPTR_MAX-n)return false;
    const auto end=address+n;
    while(address<end) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<void *>(address),&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto protection=m.Protect&0xff;
        if(protection!=PAGE_READONLY&&protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&protection!=PAGE_EXECUTE_READ&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto first=reinterpret_cast<uintptr_t>(m.BaseAddress);
        if(first>UINTPTR_MAX-m.RegionSize)return false;
        const auto next=first+m.RegionSize;if(next<=address)return false;
        address=next<end?next:end;
    }
    return true;
}

// code 只接受主模块的可执行节
bool code(uintptr_t address,size_t n) {for(const auto &r:ranges)if(address>=r.first&&address<=r.last&&n<=r.last-address)return true;return false;}

// collect 检查原生架构与主模块节边界
bool collect() {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!readable(base,sizeof(IMAGE_DOS_HEADER)))return false;
    const auto *dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000)return false;
    const auto ntAddress=base+dos->e_lfanew;if(!readable(ntAddress,sizeof(IMAGE_NT_HEADERS32)))return false;
    const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS32 *>(ntAddress);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->FileHeader.NumberOfSections>96)return false;
    const auto *sec=IMAGE_FIRST_SECTION(nt);if(!readable(reinterpret_cast<uintptr_t>(sec),sizeof(*sec)*nt->FileHeader.NumberOfSections))return false;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        if(!(sec[i].Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const auto size=sec[i].Misc.VirtualSize;
        if(!size||size>32*1024*1024||sec[i].VirtualAddress>nt->OptionalHeader.SizeOfImage||size>nt->OptionalHeader.SizeOfImage-sec[i].VirtualAddress||!readable(base+sec[i].VirtualAddress,size))return false;
        ranges.push_back({base+sec[i].VirtualAddress,base+sec[i].VirtualAddress+size});
    }
    return !ranges.empty();
}

// findUnique 以共享特征定位唯一入口，拒绝重复候选
uintptr_t findUnique(const char *text) {
    std::istringstream input(text);std::string token;std::vector<int> p;
    while(input>>token)p.push_back(token=="??"?-1:int(strtoul(token.c_str(),nullptr,16)));
    uintptr_t found=0;
    for(const auto &r:ranges) {
        if(p.empty()||p.size()>r.last-r.first)continue;
        for(auto address=r.first;address<=r.last-p.size();address++) {
            const auto *b=reinterpret_cast<const BYTE *>(address);size_t i=0;for(;i<p.size();i++)if(p[i]>=0&&b[i]!=p[i])break;
            if(i!=p.size())continue;if(found)return 0;found=address;
        }
    }
    return found;
}

// valid 检查真实相对调用指向同一份已定位参数入口
bool valid(const char *name) {
    const auto address=sites[name];if(!address)return false;
    for(const auto &spec:dangoCSystemFunctions)if(strcmp(spec.name,name)==0) {
        if(!*spec.target)return true;const auto call=address+spec.callOffset;
        return code(call,5)&&*reinterpret_cast<const BYTE *>(call)==0xe8&&call+5+*reinterpret_cast<const int32_t *>(call+1)==sites[spec.target];
    }
    return false;
}

bool matchesAt(uintptr_t address,const char *text) {
    std::istringstream input(text);std::string token;std::vector<int> pattern;
    while(input>>token) { if(pattern.size()>=32768)return false;pattern.push_back(token=="??"?-1:int(strtoul(token.c_str(),nullptr,16))); }
    if(pattern.empty()||!code(address,pattern.size()))return false;
    const auto *bytes=reinterpret_cast<const BYTE *>(address);
    for(size_t i=0;i<pattern.size();i++)if(pattern[i]>=0&&bytes[i]!=pattern[i])return false;
    return true;
}

// Display validation is independent of the reader, including the complete
// variable-length UTF16 consumer and all 130 bound direct calls / 31 implementations.
bool displayLayout() {
    const auto render=sites["render"];
    if(!matchesAt(render,dangoCSystemRenderPattern)||!matchesAt(sites["dialogue"]+50,dangoCSystemReturnPattern))return false;
    std::map<unsigned,uintptr_t> groups;std::map<uintptr_t,unsigned> targets;
    for(const auto &spec:dangoCSystemDisplayCalls) {
        const auto call=render+spec.offset;if(!code(call,5)||*reinterpret_cast<const BYTE *>(call)!=0xe8)return false;
        auto target=call+5+*reinterpret_cast<const int32_t *>(call+1);
        if((groups.count(spec.group)&&groups[spec.group]!=target)||(targets.count(target)&&targets[target]!=spec.group))return false;
        groups[spec.group]=target;targets[target]=spec.group;
        if(spec.followJump) {if(!code(target,5)||*reinterpret_cast<const BYTE *>(target)!=0xe9)return false;target+=5+*reinterpret_cast<const int32_t *>(target+1);}
        if(!matchesAt(target,spec.pattern))return false;
    }
    fontDraw=groups[dangoCSystemFontGroup];
    if(!fontDraw||dangoCSystemFontHookOffset!=53||!matchesAt(fontDraw+47,"8B 9F 48 01 00 00 66 89 55 F4"))return false;
    for(auto offset:dangoCSystemFontDrawOffsets){const auto a=fontDraw+offset;if(!code(a,3))return false;const auto *b=reinterpret_cast<const BYTE *>(a);if(b[0]!=0xff||(b[1]!=0x51&&b[1]!=0x52)||b[2]!=0x6c)return false;}
    return !groups.empty();
}

// jsonString 完整转义UTF16协议数据，保持页包边界
std::wstring jsonString(const std::wstring &s) {
    const wchar_t *hex=L"0123456789abcdef";std::wstring out=L"\"";
    for(auto c:s) {if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}else if(c<0x20){out+=L"\\u00";out+=hex[(c>>4)&15];out+=hex[c&15];}else out+=c;}
    return out+L"\"";
}

// readText 读取已解密的完整T记录，消费注音和图符但不猜测姓名
bool readText(uintptr_t record,std::wstring &out) {
    if(!readable(record,0x1118)||*reinterpret_cast<const WORD *>(record)!=L'T')return false;
    const auto units=*reinterpret_cast<const DWORD *>(record+0x1004);
    if(units<2||units>2047)return false;
    const auto *text=reinterpret_cast<const wchar_t *>(record+2);const auto n=units-1;
    for(size_t i=0;i<n;) {
        auto c=text[i++];
        if(c==0)return false;
        if(c==0xff&&i<n&&text[i]==0xff) {
            i++;if(i>=n)return false;const size_t reading=text[i++];if(reading>n-i)return false;i+=reading;if(i>=n)return false;const size_t base=text[i++];if(!base||base>n-i)return false;
            for(size_t j=0;j<base;j++){const auto b=text[i+j];if(b<0x20||b==0xff||b==0xffee)return false;out+=b;}i+=base;continue;
        }
        if(c==L'e'&&i<n&&text[i]==L'%') {if(n-i<3)return false;i+=3;out+=wchar_t(0xfffc);continue;}
        if(c==L'/'&&i<n&&text[i]==L'n'){i++;out+=L'\n';continue;}
        if(c==0xffee||(c<0x20&&c!=L'\n'&&c!=L'\r'&&c!=L'\t'))return false;
        out+=c;
    }
    // 字符读取保持UTF16原义，代理项必须完整配对
    for(size_t i=0;i<out.size();i++){const auto c=out[i];if(c>=0xd800&&c<=0xdbff){if(++i>=out.size()||out[i]<0xdc00||out[i]>0xdfff)return false;}else if(c>=0xdc00&&c<=0xdfff)return false;}
    return !out.empty();
}

bool sameRecord(uintptr_t address,const Record &record) {
    if(!readable(address,record.size()))return false;
    const auto *raw=reinterpret_cast<const BYTE *>(address);
    return memcmp(raw,record.data(),0x1000)==0&&memcmp(raw+0x1004,record.data()+0x1004,0x114)==0;
}

std::shared_ptr<Record> translatedRecord(const Record &original,std::wstring text,const std::wstring &source) {
    if(text.empty()||text.size()>2046)return {};
    std::vector<std::wstring> images;
    const auto *raw=reinterpret_cast<const wchar_t *>(original.data()+2);
    const auto count=*reinterpret_cast<const DWORD *>(original.data()+0x1004)-1;
    for(size_t i=0;i<count;) {
        if(raw[i]==0xff&&i+1<count&&raw[i+1]==0xff) {i+=2;const auto reading=raw[i++];i+=reading;const auto base=raw[i++];i+=base;continue;}
        if(raw[i]==L'e'&&i+3<count&&raw[i+1]==L'%'){images.emplace_back(raw+i,4);i+=4;}else i++;
    }
    auto trimmedSource=source;while(!trimmedSource.empty()&&(trimmedSource.back()==L'\n'||trimmedSource.back()==L'\r'||trimmedSource.back()==L' '))trimmedSource.pop_back();
    while(!text.empty()&&(text.back()==L'\n'||text.back()==L'\r'||text.back()==L' '))text.pop_back();
    if(text.empty())return {};
    if(trimmedSource.size()>=2&&trimmedSource.front()==L'【'&&trimmedSource.back()==L'】'&&!(text.front()==L'【'&&text.back()==L'】'))text=L"【"+text+L"】";
    // 全部图符被翻译源省略时，按原顺序放在句尾的闭引号之前
    // 不猜测译文词语位置；部分丢失或新增占位仍整句拒绝
    if(!images.empty()&&text.find(wchar_t(0xfffc))==std::wstring::npos){
        size_t end=text.size();while(end&&std::wstring(L"」』】\"”’）)").find(text[end-1])!=std::wstring::npos)end--;
        text.insert(end,images.size(),wchar_t(0xfffc));
    }
    std::wstring output;size_t image=0;
    for(size_t i=0;i<text.size();i++) {
        const auto c=text[i];
        if(!c||c==0xfe||c==0xff||c==0xffee||(c<0x20&&c!=L'\n'&&c!=L'\r'&&c!=L'\t')||(c>=0xd800&&c<=0xdfff)||c==L'/'||(c==L'e'&&i+1<text.size()&&text[i+1]==L'%'))return {};
        if(c==0xfffc) {if(image>=images.size())return {};output+=images[image++];}
        else if(c!=L'\r')output+=c;
        if(output.size()>2046)return {};
    }
    if(image!=images.size())return {};
    if(count&&raw[count-1]==L'\n'&&(output.empty()||output.back()!=L'\n'))output+=L'\n';
    if(output.empty()||output.size()>2046)return {};
    auto record=std::make_shared<Record>(original);memset(record->data(),0,0x1000);*reinterpret_cast<WORD *>(record->data())=L'T';
    memcpy(record->data()+2,output.data(),output.size()*2);
    *reinterpret_cast<DWORD *>(record->data()+0x1000)=0;
    *reinterpret_cast<DWORD *>(record->data()+0x1004)=DWORD(output.size()+1);
    *reinterpret_cast<DWORD *>(record->data()+0x1008)=DWORD(output.size());
    return record;
}

// Only the third display argument is redirected. The per-call shared_ptr stays
// alive until the verified return hook, even if parsing/reset happens reentrantly.
void applyRecord(hook_context *context,State &state) {
    if(quiescing||state.renderThread!=GetCurrentThreadId()||!state.translated||state.translatedRevision!=state.revision||renderFrames.size()!=1||overflowFrames||renderFrames.back().record||!sameRecord(context->stack[2],state.original))return;
    renderFrames.back().record=state.translated;
    renderFrames.back().fonts=state.fonts;
    renderFrames.back().formatPair=std::make_shared<std::array<IDWriteTextFormat *,2>>();
    context->stack[2]=reinterpret_cast<uintptr_t>(state.translated->data());
}

// drawTranslatedFont 在格式指针加载后重定向私有双格式表，保留原生绘制调用
void drawTranslatedFont(hook_context *context,HookParam *,TextBuffer *,uintptr_t *) {
    if(renderFrames.size()!=1||overflowFrames||!renderFrames.back().record||!renderFrames.back().fonts||!renderFrames.back().formatPair)return;
    if(!readable(context->ebx,8))return;
    auto &frame=renderFrames.back();auto source=reinterpret_cast<IDWriteTextFormat **>(context->ebx);
    auto first=displayFonts.resolve(*frame.fonts,source[0]),second=displayFonts.resolve(*frame.fonts,source[1]);
    if(!first||!second)return;
    (*frame.formatPair)[0]=first;(*frame.formatPair)[1]=second;context->ebx=reinterpret_cast<uintptr_t>(frame.formatPair->data());
}

// prepareFonts 核对已验证绘制对象中的两份显示格式，不触碰共享字体字段
std::shared_ptr<csystemfont::Bundle> prepareFonts(uintptr_t object,const Record &record) {
    if(!readable(object+0x148,4))return {};auto wrapper=*reinterpret_cast<uintptr_t *>(object+0x148);
    if(!readable(wrapper,8))return {};auto first=*reinterpret_cast<IDWriteTextFormat **>(wrapper),second=*reinterpret_cast<IDWriteTextFormat **>(wrapper+4);
    if(!readable(reinterpret_cast<uintptr_t>(first),4)||!readable(reinterpret_cast<uintptr_t>(second),4))return {};
    std::wstring text;if(!readText(reinterpret_cast<uintptr_t>(record.data()),text))return {};
    return displayFonts.prepare(first,second,text);
}

void rendered(hook_context *,HookParam *,TextBuffer *,uintptr_t *) {
    if(overflowFrames){overflowFrames--;activeRenders.fetch_sub(1);return;}if(renderFrames.empty())return;
    auto frame=std::move(renderFrames.back());renderFrames.pop_back();
    if(frame.request)displayClient.result(frame.mode,frame.request,true);
    // 全量显示只复制已经返回的排版结果，不参与实时请求或原始脚本写入
    if(frame.displayPage&&displayClient.currentBackfill(frame.mode))try {
        std::lock_guard<std::mutex> lock(stateMutex);auto found=states.find(frame.displayOwner);
        if(found!=states.end()&&found->second.body==frame.displayPage&&found->second.revision==frame.displayPart&&!quiescing) {
            auto &s=found->second;
            if(s.displayEpoch!=frame.mode.epoch||s.displayPage!=frame.displayPage) {
                s.displayEpoch=frame.mode.epoch;s.displayPage=frame.displayPage;s.displayPart=0;s.displayLines=0;s.displayApplied=false;s.displayRejected=false;s.displayOriginal.clear();s.displayTranslation.clear();
            }
            if(!s.displayRejected&&frame.displayPart>s.displayPart) {
                auto original=csystemsnapshot::normalize(frame.displayOriginal);std::wstring translation;
                const bool applied=frame.record&&readText(reinterpret_cast<uintptr_t>(frame.record->data()),translation);
                translation=csystemsnapshot::normalize(applied?translation:original);
                if(!original.empty()) {
                    const size_t separator=s.displayLines?1:0;
                    if(s.displayLines>=64||s.displayOriginal.size()+separator+original.size()>=csystemcontrol::DisplayCapacity||s.displayTranslation.size()+separator+translation.size()>=csystemcontrol::DisplayCapacity) {
                        s.displayRejected=true;displayClient.display(frame.mode,frame.displayPage,frame.displayPart,L"",L"",3);
                    } else {
                        if(separator){s.displayOriginal+=L'\n';s.displayTranslation+=L'\n';}
                        s.displayOriginal+=original;s.displayTranslation+=translation;s.displayLines++;s.displayPart=frame.displayPart;s.displayApplied=s.displayApplied||applied;
                        displayClient.display(frame.mode,frame.displayPage,frame.displayPart,s.displayOriginal,s.displayApplied?s.displayTranslation:L"",s.displayApplied?1:2);
                    }
                }
            }
        }
    } catch(...) { /* 显示通知失败不改变已完成的排版和注入 */ }
    activeRenders.fetch_sub(1);
}

// packet 生成带递增页面和记录身份的只读快照
void packet(TextBuffer *buffer,int kind,unsigned long long page,unsigned long long part,const std::wstring &text) {
    buffer->from(L"{\"csystem\":1,\"kind\":"+std::to_wstring(kind)+L",\"page\":"+std::to_wstring(page)+L",\"part\":"+std::to_wstring(part)+L",\"text\":"+jsonString(text)+L"}");
}

// resetTimeline 在重新加载和读档时淘汰全部旧页面，状态仅属于本Hook
void resetTimeline(hook_context *,HookParam *,TextBuffer *buffer,uintptr_t *split) {
    if(!enabled.load())return;std::lock_guard<std::mutex> lock(stateMutex);states.clear();const auto page=++serial;*split=0;packet(buffer,2,page,0,L"");
    csystemcontrol::Mode mode{};if(displayClient.mode(mode)&&mode.enabled==2)displayClient.display(mode,page,0,L"",L"",3);
}

// observe 在当前记录复制前读取来源身份，忽略可变游标以排除重复绘制
void observe(hook_context *context,HookParam *,TextBuffer *,uintptr_t *) {
    if(!enabled.load())return;
    const auto record=context->stack[0];const auto target=context->ecx;
    if(target<0x11b8)return;const auto owner=target-0x11b8;
    if(record!=owner+0x9c||!readable(record,0x1118))return;
    const auto *raw=reinterpret_cast<const BYTE *>(record);const auto units=*reinterpret_cast<const DWORD *>(record+0x1004);
    if(!units||units>2048)return;
    std::vector<BYTE> signature(raw,raw+units*2);
    signature.insert(signature.end(),raw+0x1004,raw+0x1118);
    std::lock_guard<std::mutex> lock(stateMutex);
    if(states.size()>=8&&!states.count(owner))return;
    auto &s=states[owner];if(s.source==signature)return;s.source=std::move(signature);s.revision++;s.captured=0;s.capturedMenu=0;
    if(*reinterpret_cast<const WORD *>(record)==L'M'&&*reinterpret_cast<const WORD *>(record+2)==L'#'&&*reinterpret_cast<const DWORD *>(record+0x110c)>0) {
        const auto command=*reinterpret_cast<const DWORD *>(record+0x100c);
        if(command==70)s.body=0;
        if(command==134||command==136){s.menu=0;s.body=0;}
    }
}

// capture 读取完整显示参数；注入只重定向当前排版调用的第三个参数。
void capture(hook_context *context,HookParam *hp,TextBuffer *buffer,uintptr_t *split) {
    const bool choice=hp->user_value!=0;
    if(!choice&&displayEnabled.load()){activeRenders.fetch_add(1);if(renderFrames.size()>=8||overflowFrames)overflowFrames++;else renderFrames.emplace_back();}
    if(!enabled.load())return;
    if(choice&&!choicesEnabled.load())return;
    // Hook安装在call指令前，栈中还没有压入返回地址
    const auto owner=context->esi;
    const auto record=context->stack[choice?0:2];
    const auto expectedObject=owner+(choice?(hp->user_value==1?0xfd24:hp->user_value==2?0xfb14:0xfbd0):0xf7c0);
    if(!owner||record!=owner+0x11b8||context->ecx!=expectedObject)return;
    std::wstring text;if(!readText(record,text))return;
    csystemcontrol::Mode activeMode{};
    const bool backfill=displayClient.mode(activeMode)&&activeMode.enabled==2;
    std::unique_lock<std::mutex> lock(stateMutex);
    if(states.size()>=8&&!states.count(owner))return;auto &s=states[owner];
    if(s.revision==0)s.revision=1;
    if(!s.renderThread)s.renderThread=GetCurrentThreadId();
    if(s.captured==s.revision&&s.capturedMenu==context->ecx){if(!choice)applyRecord(context,s);return;}
    s.captured=s.revision;s.capturedMenu=context->ecx;
    auto &page=choice?s.menu:s.body;
    if(!page) {
        page=++serial;
        if(!choice){const auto now=GetTickCount64();s.rapid=s.lastPageAt&&now-s.lastPageAt<130?s.rapid+1:1;s.lastPageAt=now;if(s.rapid>=3)s.lastRapidAt=now;}
    }
    // 来源代次同时提供记录身份与原生顺序，不依赖text ID非零或数值递增
    if(!backfill){*split=0;packet(buffer,choice?1:0,page,s.revision,text);}
    if(backfill&&!choice&&displayEnabled.load()&&s.renderThread==GetCurrentThreadId()&&!quiescing&&!overflowFrames&&renderFrames.size()==1) {
        auto &frame=renderFrames.back();frame.mode=activeMode;frame.displayOwner=owner;frame.displayPage=page;frame.displayPart=s.revision;frame.displayOriginal=text;
    }
    if(choice||!displayEnabled.load()||s.renderThread!=GetCurrentThreadId()||quiescing||overflowFrames||renderFrames.size()!=1||!readable(expectedObject+0x1a8,4)||*reinterpret_cast<DWORD *>(expectedObject+0x1a8)!=0)return;
    if(backfill) {
        const auto snapshot=std::atomic_load(&backfillSnapshot);
        if(!snapshot||!csystemcontrol::same(snapshot->mode,activeMode)||!displayClient.currentBackfill(snapshot->mode))return;
        const auto found=snapshot->texts.find(csystemsnapshot::normalize(text));if(found==snapshot->texts.end())return;
        Record original{};memcpy(original.data(),reinterpret_cast<const void *>(record),original.size());
        auto replacement=translatedRecord(original,found->second,text);auto fonts=replacement?prepareFonts(expectedObject,*replacement):nullptr;
        if(replacement&&fonts&&displayClient.currentBackfill(snapshot->mode)) {
            s.translated=std::move(replacement);s.fonts=std::move(fonts);s.original=original;s.translatedRevision=s.revision;applyRecord(context,s);
        }
        return;
    }
    if((GetAsyncKeyState(VK_CONTROL)&0x8000)||(s.lastRapidAt&&GetTickCount64()-s.lastRapidAt<280))return;
    csystemcontrol::Mode mode{};
    if(waiting.test_and_set())return;
    WaitingGuard waitingGuard;
    if(!displayClient.enabled(mode)){return;}
    const auto capturedPage=page,capturedPart=s.revision;
    Record original{};memcpy(original.data(),reinterpret_cast<const void *>(record),original.size());
    std::wstring checked;if(!readText(reinterpret_cast<uintptr_t>(original.data()),checked)||checked!=text){return;}
    lock.unlock();uint64_t request=0;std::wstring translated;
    const bool received=displayClient.wait(mode,capturedPage,capturedPart,text,request,translated);
    auto replacement=received?translatedRecord(original,translated,text):nullptr;
    auto fonts=replacement?prepareFonts(expectedObject,*replacement):nullptr;
    if(received&&!replacement&&!unsafeReported.exchange(true))Msg::Log("CSystem display rejected: unsafe translation or image count");
    if(replacement&&!fonts&&!fontReported.exchange(true))Msg::Log("CSystem display rejected: font unavailable or missing glyph");
    lock.lock();const auto found=states.find(owner);
    bool applied=false;
    if(replacement&&fonts&&!quiescing&&found!=states.end()&&found->second.revision==capturedPart&&found->second.body==capturedPage&&sameRecord(record,original)&&displayClient.current(mode)&&readable(expectedObject+0x1a8,4)&&*reinterpret_cast<DWORD *>(expectedObject+0x1a8)==0) {
        auto &current=found->second;current.translated=std::move(replacement);current.fonts=std::move(fonts);current.original=original;current.translatedRevision=capturedPart;applyRecord(context,current);
        applied=context->stack[2]!=record;
        if(applied){renderFrames.back().request=request;renderFrames.back().mode=mode;}
    }
    if(!applied)displayClient.result(mode,request,false);
}

// install 安装只读完整包回调，正文与可选菜单保持独立
bool install(uintptr_t address,const char *name,decltype(HookParam::text_fun) callback,uintptr_t role=0) {
    HookParam hp{};hp.address=address;hp.type=CODEC_UTF16|USING_STRING|FULL_STRING|NO_CONTEXT;hp.text_fun=callback;hp.user_value=role;
    return NewHook(hp,name);
}
}

// attach_function 核对共享读取契约后仅注册本引擎的只读入口
bool CSystem::attach_function() {
    if(!collect())return false;
    for(const auto &spec:dangoCSystemFunctions)sites[spec.name]=findUnique(spec.pattern);
    for(const auto *name:{"reader","copy","dialogue","render","parser","restore","seek"})if(!valid(name))return false;
    const bool choices=valid("menu")&&valid("choice0")&&valid("choice1")&&valid("choice2");
    const bool display=displayLayout();
    // 所有调用关系必须先验证，再安装，避免入口被改写后失配
    if(!install(sites["reader"]+24,"CSystemState",observe)||!install(sites["parser"],"CSystemState",resetTimeline)||
       !install(sites["restore"]+12,"CSystemState",resetTimeline)||!install(sites["dialogue"]+45,"CSystemDialogue",capture))return false;
    bool menuInstalled=choices;
    if(choices)for(unsigned i=0;i<3;i++)if(!install(sites["choice"+std::to_string(i)]+13,"CSystemChoices",capture,i+1))menuInstalled=false;
    choicesEnabled.store(menuInstalled);
    if(!menuInstalled)Msg::Log("CSystem optional choices unavailable");
    bool fontsInstalled=display&&displayFonts.open();
    if(fontsInstalled)fontsInstalled=install(fontDraw+dangoCSystemFontHookOffset,"CSystemState",drawTranslatedFont);
    if(fontsInstalled&&install(sites["dialogue"]+50,"CSystemState",rendered))displayEnabled.store(true);
    if(displayEnabled.load()){snapshotStopped.store(false);snapshotThread=std::thread(loadBackfillSnapshots);}
    enabled.store(true);Msg::Log("CSystem read-only ready");
    Msg::Log(displayEnabled.load()?"CSystem display ready":"CSystem display unavailable");return true;
}

// Called before the upstream detachment loop removes either member of the render
// pair. Never release records / DLL code still used by an original render call.
bool CSystemPrepareDetach() {
    {std::lock_guard<std::mutex> lock(stateMutex);quiescing=true;}
    snapshotStopping.store(true);
    const auto start=GetTickCount64();
    while((activeRenders.load()||!snapshotStopped.load())&&GetTickCount64()-start<5000)Sleep(10);
    if(activeRenders.load()||!snapshotStopped.load()){std::lock_guard<std::mutex> lock(stateMutex);quiescing=false;return false;}
    if(snapshotThread.joinable())snapshotThread.join();
    std::atomic_store(&backfillSnapshot,std::shared_ptr<const BackfillSnapshot>{});
    {std::lock_guard<std::mutex> lock(stateMutex);states.clear();}
    displayFonts.close();
    return true;
}
