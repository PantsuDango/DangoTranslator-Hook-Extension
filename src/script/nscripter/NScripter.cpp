#include "NScripter.h"
#include "NScripterLayout.generated.h"
#include "native/display.h"
#include "NScripterDisplay.h"
#include "native/sprite.h"
#include <mutex>
#include <map>
#include <sstream>
#include <atomic>
#include <thread>

namespace {
struct Range { uintptr_t first, last; bool executable, writable; };
std::vector<Range> ranges;
std::map<std::string, uintptr_t> sites, pointers;
const DangoNScripterReadLayout *readLayout=nullptr;
std::mutex stateMutex;
std::atomic<bool> installed{false};
std::atomic<bool> displayInstalled{false},quiescing{false};
std::atomic<unsigned> activeRenders{0};
nscriptercontrol::Client displayClient;
using TextOutFunction=BOOL (WINAPI *)(HDC,int,int,LPCSTR,int);
TextOutFunction originalTextOut=nullptr;
uintptr_t textOutSlot=0,verticalSlot=0;
NSDLayout bodyDisplay{};uintptr_t renderReturn=0;
thread_local std::vector<std::shared_ptr<nscripterdisplay::Frame>> frames;
thread_local size_t glyphIndex=0;
thread_local std::shared_ptr<nscripterdisplay::Frame> drawingFrame;
struct BackfillSnapshot {nscriptercontrol::Mode mode{};nscriptersnapshot::Texts texts;};
std::shared_ptr<const BackfillSnapshot> backfillSnapshot;
std::thread snapshotThread;
std::atomic<bool> snapshotStopping{false},snapshotStopped{true};
std::atomic<bool> backfillInstalled{false};
struct SpriteFrame {std::shared_ptr<nscripterdisplay::Frame> frame;uintptr_t stack=0;std::vector<int> offsets;size_t index=SIZE_MAX;};
thread_local std::vector<SpriteFrame> spriteFrames[2];
nss::Scene spriteScene{};std::vector<nss::Render> spriteLayouts;
std::vector<nss::Texture> textureLayouts;
struct TextureFrame {std::shared_ptr<nscripterdisplay::Frame> frame;uintptr_t stack=0;size_t index=SIZE_MAX;};
thread_local std::vector<TextureFrame> textureFrames;
std::atomic<bool> textureEnabled{false};
bool spriteSceneReady=false,sceneNewPage=true;std::wstring lastSceneKey;
std::atomic<unsigned> spriteEnabled{0};
// readText 声明供独立显示回调立即复制原文
bool readText(uintptr_t address,std::wstring &out);
// publishBackfillPage 声明供正文真实返回点生成独立显示副本
void publishBackfillPage(const nscripterdisplay::Frame &frame,TextBuffer *buffer);

// loadBackfillSnapshots 独立加载快照并发原生确认，绘字线程不读取文件或等待翻译
void loadBackfillSnapshots() {
 uint64_t epoch=0,helper=0;
 while(!snapshotStopping.load()) {
  nscriptercontrol::Mode mode{};
  if(displayClient.mode(mode)&&mode.enabled==2) {
   if(epoch!=mode.epoch||helper!=mode.helperCreated) {
    epoch=mode.epoch;helper=mode.helperCreated;std::shared_ptr<BackfillSnapshot> next;bool ok=false;
    try{next=std::make_shared<BackfillSnapshot>();next->mode=mode;ok=displayClient.loadSnapshot(mode,next->texts);}catch(...){ok=false;}
    if(!snapshotStopping.load()&&displayClient.currentBackfill(mode)){std::shared_ptr<const BackfillSnapshot> value=ok?next:nullptr;std::atomic_store(&backfillSnapshot,value);displayClient.acknowledge(mode,ok);}
   }
  }else{std::atomic_store(&backfillSnapshot,std::shared_ptr<const BackfillSnapshot>{});epoch=helper=0;}
  Sleep(20);
 }
 snapshotStopped.store(true);
}
struct State {
    DWORD thread=0;
    uintptr_t script=0;
    DWORD cursor=0;
    uint64_t page=0, revision=0;
    uint64_t pageTick=0;
    unsigned rapid=0;
    bool closed=true, rejected=false;
    std::wstring text;
} state;
constexpr size_t maxText=8191;

// readable 核对有界的已提交可读内存，不借用调用者缓冲
bool readable(uintptr_t address, size_t length) {
    if(!address||!length||length>32*1024*1024||address>UINTPTR_MAX-length)return false;
    const auto end=address+length;
    while(address<end) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<void *>(address),&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        auto p=m.Protect&0xff;
        if(p!=PAGE_READONLY&&p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READ&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        auto first=reinterpret_cast<uintptr_t>(m.BaseAddress);
        if(first>UINTPTR_MAX-m.RegionSize)return false;
        auto next=first+m.RegionSize;if(next<=address)return false;address=next<end?next:end;
    }
    return true;
}

// code 核对主模块可执行节中的完整指令范围
bool code(uintptr_t address,size_t length) {
    for(const auto &r:ranges)if(r.executable&&address>=r.first&&address<=r.last&&length<=r.last-address)return true;
    return false;
}

// writable 核对共享源游标和菜单表只属于主模块数据节
bool writable(uintptr_t address,size_t length) {
    for(const auto &r:ranges)if(r.writable&&!r.executable&&address>=r.first&&address<=r.last&&length<=r.last-address)return readable(address,length);
    return false;
}

// collect 核对原生架构、节范围和家族，不按EXE名称或摘要判断
bool collect() {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!readable(base,sizeof(IMAGE_DOS_HEADER)))return false;
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000)return false;
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base+dos->e_lfanew);
    if(!readable(reinterpret_cast<uintptr_t>(nt),sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->OptionalHeader.SizeOfImage>32*1024*1024||nt->FileHeader.NumberOfSections>96)return false;
    auto sections=IMAGE_FIRST_SECTION(nt);
    if(!readable(reinterpret_cast<uintptr_t>(sections),sizeof(*sections)*nt->FileHeader.NumberOfSections))return false;
    bool version=false,exec=false,window=false;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        const auto &s=sections[i];auto size=s.Misc.VirtualSize;
        if(!size)continue;
        if(s.VirtualAddress>nt->OptionalHeader.SizeOfImage||size>nt->OptionalHeader.SizeOfImage-s.VirtualAddress||!readable(base+s.VirtualAddress,size))return false;
        Range r{base+s.VirtualAddress,base+s.VirtualAddress+size,(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0,(s.Characteristics&IMAGE_SCN_MEM_WRITE)!=0};ranges.push_back(r);
        if(s.Characteristics&IMAGE_SCN_MEM_READ) {
            auto begin=reinterpret_cast<const char *>(r.first),end=reinterpret_cast<const char *>(r.last);
            const std::string a="NScripter Version ",b="NScrExecWord",c="NScrGetWindow";
            version=version||std::search(begin,end,a.begin(),a.end())!=end;
            exec=exec||std::search(begin,end,b.begin(),b.end())!=end;
            window=window||std::search(begin,end,c.begin(),c.end())!=end;
        }
    }
    return version&&exec&&window;
}

// findUnique 只接受可执行节内的唯一结构特征
uintptr_t findUnique(const char *text) {
    std::istringstream input(text);std::string token;std::vector<int> pattern;
    while(input>>token)pattern.push_back(token=="??"?-1:int(strtoul(token.c_str(),nullptr,16)));
    if(pattern.size()<8||pattern.size()>512)return 0;
    uintptr_t result=0;
    for(const auto &r:ranges) {
        if(!r.executable||pattern.size()>r.last-r.first)continue;
        for(auto at=r.first;at<=r.last-pattern.size();at++) {
            auto data=reinterpret_cast<const BYTE *>(at);size_t i=0;
            for(;i<pattern.size();i++)if(pattern[i]>=0&&data[i]!=pattern[i])break;
            if(i==pattern.size()){if(result)return 0;result=at;}
        }
    }
    return result;
}

// displayLayout 共用语义定位器恢复原文字节读取及专用绘字调用
bool displayLayout() {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);auto nt=reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base+dos->e_lfanew);
    if(!nsd::locate(reinterpret_cast<const uint8_t *>(base),nt->OptionalHeader.SizeOfImage,uint32_t(base),uint32_t(sites["render"]-base),bodyDisplay)){Msg::Log("NScripter display rejected semantic");return false;}
    textOutSlot=base+bodyDisplay.textOut;verticalSlot=base+bodyDisplay.vertical;
    // 已确认的TextOutA导入可能被字体插件接管，原文回退必须沿用游戏当前入口
    // 仅绑定已加载映像中的可执行目标，不接受数据、未提交或私有代码地址
    if(!readable(textOutSlot,4)){Msg::Log("NScripter display rejected import_slot");return false;}
    originalTextOut=*reinterpret_cast<TextOutFunction *>(textOutSlot);
    MEMORY_BASIC_INFORMATION target{};
    if(!originalTextOut||!VirtualQuery(reinterpret_cast<void *>(originalTextOut),&target,sizeof(target))||target.State!=MEM_COMMIT||target.Type!=MEM_IMAGE||(target.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
        ((target.Protect&0xff)!=PAGE_EXECUTE&&(target.Protect&0xff)!=PAGE_EXECUTE_READ&&(target.Protect&0xff)!=PAGE_EXECUTE_READWRITE&&(target.Protect&0xff)!=PAGE_EXECUTE_WRITECOPY)) {
        Msg::Log("NScripter display rejected import_target");return false;
    }
    const auto systemTextOut=GetProcAddress(GetModuleHandleW(L"gdi32.dll"),"TextOutA");
    if(reinterpret_cast<uintptr_t>(originalTextOut)!=reinterpret_cast<uintptr_t>(systemTextOut))Msg::Log("NScripter display bound existing TextOutA wrapper");
    for(size_t i=0;i<readLayout->functionCount;i++)if(!strcmp(readLayout->functions[i].name,"body"))renderReturn=sites["body"]+readLayout->functions[i].hookOffset+5;
    if(!writable(verticalSlot,4)||!code(renderReturn,5)){Msg::Log("NScripter display rejected render_state");return false;}
    return true;
}

// translatedTextOut 仅替换已绑定原文槽的实际字形，样式和DC状态每次恢复
BOOL WINAPI translatedTextOut(HDC dc,int x,int y,LPCSTR input,int length) {
    auto frame=drawingFrame;auto index=glyphIndex;drawingFrame.reset();
    if(!frame||frame->rejected||frame->translated.empty()||index>=frame->original.size())return originalTextOut(dc,x,y,input,length);
    if(frame->backfill&&!displayClient.currentBackfill(frame->mode))return originalTextOut(dc,x,y,input,length);
    if(!frame->attempted) {
        frame->attempted=true;
        if(frame->fitBody&&!frame->rasterCellWidth) {
            auto width=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+bodyDisplay.cellWidth;
            if(!bodyDisplay.cellWidth||!bodyDisplay.cellScale||bodyDisplay.cellScale>4||!writable(width,4)){frame->rejected=true;return originalTextOut(dc,x,y,input,length);}
            const auto logicalWidth=*reinterpret_cast<const int *>(width);
            if(logicalWidth<=0||logicalWidth>256){frame->rejected=true;return originalTextOut(dc,x,y,input,length);}
            frame->rasterCellWidth=logicalWidth*int(bodyDisplay.cellScale);
        }
        if(!nscripterdisplay::prepareFont(*frame,dc)){frame->rejected=true;return originalTextOut(dc,x,y,input,length);}
    }
    auto old=SelectObject(dc,frame->font->handle);
    if(!old||old==HGDI_ERROR){frame->rejected=true;return originalTextOut(dc,x,y,input,length);}
    wchar_t value=index<frame->translated.size()?frame->translated[index]:L'\x3000';
    BOOL result=frame->fitBody?nscripterdisplay::paintFittedBody(*frame,dc,x,y,index):TextOutW(dc,x,y,&value,1);
    if(SelectObject(dc,old)==HGDI_ERROR||!result)frame->rejected=true;
    if(result&&index<frame->painted.size())frame->painted[index]=true;
    return result?result:originalTextOut(dc,x,y,input,length);
}

// contextRegister 按解码的x86寄存器编号访问实际调用现场
uintptr_t &contextRegister(hook_context *context,int reg) {
    switch(reg){case 0:return context->eax;case 1:return context->ecx;case 2:return context->edx;case 3:return context->ebx;case 5:return context->ebp;case 6:return context->esi;case 7:return context->edi;default:return context->esp;}
}
// sourceCapture 在原文字节尚未被覆盖前恢复真实有效地址，不能按字形内容猜测游标
void sourceCapture(hook_context *context,HookParam *,TextBuffer *,uintptr_t *) {
    if(frames.empty())return;auto frame=frames.back();if(!frame)return;frame->sourceOffset=SIZE_MAX;
    if(frame->rejected||frame->translated.empty())return;
    const auto &operand=bodyDisplay.operand;
    auto address=contextRegister(context,operand.base)+contextRegister(context,operand.index)*operand.scale+operand.displacement;
    if(address<frame->source||address-frame->source>=frame->raw.size()||(address-frame->source)%2||!readable(address,2)||memcmp(reinterpret_cast<const void *>(address),frame->raw.data()+address-frame->source,2))return;
    frame->sourceOffset=address-frame->source;frame->sourceStack=reinterpret_cast<uintptr_t>(context->stack);
}
// glyphCapture 只重定向来源与临时字形缓冲吻合的专用正文调用
void glyphCapture(hook_context *context,HookParam *param,TextBuffer *,uintptr_t *) {
    if(frames.empty())return;auto frame=frames.back();
    if(!frame||frame->rejected||frame->translated.empty()||param->user_value>=bodyDisplay.callCount)return;
    auto &target=contextRegister(context,bodyDisplay.calls[param->user_value].reg);
    if(target!=reinterpret_cast<uintptr_t>(originalTextOut)&&target!=reinterpret_cast<uintptr_t>(translatedTextOut))return;
    auto stack=reinterpret_cast<uintptr_t>(context->stack);
    const auto arguments=bodyDisplay.calls[param->user_value].stackArguments;
    if(arguments<2||arguments>4||!readable(stack,arguments*4)||context->stack[arguments-1]!=2||frame->sourceOffset>=frame->raw.size())return;
    auto input=context->stack[arguments-2];auto offset=frame->sourceOffset;
    if(input!=frame->sourceStack+bodyDisplay.buffer||!readable(frame->source,frame->raw.size())||memcmp(reinterpret_cast<const void *>(frame->source),frame->raw.data(),frame->raw.size()))return;
    if(!readable(input,2)||memcmp(reinterpret_cast<const void *>(input),frame->raw.data()+offset,2)!=0)return;
    glyphIndex=offset/2;drawingFrame=frame;target=reinterpret_cast<uintptr_t>(translatedTextOut);
}

// rendered 在真实正文返回后确认应用并释放私有字体，取消及失败不报告成功
void rendered(hook_context *,HookParam *,TextBuffer *buffer,uintptr_t *) {
    if(frames.empty())return;auto frame=frames.back();frames.pop_back();
    if(frame&&frame->request) {
        bool applied=!frame->rejected&&!frame->painted.empty()&&std::find(frame->painted.begin(),frame->painted.end(),false)==frame->painted.end();
        displayClient.result(frame->mode,frame->request,applied);
        static std::atomic<bool> diagnosed{false};
        if(!applied&&!diagnosed.exchange(true))Msg::Log("NScripter display fallback requested=%u font=%u rejected=%u",frame->translated.size(),frame->font!=nullptr,frame->rejected);
    }
    if(frame&&frame->backfill&&!quiescing.load()&&displayClient.currentBackfill(frame->mode))publishBackfillPage(*frame,buffer);
    activeRenders.fetch_sub(1);
    drawingFrame.reset();
}

// backfillLayout 共用语义定位器恢复标准命令、显示表和独立绘字入口。
bool backfillLayout() {
 auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 auto dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);auto nt=reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base+dos->e_lfanew);
 spriteSceneReady=nss::locate(reinterpret_cast<const uint8_t *>(base),nt->OptionalHeader.SizeOfImage,uint32_t(base),bodyDisplay,spriteScene,spriteLayouts,&textureLayouts);
 return true;
}
// textureStart 在原文尺寸分配后建立独立Unicode显示帧，不改变资源规格或精灵对象
void textureStart(hook_context *context,HookParam *,TextBuffer *,uintptr_t *) {
 if(!textureEnabled.load()||textureLayouts.size()!=1||quiescing.load()||textureFrames.size()>=8)return;
 const auto &layout=textureLayouts.front();auto stack=reinterpret_cast<uintptr_t>(context->stack);
 TextureFrame value;value.stack=stack;textureFrames.push_back(value);activeRenders.fetch_add(1);
 auto snapshot=std::atomic_load(&backfillSnapshot);if(!snapshot||!displayClient.currentBackfill(snapshot->mode))return;
 if(!readable(stack+layout.kind,4)||*reinterpret_cast<const DWORD *>(stack+layout.kind)!=0)return;
 std::wstring text;auto address=stack+layout.source;if(!readText(address,text))return;
 auto original=nscriptersnapshot::normalize(text);auto found=snapshot->texts.find(original);
 if(found==snapshot->texts.end()||!nscriptersnapshot::safe(original,found->second)||!readable(stack+layout.width,4))return;
 auto frame=std::make_shared<nscripterdisplay::Frame>();frame->source=address;frame->mode=snapshot->mode;frame->backfill=true;frame->translated=found->second;
 if(!nscripterdisplay::safeSource(text,frame->original,frame->raw)||!readable(address,frame->raw.size())||memcmp(reinterpret_cast<const void *>(address),frame->raw.data(),frame->raw.size()))return;
 auto width=*reinterpret_cast<const int *>(stack+layout.width);if(width<=0||width>256||layout.scale!=3)return;
 // 每格都切取完整Unicode行，短译文也按原文纹理宽度完整绘制，保留颜色和阴影采样
 frame->fitBody=true;frame->rasterCellWidth=width*int(layout.scale);
 if(nscripterdisplay::safeTranslation(*frame))textureFrames.back().frame=std::move(frame);
}
// textureStep 从实际栈内原文游标确认当前字形序号，不借用脚本全局游标
void textureStep(hook_context *context,HookParam *,TextBuffer *,uintptr_t *) {
 if(textureFrames.empty())return;auto &value=textureFrames.back();value.index=SIZE_MAX;
 auto stack=reinterpret_cast<uintptr_t>(context->stack);const auto &layout=textureLayouts.front();
 if(value.stack!=stack||!value.frame||!readable(stack+layout.cursor,4))return;
 auto pointer=*reinterpret_cast<const DWORD *>(stack+layout.cursor);auto frame=value.frame;
 if(pointer>=frame->source&&pointer-frame->source<frame->raw.size()&&(pointer-frame->source)%2==0)value.index=(pointer-frame->source)/2;
}
// textureGlyph 只重定向已绑定资源字形调用，原有三倍采样与最终位图复制继续由引擎执行
void textureGlyph(hook_context *context,HookParam *param,TextBuffer *,uintptr_t *) {
 if(textureFrames.empty()||textureLayouts.size()!=1)return;auto &value=textureFrames.back();auto frame=value.frame;auto stack=reinterpret_cast<uintptr_t>(context->stack);
 const auto &layout=textureLayouts.front();auto call=size_t(param->user_value);if(call>=layout.calls.size())return;
 auto &target=contextRegister(context,layout.calls[call].reg);
 if(!frame||frame->rejected||value.stack!=stack||value.index>=frame->original.size()||target!=reinterpret_cast<uintptr_t>(originalTextOut)&&target!=reinterpret_cast<uintptr_t>(translatedTextOut))return;
 if(!displayClient.currentBackfill(frame->mode)||!readable(frame->source,frame->raw.size())||memcmp(reinterpret_cast<const void *>(frame->source),frame->raw.data(),frame->raw.size())){frame->rejected=true;return;}
 if(!readable(stack+layout.buffer,2)||memcmp(reinterpret_cast<const void *>(stack+layout.buffer),frame->raw.data()+value.index*2,2))return;
 glyphIndex=value.index;drawingFrame=frame;target=reinterpret_cast<uintptr_t>(translatedTextOut);
}
// textureEnd 在SEH和栈恢复之前结束资源显示作用域，DC与字体每次绘制已恢复
void textureEnd(hook_context *context,HookParam *,TextBuffer *,uintptr_t *) {
 if(textureFrames.empty()||textureFrames.back().stack!=reinterpret_cast<uintptr_t>(context->stack))return;
 auto frame=textureFrames.back().frame;
 if(frame&&!quiescing.load()&&displayClient.currentBackfill(frame->mode)) {
  auto applied=!frame->rejected&&!frame->painted.empty()&&std::find(frame->painted.begin(),frame->painted.end(),false)==frame->painted.end();
  static std::atomic<unsigned> diagnosed{0};auto bit=applied?1u:2u;
  if(!(diagnosed.fetch_or(bit)&bit))Msg::Log("NScripter texture display applied=%u source_chars=%u translation_chars=%u raster_cell=%d",applied,frame->original.size(),frame->translated.size(),frame->rasterCellWidth);
 }
 textureFrames.pop_back();drawingFrame.reset();activeRenders.fetch_sub(1);
}
// spriteStep 在首字节消费点建立私有帧，按实际有效地址跟踪后续字形。
void spriteStep(hook_context *context,HookParam *param,TextBuffer *,uintptr_t *) {
 unsigned group=unsigned(param->user_value);if(group>=spriteLayouts.size()||group>1||!(spriteEnabled.load()&(1u<<group)))return;
 const auto &layout=spriteLayouts[group];auto stack=reinterpret_cast<uintptr_t>(context->stack);auto indexAddress=stack+layout.index.displacement;
 if(!readable(indexAddress,4))return;auto index=size_t(*reinterpret_cast<const DWORD *>(indexAddress));
 const auto &operand=layout.operand;
 auto address=(operand.base==4?stack:contextRegister(context,operand.base))+contextRegister(context,operand.index)*operand.scale+operand.displacement;
 auto &list=spriteFrames[group];
 if(list.empty()||list.back().stack!=stack) {
  // 非零首游标不能被误当作完整句子；所有页清理及绘制退出仍沿用引擎。
  if(index||quiescing.load()||list.size()>=8)return;
  SpriteFrame value;value.stack=stack;list.push_back(value);activeRenders.fetch_add(1);
  const auto snapshot=std::atomic_load(&backfillSnapshot);
  if(snapshot&&displayClient.currentBackfill(snapshot->mode)&&*reinterpret_cast<DWORD *>(verticalSlot)==0) {
   std::wstring text;if(readText(address,text)&&!text.empty()) {
    size_t prefix=0;while(prefix+7<=text.size()&&text[prefix]==L'#'){bool hex=true;for(size_t j=1;j<=6;j++)if(!iswxdigit(text[prefix+j]))hex=false;if(!hex)break;prefix+=7;}
    auto original=nscriptersnapshot::normalize(text.substr(prefix));const auto found=snapshot->texts.find(original);
    if(found!=snapshot->texts.end()&&nscriptersnapshot::safe(original,found->second)) {
     auto frame=std::make_shared<nscripterdisplay::Frame>();frame->source=address;frame->mode=snapshot->mode;frame->backfill=true;frame->translated=found->second;
     BOOL fallback=FALSE;auto n=WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,&fallback);
     if(n>0&&!fallback) {
      frame->raw.resize(n);fallback=FALSE;std::vector<int> offsets(size_t(n),-1);size_t rawOffset=0;bool valid=true;
      if(WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,text.data(),int(text.size()),frame->raw.data(),n,nullptr,&fallback)!=n||fallback||!readable(address,n)||memcmp(reinterpret_cast<const void *>(address),frame->raw.data(),n))valid=false;
      for(size_t j=0;valid&&j<text.size();j++) {
       auto c=text[j];if(j<prefix||c==L'\n'){rawOffset++;continue;}
       if(c<256||(c>=0xd800&&c<=0xdfff)||(c>=0xe000&&c<=0xf8ff)||rawOffset+2>size_t(n)){valid=false;break;}
       offsets[rawOffset]=int(frame->original.size());frame->original+=c;rawOffset+=2;
      }
      frame->fitBody=frame->translated.size()>frame->original.size()&&text.find(L'\n')==std::wstring::npos;
      if(valid&&rawOffset==size_t(n)&&nscripterdisplay::safeTranslation(*frame)){list.back().frame=std::move(frame);list.back().offsets=std::move(offsets);}
     }
    }
   }
  }
 }
 auto &value=list.back();value.index=SIZE_MAX;auto frame=value.frame;
 if(frame&&address>=frame->source&&address-frame->source==index&&index<frame->raw.size()&&readable(address,1)&&*reinterpret_cast<const char *>(address)==frame->raw[index])value.index=index;
}
// spriteGlyph 仅重定向已恢复参数及来源的双字节绘字，不改写精灵和按钮对象。
void spriteGlyph(hook_context *context,HookParam *param,TextBuffer *,uintptr_t *) {
 unsigned group=unsigned(param->user_value>>8),call=unsigned(param->user_value&255);
 if(group>=spriteLayouts.size()||group>1||call>=spriteLayouts[group].calls.size()||spriteFrames[group].empty())return;
 auto &value=spriteFrames[group].back();auto frame=value.frame;auto stack=reinterpret_cast<uintptr_t>(context->stack);
 auto &target=contextRegister(context,spriteLayouts[group].calls[call].reg);
 if(!frame||frame->rejected||frame->translated.empty()||stack!=value.stack||target!=reinterpret_cast<uintptr_t>(originalTextOut)&&target!=reinterpret_cast<uintptr_t>(translatedTextOut)||value.index>=value.offsets.size()||value.offsets[value.index]<0||value.index+2>frame->raw.size())return;
 auto input=stack+spriteLayouts[group].buffer;
 if(!readable(input,2)||memcmp(reinterpret_cast<const void *>(input),frame->raw.data()+value.index,2))return;
 if(!displayClient.currentBackfill(frame->mode)||!readable(frame->source,frame->raw.size())||memcmp(reinterpret_cast<const void *>(frame->source),frame->raw.data(),frame->raw.size())){frame->rejected=true;return;}
 glyphIndex=size_t(value.offsets[value.index]);drawingFrame=frame;target=reinterpret_cast<uintptr_t>(translatedTextOut);
}
// spriteEnd 在统一函数退出前释放私有字体，所有栈和SEH恢复仍由游戏执行。
void spriteEnd(hook_context *context,HookParam *param,TextBuffer *,uintptr_t *) {
 unsigned group=unsigned(param->user_value);if(group>1||spriteFrames[group].empty())return;
 auto &list=spriteFrames[group];if(list.back().stack!=reinterpret_cast<uintptr_t>(context->stack))return;list.pop_back();drawingFrame.reset();activeRenders.fetch_sub(1);
}
// valid 核对相对调用和共同数据来源，菜单组独立降级
bool valid(bool menu) {
    for(auto name:{"render","body","preprocess","dbcs"})if(!sites[name])return false;
    if(menu&&(!sites["menu"]||!sites["copy"]))return false;
    if(!readLayout)return false;
    for(size_t i=0;i<readLayout->callCount;i++) {
        const auto &call=readLayout->calls[i];
        if(!menu&&strcmp(call.from,"menu")==0)continue;
        auto at=sites[call.from]+call.offset;
        if(!code(at,5)||*reinterpret_cast<const BYTE *>(at)!=0xe8||!sites[call.to]||at+5+*reinterpret_cast<const int32_t *>(at+1)!=sites[call.to])return false;
    }
    std::map<std::string,uintptr_t> found;
    for(size_t i=0;i<readLayout->pointerCount;i++) {
        const auto &p=readLayout->pointers[i];
        if(!menu&&strcmp(p.from,"menu")==0)continue;
        auto at=sites[p.from]+p.offset;if(!code(at,4))return false;
        auto address=*reinterpret_cast<const DWORD *>(at);
        if(!writable(address,p.size)||(found.count(p.name)&&found[p.name]!=address))return false;
        found[p.name]=address;
    }
    pointers=std::move(found);return true;
}

// readText 立即复制完整CP932显示输入，拒绝非法编码及未终止缓冲
bool readText(uintptr_t address,std::wstring &out) {
    std::string raw;raw.reserve(256);
    for(size_t n=0;n<8192;n++) {
        if(!readable(address+n,1))return false;
        auto b=*reinterpret_cast<const char *>(address+n);
        if(!b) {
            if(raw.empty())return true;
            auto units=MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,raw.data(),int(raw.size()),nullptr,0);
            if(units<=0||size_t(units)>maxText)return false;
            out.resize(units);return MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,raw.data(),int(raw.size()),out.data(),units)==units;
        }
        raw+=b;
    }
    return false;
}

// jsonString 转义页面载荷，保持原文和控制字符边界
std::wstring jsonString(const std::wstring &text) {
    const wchar_t *hex=L"0123456789abcdef";std::wstring out=L"\"";
    for(auto c:text) {
        if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}
        else if(c<0x20){out+=L"\\u00";out+=hex[(c>>4)&15];out+=hex[c&15];}
        else out+=c;
    }
    return out+L"\"";
}

// spriteText 读取标准文字精灵规格并移除开头颜色，保持独立缓存键。
bool spriteText(const std::wstring &raw,std::wstring &text) {
 if(raw.size()<3||raw[0]!=L':'||raw[1]!=L's')return false;size_t start=2;
 if(raw[start]==L'/'){start++;while(start<raw.size()&&(raw[start]>=L'0'&&raw[start]<=L'9'||raw[start]==L','||raw[start]==L' '||raw[start]==L'-'))start++;}
 if(start>=raw.size()||raw[start++]!=L';')return false;
 while(start+7<=raw.size()&&raw[start]==L'#'){bool hex=true;for(size_t j=1;j<=6;j++)if(!iswxdigit(raw[start+j]))hex=false;if(!hex)break;start+=7;}
 text=nscriptersnapshot::normalize(raw.substr(start));
 bool letter=false;for(auto c:text)if(c>=L'A'&&c<=L'Z'||c>=L'a'&&c<=L'z'||c>=0x3040&&c<=0x9fff||c>=0xff21&&c<=0xff5a)letter=true;
 return !text.empty()&&letter;
}
// sceneClear 标记精灵生命周期边界，同文重建页不能被旧页去重吞掉。
void sceneClear(hook_context *,HookParam *,TextBuffer *,uintptr_t *) {
 std::lock_guard<std::mutex> lock(stateMutex);sceneNewPage=true;
}
// sceneCapture 在print完成或btnwait进入时提交完整可见表，禁止逐行制造快进页面。
void sceneCapture(hook_context *,HookParam *,TextBuffer *buffer,uintptr_t *) {
 if(!installed.load()||quiescing.load()||!spriteSceneReady)return;
 struct Row{int x,y;uint32_t id;std::wstring text;};std::vector<Row> rows;
 for(uint32_t i=0;i<spriteScene.count;i++) {
  auto address=uintptr_t(spriteScene.names)+i*spriteScene.stride;
  if(!readable(spriteScene.visible+i*4,4)||!*reinterpret_cast<const DWORD *>(spriteScene.visible+i*4)||!readable(address,spriteScene.stride)||!memchr(reinterpret_cast<const void *>(address),0,spriteScene.stride))continue;
  std::wstring raw,text;if(!readText(address,raw)||!spriteText(raw,text))continue;
  if(!readable(spriteScene.x+i*4,4)||!readable(spriteScene.y+i*4,4))continue;
  auto x=*reinterpret_cast<const int *>(spriteScene.x+i*4),y=*reinterpret_cast<const int *>(spriteScene.y+i*4);
  if(x<-512||y<-512||x>16384||y>16384)continue;rows.push_back({x,y,i,std::move(text)});
 }
 std::sort(rows.begin(),rows.end(),[](const Row &a,const Row &b){return a.y==b.y?(a.x==b.x?a.id<b.id:a.x<b.x):a.y<b.y;});
 std::wstring text,segments=L"[",key;
 for(const auto &row:rows){if(!text.empty()){text+=L'\n';segments+=L',';}text+=row.text;segments+=jsonString(row.text);key+=std::to_wstring(row.id)+L":"+std::to_wstring(row.x)+L":"+std::to_wstring(row.y)+jsonString(row.text);}
 segments+=L']';std::lock_guard<std::mutex> lock(stateMutex);
 if(state.thread&&state.thread!=GetCurrentThreadId())return;state.thread=GetCurrentThreadId();
 if(rows.empty()&&lastSceneKey.empty())return;
 if(!sceneNewPage&&key==lastSceneKey)return;
 state.page++;state.closed=true;state.text.clear();lastSceneKey=std::move(key);sceneNewPage=false;
 const auto snapshot=std::atomic_load(&backfillSnapshot);uint64_t generation=snapshot&&displayClient.currentBackfill(snapshot->mode)?snapshot->mode.generation:0;
 std::wstring value=L"{\"nscripter\":1,\"kind\":3,\"page\":"+std::to_wstring(state.page)+L",\"revision\":"+std::to_wstring(++state.revision)+L",\"tick\":"+std::to_wstring(GetTickCount64())+L",\"end\":true,\"reset\":"+(rows.empty()?L"true":L"false")+L",\"segment\":false,\"backfillGeneration\":"+std::to_wstring(generation)+L",\"text\":"+jsonString(text)+L",\"segments\":"+segments+L"}";
 if(rows.size()>256||value.size()>TEXT_BUFFER_SIZE/sizeof(wchar_t)) {
  // 超限页仍通知清理旧事件，不能让上一页待显示内容越过当前页面。
  value=L"{\"nscripter\":1,\"kind\":3,\"page\":"+std::to_wstring(state.page)+L",\"revision\":"+std::to_wstring(state.revision)+L",\"tick\":"+std::to_wstring(GetTickCount64())+L",\"end\":true,\"reset\":true,\"segment\":false,\"backfillGeneration\":"+std::to_wstring(generation)+L",\"text\":\"\",\"segments\":[]}";
 }
 buffer->from(value);
}
// packet 输出有界完整页副本，片段丢失时不会把剩余半句冒充完整页
void packet(TextBuffer *buffer,int kind,bool reset=false,const std::wstring *segment=nullptr,uint64_t backfillGeneration=0) {
    std::wstring value=L"{\"nscripter\":1,\"kind\":"+std::to_wstring(kind)+L",\"page\":"+std::to_wstring(state.page)+L",\"revision\":"+std::to_wstring(++state.revision)+L",\"tick\":"+std::to_wstring(GetTickCount64())+
        L",\"end\":"+(state.closed?L"true":L"false")+L",\"reset\":"+(reset?L"true":L"false")+L",\"segment\":"+(segment?L"true":L"false")+L",\"backfillGeneration\":"+std::to_wstring(backfillGeneration)+L",\"text\":"+jsonString(kind==2?L"":segment?*segment:state.text)+L"}";
    if(value.size()>TEXT_BUFFER_SIZE/sizeof(wchar_t)) {
        state.text.clear();state.rejected=true;packet(buffer,2,true,nullptr,backfillGeneration);return;
    }
    buffer->from(value);
}

// publishBackfillPage 仅在当前正文排版结束后输出原文页，不进入逐句注入请求
void publishBackfillPage(const nscripterdisplay::Frame &frame,TextBuffer *buffer) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if(state.thread!=GetCurrentThreadId()||state.page!=frame.page||state.revision!=frame.part)return;
    packet(buffer,state.rejected?2:0,state.rejected,nullptr,frame.mode.generation);
}

// bodyCapture 只在真实新正文调用点采集，字体及textgosub恢复不产生重复对白
void bodyCapture(hook_context *context,HookParam *,TextBuffer *buffer,uintptr_t *) {
    if(!installed.load())return;
    std::shared_ptr<nscripterdisplay::Frame> frame;
    if(displayInstalled.load()) {
        if(!quiescing.load()&&frames.size()<8)frame=std::make_shared<nscripterdisplay::Frame>();
        frames.push_back(frame);activeRenders.fetch_add(1);
    }
    std::unique_lock<std::mutex> lock(stateMutex);
    // rememberBackfill 保存本次原文或拒绝页，真实返回后才能发出显示通知。
    auto rememberBackfill=[&]() {
        const auto snapshot=std::atomic_load(&backfillSnapshot);
        if(!frame||!snapshot||frames.size()!=1||quiescing.load()||!displayClient.currentBackfill(snapshot->mode))return false;
        frame->mode=snapshot->mode;frame->backfill=true;frame->page=state.page;frame->part=state.revision;
        return true;
    };
    auto thread=GetCurrentThreadId();if(state.thread&&state.thread!=thread)return;state.thread=thread;
    auto script=*reinterpret_cast<const DWORD *>(pointers["script"]),cursor=*reinterpret_cast<const DWORD *>(pointers["cursor"]);
    bool reset=state.script&&(script!=state.script||cursor<state.cursor);
    if(state.closed||reset){lastSceneKey.clear();sceneNewPage=true;auto tick=GetTickCount64();state.rapid=state.pageTick&&tick-state.pageTick<=160?state.rapid+1:1;state.pageTick=tick;state.page++;state.text.clear();state.rejected=false;state.closed=false;}
    state.script=script;state.cursor=cursor;
    std::wstring text;
    if(!script||!readText(context->ecx,text)) {
        state.rejected=true;state.text.clear();packet(buffer,2,true);if(rememberBackfill())buffer->clear();return;
    }
    // 完整输入已由原生双字节前处理完成，Unicode中的反斜线不混淆CP932尾字节
    state.closed=text.find_first_of(L"@\\")!=std::wstring::npos;
    if(state.rejected){packet(buffer,2,true);if(rememberBackfill())buffer->clear();return;}
    if(state.text.size()+text.size()+1>maxText){state.rejected=true;state.text.clear();packet(buffer,2,true);if(rememberBackfill())buffer->clear();return;}
    if(!state.text.empty())state.text+=L'\n';state.text+=text;
    nscriptercontrol::Mode mode{};bool enabled=frame&&displayClient.mode(mode)&&mode.enabled==1;
    packet(buffer,0,reset,enabled?&text:nullptr);
    auto capturedPage=state.page,capturedPart=state.revision;auto capturedScript=state.script;auto capturedCursor=state.cursor;auto rapid=state.rapid;
    bool backfill=rememberBackfill();
    lock.unlock();
    const auto snapshot=std::atomic_load(&backfillSnapshot);
    if(backfill&&snapshot&&displayClient.currentBackfill(snapshot->mode)) {
        frame->source=context->ecx;frame->mode=snapshot->mode;frame->backfill=true;frame->page=capturedPage;frame->part=capturedPart;
        if(*reinterpret_cast<DWORD *>(verticalSlot)==0&&nscripterdisplay::safeSource(text,frame->original,frame->raw)) {
            const auto found=snapshot->texts.find(nscriptersnapshot::normalize(frame->original));
            if(found!=snapshot->texts.end()){frame->translated=found->second;frame->fitBody=frame->translated.size()>frame->original.size();if(!nscripterdisplay::safeTranslation(*frame))frame->translated.clear();}
        }
        buffer->clear();return;
    }
    FILETIME now{};GetSystemTimeAsFileTime(&now);auto clock=uint64_t(now.dwHighDateTime)<<32|now.dwLowDateTime;
    if(!enabled||frames.size()!=1||quiescing.load()||rapid>=3||clock<mode.gameCreated+30000000||GetAsyncKeyState(VK_CONTROL)&0x8000||!readable(textOutSlot,4)||*reinterpret_cast<TextOutFunction *>(textOutSlot)!=originalTextOut||*reinterpret_cast<DWORD *>(verticalSlot)!=0)return;
    frame->source=context->ecx;frame->mode=mode;
    if(!nscripterdisplay::safeSource(text,frame->original,frame->raw))return;
    bool received=displayClient.wait(mode,capturedPage,capturedPart,frame->original,frame->request,frame->translated);
    if(frame->request)buffer->clear();
    lock.lock();
    if(!received||!nscripterdisplay::safeTranslation(*frame)||quiescing.load()||state.script!=capturedScript||state.cursor!=capturedCursor||state.page!=capturedPage||state.revision!=capturedPart||!displayClient.current(mode)||!readable(frame->source,frame->raw.size())||memcmp(reinterpret_cast<const void *>(frame->source),frame->raw.data(),frame->raw.size())!=0) {frame->translated.clear();frame->rejected=true;}
}

// menuCapture 在数量发布之后只读完整显示表，标签和分支始终不变
void menuCapture(hook_context *,HookParam *,TextBuffer *buffer,uintptr_t *) {
    if(!installed.load())return;
    std::lock_guard<std::mutex> lock(stateMutex);
    auto thread=GetCurrentThreadId();if(state.thread&&state.thread!=thread)return;state.thread=thread;
    state.page++;state.text.clear();state.closed=true;state.rejected=false;
    auto count=*reinterpret_cast<const DWORD *>(pointers["count"]);
    if(!count||count>32){packet(buffer,2,true);return;}
    for(DWORD i=0;i<count;i++) {
        auto address=*reinterpret_cast<const DWORD *>(pointers["choices"]+i*4);std::wstring text;
        if(!readText(address,text)||text.empty()||state.text.size()+text.size()+1>maxText){state.text.clear();packet(buffer,2,true);return;}
        if(i)state.text+=L'\n';state.text+=text;
    }
    packet(buffer,1);
}
}

// unavailable 输出内部阶段诊断并让Host立即报告当前读取布局不可用
bool unavailable(const char *stage) {
    Msg::Log("NScripter reader rejected %s",stage);
    Msg::Log("NScripter read-only unavailable");
    return false;
}

// attach_function 安装已验证的独立只读正文与可选菜单入口
bool NScripter::attach_function() {
    if(!collect())return unavailable("module");
    size_t matched=0;const DangoNScripterReadLayout *selected=nullptr;
    std::map<std::string,uintptr_t> selectedSites,selectedPointers;
    for(const auto &candidate:dangoNScripterReadLayouts) {
        sites.clear();pointers.clear();readLayout=&candidate;
        for(size_t i=0;i<candidate.functionCount;i++){const auto &spec=candidate.functions[i];sites[spec.name]=findUnique(spec.pattern);}
        if(!valid(false))continue;
        matched++;selected=&candidate;selectedSites=sites;selectedPointers=pointers;
    }
    if(matched!=1){readLayout=nullptr;return unavailable(matched?"ambiguous_layout":"layout");}
    readLayout=selected;sites=std::move(selectedSites);pointers=std::move(selectedPointers);
    auto bodyPointers=pointers;bool menu=valid(true);if(!menu)pointers=std::move(bodyPointers);
    const bool display=displayLayout();
    const bool backfill=backfillLayout();
    size_t bodyOffset=0,menuOffset=0;
    for(size_t i=0;i<readLayout->functionCount;i++){const auto &spec=readLayout->functions[i];if(!strcmp(spec.name,"body"))bodyOffset=spec.hookOffset;if(!strcmp(spec.name,"menu"))menuOffset=spec.hookOffset;}
    HookParam hp{};hp.address=sites["body"]+bodyOffset;hp.type=USING_STRING|CODEC_UTF16|FULL_STRING|NO_CONTEXT;hp.text_fun=bodyCapture;
    if(!NewHook(hp,"NScripterDialogue"))return unavailable("installation");
    if(menu){hp.address=sites["menu"]+menuOffset;hp.text_fun=menuCapture;menu=NewHook(hp,"NScripterChoices");}
    if(display) {
        auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        HookParam write{};write.type=USING_STRING|CODEC_UTF16|FULL_STRING|NO_CONTEXT;write.text_fun=rendered;write.address=renderReturn;
        bool ready=NewHook(write,"NScripterRenderReturn");
        write.type=NO_CONTEXT;
        if(ready){write.text_fun=sourceCapture;write.address=base+bodyDisplay.source;ready=NewHook(write,"NScripterGlyphSource");}
        for(unsigned i=0;ready&&i<bodyDisplay.callCount;i++){write.text_fun=glyphCapture;write.user_value=i;write.address=base+bodyDisplay.calls[i].rva;ready=NewHook(write,(std::string("NScripterGlyph")+std::to_string(i)).c_str());}
        displayInstalled.store(ready);
    }
    installed.store(true);
    if(displayInstalled.load()&&backfill) {
        unsigned enabledSprites=0;for(unsigned i=0;i<spriteLayouts.size()&&i<2;i++) {
            const auto &spec=spriteLayouts[i];auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            HookParam write{};write.type=NO_CONTEXT;write.user_value=i;write.address=base+spec.end;write.text_fun=spriteEnd;
            bool ready=NewHook(write,(std::string("NScripterSpriteEnd")+std::to_string(i)).c_str());
            for(unsigned j=0;ready&&j<spec.calls.size();j++){write.user_value=(i<<8)|j;write.address=base+spec.calls[j].rva;write.text_fun=spriteGlyph;ready=NewHook(write,(std::string("NScripterSpriteGlyph")+std::to_string(i)+"_"+std::to_string(j)).c_str());}
            if(ready){write.user_value=i;write.address=base+spec.source;write.text_fun=spriteStep;ready=NewHook(write,(std::string("NScripterSpriteSource")+std::to_string(i)).c_str());}
            if(ready)enabledSprites|=1u<<i;
        }
        spriteEnabled.store(enabledSprites);backfillInstalled.store(true);snapshotStopped.store(false);snapshotThread=std::thread(loadBackfillSnapshots);
        Msg::Log("NScripter backfill sprite paths=%u",enabledSprites);
        if(textureLayouts.size()==1) {
            const auto &spec=textureLayouts.front();auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));HookParam write{};write.type=NO_CONTEXT;
            write.address=base+spec.end;write.text_fun=textureEnd;bool ready=NewHook(write,"NScripterTextureEnd");
            for(unsigned j=0;ready&&j<spec.calls.size();j++){write.user_value=j;write.address=base+spec.calls[j].rva;write.text_fun=textureGlyph;ready=NewHook(write,(std::string("NScripterTextureGlyph")+std::to_string(j)).c_str());}
            if(ready){write.address=base+spec.step;write.text_fun=textureStep;ready=NewHook(write,"NScripterTextureStep");}
            if(ready){write.address=base+spec.start;write.text_fun=textureStart;ready=NewHook(write,"NScripterTextureSource");}
            textureEnabled.store(ready);
        }
        Msg::Log("NScripter backfill texture paths=%u",textureEnabled.load());
    }
    if(spriteSceneReady) {
        auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));HookParam page{};page.type=USING_STRING|CODEC_UTF16|FULL_STRING|NO_CONTEXT;page.text_fun=sceneCapture;
        page.address=base+spriteScene.printReturn;bool ready=NewHook(page,"NScripterSprites");
        page.address=base+spriteScene.wait;ready=NewHook(page,"NScripterSprites")&&ready;
        page.type=NO_CONTEXT;page.text_fun=sceneClear;page.address=base+spriteScene.clear;ready=NewHook(page,"NScripterSpriteClear")&&ready;
        spriteSceneReady=ready;Msg::Log("NScripter sprite scene ready=%u renderers=%u",ready,spriteLayouts.size());
    }
    Msg::Log("NScripter read-only ready");
    if(!menu)Msg::Log("NScripter choices unavailable");
    Msg::Log(displayInstalled.load()?"NScripter display ready":"NScripter display unavailable");
    Msg::Log(backfillInstalled.load()?"NScripter backfill ready":"NScripter backfill unavailable");
    return true;
}

// NScripterPrepareDetach 先撤销新请求，再等待持有私有字形及返回点的正文退出
bool NScripterPrepareDetach() {
    quiescing.store(true);
    auto start=GetTickCount64();while(activeRenders.load()&&GetTickCount64()-start<5000)Sleep(10);
    if(activeRenders.load()){Msg::Log("NScripter detach deferred active=%u",activeRenders.load());quiescing.store(false);return false;}
    snapshotStopping.store(true);
    start=GetTickCount64();while(!snapshotStopped.load()&&GetTickCount64()-start<5000)Sleep(10);
    if(!snapshotStopped.load()){Msg::Log("NScripter detach deferred loader");return false;}
    if(snapshotThread.joinable())snapshotThread.join();std::atomic_store(&backfillSnapshot,std::shared_ptr<const BackfillSnapshot>{});
    return true;
}
