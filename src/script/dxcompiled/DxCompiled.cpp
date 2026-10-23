#if defined(DANGO_DXCOMPILED_TEST)
#include "tests/fixture_support.h"
#endif
#include "DxCompiled.h"
#include "DxCompiledLayout.h"
#include "DxCompiledText.h"
#include "DxCompiledControl.h"
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <set>
#include <intrin.h>
#include "MinHook.h"

namespace {
dxlayout::Layout layout;uintptr_t module=0;
std::atomic<bool> installed{false},quiescing{false},stopping{false},stopped{true};
std::atomic<unsigned> activeDraws{0};std::thread loader;
dxcompiledcontrol::Client client;
struct Snapshot {dxcompiledcontrol::Mode mode{};dxcompiledsnapshot::Texts texts;};
std::shared_ptr<const Snapshot> snapshot;
std::recursive_mutex mutex;
uint64_t serial=0,page=0,bodyPage=0;DWORD thread=0;std::string tape;std::vector<dxtext::Segment> segments;size_t lastSegment=SIZE_MAX;uint32_t lastCursor=0;int lastTapePage=-1;
uintptr_t address(dxlayout::Field n){return layout.fields[n]?module+layout.fields[n]:0;}
uint32_t number(dxlayout::Field n){auto p=address(n);return p&&dxlayout::readable(p,4)?*reinterpret_cast<uint32_t*>(p):0;}
bool textAt(uintptr_t p,size_t cap,std::string &out){if(!dxlayout::readable(p,cap))return false;auto s=(const char*)p;auto end=(const char*)memchr(s,0,cap);if(!end)return false;out.assign(s,end);return true;}
bool bindThread(){auto t=GetCurrentThreadId();if(thread&&thread!=t)return false;thread=t;return true;}
std::wstring packet(int kind,uint64_t p,const std::wstring &text,bool segment,uint64_t generation=0,const std::wstring &original=L"",const std::wstring &translated=L"",const std::vector<std::wstring>&parts={}){
 auto out=L"{\"nscripter\":1,\"kind\":"+std::to_wstring(kind)+L",\"page\":"+std::to_wstring(p?p:1)+L",\"revision\":"+std::to_wstring(++serial)+L",\"tick\":"+std::to_wstring(GetTickCount64())+L",\"end\":true,\"reset\":false,\"segment\":"+(segment?L"true":L"false")+L",\"backfillGeneration\":"+std::to_wstring(generation)+L",\"text\":"+dxtext::quote(text)+L",\"original\":"+dxtext::quote(original)+L",\"translation\":"+dxtext::quote(translated);
 if(kind==3){out+=L",\"segments\":[";for(size_t i=0;i<parts.size();i++){if(i)out+=L',';out+=dxtext::quote(parts[i]);}out+=L']';}return out+L'}';
}
void output(TextBuffer *b,const std::wstring&s){if(!s.empty()&&s.size()<TEXT_BUFFER_SIZE/2)b->from(s);}
std::shared_ptr<const Snapshot> current(){auto s=std::atomic_load(&snapshot);return s&&!quiescing.load()&&client.currentBackfill(s->mode)?s:nullptr;}
void loadSnapshots(){uint64_t epoch=0,helper=0;while(!stopping.load()){
 dxcompiledcontrol::Mode mode{};if(client.mode(mode)&&mode.enabled==2){if(mode.epoch!=epoch||mode.helperCreated!=helper){epoch=mode.epoch;helper=mode.helperCreated;bool ok=false;std::shared_ptr<Snapshot> next;try{next=std::make_shared<Snapshot>();next->mode=mode;ok=client.loadSnapshot(mode,next->texts);}catch(...){ok=false;}if(!stopping.load()&&client.currentBackfill(mode)){std::shared_ptr<const Snapshot> value=ok?next:nullptr;std::atomic_store(&snapshot,value);client.acknowledge(mode,ok);}}}else{std::atomic_store(&snapshot,std::shared_ptr<const Snapshot>{});epoch=helper=0;}Sleep(20);}stopped.store(true);}
// Accepted is the return from the verified bounded copy, after activation. The
// outer receiver is retried while busy; only successful copies create a page.
void accepted(hook_context *c,HookParam*,TextBuffer*,uintptr_t*){
 if(!installed.load()||quiescing.load()||c->eax!=0)return;std::lock_guard<std::recursive_mutex> lock(mutex);if(!bindThread())return;
 bodyPage=++page;lastSegment=SIZE_MAX;lastTapePage=-1;lastCursor=0;segments.clear();tape.clear();std::string raw;
 if(!textAt(address(dxlayout::Buffer),layout.fields[dxlayout::Capacity],raw)||!dxtext::parse(raw,segments))return;tape=std::move(raw);
}
void step(hook_context*,HookParam*,TextBuffer *b,uintptr_t*){
 if(!installed.load()||quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);if(!bindThread()||!number(dxlayout::Active)||number(dxlayout::Opening)!=1)return;
 if(segments.empty()){std::string raw;if(!textAt(address(dxlayout::Buffer),layout.fields[dxlayout::Capacity],raw)||!dxtext::parse(raw,segments))return;tape=std::move(raw);lastSegment=SIZE_MAX;lastTapePage=-1;lastCursor=0;bodyPage=++page;}
 auto cursor=number(dxlayout::Cursor);std::string raw;if(cursor>tape.size()||!textAt(address(dxlayout::Buffer),layout.fields[dxlayout::Capacity],raw)||raw!=tape){segments.clear();return;}
 if(cursor<lastCursor){bodyPage=++page;lastSegment=SIZE_MAX;lastTapePage=-1;}lastCursor=cursor;
 for(size_t n=0;n<segments.size();n++){auto&s=segments[n];if(s.cells.empty()||cursor<=s.cells.front().offset||cursor>s.end+1)continue;if(lastSegment==n)return;if(lastSegment!=SIZE_MAX)bodyPage=++page;lastTapePage=s.page;lastSegment=n;
  if(!current())output(b,packet(0,bodyPage,s.original,true));return;}
}
using Draw=int(__cdecl*)(int,int,const char*,unsigned,int,unsigned,int);
using DefaultDraw=int(__cdecl*)(int,int,const char*,unsigned,unsigned);
using WideDraw=int(__cdecl*)(int,int,const wchar_t*,size_t,unsigned,int,unsigned,int);
using DxCreateFont=int(__cdecl*)(const char*,int,int,int,int,int,int,int);
Draw originalDraw=nullptr;DefaultDraw originalDefault=nullptr;
struct Activity {Activity(){activeDraws.fetch_add(1);}~Activity(){activeDraws.fetch_sub(1);}};
// The DxLib handle is intentionally process-owned and retained until DxLib's
// normal termination. A named, identity-bound cache survives DLL reattachment,
// limiting the entire process to one extra font (not one font per helper).
int privateFont=-1;HANDLE fontMap=nullptr;int *fontSlot=nullptr;HDC glyphDC=nullptr;HFONT glyphFont=nullptr;HGDIOBJ oldGlyphFont=nullptr;
bool ensureFont(){
 if(privateFont>=0)return true;if(!layout.fields[dxlayout::FontCreate]||!layout.fields[dxlayout::FontSize]||layout.fields[dxlayout::FontSize]>128||layout.fields[dxlayout::FontThickness]>16)return false;
 if(!fontSlot){auto name=L"Local\\Dango.DxCompiled.Font."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(dxlayout::created());fontMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4,name.c_str());if(!fontMap)return false;auto fresh=GetLastError()!=ERROR_ALREADY_EXISTS;fontSlot=(int*)MapViewOfFile(fontMap,FILE_MAP_ALL_ACCESS,0,0,4);if(!fontSlot){CloseHandle(fontMap);fontMap=nullptr;return false;}if(fresh)*fontSlot=-1;else{CloseHandle(fontMap);fontMap=nullptr;}}
 if(*fontSlot<0){auto fn=(DxCreateFont)address(dxlayout::FontCreate);auto h=fn("Microsoft YaHei",layout.fields[dxlayout::FontSize],layout.fields[dxlayout::FontThickness],layout.fields[dxlayout::FontType],134,-1,0,-1);if(h<0)return false;*fontSlot=h;}
 if(!glyphDC){auto dc=CreateCompatibleDC(nullptr);auto font=CreateFontW(-int(layout.fields[dxlayout::FontSize]),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,GB2312_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");auto previous=dc&&font?SelectObject(dc,font):nullptr;if(!previous||previous==HGDI_ERROR){if(font)DeleteObject(font);if(dc)DeleteDC(dc);return false;}glyphDC=dc;glyphFont=font;oldGlyphFont=previous;}
 privateFont=*fontSlot;return true;
}
bool glyphs(const std::wstring&t){if(!ensureFont())return false;std::vector<WORD> indices(t.size());if(GetGlyphIndicesW(glyphDC,t.data(),int(t.size()),indices.data(),GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR)return false;return std::find(indices.begin(),indices.end(),0xffff)==indices.end();}
int *sizedFontSlots=nullptr;HANDLE sizedFontMap=nullptr;
// itemFont 在原行的全角宽度预算内缩小长译文字号，字体按进程和字号有界复用
int itemFont(const std::wstring &source,const std::wstring &painted){
 if(!ensureFont())return -1;auto units=[](const std::wstring &s){size_t n=0;for(auto c:s)n+=(c<128||(c>=0xff61&&c<=0xff9f))?1:2;return n;};
 auto capacity=units(source),length=units(painted);if(!capacity||!length)return -1;if(length<=capacity)return privateFont;
 auto size=int(layout.fields[dxlayout::FontSize]*capacity/length);if(size<(std::max)(12,int(layout.fields[dxlayout::FontSize]*3/5))||size>128)return -1;
 if(!sizedFontSlots){auto name=L"Local\\Dango.DxCompiled.SizedFonts."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(dxlayout::created());sizedFontMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,129*sizeof(int),name.c_str());if(!sizedFontMap)return -1;bool fresh=GetLastError()!=ERROR_ALREADY_EXISTS;sizedFontSlots=(int*)MapViewOfFile(sizedFontMap,FILE_MAP_ALL_ACCESS,0,0,129*sizeof(int));if(!sizedFontSlots){CloseHandle(sizedFontMap);sizedFontMap=nullptr;return -1;}if(fresh)std::fill(sizedFontSlots,sizedFontSlots+129,-1);else{CloseHandle(sizedFontMap);sizedFontMap=nullptr;}}
 if(sizedFontSlots[size]<0)sizedFontSlots[size]=((DxCreateFont)address(dxlayout::FontCreate))("Microsoft YaHei",size,layout.fields[dxlayout::FontThickness],layout.fields[dxlayout::FontType],134,-1,0,-1);return sizedFontSlots[size];
}
std::set<std::string> fallbackDiagnostics;
uint64_t diagnosticGeneration=0;
// displayFallback 按代次与来源去重记录局部回退原因，不输出游戏正文且限制日志数量
void displayFallback(const char *kind,size_t source,const char *reason,size_t length,size_t capacity){
 auto s=current();if(!s)return;if(diagnosticGeneration!=s->mode.generation){diagnosticGeneration=s->mode.generation;fallbackDiagnostics.clear();}auto key=std::string(kind)+std::to_string(source)+reason;
 if(fallbackDiagnostics.size()<128&&fallbackDiagnostics.insert(key).second)Msg::Log("DxCompiled display fallback kind=%s source=%u reason=%s translated=%u capacity=%u",kind,unsigned(source),reason,unsigned(length),unsigned(capacity));
}
struct Frame {std::shared_ptr<const Snapshot> resource;std::wstring rows[40];std::set<int> expected,painted;std::wstring original,translated;bool changed=false;uint64_t page=0;};
thread_local std::shared_ptr<Frame> frame;
std::wstring lastDisplay;uint64_t lastDisplayGeneration=0;
struct Menu {bool active=false;std::shared_ptr<const Snapshot> resource;std::vector<std::wstring> originals,display;std::set<size_t> painted;};
thread_local Menu menu;
struct Visible {std::wstring original,display;std::shared_ptr<const Snapshot> resource;};
struct LiteralVisible {Visible value;int x=0,y=0;bool applied=false;std::set<size_t> lines;size_t expected=0;std::set<std::pair<int,int>> painted;};
struct LiteralFrame {uintptr_t owner=0;std::shared_ptr<const Snapshot> resource;std::map<uint64_t,LiteralVisible> visible;std::vector<Visible> inherited;};
thread_local std::vector<LiteralFrame> literalFrames;
struct LiteralState {std::wstring key;uint64_t tick=0;};
std::map<uintptr_t,LiteralState> literalStates;
// Literal groups own only the proved draw arguments. Complementary masks are
// flattened into one translated sentence; original buffers and colors stay intact.
void literalBegin(hook_context*,HookParam*hp,TextBuffer*,uintptr_t*){
 if(quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);if(!bindThread())return;
 if(literalFrames.size()>=8)literalFrames.clear();literalFrames.push_back({hp->user_value,current(),{}});
}
std::vector<Visible> takeLiterals(uintptr_t owner){
 std::vector<Visible> values;if(literalFrames.empty()||literalFrames.back().owner!=owner)return values;auto f=std::move(literalFrames.back());literalFrames.pop_back();if(f.resource!=current())return values;
 // 物品说明按实际显示坐标排列，不让常量地址顺序打乱名称和正文。
 std::vector<LiteralVisible*> items;for(auto &item:f.visible){if(item.second.value.original.empty())continue;if(item.second.expected&&!item.second.applied)item.second.value.display=item.second.value.original;if(item.first>>63)items.push_back(&item.second);else values.push_back(std::move(item.second.value));}
 std::stable_sort(items.begin(),items.end(),[](const LiteralVisible*a,const LiteralVisible*b){return a->y!=b->y?a->y<b->y:a->x<b->x;});for(auto item:items)values.push_back(std::move(item->value));values.insert(values.end(),f.inherited.begin(),f.inherited.end());return values;
}
// One return callback has one output buffer. Menu items and their description
// must be published together, before committing deduplication for either part.
void visibleOutput(uintptr_t owner,TextBuffer*b,const std::vector<Visible>&values){
 if(values.empty())return;auto resource=current();for(auto &v:values)if(v.resource!=resource)return;
 // A verified enclosing draw owner publishes its nested consumers as one page,
 // avoiding out-of-order delivery by separate native text-thread workers.
 if(!literalFrames.empty()&&literalFrames.back().resource==resource&&literalFrames.back().inherited.size()+values.size()<=256){auto &dest=literalFrames.back().inherited;dest.insert(dest.end(),values.begin(),values.end());return;}
 std::wstring original,display;std::vector<std::wstring> parts;bool changed=false;for(auto &v:values){if(!original.empty()){original+=L'\n';display+=L'\n';}original+=v.original;display+=v.display;parts.push_back(v.display);changed=changed||v.original!=v.display;}
 auto key=std::to_wstring(resource?resource->mode.generation:0)+dxtext::quote(original)+dxtext::quote(display);auto now=GetTickCount64();auto&state=literalStates[owner];bool fresh=key!=state.key||now-state.tick>250;state.tick=now;if(!fresh)return;state.key=key;output(b,packet(3,++page,display,false,resource?resource->mode.generation:0,resource?original:L"",resource&&changed?display:L"",parts));
}
void literalEnd(hook_context*,HookParam*hp,TextBuffer*b,uintptr_t*){
 if(quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);visibleOutput(hp->user_value,b,takeLiterals(hp->user_value));
}
// itemGroupTranslation 按原固定行容量分配完整译文，所有原行一致后才允许使用
bool itemGroupTranslation(const dxlayout::ItemText &source,const std::wstring &translation,std::wstring &painted){
 if(source.key.empty()||source.widths.empty()||source.line>=source.widths.size())return false;
 for(auto &part:layout.itemTexts)if(part.group==source.group){std::string raw;if(!part.capacity||!textAt(module+part.text,part.capacity,raw)||raw!=part.raw)return false;}
 std::vector<std::wstring> lines(source.widths.size());size_t row=0;for(auto c:translation){if(c==L'\n'){if(++row>=lines.size())return false;continue;}while(row<lines.size()&&lines[row].size()>=source.widths[row])row++;if(row>=lines.size())return false;lines[row]+=c;}
 std::wstring glyphText;for(auto &line:lines)glyphText+=line;if(glyphText.empty()||!glyphs(glyphText))return false;painted=lines[source.line];if(painted.empty()&&!source.original.empty())painted=L"　";return true;
}
// 物品原串仅在已证明的调用点及有限来源集合内替换，重复描边共享同一通知。
bool itemDraw(int x,int y,const char*text,unsigned color,int font,unsigned edge,bool def,uintptr_t caller,int &result){
 if(literalFrames.empty())return false;auto &frame=literalFrames.back();auto resource=current();if(frame.resource!=resource)return false;
 for(auto &call:layout.itemCalls){if(module+call.site!=caller||call.owner!=frame.owner||call.domain>=layout.itemDomains.size())continue;
  // 已核对完整局部复制的来源可按精确原始字节匹配，仍限当前显示归属及有限文字域
  std::string copied;if(call.byValue&&!textAt((uintptr_t)text,512,copied))continue;
  for(auto index:layout.itemDomains[call.domain]){if(index>=layout.itemTexts.size())continue;auto &s=layout.itemTexts[index];if(!s.capacity||(module+s.text!=(uintptr_t)text&&(!call.byValue||copied!=s.raw)))continue;
   if(module+s.text!=(uintptr_t)text){std::string source;if(!textAt(module+s.text,s.capacity,source)||source!=s.raw)return false;}
   std::string raw;if(!textAt((uintptr_t)text,s.capacity,raw)||raw!=s.raw)return false;auto original=s.group?s.key:dxcompiledsnapshot::normalize(s.original);if(original.empty())return false;
   auto fallback=[&](){return def?originalDefault(x,y,text,color,edge):originalDraw(x,y,text,color,font,edge,0);};
   auto found=resource?resource->texts.find(original):dxcompiledsnapshot::Texts::const_iterator{};
   std::wstring groupPainted;bool safe=resource&&found!=resource->texts.end()&&(s.group?itemGroupTranslation(s,found->second,groupPainted):(found->second.find(L'\n')==std::wstring::npos&&glyphs(found->second)));
   const uint64_t key=(uint64_t(1)<<63)|(s.group?s.group:s.text);auto &v=frame.visible[key];
   if(safe){size_t indent=0;while(indent<s.original.size()&&dxcompiledsnapshot::whitespace(s.original[indent]))indent++;auto painted=s.group?groupPainted:s.original.substr(0,indent)+found->second;
    // 原始空数组不提供额外字格，保持原空绘制并参与整组完成核对。
    bool empty=s.group&&painted.empty();auto fittedFont=empty?0:itemFont(s.original,painted);if(fittedFont>=0){result=empty?fallback():((WideDraw)address(dxlayout::DrawWide))(x,y,painted.c_str(),painted.size(),color,fittedFont,edge,0);if(result==0){if(s.group){v.value={original,found->second,resource};v.x=x;if(v.lines.empty())v.y=y;v.lines.insert(s.line);if(!v.expected)v.expected=s.widths.size();v.applied=v.lines.size()==v.expected;}else v={{original,found->second,resource},x,y,true};return true;}}
    else displayFallback("item",s.text,"width",painted.size(),s.original.size());
   }
   else if(resource)displayFallback("item",s.text,found==resource->texts.end()?"missing":"characters_or_newline",found==resource->texts.end()?0:found->second.size(),s.original.size());
   result=fallback();if(result==0){v={{original,original,resource},x,y,false};if(s.group)v.expected=s.widths.size()+1;}return true;
  }
 }return false;
}
bool literalDraw(int x,int y,const char*text,unsigned color,int font,unsigned edge,bool def,uintptr_t caller,int &result){
 if(literalFrames.empty())return false;auto &frame=literalFrames.back();auto resource=current();if(frame.resource!=resource)return false;
 for(auto &s:layout.literals){if(module+s.site!=caller||module+s.text!=(uintptr_t)text||s.owner!=frame.owner)continue;
  std::string raw;if(!textAt((uintptr_t)text,s.capacity,raw)||raw!=s.raw)return false;
  auto &v=frame.visible[s.group];auto found=resource?resource->texts.find(s.original):dxcompiledsnapshot::Texts::const_iterator{};
  auto fallback=[&](){return def?originalDefault(x,y,text,color,edge):originalDraw(x,y,text,color,font,edge,0);};
  // 遮罩所有原层仍一致时才绘制整句，副层只抑制实际成功绘过的坐标
  bool intact=true;if(s.role)for(auto &part:layout.literals)if(part.group==s.group&&part.owner==s.owner){std::string source;if(!textAt(module+part.text,part.capacity,source)||source!=part.raw){intact=false;break;}}
  if(s.role==2){if(intact&&v.applied&&v.painted.count({x,y})){result=0;return true;}result=fallback();if(result==0&&!v.applied)v.value={s.original,s.original,resource};return true;}
  bool safe=intact&&(v.painted.size()<128||v.painted.count({x,y}))&&resource&&found!=resource->texts.end()&&found->second.find(L'\n')==std::wstring::npos&&found->second.size()<=s.original.size()&&glyphs(found->second);
  std::wstring painted;if(safe){painted=found->second;if(s.role==0){std::wstring source;safe=dxtext::decode(s.raw.data(),s.raw.size(),source);size_t indent=0;while(indent<source.size()&&dxcompiledsnapshot::whitespace(source[indent]))indent++;painted=source.substr(0,indent)+painted;}}
  if(safe){result=((WideDraw)address(dxlayout::DrawWide))(x,y,painted.c_str(),painted.size(),color,privateFont,edge,0);if(result==0){v.value={s.original,found->second,resource};v.x=x;v.y=y;v.applied=true;v.painted.insert({x,y});return true;}}
  result=fallback();if(result==0)v={{s.original,s.original,resource},x,y,false};return true;
 }return false;
}
thread_local std::map<uintptr_t,Visible> visibleSubtitles;
struct SubtitleState {std::wstring key;uint64_t tick=0;};
std::map<uintptr_t,SubtitleState> subtitleStates;
void subtitleBegin(hook_context*,HookParam*,TextBuffer*,uintptr_t*){visibleSubtitles.clear();}
void subtitleEnd(hook_context*,HookParam*hp,TextBuffer*b,uintptr_t*){
 if(quiescing.load()||visibleSubtitles.empty())return;std::lock_guard<std::recursive_mutex> lock(mutex);auto resource=current();std::wstring original,display;bool changed=false;std::vector<std::wstring> parts;
 for(auto&item:visibleSubtitles){auto&v=item.second;if(v.resource!=resource){visibleSubtitles.clear();return;}if(!original.empty()){original+=L'\n';display+=L'\n';}original+=v.original;display+=v.display;parts.push_back(resource?v.display:v.original);changed=changed||v.original!=v.display;}
 visibleSubtitles.clear();auto now=GetTickCount64();auto key=std::to_wstring(resource?resource->mode.generation:0)+dxtext::quote(original)+dxtext::quote(display);auto&state=subtitleStates[hp->user_value];bool fresh=key!=state.key||now-state.tick>250;state.tick=now;if(!fresh)return;state.key=key;auto p=++page;
 output(b,packet(3,p,resource?display:original,false,resource?resource->mode.generation:0,resource?original:L"",resource&&changed?display:L"",parts));
}
void renderBegin(hook_context*,HookParam*,TextBuffer*,uintptr_t*){
 frame.reset();if(!installed.load()||quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);if(!bindThread()||!number(dxlayout::Active)||number(dxlayout::Opening)!=1)return;auto resource=current();if(!resource)return;
 auto next=std::make_shared<Frame>();next->resource=resource;next->page=bodyPage;auto cursor=number(dxlayout::Cursor);int tapePage=0;for(auto&s:segments)if(s.begin<cursor)tapePage=s.page;
 std::vector<std::wstring> rowCells[40];for(int r=0;r<40;r++){std::string raw;auto base=address(r<20?dxlayout::Rows:dxlayout::Highlight);if(!textAt(base+(r%20)*49,49,raw)||!dxtext::rowCells(raw,rowCells[r]))return;}
 size_t rowExtra[20]{};
 for(auto&s:segments){if(s.page!=tapePage||s.cells.empty()||cursor<=s.cells.front().offset)continue;if(!next->original.empty()){next->original+=L'\n';next->translated+=L'\n';}next->original+=s.original;std::wstring display=s.original;
  auto span=dxtext::displaySpan(s,segments);auto found=resource->texts.find(s.original);std::wstring translated;bool complete=cursor>=s.end;bool ok=false;const char *reason="missing";
  // 同行前段已扩展时扣除其私有显示宽度，后段不能再次使用这些行尾空位。
  span.cells.erase(std::remove_if(span.cells.begin(),span.cells.end(),[&](const dxtext::Cell&c){return c.row<0||c.row>=20||c.column<0||size_t(c.column)+rowExtra[c.row]>=24;}),span.cells.end());
  size_t inlineExtra=0;
  if(complete&&found!=resource->texts.end()){reason="width";bool fits=dxtext::fit(span,found->second,translated);
   // 仅对等宽单行使用整行剩余容量，后续段在绘制副本中顺延，原槽和游标不动。
   if(!fits&&!span.cells.empty()&&s.cells.front().row==s.cells.back().row&&found->second.find(L'\n')==std::wstring::npos&&dxcompiledsnapshot::safe(s.original,found->second)&&found->second.size()>span.cells.size()){
    int row=s.cells.front().row;size_t end=0;bool uniform=!s.unsafe;
    for(auto&part:segments)if(part.page==s.page)for(auto&c:part.cells)if(c.row==row){end=(std::max)(end,size_t(c.column+1));uniform=uniform&&c.glyph.size()==1;}
    for(int layer:{row,row+20})for(auto&cell:rowCells[layer])uniform=uniform&&cell.size()==1;
    auto extra=found->second.size()-span.cells.size();
    if(uniform&&(std::max)(end,size_t(span.cells.back().column+1))+rowExtra[row]+extra<=24){translated=found->second;inlineExtra=extra;fits=true;}
   }
   if(fits){reason="font_or_glyphs";ok=glyphs(translated);}
  }
  bool styled=std::any_of(s.cells.begin(),s.cells.end(),[](const dxtext::Cell&c){return c.highlight;});
  // 核对两层互补字格后整句使用普通色，不猜测中文关键词的高亮位置。
  if(ok){reason="layout";for(auto&c:span.cells){if(c.row<0||c.row>=20||c.column<0||c.column>=24){ok=false;break;}auto &base=rowCells[c.row],&over=rowCells[c.row+20];size_t col=c.column;
   if(c.offset==SIZE_MAX){if((col<base.size()&&base[col]!=L"　")||(col<over.size()&&over[col]!=L"　")){ok=false;break;}}
   else if(col>=base.size()||base[col]!=(c.highlight?L"　":c.glyph)||(styled&&(col>=over.size()||over[col]!=(c.highlight?c.glyph:L"　")))){ok=false;break;}
  }}
  if(ok){for(size_t n=0;n<span.cells.size();n++){auto&c=span.cells[n];auto &base=rowCells[c.row],&over=rowCells[c.row+20];size_t col=c.column;
    auto painted=translated.substr(n,inlineExtra&&n+1==span.cells.size()?std::wstring::npos:1);
    if(col<base.size()||painted!=L"　"){if(base.size()<=col)base.resize(col+1,L"　");base[col]=painted;next->expected.insert(c.row);}
    if((styled||(inlineExtra&&!over.empty()))&&(col<over.size()||painted!=L"　")){if(over.size()<=col)over.resize(col+1,L"　");over[col]=std::wstring(painted.size(),L'　');next->expected.insert(c.row+20);}
   }if(inlineExtra)rowExtra[s.cells.front().row]+=inlineExtra;display=found->second;next->changed=true;}
  else if(complete)displayFallback("body",std::hash<std::string>{}(tape)^s.begin,reason,found==resource->texts.end()?0:found->second.size(),span.cells.size());
  next->translated+=display;
 }
 for(int r=0;r<40;r++)for(auto &cell:rowCells[r])next->rows[r]+=cell;
 if(!next->original.empty())frame=std::move(next);
}
// DrawStringToHandle and the default wrapper keep all positions, edge colors,
// font sizing and input behaviour. Only verified display arguments are redirected.
int drawCommon(int x,int y,const char*text,unsigned color,int font,unsigned edge,int vertical,bool def,uintptr_t caller){
 Activity activity;if(quiescing.load())return def?originalDefault(x,y,text,color,edge):originalDraw(x,y,text,color,font,edge,vertical);
 std::lock_guard<std::recursive_mutex> lock(mutex);auto resource=current();std::wstring translated,original;int row=-1;size_t menuIndex=SIZE_MAX,textCapacity=50;bool subtitleText=false;
 int literalResult=0;if(!vertical&&(itemDraw(x,y,text,color,font,edge,def,caller,literalResult)||literalDraw(x,y,text,color,font,edge,def,caller,literalResult)))return literalResult;
 auto pointer=(uintptr_t)text;
 if(!vertical&&frame&&resource==frame->resource&&pointer>=address(dxlayout::Rows)&&pointer<address(dxlayout::Rows)+980&&(pointer-address(dxlayout::Rows))%49==0){row=int((pointer-address(dxlayout::Rows))/49);if(frame->expected.count(row))translated=frame->rows[row];}
 if(translated.empty()&&!vertical&&frame&&resource==frame->resource&&pointer>=address(dxlayout::Highlight)&&pointer<address(dxlayout::Highlight)+980&&(pointer-address(dxlayout::Highlight))%49==0){row=20+int((pointer-address(dxlayout::Highlight))/49);if(frame->expected.count(row))translated=frame->rows[row];}
 if(translated.empty()&&!vertical){
  bool owned=false;for(auto&s:layout.subtitles)if(pointer==module+s.destination){owned=true;subtitleText=true;textCapacity=s.capacity;break;}
  if(menu.active&&menu.resource==resource){if(pointer>=address(dxlayout::ChoiceBuffer)&&pointer<address(dxlayout::ChoiceBuffer)+450&&(pointer-address(dxlayout::ChoiceBuffer))%50==0){menuIndex=(pointer-address(dxlayout::ChoiceBuffer))/50;owned=menuIndex<menu.originals.size();}else if(caller>=address(dxlayout::YesNo)&&caller<address(dxlayout::YesNoEnd)){owned=true;}}
  std::string raw;if(owned&&textAt(pointer,textCapacity,raw)&&dxtext::decode(raw.data(),raw.size(),original)){
   original=dxcompiledsnapshot::normalize(original);if(menuIndex==SIZE_MAX&&menu.active){for(size_t n=0;n<menu.originals.size();n++)if(menu.originals[n]==original){menuIndex=n;break;}}
   if(resource){auto found=resource->texts.find(original);if(found!=resource->texts.end()&&found->second.find(L'\n')==std::wstring::npos&&found->second.size()<=original.size()&&glyphs(found->second))translated=found->second;}
  }
 }
 if(!translated.empty()&&ensureFont()){
  int result=((WideDraw)address(dxlayout::DrawWide))(x,y,translated.c_str(),translated.size(),color,privateFont,edge,vertical);
  if(result==0){if(row>=0)frame->painted.insert(row);if(menuIndex!=SIZE_MAX&&menuIndex<menu.display.size()){menu.display[menuIndex]=translated;menu.painted.insert(menuIndex);}if(subtitleText&&!original.empty())visibleSubtitles[pointer]={original,translated,resource};return result;}
 }
 // Failed or unowned draws always execute the original consumer.
 auto result=def?originalDefault(x,y,text,color,edge):originalDraw(x,y,text,color,font,edge,vertical);if(result==0){if(subtitleText&&!original.empty())visibleSubtitles[pointer]={original,original,resource};if(menuIndex!=SIZE_MAX&&menuIndex<menu.display.size())menu.painted.insert(menuIndex);}return result;
}
int __cdecl draw(int x,int y,const char*t,unsigned c,int f,unsigned e,int v){return drawCommon(x,y,t,c,f,e,v,false,(uintptr_t)_ReturnAddress());}
int __cdecl drawDefault(int x,int y,const char*t,unsigned c,unsigned e){return drawCommon(x,y,t,c,-1,e,0,true,(uintptr_t)_ReturnAddress());}
void renderEnd(hook_context*,HookParam*,TextBuffer*b,uintptr_t*){
 if(quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);auto resource=current();if(!resource)return;
 if(frame&&frame->resource==resource){auto f=std::move(frame);if(!f->painted.empty()&&f->painted!=f->expected)return;bool applied=f->changed&&f->painted==f->expected;auto displayed=applied?f->translated:f->original;auto key=std::to_wstring(f->page)+dxtext::quote(displayed);if(lastDisplayGeneration!=resource->mode.generation||lastDisplay!=key){lastDisplay=key;lastDisplayGeneration=resource->mode.generation;output(b,packet(0,f->page,displayed,false,resource->mode.generation,f->original,applied?displayed:L""));return;}}
}
void menuBegin(hook_context*c,HookParam*hp,TextBuffer*,uintptr_t*){
 HookParam literal{};literal.user_value=layout.fields[hp->user_value?dxlayout::YesNo:dxlayout::Choice];literalBegin(nullptr,&literal,nullptr,nullptr);
 menu={};if(!installed.load()||quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);if(!bindThread())return;if(BYTE(c->stack[1])!=0)return;menu.resource=current();
 if(hp->user_value){menu.originals={L"はい",L"いいえ"};}else{auto countAddress=address(dxlayout::ChoiceCount);if(!dxlayout::readable(countAddress,1))return;auto count=*reinterpret_cast<const BYTE*>(countAddress);if(count<1||count>9)return;for(size_t n=0;n<count;n++){std::string raw;std::wstring s;if(!textAt(address(dxlayout::ChoiceBuffer)+50*n,50,raw)||!dxtext::decode(raw.data(),raw.size(),s))return;s=dxcompiledsnapshot::normalize(s);if(s.empty())return;menu.originals.push_back(s);}}
 menu.display=menu.originals;menu.active=true;
}
void menuEnd(hook_context*,HookParam*hp,TextBuffer*b,uintptr_t*){
 if(quiescing.load())return;std::lock_guard<std::recursive_mutex> lock(mutex);auto owner=layout.fields[hp->user_value?dxlayout::YesNo:dxlayout::Choice];auto literals=takeLiterals(owner);auto m=std::move(menu);menu={};std::vector<Visible> values;
 if(m.active&&m.painted.size()==m.originals.size()&&m.resource==current())for(size_t n=0;n<m.originals.size();n++)values.push_back({m.originals[n],m.display[n],m.resource});
 values.insert(values.end(),literals.begin(),literals.end());visibleOutput(owner,b,values);
}
std::vector<uintptr_t> captureSites;
bool hook(uintptr_t p,const char*name,decltype(HookParam::text_fun) callback,uintptr_t user=0){
 if(!p)return false;
 // A detour can span more than its five-byte jump. Nearby callbacks need one
 // combined boundary, never independently overlapping MinHook patches.
 for(auto existing:captureSites)if((p>existing?p-existing:existing-p)<32){Msg::Log("DxCompiled overlapping capture boundary rejected");return false;}
 HookParam hp{};hp.address=p;hp.type=USING_STRING|CODEC_UTF16|FULL_STRING|NO_CONTEXT;hp.text_fun=callback;hp.user_value=user;if(!NewHook(hp,name))return false;captureSites.push_back(p);return true;
}
}
bool DxCompiled::attach_function(){
 module=(uintptr_t)GetModuleHandleW(nullptr);if(!dxlayout::load(layout)){Msg::Log("DxCompiled layout rejected stage=%d",dxlayout::failure);Msg::Log("DxCompiled read-only unavailable");return false;}
 if(!hook(address(dxlayout::Accepted),"DxCompiledAccepted",accepted)||!hook(address(dxlayout::Step),"DxCompiledDialogue",step)){Msg::Log("DxCompiled read-only unavailable");return false;}
 installed.store(true);Msg::Log("DxCompiled read-only ready");
 bool display=layout.display&&address(dxlayout::RenderEnd)&&address(dxlayout::FontCreate)&&address(dxlayout::DrawWide);
 if(display){display=MH_CreateHook((void*)address(dxlayout::Draw),(void*)draw,(void**)&originalDraw)==MH_OK;if(display&&address(dxlayout::DrawDefault))display=MH_CreateHook((void*)address(dxlayout::DrawDefault),(void*)drawDefault,(void**)&originalDefault)==MH_OK;
  if(display)display=hook(address(dxlayout::Render),"DxCompiledRenderBegin",renderBegin)&&hook(address(dxlayout::RenderEnd),"DxCompiledRenderReturn",renderEnd);
  if(display&&address(dxlayout::Choice)&&address(dxlayout::ChoiceEnd))display=hook(address(dxlayout::Choice),"DxCompiledMenuBegin",menuBegin)&&hook(address(dxlayout::ChoiceEnd),"DxCompiledSprites",menuEnd);
  if(display&&address(dxlayout::YesNo)&&address(dxlayout::YesNoEnd))display=hook(address(dxlayout::YesNo),"DxCompiledMenuBegin",menuBegin,1)&&hook(address(dxlayout::YesNoEnd),"DxCompiledSprites",menuEnd,1);
  std::set<uint32_t> owners;for(auto&s:layout.subtitles)if(display&&owners.insert(s.owner).second)display=hook(module+s.owner,"DxCompiledSubtitleBegin",subtitleBegin,s.owner)&&hook(module+s.end,"DxCompiledSprites",subtitleEnd,s.owner);
  std::set<uint32_t> literalOwners;for(auto&s:layout.literals)if(display&&s.owner!=layout.fields[dxlayout::Choice]&&s.owner!=layout.fields[dxlayout::YesNo]&&literalOwners.insert(s.owner).second){if(!hook(module+s.end,"DxCompiledSprites",literalEnd,s.owner)||!hook(module+s.owner,"DxCompiledLiteralBegin",literalBegin,s.owner))Msg::Log("DxCompiled optional literal capture unavailable");}
  literalOwners.insert(layout.fields[dxlayout::Choice]);literalOwners.insert(layout.fields[dxlayout::YesNo]);
  for(auto&s:layout.itemCalls)if(display&&literalOwners.insert(s.owner).second){if(!hook(module+s.end,"DxCompiledSprites",literalEnd,s.owner)||!hook(module+s.owner,"DxCompiledLiteralBegin",literalBegin,s.owner))Msg::Log("DxCompiled optional item capture unavailable");}
  if(display)display=MH_EnableHook((void*)address(dxlayout::Draw))==MH_OK&&(!originalDefault||MH_EnableHook((void*)address(dxlayout::DrawDefault))==MH_OK);
 }
 if(display){stopped.store(false);loader=std::thread(loadSnapshots);Msg::Log("DxCompiled backfill ready");}
 else{if(originalDraw){MH_DisableHook((void*)address(dxlayout::Draw));MH_RemoveHook((void*)address(dxlayout::Draw));originalDraw=nullptr;}if(originalDefault){MH_DisableHook((void*)address(dxlayout::DrawDefault));MH_RemoveHook((void*)address(dxlayout::DrawDefault));originalDefault=nullptr;}Msg::Log("DxCompiled display unavailable");}
 return true;
}
bool DxCompiledPrepareDetach(){
 quiescing.store(true);
 // 一次暂停线程撤销本组件全部入口，避免界面增多后逐个冻结线程拖慢卸载与重连
 auto disabled=MH_DisableHook(MH_ALL_HOOKS);if(disabled!=MH_OK&&disabled!=MH_ERROR_DISABLED)return false;
 auto start=GetTickCount64();while(activeDraws.load()&&GetTickCount64()-start<5000)Sleep(10);if(activeDraws.load())return false;
 stopping.store(true);start=GetTickCount64();while(!stopped.load()&&GetTickCount64()-start<5000)Sleep(10);if(!stopped.load())return false;if(loader.joinable())loader.join();std::atomic_store(&snapshot,std::shared_ptr<const Snapshot>{});
 if(glyphDC&&oldGlyphFont)SelectObject(glyphDC,oldGlyphFont);if(glyphFont)DeleteObject(glyphFont);if(glyphDC)DeleteDC(glyphDC);
 if(fontSlot){UnmapViewOfFile(fontSlot);fontSlot=nullptr;}
 if(sizedFontSlots){UnmapViewOfFile(sizedFontSlots);sizedFontSlots=nullptr;}
 // Keep the first font cache mapping handle referenced until process exit. It is
 // process-owned like its DxLib handle, independent of this unloadable DLL.
 return true;
}
