#include "AbelAST.h"
#include "AbelASTLayout.h"
#include "AbelASTSnapshot.h"
#include "AbelASTControl.h"
#include "MinHook.h"
#include <memory>
#include <atomic>
#include <thread>
#include <map>
#include <mutex>
#include <set>
#include <algorithm>
#include <intrin.h>

namespace {
abellayout::Layout layout;uintptr_t module=0;std::recursive_mutex mutex;
uint64_t page=0,serial=0;DWORD thread=0;
struct Position {uintptr_t engine=0,script=0,group=0;uint32_t index=0,begin=0,end=0;std::wstring text;};
Position previous;std::wstring speaker;std::vector<std::wstring> menuTexts;uint64_t menuPage=0;
std::atomic<bool> quiescing{false},stopping{false},stopped{true};std::atomic<unsigned> activeDraws{0},activeAuxiliary{0};std::thread loader;
abelastcontrol::Client client;
struct Snapshot {abelastcontrol::Mode mode{};abelastsnapshot::Texts texts;};
std::shared_ptr<const Snapshot> snapshot;
struct Slot {std::wstring expected,translated,original;std::shared_ptr<const Snapshot> resource;uint64_t page=0;bool mapped=false;bool isName=false,isChoice=false;};
std::map<uintptr_t,Slot> slots;
using Draw=void(__thiscall*)(void*,int,int,const char*,unsigned);
using Glyph=unsigned short(__thiscall*)(void*,unsigned short);
using FontCreate=void*(__thiscall*)(void*,void*,const char*,int);
using FontDestroy=void(__thiscall*)(void*);
using StringCreate=void*(__thiscall*)(void*,const char*);
using StringFree=void(__thiscall*)(void*);
StringCreate originalStringCreate=nullptr;StringFree originalStringFree=nullptr;
Draw originalDraw=nullptr;Glyph originalGlyph=nullptr;
decltype(&TextOutA) originalTextOut=nullptr;uintptr_t textOutTarget=0;
struct PrivateFont {std::array<BYTE,68> object{};HFONT wide=nullptr;bool initialized=false;std::map<std::wstring,bool> coverage;};
std::map<std::pair<uintptr_t,int>,std::unique_ptr<PrivateFont>> fonts;
thread_local PrivateFont *drawingFont=nullptr;thread_local wchar_t drawingCharacter=0;
thread_local PrivateFont *notificationFont=nullptr;thread_local const Slot *drawingSlot=nullptr;thread_local bool lastGlyph=false;
std::wstring lastDisplay;uint64_t lastGeneration=0;
bool loggedSegment=false,loggedCopy=false,loggedDraw=false;
// current 每次显示重新核对Helper和加载代次，关闭后立即原文回退
std::shared_ptr<const Snapshot> current(){auto s=std::atomic_load(&snapshot);return s&&!quiescing.load()&&client.currentBackfill(s->mode)?s:nullptr;}
// loadSnapshots 在独立线程加载不可变译文快照，零映射也正常确认
void loadSnapshots(){uint64_t epoch=0,helper=0;while(!stopping.load()){abelastcontrol::Mode mode{};if(client.mode(mode)&&mode.enabled==2){if(mode.epoch!=epoch||mode.helperCreated!=helper){epoch=mode.epoch;helper=mode.helperCreated;bool ok=false;std::shared_ptr<Snapshot> next;try{next=std::make_shared<Snapshot>();next->mode=mode;ok=client.loadSnapshot(mode,next->texts);}catch(...){ok=false;}if(!stopping.load()&&client.currentBackfill(mode)){std::shared_ptr<const Snapshot> value=ok?next:nullptr;std::atomic_store(&snapshot,value);client.acknowledge(mode,ok);}}}else{std::atomic_store(&snapshot,std::shared_ptr<const Snapshot>{});epoch=helper=0;}Sleep(20);}stopped.store(true);}
// number 有界读取原生对象字段，不调用不可信虚方法
bool number(uintptr_t p,uint32_t &value){if(!abellayout::readable(p,4))return false;memcpy(&value,(void*)p,4);return true;}
// field 读取已经核对的对象字段
bool field(uintptr_t p,abellayout::Field key,uint32_t &value){return p&&number(p+layout.fields[key],value);}
// stringAt 严格解码MFC字符串，拒绝无终止及编码替换
bool stringAt(uintptr_t p,std::wstring &text){
 uint32_t length=0;if(p<12||!number(p-8,length)||length>8192||!abellayout::readable(p,length+1)||((char*)p)[length])return false;
 if(memchr((void*)p,0,length))return false;auto n=MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,(char*)p,length,nullptr,0);if(length&&n<=0)return false;text.resize(n);return !n||MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,(char*)p,length,text.data(),n)==n;
}
// quote 转义协议文本，不改变正文语义
std::wstring quote(const std::wstring &s){std::wstring out=L"\"";for(auto c:s){switch(c){case L'"':out+=L"\\\"";break;case L'\\':out+=L"\\\\";break;case L'\n':out+=L"\\n";break;case L'\r':out+=L"\\r";break;case L'\t':out+=L"\\t";break;default:if(c<32){wchar_t b[7]{};swprintf_s(b,L"\\u%04x",unsigned(c));out+=b;}else out+=c;}}return out+L'"';}
// packet 发布完整语义段，页号和修订号不依赖原文是否重复
std::wstring packet(int kind,const std::wstring &text,uint64_t fixed=0){return L"{\"nscripter\":1,\"kind\":"+std::to_wstring(kind)+L",\"page\":"+std::to_wstring(fixed?fixed:++page)+L",\"revision\":"+std::to_wstring(++serial)+L",\"tick\":"+std::to_wstring(GetTickCount64())+L",\"end\":true,\"segment\":true,\"backfillGeneration\":0,\"text\":"+quote(text)+L",\"speaker\":"+quote(speaker)+L"}";}
// nodeText 只接受已经定位的普通字符或RUBY执行方法
bool nodeText(uintptr_t node,std::wstring &text){uint32_t vt=0,execute=0,p=0;if(!number(node,vt)||!number(vt+28,execute))return false;auto key=execute==module+layout.fields[abellayout::Character]?abellayout::CharField:execute==module+layout.fields[abellayout::Ruby]?abellayout::RubyField:abellayout::Render;if(key==abellayout::Render||!field(node,key,p)||!stringAt(p,text))return false;return text.find_first_of(L";\t\n\r")==std::wstring::npos;}
// segment 从当前执行游标向两端恢复连续显示段，不把未来命令后的文字提前提交
bool segment(uintptr_t engine,uintptr_t executing,Position &position){
 uint32_t script=0,groupIndex=0,index=0,groups=0,count=0,group=0,array=0,nodes=0;
 if(!field(engine,abellayout::ScriptField,script)||!field(engine,abellayout::GroupField,groupIndex)||!field(engine,abellayout::IndexField,index)||!index||!number(script+4,groups)||!number(script+8,count)||count>100000||groupIndex>=count||!number(groups+groupIndex*4,group)||!field(group,abellayout::ArrayField,array)||!field(group,abellayout::CountField,nodes)||nodes>32768||index>nodes)return false;
 uint32_t current=0;if(!number(array+(index-1)*4,current)||current!=executing)return false;
 uint32_t begin=index-1,end=index;std::wstring part,text;if(!nodeText(current,text))return false;
 while(begin){uint32_t p=0;if(!number(array+(begin-1)*4,p)||!nodeText(p,part))break;if(part.size()+text.size()>4095)return false;text=part+text;begin--;}
 while(end<nodes){uint32_t p=0;if(!number(array+end*4,p)||!nodeText(p,part))break;if(part.size()+text.size()>4095)return false;text+=part;end++;}
 text=abelastsnapshot::normalize(text);if(text.empty())return false;position={engine,script,group,index-1,begin,end,std::move(text)};return true;
}
// remember 记录不可变原文字段身份，歧义指针不选择任意译文
void remember(uintptr_t pointer,const std::wstring &expected,const std::wstring &translated,const std::wstring &original,uint64_t p,const std::shared_ptr<const Snapshot>&s,bool mapped=true){if(slots.size()>32768)slots.clear();auto found=slots.find(pointer);if(found!=slots.end()&&found->second.resource==s&&found->second.page==p&&found->second.expected==expected&&found->second.translated!=translated){found->second.mapped=false;return;}slots[pointer]={expected,translated,original,s,p,mapped};}
// fullCell 按引擎CP932双字节字格判断宽度，省略号等低码位全角符号也可分配中文字格
bool fullCell(const std::wstring &text){if(text.size()!=1)return false;BOOL fallback=FALSE;char encoded[2]{};return WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,text.data(),1,encoded,2,nullptr,&fallback)==2&&!fallback;}
// rememberBody 对单字双字节显示段分配私有字形，不改变原节点数量和原文宽度
void rememberBody(const Position &p){
 auto s=current();if(!s)return;auto found=s->texts.find(p.text);bool fit=found!=s->texts.end()&&found->second.size()<=p.end-p.begin&&found->second.find(L'\n')==std::wstring::npos;
 uint32_t array=0;if(!field(p.group,abellayout::ArrayField,array))return;std::vector<std::pair<uintptr_t,std::wstring>> chars;
 for(auto n=p.begin;n<p.end;n++){uint32_t node=0,vt=0,exec=0,pointer=0;std::wstring raw;if(!number(array+n*4,node)||!number(node,vt)||!number(vt+28,exec))return;auto key=exec==module+layout.fields[abellayout::Ruby]?abellayout::RubyField:abellayout::CharField;if(!field(node,key,pointer)||!stringAt(pointer,raw))return;if(key!=abellayout::CharField||!fullCell(raw))fit=false;chars.push_back({pointer,raw});}
 // 同段共享字符串指针出现不同目标字时，整段回退以免半句中文
 std::map<uintptr_t,std::wstring> assigned;if(fit)for(size_t n=0;n<chars.size();n++){auto translated=n<found->second.size()?found->second.substr(n,1):L"";auto old=assigned.find(chars[n].first);if(old!=assigned.end()&&old->second!=translated){fit=false;break;}assigned[chars[n].first]=translated;}
 // 提前为整段保留容量，不能在登记途中清空而造成半段回填
 if(slots.size()+chars.size()*2>32768)slots.clear();
 for(size_t n=0;n<chars.size();n++)remember(chars[n].first,chars[n].second,fit&&n<found->second.size()?found->second.substr(n,1):L"",p.text,page,s,fit);
}
// fontFor 按引擎采样字格申请包含内部行距的完整字体高度，独立字库不改原字体对象
PrivateFont *fontFor(uintptr_t original){uint32_t size=0,atlas=0,context=0;if(!number(original+12,size)||size<8||size>96||!number(original+64,atlas)||!number(atlas,context)||!context)return nullptr;auto key=std::make_pair(context,int(size));auto found=fonts.find(key);if(found!=fonts.end())return found->second.get();if(fonts.size()>=16)return nullptr;auto f=std::make_unique<PrivateFont>();((FontCreate)(module+layout.fields[abellayout::FontCreate]))(f->object.data(),(void*)context,"MS Gothic",int(size));f->initialized=true;HFONT previous=nullptr;memcpy(&previous,f->object.data(),4);f->wide=CreateFontW(int(size)*4,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,GB2312_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");if(!f->wide){((FontDestroy)(module+layout.fields[abellayout::FontDestroy]))(f->object.data());return nullptr;}DeleteObject(previous);memcpy(f->object.data(),&f->wide,4);auto result=f.get();fonts.emplace(key,std::move(f));return result;}
// covered 核对整句译文的真实字形，缺字时整句回退
bool covered(PrivateFont *f,const std::wstring &text){if(!f)return false;auto known=f->coverage.find(text);if(known!=f->coverage.end())return known->second;auto dc=CreateCompatibleDC(nullptr);if(!dc)return false;auto old=SelectObject(dc,f->wide);std::vector<WORD> glyphs(text.size());bool ok=old&&old!=HGDI_ERROR&&(text.empty()||GetGlyphIndicesW(dc,text.data(),int(text.size()),glyphs.data(),GGI_MARK_NONEXISTING_GLYPHS)!=GDI_ERROR&&std::find(glyphs.begin(),glyphs.end(),0xffff)==glyphs.end());if(old&&old!=HGDI_ERROR)SelectObject(dc,old);DeleteDC(dc);if(f->coverage.size()>=4096)f->coverage.clear();f->coverage.emplace(text,ok);return ok;}
// fits 固定全角译文字格不能超过原CP932显示宽度，半角姓名和选项按半格计量
bool fits(const std::wstring &original,const std::wstring &translated){auto bytes=WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,original.data(),int(original.size()),nullptr,0,nullptr,nullptr);return bytes>0&&translated.size()*2<=size_t(bytes);}
// AuxiliaryCall 计数全局字形和GDI跳板的调用，卸载必须等待其他线程退出
struct AuxiliaryCall {AuxiliaryCall(){activeAuxiliary.fetch_add(1);}~AuxiliaryCall(){activeAuxiliary.fetch_sub(1);}};
// stringCreate 只沿已核对的显示调用点传递原文归属，真实深拷贝后绑定显示副本
void* __fastcall stringCreate(void *self,void*,const char *source){
 AuxiliaryCall active;auto caller=(uintptr_t)_ReturnAddress();std::lock_guard<std::recursive_mutex> lock(mutex);
 Slot origin;bool owned=false;auto s=current();auto found=slots.find((uintptr_t)source);std::wstring raw;
 if(s&&found!=slots.end()&&found->second.resource==s&&caller>=module&&std::find(layout.copyCallers.begin(),layout.copyCallers.end(),uint32_t(caller-module))!=layout.copyCallers.end()&&stringAt((uintptr_t)source,raw)&&raw==found->second.expected){origin=found->second;owned=true;}
 auto result=originalStringCreate(self,source);uint32_t copied=0;
 if(owned&&current()==s&&number((uintptr_t)self,copied)&&copied!=(uintptr_t)source&&stringAt(copied,raw)&&raw==origin.expected){if(slots.size()<32768){slots[copied]=std::move(origin);if(!loggedCopy){loggedCopy=true;Msg::Log("AbelAST display copy bound v=20261017-2");}}}
 return result;
}
// stringFree 在MFC最终释放分配块时撤销归属，防止地址复用后误改无关文字
void __fastcall stringFree(void *header,void*){AuxiliaryCall active;std::lock_guard<std::recursive_mutex> lock(mutex);slots.erase((uintptr_t)header+12);originalStringFree(header);}
// glyph 只在私有字库作用域用Unicode作为缓存键，原字库调用不变
unsigned short __fastcall glyph(void *self,void*,unsigned short code){AuxiliaryCall active;if(drawingFont&&self==drawingFont->object.data()&&drawingCharacter){auto key=static_cast<unsigned short>(drawingCharacter);if((key&255)==0xf0)key=0xd800+(key>>8);return originalGlyph(self,key);}return originalGlyph(self,code);}
// textOut 只对私有字体的字形生成改用UTF16，原游戏GDI文字保持原编码
BOOL WINAPI textOut(HDC dc,int x,int y,LPCSTR text,int n){AuxiliaryCall active;if(drawingFont&&drawingCharacter&&GetCurrentObject(dc,OBJ_FONT)==drawingFont->wide)return TextOutW(dc,x,y,&drawingCharacter,1);return originalTextOut(dc,x,y,text,n);}
// draw 仅重定向已经证明归属的显示参数，原显示列表和存档内容不写
void __fastcall draw(void *self,void*,int x,int y,const char *text,unsigned color){
 activeDraws.fetch_add(1);struct Exit{~Exit(){activeDraws.fetch_sub(1);}} exit;
 std::lock_guard<std::recursive_mutex> lock(mutex);if(quiescing.load()||drawingFont){originalDraw(self,x,y,text,color);return;}auto resource=current();auto found=slots.find((uintptr_t)text);std::wstring raw;
 if(!resource||found==slots.end()||found->second.resource!=resource||!stringAt((uintptr_t)text,raw)||raw!=found->second.expected){originalDraw(self,x,y,text,color);return;}
 if(!loggedDraw){loggedDraw=true;Msg::Log("AbelAST owned display reached v=20261017-2");}
 auto &slot=found->second;auto entry=resource->texts.find(slot.original);auto font=fontFor((uintptr_t)self);notificationFont=font;
 if(!slot.mapped||entry==resource->texts.end()||!covered(font,entry->second)){drawingSlot=&slot;lastGlyph=true;originalDraw(self,x,y,text,color);drawingSlot=nullptr;notificationFont=nullptr;lastGlyph=false;return;}
 uint32_t size=0;if(!number((uintptr_t)self+12,size)){originalDraw(self,x,y,text,color);return;}drawingFont=font;drawingSlot=&slot;
 for(size_t n=0;n<slot.translated.size();n++){drawingCharacter=slot.translated[n];lastGlyph=n+1==slot.translated.size();originalDraw(font->object.data(),x+int(n*size),y,"\x81\x40",color);}
 drawingFont=nullptr;drawingCharacter=0;drawingSlot=nullptr;notificationFont=nullptr;lastGlyph=false;
}
// drawEnd 只在真实绘制已完成时发送当前资源代次的显示通知
void drawEnd(hook_context*,HookParam*,TextBuffer *b,uintptr_t*){
 if(!drawingSlot||!lastGlyph||drawingSlot->isName)return;auto resource=current();if(!resource||resource!=drawingSlot->resource)return;
 std::wstring original=drawingSlot->original,translated,parts;bool menu=drawingSlot->isChoice;
 if(menu){original.clear();bool any=false;for(auto &v:menuTexts){if(!original.empty()){original+=L'\n';translated+=L'\n';parts+=L',';}original+=v;parts+=quote(v);auto found=resource->texts.find(v);if(found!=resource->texts.end()&&fits(v,found->second)&&covered(notificationFont,found->second)){translated+=found->second;any=true;}else translated+=v;}if(!any)translated.clear();}
 else{auto found=resource->texts.find(original);if(drawingFont&&drawingSlot->mapped&&found!=resource->texts.end())translated=found->second;}
 auto key=std::to_wstring(drawingSlot->page)+original;if(lastGeneration==resource->mode.generation&&lastDisplay==key)return;lastGeneration=resource->mode.generation;lastDisplay=key;
 auto p=L"{\"nscripter\":1,\"kind\":"+std::to_wstring(menu?3:0)+L",\"page\":"+std::to_wstring(drawingSlot->page?drawingSlot->page:1)+L",\"revision\":"+std::to_wstring(++serial)+L",\"tick\":"+std::to_wstring(GetTickCount64())+L",\"end\":true,\"segment\":false,\"backfillGeneration\":"+std::to_wstring(resource->mode.generation)+L",\"original\":"+quote(original)+L",\"speaker\":"+quote(menu?L"":speaker)+L",\"text\":"+quote(original)+L",\"translation\":"+quote(translated);if(menu)p+=L",\"segments\":["+parts+L"]";p+=L"}";if(p.size()<TEXT_BUFFER_SIZE/2)b->from(p);
}
// dialogue 在首个实际执行字符时提交完整段，同文新页仍保持不同身份
void dialogue(hook_context *c,HookParam*,TextBuffer *b,uintptr_t*){
 std::lock_guard<std::recursive_mutex> lock(mutex);auto t=GetCurrentThreadId();if(thread&&thread!=t)return;Position now;if(!segment(c->stack[1],c->ecx,now)){previous={};return;}thread=t;
 bool duplicate=previous.engine==now.engine&&previous.script==now.script&&previous.group==now.group&&previous.begin==now.begin&&previous.end==now.end&&previous.text==now.text&&now.index>previous.index;
 previous=now;if(!duplicate){if(!loggedSegment){loggedSegment=true;Msg::Log("AbelAST dialogue captured v=20261017-2");}auto p=packet(0,now.text);rememberBody(now);if(!current()&&p.size()<TEXT_BUFFER_SIZE/2)b->from(p);}
}
// name 只读取明确要求显示的姓名参数，姓名为空仍结束旧段身份
void name(hook_context *c,HookParam*,TextBuffer*,uintptr_t*){
 std::lock_guard<std::recursive_mutex> lock(mutex);previous={};speaker.clear();if(!c->stack[4])return;std::wstring text;if(!stringAt(c->stack[1],text))return;speaker=abelastsnapshot::normalize(text);if(speaker.empty())return;auto s=current();if(s){auto found=s->texts.find(speaker);remember(c->stack[1],text,found!=s->texts.end()?found->second:L"",speaker,page+1,s,found!=s->texts.end()&&fits(text,found->second));slots[c->stack[1]].isName=true;}
}
struct ChoiceFrame {uintptr_t engine=0,pointer=0;uint32_t count=0;std::wstring text;};
thread_local std::vector<ChoiceFrame> choices;
// choiceBegin 保存登记前计数，条件未通过的选项不会成为监听事件
void choiceBegin(hook_context *c,HookParam*,TextBuffer*,uintptr_t*){std::lock_guard<std::recursive_mutex> lock(mutex);ChoiceFrame f;f.engine=c->ecx;f.pointer=c->stack[1];uint32_t scene=0;if(!layout.fields[abellayout::ChoiceScene]||!field(f.engine,abellayout::ChoiceScene,scene)||scene){choices.push_back({});return;}if(choices.size()>=16)choices.clear();if(!field(f.engine,abellayout::ChoiceCount,f.count)||!stringAt(f.pointer,f.text)){choices.push_back({});return;}auto s=current();auto text=abelastsnapshot::normalize(f.text);if(s&&!text.empty()){auto found=s->texts.find(text);remember(f.pointer,f.text,found!=s->texts.end()?found->second:L"",text,f.count?menuPage:page+1,s,found!=s->texts.end()&&fits(f.text,found->second));slots[f.pointer].isChoice=true;}choices.push_back(std::move(f));}
// choiceEnd 只在真实登记计数增加后提交显示值，不改原分支编号
void choiceEnd(hook_context*,HookParam*,TextBuffer *b,uintptr_t*){
 std::lock_guard<std::recursive_mutex> lock(mutex);if(choices.empty())return;auto f=std::move(choices.back());choices.pop_back();uint32_t count=0;if(!f.engine||!field(f.engine,abellayout::ChoiceCount,count)||count!=f.count+1)return;auto text=abelastsnapshot::normalize(f.text);if(text.empty())return;if(f.count==0){menuTexts.clear();menuPage=++page;}if(menuTexts.size()>=128)return;menuTexts.push_back(text);std::wstring joined;for(auto &v:menuTexts){if(!joined.empty())joined+=L'\n';joined+=v;}auto p=packet(1,joined,menuPage);auto s=current();if(s){auto found=s->texts.find(text);remember(f.pointer,f.text,found!=s->texts.end()?found->second:L"",text,menuPage,s,found!=s->texts.end()&&fits(f.text,found->second));slots[f.pointer].isChoice=true;}else{auto key=p.find(L"\"segment\":true");if(key!=std::wstring::npos)p.replace(key,14,L"\"segment\":false");b->from(p);}
}
// install 安装单一专用完整文字回调，不启用通用GDI采集
bool install(uint32_t rva,const char *name,decltype(HookParam::text_fun) fn){HookParam hp{};hp.address=module+rva;hp.type=USING_STRING|CODEC_UTF16|FULL_STRING|NO_CONTEXT;hp.text_fun=fn;return rva&&NewHook(hp,name);}
}
// attach_function 先核对全部语义证据，显示能力与可靠读取分开登记
bool AbelAST::attach_function(){module=(uintptr_t)GetModuleHandleW(nullptr);if(!abellayout::load(layout)){Msg::Log("AbelAST read-only unavailable");return false;}if(!install(layout.fields[abellayout::Character],"AbelASTDialogue",dialogue)||!install(layout.fields[abellayout::Ruby],"AbelASTDialogue",dialogue)){Msg::Log("AbelAST read-only unavailable");return false;}if(layout.fields[abellayout::Name])install(layout.fields[abellayout::Name],"AbelASTDialogue",name);if(layout.fields[abellayout::ChoiceCount]&&layout.fields[abellayout::ChoiceEnd]){install(layout.fields[abellayout::Choice],"AbelASTChoiceBegin",choiceBegin);install(layout.fields[abellayout::ChoiceEnd],"AbelASTChoices",choiceEnd);}Msg::Log("AbelAST read-only ready");
 bool display=layout.display&&layout.fields[abellayout::Draw]&&layout.fields[abellayout::Glyph]&&layout.fields[abellayout::FontCreate]&&layout.fields[abellayout::FontDestroy]&&layout.fields[abellayout::TextOut]&&layout.fields[abellayout::StringCreate]&&layout.fields[abellayout::StringFree];
 std::vector<void*> createdHooks;
 // create 记录本适配器拥有的跳板，安装失败只回滚独立显示组
 auto create=[&](void *target,void *callback,void **original){if(MH_CreateHook(target,callback,original)!=MH_OK)return false;createdHooks.push_back(target);return true;};
 if(display){uint32_t output=0;display=number(module+layout.fields[abellayout::TextOut],output);textOutTarget=output;
  if(display)display=create((void*)(module+layout.fields[abellayout::Draw]),(void*)draw,(void**)&originalDraw)&&create((void*)(module+layout.fields[abellayout::Glyph]),(void*)glyph,(void**)&originalGlyph)&&create((void*)textOutTarget,(void*)textOut,(void**)&originalTextOut)&&create((void*)(module+layout.fields[abellayout::StringCreate]),(void*)stringCreate,(void**)&originalStringCreate)&&create((void*)(module+layout.fields[abellayout::StringFree]),(void*)stringFree,(void**)&originalStringFree)&&install(layout.fields[abellayout::DrawEnd],"AbelASTRenderReturn",drawEnd);
  if(display)for(auto target:createdHooks)if(MH_EnableHook(target)!=MH_OK){display=false;break;}
  if(!display){for(auto target:createdHooks)MH_DisableHook(target);while(activeDraws.load()||activeAuxiliary.load())Sleep(1);for(auto target:createdHooks)MH_RemoveHook(target);originalDraw=nullptr;originalGlyph=nullptr;originalTextOut=nullptr;originalStringCreate=nullptr;originalStringFree=nullptr;textOutTarget=0;}
 }
 if(display){stopped.store(false);loader=std::thread(loadSnapshots);Msg::Log("AbelAST display copy tracking v=20261017-2");Msg::Log("AbelAST backfill ready");}else Msg::Log("AbelAST display unavailable");return true;}
// AbelASTPrepareDetach 等待采集锁释放，不把对象指针保留到下一会话
bool AbelASTPrepareDetach(){quiescing.store(true);if(originalDraw)MH_DisableHook((void*)(module+layout.fields[abellayout::Draw]));auto start=GetTickCount64();while(activeDraws.load()&&GetTickCount64()-start<5000)Sleep(10);if(activeDraws.load())return false;if(originalGlyph)MH_DisableHook((void*)(module+layout.fields[abellayout::Glyph]));if(originalTextOut)MH_DisableHook((void*)textOutTarget);if(originalStringCreate)MH_DisableHook((void*)(module+layout.fields[abellayout::StringCreate]));if(originalStringFree)MH_DisableHook((void*)(module+layout.fields[abellayout::StringFree]));start=GetTickCount64();while(activeAuxiliary.load()&&GetTickCount64()-start<5000)Sleep(10);if(activeAuxiliary.load())return false;stopping.store(true);start=GetTickCount64();while(!stopped.load()&&GetTickCount64()-start<5000)Sleep(10);if(!stopped.load())return false;if(loader.joinable())loader.join();std::lock_guard<std::recursive_mutex> lock(mutex);for(auto &f:fonts)if(f.second->initialized)((FontDestroy)(module+layout.fields[abellayout::FontDestroy]))(f.second->object.data());fonts.clear();slots.clear();previous={};return true;}
