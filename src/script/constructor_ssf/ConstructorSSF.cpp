#include "ConstructorSSF.h"
#include "ConstructorSSFLayout.h"
#include "ConstructorSSFSnapshot.h"
#include "ConstructorSSFControl.h"
#include "ConstructorSSFFont.h"
#include "ConstructorSSFText.h"
#include "MinHook.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <map>
#include <algorithm>

namespace {
ssflayout::Layout layout;uintptr_t module=0;std::recursive_mutex mutex;
constructorssfcontrol::Client client;
std::atomic<bool> quiescing{false},stopping{false},stopped{true};std::atomic<unsigned> active{0};std::thread loader;
uint64_t page=0,revision=0,menuPage=0;DWORD gameThread=0;
struct Resource {constructorssfcontrol::Mode mode{};constructorssfsnapshot::Texts texts;std::shared_ptr<const ssffont::Raster> raster;};
std::shared_ptr<const Resource> resource;
ssffont::Renderer renderer;
struct Source {uintptr_t address=0,owner=0;std::string bytes;std::wstring original;uint64_t page=0;};
struct Slot {uintptr_t owner=0;Source source;size_t line=0;bool menu=false;std::string expected;};
std::map<uintptr_t,Source> sources;std::map<uintptr_t,Slot> slots;
std::vector<std::wstring> menuTexts;
std::vector<uintptr_t> menuAddresses;
std::map<uintptr_t,std::wstring> menuRendered;
uint64_t menuGeneration=0;
using Line=int(__thiscall*)(void*,char*,char**,int);Line originalLine=nullptr;
using Draw=void(__cdecl*)(const char*,int,int,DWORD,unsigned char,unsigned char);Draw originalDraw=nullptr;
struct Flight {Flight(){active.fetch_add(1);}~Flight(){active.fetch_sub(1);}};
struct Notification {std::shared_ptr<const Resource> resource;Slot slot;std::wstring translation;bool drawn=false;};
thread_local Notification *notification=nullptr;
std::wstring lastDisplay;uint64_t lastGeneration=0;
uint64_t fallbackGeneration=0;unsigned fallbackBits=0;
void fallback(uint64_t generation,unsigned reason){if(fallbackGeneration!=generation){fallbackGeneration=generation;fallbackBits=0;}if(!(fallbackBits&(1u<<reason))){fallbackBits|=1u<<reason;const char *names[]={"missing_translation","line_layout","font_or_glyphs","renderer"};Msg::Log("ConstructorSSF display fallback reason=%s",names[reason]);}}

bool number(uintptr_t p,uint32_t &value){if(!ssflayout::readable(p,4))return false;memcpy(&value,(void*)p,4);return true;}
bool stringAt(uintptr_t p,std::string &raw,std::wstring &text,size_t maximum=8191){raw.clear();text.clear();if(!p)return false;for(size_t n=0;n<=maximum;n++){if(!ssflayout::readable(p+n,1))return false;auto c=*(char*)(p+n);if(!c){if(raw.empty())return true;auto size=MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,raw.data(),int(raw.size()),nullptr,0);if(size<=0)return false;text.resize(size);return MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,raw.data(),int(raw.size()),text.data(),size)==size;}raw+=c;}return false;}
std::wstring quote(const std::wstring &s){std::wstring out=L"\"";for(auto c:s){switch(c){case L'"':out+=L"\\\"";break;case L'\\':out+=L"\\\\";break;case L'\n':out+=L"\\n";break;case L'\r':out+=L"\\r";break;case L'\t':out+=L"\\t";break;default:if(c<32){wchar_t b[7]{};swprintf_s(b,L"\\u%04x",unsigned(c));out+=b;}else out+=c;}}return out+L'"';}
std::shared_ptr<const Resource> current(){auto s=std::atomic_load(&resource);return s&&!quiescing.load()&&client.currentBackfill(s->mode)?s:nullptr;}
// Loading is independent of translation requests. Empty maps acknowledge just
// like nonempty ones, and missing glyphs only affect individual messages.
void loadResources(){uint64_t epoch=0,helper=0;while(!stopping.load()){constructorssfcontrol::Mode mode{};if(client.mode(mode)&&mode.enabled==2){if(mode.epoch!=epoch||mode.helperCreated!=helper){epoch=mode.epoch;helper=mode.helperCreated;bool ok=false;std::shared_ptr<Resource> next;try{next=std::make_shared<Resource>();next->mode=mode;ok=client.loadSnapshot(mode,next->texts);if(ok){auto raster=std::make_shared<ssffont::Raster>();raster->build(next->texts,mode.font);next->raster=raster;}}catch(...){ok=false;}if(!stopping.load()&&client.currentBackfill(mode)){std::shared_ptr<const Resource> value=ok?next:nullptr;std::atomic_store(&resource,value);client.acknowledge(mode,ok);}}}else{std::atomic_store(&resource,std::shared_ptr<const Resource>{});epoch=helper=0;}Sleep(20);}stopped.store(true);}
std::wstring packet(int kind,const std::wstring &text,uint64_t id,uint64_t generation=0,const std::wstring &translation=L""){
 auto p=L"{\"nscripter\":1,\"kind\":"+std::to_wstring(kind)+L",\"page\":"+std::to_wstring(id)+L",\"revision\":"+std::to_wstring(++revision)+L",\"tick\":"+std::to_wstring(GetTickCount64())+L",\"end\":true,\"segment\":false,\"backfillGeneration\":"+std::to_wstring(generation)+L",\"text\":"+quote(text)+L",\"original\":"+quote(text)+L",\"translation\":"+quote(translation);
 if(kind==3){p+=L",\"segments\":[";bool first=true;for(auto &s:menuTexts){if(!first)p+=L',';first=false;p+=quote(s);}p+=L']';}return p+L'}';
}
bool object(hook_context *c,uintptr_t &self){uint32_t p=0;if(!number(c->ebp+int32_t(layout.fields[ssflayout::ObjectLocal]),p)||!ssflayout::readable(p,layout.fields[ssflayout::SourceField]+4))return false;self=p;auto t=GetCurrentThreadId();if(gameThread&&gameThread!=t)return false;gameThread=t;return true;}
// Capture full command text before the original consumer advances its pointer.
void body(hook_context *c,HookParam*,TextBuffer *b,uintptr_t*){
 Flight flight;std::lock_guard<std::recursive_mutex> lock(mutex);if(quiescing.load())return;uintptr_t self=0;uint32_t pointer=0;if(!object(c,self)||!number(c->ebp+int32_t(layout.fields[ssflayout::TextLocal]),pointer))return;Source source;source.address=pointer;source.owner=self;std::wstring text;if(!stringAt(pointer,source.bytes,text))return;source.original=constructorssfsnapshot::normalize(text);if(source.original.empty())return;source.page=++page;
 if(sources.size()>=16384){sources.clear();slots.clear();}sources[pointer]=source;menuTexts.clear();menuAddresses.clear();menuRendered.clear();menuPage=0;
 auto p=packet(0,source.original,source.page);if(!current()&&p.size()<TEXT_BUFFER_SIZE/2)b->from(p);
}
// Menu submission happens only after every label and branch has been registered.
// Labels remain in their original 64-byte slots and branch targets are read-only.
void choices(hook_context *c,HookParam*,TextBuffer *b,uintptr_t*){
 Flight flight;std::lock_guard<std::recursive_mutex> lock(mutex);if(quiescing.load())return;uintptr_t self=0;if(!object(c,self)||!ssflayout::readable(self+layout.fields[ssflayout::ChoiceCount],1))return;unsigned count=*(BYTE*)(self+layout.fields[ssflayout::ChoiceCount]);if(!count||count>8)return;menuPage=++page;menuTexts.clear();std::vector<std::pair<uintptr_t,Slot>> pending;
 for(unsigned n=0;n<count;n++){uintptr_t pointer=self+layout.fields[ssflayout::ChoiceLabels]+n*64;std::string raw;std::wstring text;if(!stringAt(pointer,raw,text,63)){menuTexts.clear();return;}text=constructorssfsnapshot::normalize(text);if(text.empty()){menuTexts.clear();return;}menuTexts.push_back(text);Source source{pointer,self,raw,text,menuPage};pending.push_back({pointer,Slot{self,source,0,true,raw}});}
 menuAddresses.clear();menuRendered.clear();menuGeneration=0;for(auto &s:pending){menuAddresses.push_back(s.first);slots[s.first]=std::move(s.second);}std::wstring text;for(auto &s:menuTexts){if(!text.empty())text+=L'\n';text+=s;}auto p=packet(3,text,menuPage);if(!current()&&p.size()<TEXT_BUFFER_SIZE/2)b->from(p);
}
// Bind original display buffers only after the original append/advance method
// returns. The same path rebuilds history, so no history source is rewritten.
int __fastcall line(void *self,void*,char *destination,char **source,int amount){
 Flight flight;std::lock_guard<std::recursive_mutex> lock(mutex);auto owner=(uintptr_t)self;auto dest=(uintptr_t)destination;uintptr_t from=0;if(ssflayout::readable((uintptr_t)source,4))from=(uintptr_t)*source;auto result=originalLine(self,destination,source,amount);if(quiescing.load()||!from)return result;
 bool owned=false;for(auto key:{ssflayout::BodyLines,ssflayout::HistoryLines}){auto base=owner+layout.fields[key];if(layout.fields[key]&&dest>=base&&dest-base<2048&&(dest-base)%128==0)owned=true;}if(!owned)return result;
 auto found=sources.upper_bound(from);if(found==sources.begin()){slots.erase(dest);return result;}--found;auto s=found->second;if(s.owner!=owner||from<s.address||from-s.address>s.bytes.size()){slots.erase(dest);return result;}
 std::string raw;std::wstring text;size_t lineIndex=0;for(size_t n=0;n<from-s.address;n++)if(s.bytes[n]=='\n')lineIndex++;
 if(!stringAt(dest,raw,text,127)){slots.erase(dest);return result;}slots[dest]=Slot{owner,std::move(s),lineIndex,false,std::move(raw)};return result;
}
// Reflow the entire message before drawing any part. A too-long line or unknown
// control makes the entire message use the original renderer.
bool plainGlyphs(const std::string &raw){
 for(size_t n=0;n<raw.size();){auto c=(BYTE)raw[n];if(c=='\r'){if(n+1==raw.size()||raw[n+1]!='\n')return false;n+=2;continue;}if(c=='\n')return false;if(!((c>=0x81&&c<=0x9f)||(c>=0xe0&&c<=0xfc))||n+1==raw.size())return false;auto d=(BYTE)raw[n+1];if(d<0x40||d>0xfc||d==0x7f)return false;n+=2;}return true;
}
bool linesFor(const Source &source,const std::wstring &translated,ssftext::Plan &plan){
 std::wstring full;std::string raw;if(!stringAt(source.address,raw,full)||raw!=source.bytes)return false;
 return plainGlyphs(raw)&&ssftext::plan(full,translated,plan);
}
// Only buffers created by the verified message/menu consumers can be replaced.
// Generic UI glyph calls keep their original path even when the text is equal.
void __cdecl draw(const char *text,int x,int y,DWORD color,unsigned char size,unsigned char shadow){
 Flight flight;std::lock_guard<std::recursive_mutex> lock(mutex);auto s=current();auto found=slots.find((uintptr_t)text);if(!s||found==slots.end()){if(!s)renderer.release();originalDraw(text,x,y,color,size,shadow);return;}
 auto slot=found->second;std::string raw;std::wstring visible;if(!stringAt((uintptr_t)text,raw,visible,255)||raw!=slot.expected){slots.erase(found);originalDraw(text,x,y,color,size,shadow);return;}
 // Blank line slots have no visible result to report; do not overwrite a
 // preceding successful message notification with a false fallback.
 if(visible.empty()){originalDraw(text,x,y,color,size,shadow);return;}
 auto entry=s->texts.find(slot.source.original);bool fit=entry!=s->texts.end(),preserve=false;unsigned reason=fit?1:0;int drawSize=size;std::wstring translatedLine,fullTranslation;
 if(slot.menu){if(slot.source.page!=menuPage)fit=false;if(fit){fullTranslation=entry->second;translatedLine=entry->second;fit=plainGlyphs(raw)&&raw.find_first_of("\r\n")==std::string::npos&&translatedLine.find(L'\n')==std::wstring::npos&&translatedLine.size()*2<=raw.size();}}
 else if(fit){ssftext::Plan plan;fullTranslation=entry->second;fit=linesFor(slot.source,entry->second,plan)&&slot.line<plan.lines.size();if(fit){auto &original=plan.original[slot.line];fit=visible.size()<=original.size()&&original.compare(0,visible.size(),visible)==0;preserve=plan.preserve[slot.line];auto length=plan.lines[slot.line].size();auto count=original.empty()?0:(length*visible.size()+original.size()-1)/original.size();translatedLine=plan.lines[slot.line].substr(0,count);if(length>ssftext::NormalCells)drawSize=int(size*ssftext::NormalCells/length);if(drawSize<8)fit=false;}}
 if(fit&&(!s->raster||!s->raster->covers(fullTranslation))){fit=false;reason=2;}
 Notification notify{s,slot,L"",false};notification=&notify;
 if(fit&&preserve&&current()==s){notify.translation=fullTranslation;notify.drawn=true;originalDraw(text,x,y,color,size,shadow);notification=nullptr;return;}
 bool rendered=false;if(fit&&current()==s){auto getter=(void*(__cdecl*)())(module+layout.fields[ssflayout::Device]);auto device=getter();if(ssflayout::readable((uintptr_t)device,4)){if(slot.menu)x+=(int(raw.size()/2)-int(translatedLine.size()))*int(size)/2;rendered=renderer.draw(device,s->raster,fullTranslation,translatedLine,x,y,drawSize,color,shadow!=0);}}
 if(rendered){notify.translation=fullTranslation;notify.drawn=true;originalDraw("",x,y,color,size,shadow);}else {fallback(s->mode.generation,fit?3:reason);originalDraw(text,x,y,color,size,shadow);}
 notification=nullptr;
}
// The notification is emitted by the original draw function's return hook.
// Backfill notifications never enter ordinary translation or embed requests.
void drawEnd(hook_context*,HookParam*,TextBuffer *b,uintptr_t*){
 if(!notification)return;auto &n=*notification;if(current()!=n.resource)return;std::wstring original=n.slot.source.original,translation=n.drawn?n.translation:L"";int kind=0;uint64_t id=n.slot.source.page;
 if(n.slot.menu){if(menuGeneration!=n.resource->mode.generation){menuRendered.clear();menuGeneration=n.resource->mode.generation;}menuRendered[n.slot.source.address]=translation;if(menuRendered.size()!=menuAddresses.size())return;kind=3;id=menuPage;original.clear();translation.clear();bool any=false;for(size_t k=0;k<menuTexts.size();k++){auto f=menuRendered.find(menuAddresses[k]);if(f==menuRendered.end())return;if(k){original+=L'\n';translation+=L'\n';}original+=menuTexts[k];if(!f->second.empty()){translation+=f->second;any=true;}else translation+=menuTexts[k];}if(!any)translation.clear();}
 auto key=std::to_wstring(id)+L"|"+original+L"|"+translation;if(lastGeneration==n.resource->mode.generation&&key==lastDisplay)return;lastGeneration=n.resource->mode.generation;lastDisplay=key;auto p=packet(kind,original,id,n.resource->mode.generation,translation);if(p.size()<TEXT_BUFFER_SIZE/2)b->from(p);
}
bool install(uint32_t rva,const char *name,decltype(HookParam::text_fun) fn){HookParam hp{};hp.address=module+rva;hp.type=USING_STRING|CODEC_UTF16|FULL_STRING|NO_CONTEXT;hp.text_fun=fn;return rva&&NewHook(hp,name);}
}

bool ConstructorSSF::attach_function(){
 module=(uintptr_t)GetModuleHandleW(nullptr);if(!ssflayout::load(layout)){Msg::Log("ConstructorSSF read-only unavailable");return false;}
 if(!install(layout.fields[ssflayout::Body],"ConstructorSSFDialogue",body)){Msg::Log("ConstructorSSF read-only unavailable");return false;}
 if(layout.fields[ssflayout::Choices]&&layout.fields[ssflayout::ChoiceLabels]&&layout.fields[ssflayout::ChoiceCount])install(layout.fields[ssflayout::Choices],"ConstructorSSFChoices",choices);
 Msg::Log("ConstructorSSF read-only ready");bool display=layout.display&&layout.fields[ssflayout::Line]&&layout.fields[ssflayout::BodyLines]&&layout.fields[ssflayout::Draw]&&layout.fields[ssflayout::DrawEnd]&&layout.fields[ssflayout::Device];std::vector<void*> created;
 auto create=[&](uint32_t rva,void *callback,void **original){auto target=(void*)(module+rva);if(MH_CreateHook(target,callback,original)!=MH_OK)return false;created.push_back(target);return true;};
 if(display){display=create(layout.fields[ssflayout::Line],(void*)line,(void**)&originalLine)&&create(layout.fields[ssflayout::Draw],(void*)draw,(void**)&originalDraw)&&install(layout.fields[ssflayout::DrawEnd],"ConstructorSSFRenderReturn",drawEnd);if(display)for(auto target:created)if(MH_EnableHook(target)!=MH_OK){display=false;break;}if(!display){for(auto target:created)MH_DisableHook(target);while(active.load())Sleep(1);for(auto target:created)MH_RemoveHook(target);originalLine=nullptr;originalDraw=nullptr;}}
 if(display){stopped.store(false);loader=std::thread(loadResources);Msg::Log("ConstructorSSF backfill ready");}else Msg::Log("ConstructorSSF display unavailable");return true;
}
bool ConstructorSSFPrepareDetach(){
 quiescing.store(true);if(originalDraw)MH_DisableHook((void*)(module+layout.fields[ssflayout::Draw]));if(originalLine)MH_DisableHook((void*)(module+layout.fields[ssflayout::Line]));auto start=GetTickCount64();while(active.load()&&GetTickCount64()-start<5000)Sleep(10);if(active.load())return false;stopping.store(true);start=GetTickCount64();while(!stopped.load()&&GetTickCount64()-start<5000)Sleep(10);if(!stopped.load())return false;if(loader.joinable())loader.join();std::lock_guard<std::recursive_mutex> lock(mutex);renderer.release();slots.clear();sources.clear();menuTexts.clear();std::atomic_store(&resource,std::shared_ptr<const Resource>{});return true;
}
