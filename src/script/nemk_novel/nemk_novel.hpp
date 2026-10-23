#pragma once
#include "NemkControl.h"
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <regex>
#include <sstream>
#include <climits>
#include <MinHook.h>
#pragma comment(lib,"advapi32.lib")

// All method/field identities come from the live IL2CPP API. No RVA or game whitelist.
namespace nemknovel {
// Unity 6 uses address-sized GC handles; older numeric handles also fit this ABI.
using H=uintptr_t;
using O=void*;using C=void*;using M=const void*;using F=void*;using T=const void*;
struct API {
    O (*domain_get)();void **(*domain_get_assemblies)(O,size_t*);O (*assembly_get_image)(O);const char*(*image_get_name)(O);
    C (*class_from_name)(O,const char*,const char*);C (*class_get_parent)(C);const char*(*class_get_name)(C);const char*(*class_get_namespace)(C);
    M (*class_get_methods)(C,void**);F (*class_get_fields)(C,void**);const char*(*method_get_name)(M);uint32_t(*method_get_param_count)(M);T(*method_get_param)(M,uint32_t);T(*method_get_return_type)(M);uint32_t(*method_get_flags)(M,uint32_t*);
    const char*(*field_get_name)(F);T(*field_get_type)(F);uint32_t(*field_get_flags)(F);void(*field_get_value)(O,F,void*);void(*field_set_value)(O,F,void*);
    O(*type_get_object)(T);C(*class_from_type)(T);T(*class_get_type)(C);char*(*type_get_name)(T);void(*free)(void*);int(*class_value_size)(C,uint32_t*);bool(*class_is_valuetype)(C);
    O(*runtime_invoke)(M,void*,void**,O*);O(*object_new)(C);C(*object_get_class)(O);void*(*object_unbox)(O);
    int(*string_length)(O);const wchar_t*(*string_chars)(O);O(*string_new_utf16)(const wchar_t*,int);O(*string_new)(const char*);
    H(*gchandle_new)(O,bool);H(*gchandle_new_weakref)(O,bool);O(*gchandle_get_target)(H);void(*gchandle_free)(H);
    O(*thread_attach)(O);void(*thread_detach)(O);O(*array_new)(C,size_t);uintptr_t(*array_length)(O);
} a{};
template<class X> bool api(HMODULE module,X &f,const char *name){f=reinterpret_cast<X>(GetProcAddress(module,name));return f!=nullptr;}
inline std::atomic<bool> installed{false},active{false},detaching{false},resourcesOwned{false},needCleanup{false};
inline thread_local bool reentry=false;
inline DWORD unityThread=0;
inline void* localizedAddress=nullptr;
inline bool presentation=false;
inline nemkcontrol::Shared *channel=nullptr;inline HANDLE channelMap=nullptr,host=nullptr;inline BYTE channelToken[16]{};
inline uint64_t generation=0,sequence=0;inline ULONGLONG requestedAt=0;inline nemkcontrol::Command pending;
inline C printerClass=nullptr,managerClass=nullptr,nameClass=nullptr,choiceClass=nullptr,viewClass=nullptr,buttonClass=nullptr;
inline M selectedLocale=nullptr,stringDatabase=nullptr;
inline std::wstring fontPath;
struct Row {std::wstring key,raw,translated;};
struct Table {std::wstring name;std::vector<Row> rows;};
struct Picture {std::wstring entry,state,sprite,original;std::vector<BYTE> png;H replacement=0;};
struct FontRecord {H target=0,original=0;};
struct DisplayRecord {H text=0,printer=0;std::wstring original,translated;};
inline std::vector<DisplayRecord> displays;
inline std::set<int> glyphs;
inline bool fontFailed=false;
inline std::vector<H> unityObjects;
struct SpriteRecord {H button=0,entry=0,original[3]{},ours[3]{};};
inline std::vector<Table> tables;inline std::vector<Picture> pictures;
inline std::vector<FontRecord> fonts;inline std::vector<SpriteRecord> spriteRecords;inline H fontRoot=0,fontSourceRoot=0;
inline std::map<std::pair<std::wstring,std::wstring>,std::wstring> originals;
inline std::map<std::wstring,std::wstring> displayTranslations;
inline std::map<std::wstring,std::wstring> displaySources;
inline std::map<H,std::pair<H,std::wstring>> printerNames;
inline std::map<H,std::wstring> localizedIdentities;
struct ImageGroup {H parent=0;std::wstring text;bool visible=false;};
inline std::vector<ImageGroup> imageGroups;
inline bool livePresentation=false;inline std::atomic<bool> liveEnabled{false};
inline uint64_t liveEpoch=0;inline LONG liveReplyStamp=0;
struct LivePage {H printer=0,text=0;uint64_t epoch=0,generation=0,sequence=0,owner=0;std::wstring raw,translated,reply;int visible=0;ULONGLONG replyAt=0;bool applied=false,answered=false;};
inline LivePage livePage;
using LocalizedGetter=bool(*)(O,O,O*,M);
inline LocalizedGetter originalLocalized=nullptr;

std::wstring sourceIdentity(O value);
O component(O obj,C typeClass);
void imageChoice(O button,TextBuffer* output);
void clearLivePage(bool restore);
void liveFrame();

std::string type(T type){if(!type)return {};char*p=a.type_get_name(type);if(!p)return {};std::string s(p);a.free(p);return s;}
std::string klassName(C c){if(!c)return {};const char*n=a.class_get_name(c),*s=a.class_get_namespace(c);return n?(s&&*s?std::string(s)+".":std::string())+n:std::string();}
C klass(const char *image,const char *space,const char *name){size_t count=0;auto assemblies=a.domain_get_assemblies(a.domain_get(),&count);C result=nullptr;for(size_t i=0;i<count;i++){auto im=a.assembly_get_image(assemblies[i]);auto n=a.image_get_name(im);if(n&&std::string(n)==image){auto c=a.class_from_name(im,space,name);if(result&&c!=result)return nullptr;result=c;}}return result;}
M method(C c,const char *name,const std::vector<std::string> &parameters,const char *result=nullptr,int isStatic=-1){for(;c;c=a.class_get_parent(c)){void*it=nullptr;M found=nullptr;while(auto m=a.class_get_methods(c,&it)){if(strcmp(a.method_get_name(m),name)||a.method_get_param_count(m)!=parameters.size())continue;bool match=true;for(size_t i=0;i<parameters.size();i++)if(!parameters[i].empty()&&type(a.method_get_param(m,unsigned(i)))!=parameters[i])match=false;if(result&&type(a.method_get_return_type(m))!=result)match=false;uint32_t flags=a.method_get_flags(m,nullptr);if(isStatic>=0&&bool(flags&0x10)!=bool(isStatic))match=false;if(match){if(found)return nullptr;found=m;}}if(found)return found;}return nullptr;}
M countMethod(C c,const char *name,unsigned count){for(;c;c=a.class_get_parent(c)){void*it=nullptr;M result=nullptr;while(auto m=a.class_get_methods(c,&it))if(!strcmp(a.method_get_name(m),name)&&a.method_get_param_count(m)==count){if(result)return nullptr;result=m;}if(result)return result;}return nullptr;}
F field(C c,const char *name,const char *expected=nullptr){for(;c;c=a.class_get_parent(c)){void*it=nullptr;F result=nullptr;while(auto f=a.class_get_fields(c,&it))if(!strcmp(a.field_get_name(f),name)&&!(a.field_get_flags(f)&0x10)&&(!expected||type(a.field_get_type(f))==expected)){if(result)return nullptr;result=f;}if(result)return result;}return nullptr;}
O get(O o,const char *name,const char *expected=nullptr){O result=nullptr;if(o)if(auto f=field(a.object_get_class(o),name,expected))a.field_get_value(o,f,&result);return result;}
void set(O o,F f,O value){if(o&&f)a.field_set_value(o,f,&value);}
O invoke(M m,O obj,std::vector<void*> args={}){if(!m)throw std::runtime_error("method unavailable");O error=nullptr;void *instance=obj;if(obj&&a.class_is_valuetype(a.object_get_class(obj)))instance=a.object_unbox(obj);auto result=a.runtime_invoke(m,instance,args.empty()?nullptr:args.data(),&error);if(error)throw std::runtime_error("IL2CPP invocation failed");return result;}
O call(O obj,const char*name,std::vector<void*>args={}){if(!obj)throw std::runtime_error("null object");return invoke(countMethod(a.object_get_class(obj),name,unsigned(args.size())),obj,args);}
std::wstring str(O value,size_t limit=16384){if(!value)return {};if(klassName(a.object_get_class(value))!="System.String")return {};int n=a.string_length(value);if(n<0||size_t(n)>limit)return {};auto chars=a.string_chars(value);if(!chars||IsBadReadPtr(chars,size_t(n)*2))return {};std::wstring s(chars,size_t(n));return s.find(L'\0')==s.npos?s:std::wstring();}
O string(const std::wstring&s){return a.string_new_utf16(s.data(),int(s.size()));}
H root(O o,bool weak=false){return !o?0:weak?a.gchandle_new_weakref(o,false):a.gchandle_new(o,false);}
O target(H h){return h?a.gchandle_get_target(h):nullptr;}
void release(H &h){if(h)a.gchandle_free(h);h=0;}
struct Hold {H h;explicit Hold(O o):h(root(o)){if(o&&!h)throw std::runtime_error("GC root");}~Hold(){release(h);}Hold(const Hold&)=delete;Hold&operator=(const Hold&)=delete;O get()const{return target(h);}};
void ownUnityObject(O object){if(!object)return;if(unityObjects.size()>=16384)throw std::runtime_error("owned Unity object budget");auto h=root(object);if(!h)throw std::runtime_error("Unity object root");unityObjects.push_back(h);needCleanup=true;}
void destroyUnityObjects(){if(unityObjects.empty())return;auto objectClass=klass("UnityEngine.CoreModule.dll","UnityEngine","Object");auto destroy=method(objectClass,"Destroy",{"UnityEngine.Object"},"System.Void",1);for(auto&h:unityObjects){try{if(target(h)&&destroy)invoke(destroy,nullptr,{target(h)});}catch(...){}release(h);}unityObjects.clear();}
std::wstring normalize(std::wstring s){
    for(size_t at=0;(at=s.find(L"\r\n",at))!=s.npos;)s.replace(at,2,L"\n");std::replace(s.begin(),s.end(),L'\r',L'\n');
    static const std::wregex token(L"<noparse>[\\s\\S]*?</noparse>|<[^<>]*>",std::regex_constants::icase);
    static const std::set<std::wstring> tags={L"b",L"i",L"u",L"s",L"strikethrough",L"size",L"color",L"alpha",L"sprite",L"mark",L"cspace",L"mspace",L"font",L"font-weight",L"align",L"indent",L"line-indent",L"line-height",L"margin",L"margin-left",L"margin-right",L"pos",L"voffset",L"width",L"rotate",L"nobr",L"uppercase",L"lowercase",L"allcaps",L"smallcaps",L"sub",L"sup",L"style",L"gradient",L"material",L"link",L"action"};
    std::wstring out;size_t at=0;
    for(std::wsregex_iterator it(s.begin(),s.end(),token),end;it!=end;++it){auto tag=it->str(),name=tag.substr(1,tag.size()-2);out.append(s,at,size_t(it->position())-at);at=size_t(it->position()+it->length());
        auto start=name.find_first_not_of(L" /\t\n"),last=name.find_last_not_of(L" /\t\n");name=start==name.npos?L"":name.substr(start,last-start+1);for(auto&c:name)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';name=name.substr(0,name.find_first_of(L"= /\t\n"));
        auto lower=tag;for(auto&c:lower)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';if(lower.rfind(L"<noparse>",0)==0&&lower.find(L"</noparse>")!=lower.npos){out+=tag;continue;}
        if(name==L"br"||name==L"page")out+=L'\n';else if(name==L"space")out+=L' ';else if(!tags.count(name)){
            bool hex=(name.size()==4||name.size()==7||name.size()==9)&&name[0]==L'#';for(size_t i=1;hex&&i<name.size();i++)hex=(name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f');if(!hex)out+=tag;
        }
    }
    out.append(s,at,s.size()-at);auto begin=out.find_first_not_of(L" \t\n\ufeff"),end=out.find_last_not_of(L" \t\n");return begin==out.npos?std::wstring():out.substr(begin,end-begin+1);
}
std::string hash(const std::vector<BYTE>&data){HCRYPTPROV provider=0;HCRYPTHASH h=0;BYTE out[32]{};DWORD size=32;bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&h)&&CryptHashData(h,data.data(),DWORD(data.size()),0)&&CryptGetHashParam(h,HP_HASHVAL,out,&size,0);if(h)CryptDestroyHash(h);if(provider)CryptReleaseContext(provider,0);if(!ok)return {};std::string result;const char*hex="0123456789abcdef";for(auto v:out){result+=hex[v>>4];result+=hex[v&15];}return result;}
std::vector<BYTE> read(const std::filesystem::path&path,size_t max){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return {};auto size=f.tellg();if(size<0||uint64_t(size)>max)return {};std::vector<BYTE>b(static_cast<size_t>(size));f.seekg(0);if(!b.empty()&&!f.read(reinterpret_cast<char*>(b.data()),b.size()))return {};return b;}
std::wstring json(const std::wstring&s){std::wstring out=L"\"";for(auto c:s){if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}else if(c==L'\n')out+=L"\\n";else if(c==L'\r')out+=L"\\r";else if(c==L'\t')out+=L"\\t";else if(c>=32)out+=c;}return out+L"\"";}
void page(TextBuffer*b,O owner,const std::wstring&text,const std::wstring&speaker,const char*kind,const std::vector<std::wstring>&segments={}){if(text.empty())return;if(strcmp(kind,"line"))clearLivePage(true);std::wstring parts=L"[";for(const auto&part:segments){if(parts.size()>1)parts+=L",";parts+=json(part);}parts+=L"]";++sequence;std::wstring packet=L"{\"sequence\":"+std::to_wstring(sequence)+L",\"owner\":"+std::to_wstring(reinterpret_cast<uintptr_t>(owner))+L",\"generation\":"+std::to_wstring(generation)+L",\"liveEpoch\":"+std::to_wstring(livePage.sequence==sequence?livePage.epoch:0)+L",\"kind\":\""+std::wstring(kind,kind+strlen(kind))+L"\",\"backfill\":"+(active.load()?L"true":L"false")+L",\"text\":"+json(text)+L",\"speaker\":"+json(speaker)+L",\"segments\":"+parts+L"}";b->from(packet);}

void closeChannel(){if(channel)UnmapViewOfFile(channel);if(channelMap)CloseHandle(channelMap);if(host)CloseHandle(host);channel=nullptr;channelMap=host=nullptr;generation=0;pending={};requestedAt=0;liveEnabled=false;liveEpoch=0;liveReplyStamp=0;}
bool openChannel(){
    if(channel)return true;
    auto time=nemkcontrol::creation(GetCurrentProcess());
    channelMap=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,nemkcontrol::name(GetCurrentProcessId(),time).c_str());
    if(!channelMap)return false;
    channel=static_cast<nemkcontrol::Shared*>(MapViewOfFile(channelMap,FILE_MAP_ALL_ACCESS,0,0,nemkcontrol::Capacity));
    if(!channel||memcmp(channel->magic,"DNMC0002",8)||channel->target!=GetCurrentProcessId()||channel->targetTime!=time){closeChannel();return false;}
    memcpy(channelToken,channel->token,16);host=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,channel->host);
    if(!host||WaitForSingleObject(host,0)!=WAIT_TIMEOUT||nemkcontrol::creation(host)!=channel->hostTime){closeChannel();return false;}
    detaching=false;return true;
}
bool hostAlive(){return channel&&host&&WaitForSingleObject(host,0)==WAIT_TIMEOUT&&nemkcontrol::creation(host)==channel->hostTime&&!memcmp(channelToken,channel->token,16);}
void ack(uint32_t status){if(!channel)return;nemkcontrol::Result r;r.generation=generation;r.status=status;memcpy(r.digest,pending.digest,64);InterlockedIncrement(&channel->resultStamp);memcpy(&channel->result,&r,sizeof(r));MemoryBarrier();InterlockedIncrement(&channel->resultStamp);}
bool command(nemkcontrol::Command&out){if(!channel)return false;LONG stamp=InterlockedCompareExchange(&channel->commandStamp,0,0);if(stamp&1)return false;memcpy(&out,&channel->command,sizeof(out));MemoryBarrier();return stamp==InterlockedCompareExchange(&channel->commandStamp,0,0)&&out.generation>generation&&out.enabled<=2&&out.path[2047]==0&&out.font[2047]==0&&out.digest[64]==0&&out.fingerprint[64]==0;}

struct Cursor {const std::vector<BYTE>&b;size_t p=0;uint32_t u32(){if(b.size()-p<4)throw std::runtime_error("pack truncated");uint32_t n;memcpy(&n,b.data()+p,4);p+=4;return n;}std::wstring text(size_t max=1<<20){uint32_t n=u32();if(n>max||size_t(n)*2>b.size()-p)throw std::runtime_error("pack string");std::wstring s(reinterpret_cast<const wchar_t*>(b.data()+p),n);p+=size_t(n)*2;if(s.find(L'\0')!=s.npos)throw std::runtime_error("pack embedded zero");return s;}std::vector<BYTE>bytes(size_t max){uint32_t n=u32();if(n>max||n>b.size()-p)throw std::runtime_error("pack bytes");std::vector<BYTE>out(b.begin()+p,b.begin()+p+n);p+=n;return out;}};
// addDisplaySource 合并资源格式证明的原文别名，冲突永久保留为空而不任选候选
void addDisplaySource(const std::wstring&raw,const std::wstring&source){
    auto key=normalize(raw),canonical=normalize(source);if(key.empty()||canonical.empty())return;
    auto found=displaySources.find(key);if(found==displaySources.end())displaySources[key]=canonical;
    else if(found->second!=canonical)found->second.clear();
}
// addDisplayTranslation 收录当前语种快照，等义样式差异展平且歧义译文不再重新启用
void addDisplayTranslation(const std::wstring&raw,const std::wstring&translated){
    addDisplaySource(raw,raw);if(raw==translated||translated.empty())return;auto key=normalize(raw);
    auto found=displayTranslations.find(key);if(found==displayTranslations.end())displayTranslations[key]=translated;
    else if(!found->second.empty()&&found->second!=translated)found->second=normalize(found->second)==normalize(translated)?normalize(translated):L"";
}
void loadPack(){
    namespace fs=std::filesystem;wchar_t executable[32768]{};if(!GetModuleFileNameW(nullptr,executable,32768))throw std::runtime_error("game path");
    auto cache=fs::weakly_canonical(fs::path(executable).parent_path()/L"dango_cache");auto path=fs::weakly_canonical(pending.path);
    if(path.parent_path()!=cache||path.extension()!=L".nmk"||fs::is_symlink(path))throw std::runtime_error("resource ownership");
    auto data=read(path,60<<20);if(data.size()<44||memcmp(data.data(),"DNMK0002",8)||hash(data)!=pending.digest)throw std::runtime_error("resource digest");
    const char*hex="0123456789abcdef";std::string fp;for(size_t i=8;i<40;i++){fp+=hex[data[i]>>4];fp+=hex[data[i]&15];}if(fp!=pending.fingerprint)throw std::runtime_error("resource fingerprint");
    Cursor cur{data,40};auto count=cur.u32();if(count>4096)throw std::runtime_error("table budget");tables.clear();pictures.clear();originals.clear();displayTranslations.clear();displaySources.clear();
    std::set<std::wstring> names;size_t rows=0;
    for(uint32_t i=0;i<count;i++){Table t;t.name=cur.text(512);if(t.name.empty()||!names.insert(t.name).second)throw std::runtime_error("table identity");auto n=cur.u32();if(n>100000||rows+n>1000000)throw std::runtime_error("row budget");rows+=n;std::set<std::wstring>keys;
        for(uint32_t j=0;j<n;j++){Row r{cur.text(2048),cur.text(16384),cur.text(16384)};if(r.key.empty()||!keys.insert(r.key).second)throw std::runtime_error("key identity");originals[{t.name,r.key}]=r.raw;addDisplayTranslation(r.raw,r.translated);t.rows.push_back(std::move(r));}tables.push_back(std::move(t));}
    count=cur.u32();if(count>1000000||rows+count>1000000)throw std::runtime_error("direct budget");
    for(uint32_t i=0;i<count;i++){auto raw=cur.text(16384),translated=cur.text(16384);addDisplayTranslation(raw,translated);}
    count=cur.u32();if(count>1000000)throw std::runtime_error("alias budget");
    for(uint32_t i=0;i<count;i++){auto raw=cur.text(16384),source=cur.text(16384);auto key=displaySources.find(normalize(source));if(key==displaySources.end())throw std::runtime_error("alias source missing");addDisplaySource(raw,source);}
    count=cur.u32();if(count>4096)throw std::runtime_error("image budget");std::set<std::pair<std::wstring,std::wstring>> imageKeys;
    for(uint32_t i=0;i<count;i++){Picture p;p.entry=cur.text(2048);p.state=cur.text(32);p.sprite=cur.text(2048);p.original=cur.text(4096);p.png=cur.bytes(4<<20);if((p.state!=L"normal"&&p.state!=L"highlighted"&&p.state!=L"flashing")||(!p.png.empty()&&(p.png.size()<24||memcmp(p.png.data(),"\x89PNG\r\n\x1a\n",8)))||!imageKeys.insert({p.entry,p.state}).second)throw std::runtime_error("image identity");pictures.push_back(std::move(p));}
    if(cur.p!=data.size())throw std::runtime_error("pack trailing bytes");
    if(pending.enabled==2)return;
    fontPath=pending.font;if(hash(read(fontPath,32<<20))!="84bbd4ace91d327b3ad1a581c688196278a4e41308520176f419180064e4af2b")throw std::runtime_error("font integrity");
}

// Build a boxed TableReference through Unity's own conversion; never assume a struct offset.
O tableReference(const std::wstring&name){auto c=klass("Unity.Localization.dll","UnityEngine.Localization.Tables","TableReference");return invoke(method(c,"op_Implicit",{"System.String"},"UnityEngine.Localization.Tables.TableReference",1),nullptr,{string(name)});}
bool boolValue(O o){return o&&klassName(a.object_get_class(o))=="System.Boolean"&&*static_cast<BYTE*>(a.object_unbox(o));}

// fontDiagnostic 按固定阶段各记录一次共用字体状态，不包含剧情文字
void fontDiagnostic(const char *stage){static std::set<std::string> logged;if(logged.insert(stage).second){auto message=std::string("NemkNovel font stage=")+stage;Msg::Log(message.c_str());}}
bool ensureFont(){if(target(fontRoot))return true;auto fontClass=klass("UnityEngine.TextRenderingModule.dll","UnityEngine","Font");auto tmpClass=klass("Unity.TextMeshPro.dll","TMPro","TMP_FontAsset");if(!fontClass||!tmpClass){fontDiagnostic("class-missing");return false;}fontDiagnostic("source-create");auto source=a.object_new(fontClass);Hold heldSource(source);invoke(method(fontClass,".ctor",{"System.String"},"System.Void",0),source,{string(fontPath)});ownUnityObject(source);fontSourceRoot=root(source);if(!fontSourceRoot)return false;int point=90,padding=9,render=4165,width=2048,height=2048,population=1;bool multi=true;
    auto create=method(tmpClass,"CreateFontAsset",{"UnityEngine.Font","System.Int32","System.Int32","UnityEngine.TextCore.LowLevel.GlyphRenderMode","System.Int32","System.Int32","TMPro.AtlasPopulationMode","System.Boolean"},"TMPro.TMP_FontAsset",1);
    if(!create){fontDiagnostic("create-method-missing");return false;}fontDiagnostic("atlas-create");auto font=invoke(create,nullptr,{source,&point,&padding,&render,&width,&height,&population,&multi});ownUnityObject(font);fontRoot=root(font);if(fontRoot)fontDiagnostic("atlas-created");return fontRoot!=0;}
bool fontForImpl(O text,const std::wstring&value){if(fontFailed||!text||!ensureFont())return false;auto font=target(fontRoot);
    // TMP动态补字重载使用System.Char，Int32单参数重载不会向动态图集添加字形
    auto has=method(a.object_get_class(font),"HasCharacter",{"System.Char","System.Boolean","System.Boolean"},"System.Boolean",0);if(!has){fontDiagnostic("glyph-method-missing");return false;}bool fallback=true,add=true;auto proposed=glyphs;for(auto c:value)if(c>=32)proposed.insert(c);if(proposed.size()>8192){fontDiagnostic("glyph-budget");return false;}auto atlases=call(font,"get_atlasTextures");if(atlases&&a.array_length(atlases)>16){fontDiagnostic("atlas-budget");return false;}for(auto c:value){if(c<32)continue;uint16_t cp=c;if(c>=0xd800&&c<=0xdfff)return false;if(!boolValue(invoke(has,font,{&cp,&fallback,&add}))){fontDiagnostic("glyph-missing");return false;}}
    glyphs=std::move(proposed);
    FontRecord*record=nullptr;for(auto&r:fonts)if(target(r.target)==text){record=&r;break;}if(!record){if(fonts.size()>=256)return false;auto original=call(text,"get_font");if(!original)return false;fonts.push_back({root(text,true),root(original)});record=&fonts.back();}else{auto current=call(text,"get_font");if(current&&current!=font&&current!=target(record->original)){release(record->original);record->original=root(current);}}call(text,"set_font",{font});fontDiagnostic("applied");return true;}
bool fontFor(O text,const std::wstring&value){try{return fontForImpl(text,value);}catch(...){fontDiagnostic("exception");fontFailed=true;return false;}}
void originalFont(O text){for(auto&r:fonts)if(target(r.target)==text&&target(r.original)){if(call(text,"get_font")==target(fontRoot))call(text,"set_font",{target(r.original)});return;}}
void restoreFonts(){for(auto&r:fonts){auto t=target(r.target),original=target(r.original);if(t&&original){try{if(call(t,"get_font")==target(fontRoot))call(t,"set_font",{original});}catch(...){}}release(r.target);release(r.original);}fonts.clear();}
void restoreSprites();
void restoreDisplays(){
    for(auto&r:displays){auto text=target(r.text);if(!text)continue;try{
        if(str(call(text,"get_text"))!=r.translated)continue;
        auto printer=target(r.printer);
        if(printer){
            auto stateField=field(a.object_get_class(printer),"state"),timerField=field(a.object_get_class(printer),"characterTimer");
            int state=0;float timer=0;if(stateField)a.field_get_value(printer,stateField,&state);if(timerField)a.field_get_value(printer,timerField,&timer);
            auto visible=call(text,"get_maxVisibleCharacters");Hold heldVisible(visible);
            invoke(method(a.object_get_class(printer),"ShowMessage",{"System.String"},"System.Void",0),printer,{string(r.original)});
            if(stateField)a.field_set_value(printer,stateField,&state);if(timerField)a.field_set_value(printer,timerField,&timer);
            if(visible)call(text,"set_maxVisibleCharacters",{a.object_unbox(visible)});
        }else call(text,"set_text",{string(r.original)});
    }catch(...){}}
    for(auto&r:displays){release(r.text);release(r.printer);}displays.clear();
}
void rememberDisplay(O text,O printer,const std::wstring&raw,const std::wstring&translated){
    for(auto it=displays.begin();it!=displays.end();){if(!target(it->text)){release(it->text);release(it->printer);it=displays.erase(it);}else ++it;}
    for(auto&r:displays)if(target(r.text)==text){release(r.printer);r.printer=root(printer,true);r.original=raw;r.translated=translated;return;}
    if(displays.size()<256)displays.push_back({root(text,true),root(printer,true),raw,translated});
}

// 逐句只接受普通显示文字，标签、占位符、代理码元和异常控制符整句回退
bool livePlain(const std::wstring &s){if(s.empty()||s.size()>4096)return false;for(auto c:s)if((c<32&&c!=L'\n'&&c!=L'\r'&&c!=L'\t')||c==L'<'||c==L'>'||c==L'{'||c==L'}'||(c>=0xd800&&c<=0xdfff))return false;return true;}
// liveSpriteSafe 仅接受当前TMP内的数字索引图标及颜色和着色属性，不加载外部资源
bool liveSpriteSafe(const std::wstring &tag,const std::wregex &color){
    static const std::wregex number(L"[0-9]{1,5}");
    std::wistringstream input(tag);std::wstring part;bool indexed=false,colored=false,tinted=false;
    if(!(input>>part))return false;
    if(part!=L"sprite"){
        if(part.rfind(L"sprite=",0)!=0)return false;auto value=part.substr(7);
        if(!std::regex_match(value,number)||std::stoul(value)>65535)return false;indexed=true;
    }
    while(input>>part){
        auto eq=part.find(L'=');if(eq==part.npos)return false;auto name=part.substr(0,eq),value=part.substr(eq+1);
        if(name==L"index"&&!indexed){if(!std::regex_match(value,number)||std::stoul(value)>65535)return false;indexed=true;}
        else if(name==L"color"&&!colored){if(!std::regex_match(value,color))return false;colored=true;}
        else if(name==L"tint"&&!tinted){if(value!=L"0"&&value!=L"1")return false;tinted=true;}
        else return false;
    }
    return indexed;
}
// liveSourceSafe 接受已知装饰样式和内嵌数字图标，原始标签保留用于身份核对与恢复
bool liveSourceSafe(const std::wstring &s,size_t limit=4096){
    if(s.empty()||s.size()>limit)return false;
    static const std::wregex color(L"#[0-9a-f]{3}(?:[0-9a-f]{3}|[0-9a-f]{5})?|black|blue|green|orange|purple|red|white|yellow");
    static const std::wregex size(L"[+-]?(?:[0-9]{1,4}(?:\\.[0-9]{1,3})?|\\.[0-9]{1,3})(?:%|em|px)?");
    std::vector<std::wstring> stack;size_t tokens=0;bool visible=false;
    for(size_t i=0;i<s.size();++i){
        auto c=s[i];
        if(c!=L'<'){
            if((c<32&&c!=L'\n'&&c!=L'\r'&&c!=L'\t')||c==L'>'||c==L'{'||c==L'}'||(c>=0xd800&&c<=0xdfff))return false;
            if(c>L' '&&c!=0x3000)visible=true;continue;
        }
        auto end=s.find(L'>',i+1);if(end==s.npos||end-i>128||++tokens>128)return false;
        auto tag=s.substr(i+1,end-i-1);i=end;
        for(auto &v:tag)if(v>=L'A'&&v<=L'Z')v+=L'a'-L'A';
        bool closing=!tag.empty()&&tag[0]==L'/';if(closing)tag.erase(0,1);
        if(!closing&&tag.rfind(L"sprite",0)==0){if(!liveSpriteSafe(tag,color))return false;continue;}
        auto eq=tag.find(L'=');auto name=tag.substr(0,eq);std::wstring value;
        if(eq!=tag.npos){
            value=tag.substr(eq+1);
            if(value.size()>=2&&(value.front()==L'\"'||value.front()==L'\'' )&&value.back()==value.front())value=value.substr(1,value.size()-2);
        }
        if(!closing&&eq==tag.npos&&!name.empty()&&name[0]==L'#'){value=name;name=L"color";eq=0;}
        if(closing){if(eq!=tag.npos||stack.empty()||stack.back()!=name)return false;stack.pop_back();continue;}
        if(tag==L"br"||tag==L"br/"||tag==L"br /")continue;
        if(name==L"color"){if(eq==tag.npos||!std::regex_match(value,color))return false;}
        else if(name==L"size"){if(eq==tag.npos||!std::regex_match(value,size))return false;}
        else if(name==L"b"||name==L"i"||name==L"u"||name==L"s"||name==L"strikethrough"){if(eq!=tag.npos)return false;}
        else return false;
        if(stack.size()>=32)return false;stack.push_back(name);
    }
    return visible&&stack.empty();
}
// liveDiagnostic 每个开关代次最多记录64项无正文诊断，避免逐帧刷日志
void liveDiagnostic(const char *reason,uint64_t seq){
    static uint64_t epoch=0;static unsigned count=0;if(epoch!=liveEpoch){epoch=liveEpoch;count=0;}if(count>=64)return;++count;
    auto message=std::string("NemkNovel live stage=")+reason+" epoch="+std::to_string(liveEpoch)+" sequence="+std::to_string(seq);Msg::Log(message.c_str());
}
// 原生确认绑定独立实时代次与正文页，写入失败不关闭已经可用的只读监听
void liveAck(uint32_t status,uint64_t seq=0){if(!channel||!liveEpoch)return;nemkcontrol::LiveResult r;r.epoch=liveEpoch;r.sequence=seq;r.status=status;InterlockedIncrement(&channel->liveResultStamp);memcpy(&channel->liveResult,&r,sizeof(r));MemoryBarrier();InterlockedIncrement(&channel->liveResultStamp);}
// 仅恢复仍由本组件持有的TMP显示，不调用ShowMessage或更改剧情打印状态
void clearLivePage(bool restore){
    auto text=target(livePage.text);
    if(restore&&text){try{if(livePage.applied&&str(call(text,"get_text"))==livePage.translated){call(text,"set_text",{string(livePage.raw)});auto visible=call(text,"get_maxVisibleCharacters");if(visible&&*static_cast<int*>(a.object_unbox(visible))==INT_MAX)call(text,"set_maxVisibleCharacters",{&livePage.visible});}originalFont(text);}catch(...){}}
    if(livePage.replyAt&&!livePage.answered)liveAck(5,livePage.sequence);
    release(livePage.printer);release(livePage.text);livePage={};
}
// ShowMessage入口只绑定原文身份，异步译文必须等原打字完成才能更换显示
void captureLivePage(O printer,O text,const std::wstring &raw){
    if(!liveEnabled||active||!livePresentation||!text)return;
    if(!liveSourceSafe(raw)){liveDiagnostic("source-format-skipped",sequence+1);return;}
    livePage.printer=root(printer,true);livePage.text=root(text,true);livePage.epoch=liveEpoch;livePage.generation=generation;livePage.sequence=sequence+1;livePage.owner=reinterpret_cast<uintptr_t>(printer);livePage.raw=raw;needCleanup=true;
    liveDiagnostic("bound",livePage.sequence);
}
// 每帧读取独立开关和译文槽，等待期间立即返回，不阻塞Unity主线程
void liveFrame(){
    if(!channel)return;
    nemkcontrol::LiveCommand c;LONG stamp=InterlockedCompareExchange(&channel->liveStamp,0,0);
    if(!(stamp&1)){memcpy(&c,&channel->live,sizeof(c));MemoryBarrier();if(stamp==InterlockedCompareExchange(&channel->liveStamp,0,0)&&c.epoch>liveEpoch&&c.enabled<=1&&c.font[2047]==0){
        clearLivePage(true);if(!active)restoreFonts();liveEpoch=c.epoch;liveEnabled=false;liveReplyStamp=0;
        if(c.enabled&&!active&&livePresentation){auto path=std::wstring(c.font);if(hash(read(path,32<<20))=="84bbd4ace91d327b3ad1a581c688196278a4e41308520176f419180064e4af2b"){fontPath=path;liveEnabled=true;needCleanup=true;}}
        liveAck(liveEnabled?1:c.enabled?3:2);
    }}
    if(!liveEnabled||active)return;
    nemkcontrol::LiveReply r;stamp=InterlockedCompareExchange(&channel->replyStamp,0,0);
    if(stamp&&!(stamp&1)&&stamp!=liveReplyStamp){memcpy(&r,&channel->reply,sizeof(r));MemoryBarrier();if(stamp==InterlockedCompareExchange(&channel->replyStamp,0,0)){
        liveReplyStamp=stamp;
        if(r.text[4096]==0&&r.epoch==liveEpoch&&r.epoch==livePage.epoch&&r.generation==generation&&r.generation==livePage.generation&&r.sequence==livePage.sequence&&r.owner==livePage.owner&&!livePage.answered&&!livePage.replyAt){livePage.reply=r.text;livePage.replyAt=GetTickCount64();}
    }}
    if(!livePage.text)return;
    auto printer=target(livePage.printer),text=target(livePage.text);
    if(!printer||!text||get(printer,"text","TMPro.TextMeshProUGUI")!=text||!boolValue(call(printer,"get_isActiveAndEnabled"))||!boolValue(call(text,"get_isActiveAndEnabled"))||str(call(text,"get_text"))!=(livePage.applied?livePage.translated:livePage.raw)){liveDiagnostic("page-changed",livePage.sequence);clearLivePage(true);return;}
    if(livePage.answered||!livePage.replyAt)return;
    if(!livePlain(livePage.reply)){liveDiagnostic("reply-format-skipped",livePage.sequence);liveAck(5,livePage.sequence);livePage.answered=true;return;}
    // 当前页逐字显示或暂停可以持续任意时长，由上方页面身份检查负责淘汰旧译文
    if(boolValue(call(printer,"get_IsPrinting"))||boolValue(call(printer,"get_IsPaused")))return;
    auto visible=call(text,"get_maxVisibleCharacters");if(!visible||klassName(a.object_get_class(visible))!="System.Int32")throw std::runtime_error("live visibility contract");
    livePage.visible=*static_cast<int*>(a.object_unbox(visible));
    if(!fontFor(text,livePage.reply)){originalFont(text);liveDiagnostic("font-skipped",livePage.sequence);liveAck(5,livePage.sequence);livePage.answered=true;return;}
    livePage.translated=livePage.reply;livePage.applied=true;livePage.answered=true;
    call(text,"set_text",{string(livePage.translated)});int all=INT_MAX;call(text,"set_maxVisibleCharacters",{&all});
    if(str(call(text,"get_text"))!=livePage.translated)throw std::runtime_error("live display verification");
    liveAck(4,livePage.sequence);
    liveDiagnostic("applied",livePage.sequence);
}
// 清屏使同文新页和读档后的旧回复失效，原始清屏继续由游戏执行
void clearMessage(hook_context*,HookParam*,TextBuffer*,uintptr_t*){if(reentry||GetCurrentThreadId()!=unityThread)return;clearLivePage(true);}
void off(){active=false;clearLivePage(true);restoreDisplays();restoreSprites();restoreFonts();resourcesOwned=false;for(auto&identity:localizedIdentities){auto h=identity.first;release(h);}localizedIdentities.clear();displayTranslations.clear();displaySources.clear();originals.clear();tables.clear();for(auto&p:pictures)release(p.replacement);pictures.clear();release(fontRoot);release(fontSourceRoot);destroyUnityObjects();glyphs.clear();fontFailed=false;for(auto&r:imageGroups)release(r.parent);imageGroups.clear();for(auto&r:printerNames){auto h=r.first;release(h);release(r.second.first);}printerNames.clear();needCleanup=false;}

void pruneImageGroups(){
    for(auto it=imageGroups.begin();it!=imageGroups.end();){auto parent=target(it->parent);if(!parent){release(it->parent);it=imageGroups.erase(it);continue;}
        try{auto go=call(parent,"get_gameObject");if(!boolValue(call(go,"get_activeInHierarchy"))){it->visible=false;it->text.clear();}}catch(...){it->visible=false;it->text.clear();}++it;}
}
void frame(){if(reentry||!unityThread||unityThread!=GetCurrentThreadId())return;struct Guard{Guard(){reentry=true;}~Guard(){reentry=false;}}guard;
    try{
        if(detaching.load()|| (channel&&!hostAlive())){try{off();ack(2);closeChannel();}catch(...){ack(3);}return;}
        if(!openChannel())return;
        pruneImageGroups();
        nemkcontrol::Command cmd;if(command(cmd)){
            // Do not advance the command generation until old resources have actually restored.
            off();generation=cmd.generation;pending=cmd;requestedAt=GetTickCount64();
            if(!cmd.enabled){ack(2);requestedAt=0;}
            else {loadPack();needCleanup=true;if(cmd.enabled==2){ack(1);requestedAt=0;}
                else if(!presentation)throw std::runtime_error("presentation contract unavailable");}
        }
        if(pending.enabled==1&&!active&&requestedAt){
            // 完整资源已核对并载入，零映射同样可以启用，字形和单句回退留在实际显示入口
            active=true;resourcesOwned=true;requestedAt=0;ack(1);
            Msg::Log("NemkNovel backfill stage=loaded");
        }
    }catch(...){try{off();}catch(...){}ack(3);requestedAt=0;}
    if(channel&&!detaching){try{liveFrame();}catch(...){liveDiagnostic("display-exception",livePage.sequence);clearLivePage(true);liveAck(5,sequence);}}
}
void frameHook(hook_context*,HookParam*,TextBuffer*,uintptr_t*){try{if(!unityThread)unityThread=GetCurrentThreadId();frame();}catch(...){}}

// Nameplate association uses the actual NovelManager's printer field; independent printers keep no speaker.
void bindManagerImpl(hook_context*ctx,HookParam*hp,TextBuffer*,uintptr_t*){if(reentry||(unityThread&&unityThread!=GetCurrentThreadId()))return;clearLivePage(true);auto methodInfo=reinterpret_cast<M>(hp->user_value);auto ret=a.class_from_type(a.method_get_return_type(methodInfo));uint32_t alignment=0;bool hidden=ret&&a.class_is_valuetype(ret)&&a.class_value_size(ret,&alignment)>8;auto manager=reinterpret_cast<O>(ctx->argof(hidden?2:1));auto printer=get(manager,"printer","Novel.TextPrinter"),name=get(manager,"nameplate","Novel.Nameplate");if(!printer||!name)return;auto text=get(name,"text");std::wstring speaker;try{if(text)speaker=str(call(text,"get_text"));}catch(...){}for(auto it=printerNames.begin();it!=printerNames.end();){if(!target(it->first)){auto key=it->first;auto name=it->second.first;release(key);release(name);it=printerNames.erase(it);}else ++it;}for(auto&r:printerNames)if(target(r.first)==printer){release(r.second.first);r.second={root(name,true),speaker};return;}if(printerNames.size()<128){printerNames[root(printer,true)]={root(name,true),speaker};needCleanup=true;}}
void bindManager(hook_context*ctx,HookParam*hp,TextBuffer*b,uintptr_t*v){try{bindManagerImpl(ctx,hp,b,v);}catch(...){}}
O component(O obj,C typeClass){if(!obj||!typeClass)return nullptr;auto reflected=a.type_get_object(a.class_get_type(typeClass));Hold held(reflected);return invoke(method(a.object_get_class(obj),"GetComponent",{"System.Type"}),obj,{reflected});}
void imageChoice(O button,TextBuffer*output){
    if(pictures.empty()||!boolValue(call(button,"get_isActiveAndEnabled")))return;
    auto transform=call(button,"get_transform"),parent=transform?call(transform,"get_parent"):nullptr;if(!parent)return;
    auto countObject=call(parent,"get_childCount");if(!countObject)return;int count=*static_cast<int*>(a.object_unbox(countObject));if(count<1||count>64)return;
    std::wstring text;std::vector<std::wstring> segments;int visible=0;
    for(int index=0;index<count;index++){
        auto child=call(parent,"GetChild",{&index});auto item=component(child,buttonClass);if(!item||!boolValue(call(item,"get_isActiveAndEnabled")))continue;
        visible++;auto entry=get(item,"entry","ButtonEntry");if(!entry)return;auto name=str(call(entry,"get_name"));
        auto found=std::find_if(pictures.begin(),pictures.end(),[&](const Picture&p){return p.entry==name&&p.state==L"normal";});if(found==pictures.end())return;
        if(!text.empty())text+=L"\n";text+=found->original;segments.push_back(found->original);
    }
    if(!visible||text.empty())return;
    ImageGroup*group=nullptr;for(auto&r:imageGroups)if(target(r.parent)==parent){group=&r;break;}
    if(!group){if(imageGroups.size()>=64)return;imageGroups.push_back({root(parent,true),L"",false});group=&imageGroups.back();needCleanup=true;}
    if(group->visible&&group->text==text)return;group->visible=true;group->text=text;page(output,parent,text,L"","options",segments);
}
std::wstring localizedComponentOriginal(O text){
    if(!text||!active)return {};
    auto event=component(text,klass("Unity.Localization.dll","UnityEngine.Localization.Components","LocalizeStringEvent"));if(!event)return {};
    auto reference=call(event,"get_StringReference");if(!reference)return {};
    auto tr=call(reference,"get_TableReference");Hold heldTr(tr);auto er=call(reference,"get_TableEntryReference");Hold heldEr(er);
    auto database=invoke(stringDatabase,nullptr);Hold heldDatabase(database);
    auto table=invoke(method(a.object_get_class(database),"GetTable",{"UnityEngine.Localization.Tables.TableReference","UnityEngine.Localization.Locale"}),database,{a.object_unbox(tr),invoke(selectedLocale,nullptr)});if(!table)return {};
    Hold heldTable(table);auto entry=invoke(method(a.object_get_class(table),"GetEntryFromReference",{"UnityEngine.Localization.Tables.TableEntryReference"}),table,{a.object_unbox(er)});if(!entry)return {};
    Hold heldEntry(entry);auto name=str(call(table,"get_TableCollectionName")),key=str(call(entry,"get_Key"));auto found=originals.find({name,key});return found==originals.end()?std::wstring():found->second;
}
// displaySource 识别本组件显示副本并返回原资源键对应原文，不反向猜测重复译文
std::wstring displaySource(O text){
    if(!text)return {};auto input=call(text,"get_text");if(!active)return str(input);
    for(const auto&r:displays)if(target(r.text)==text&&str(input)==r.translated){auto found=displaySources.find(normalize(r.original));return found!=displaySources.end()?found->second:r.original;}
    auto raw=sourceIdentity(input);if(raw.empty()&&selectedLocale&&stringDatabase)try{raw=localizedComponentOriginal(text);}catch(...){}return raw;
}
// applyText 用资源快照翻译显示参数，未命中或缺字时严格保留当前游戏语种的输入
void applyText(O text,O printer,const std::wstring&raw,std::wstring&translated,const std::wstring&current=L""){
    auto fallback=current.empty()?raw:current;
    auto found=displayTranslations.find(normalize(raw));translated=found!=displayTranslations.end()&&!found->second.empty()?found->second:fallback;
    if(translated!=fallback&&(!liveSourceSafe(fallback,16384)||!liveSourceSafe(translated,16384)))translated=fallback;
    if(translated!=fallback&&!fontFor(text,translated))translated=fallback;
    if(translated==fallback)originalFont(text);
    if(!printer&&text){call(text,"set_text",{string(translated)});rememberDisplay(text,nullptr,fallback,translated);}
}
// resourceDiagnostic 每个资源代次最多记录64条固定阶段，不输出原文和译文
void resourceDiagnostic(const char*stage){static uint64_t seen=0;static unsigned count=0;if(seen!=generation){seen=generation;count=0;}if(count++>=64)return;auto msg=std::string("NemkNovel backfill stage=")+stage;Msg::Log(msg.c_str());}
// labelDisplay 仅替换姓名和文字选项的显示参数，选项索引和调用者原串保持不变
void labelDisplay(hook_context*ctx,HookParam*hp,TextBuffer*,uintptr_t*){
    if(reentry||GetCurrentThreadId()!=unityThread||!active)return;
    try{auto owner=reinterpret_cast<O>(ctx->argof(1,hp));auto input=reinterpret_cast<O>(ctx->argof(2,hp));auto raw=str(input),canonical=sourceIdentity(input);auto text=get(owner,"text","TMPro.TextMeshProUGUI");if(raw.empty()||canonical.empty()||!text)return;
        std::wstring translated;applyText(text,owner,canonical,translated,raw);if(translated==raw)return;
        ctx->argof(2,hp)=reinterpret_cast<uintptr_t>(string(translated));rememberDisplay(text,nullptr,raw,translated);resourceDiagnostic("label-applied");
    }catch(...){resourceDiagnostic("label-skipped");}
}
void message(hook_context*ctx,HookParam*hp,TextBuffer*b,uintptr_t*){
    if(reentry||(unityThread&&unityThread!=GetCurrentThreadId()))return;
    try{
        clearLivePage(true);frame();auto printer=reinterpret_cast<O>(ctx->argof(1,hp)),input=reinterpret_cast<O>(ctx->argof(2,hp));auto text=str(input);if(text.empty())return;
        std::wstring speaker;
        for(auto&r:printerNames)if(target(r.first)==printer){auto plate=target(r.second.first),nameText=get(plate,"text");
            if(nameText&&boolValue(call(plate,"get_isActiveAndEnabled"))){speaker=displaySource(nameText);if(active&&!speaker.empty()){std::wstring translated;applyText(nameText,nullptr,speaker,translated,str(call(nameText,"get_text")));}}break;}
        std::wstring original=active?sourceIdentity(input):text;bool known=!original.empty();
        if(!known)original=text;
        if(active&&known){auto tmp=get(printer,"text");std::wstring translated;applyText(tmp,printer,original,translated,text);if(translated!=text){ctx->argof(2,hp)=reinterpret_cast<uintptr_t>(string(translated));rememberDisplay(tmp,printer,text,translated);resourceDiagnostic("body-applied");}}
        else if(!active&&liveEnabled)captureLivePage(printer,get(printer,"text","TMPro.TextMeshProUGUI"),text);
        page(b,printer,original,speaker,"line");
    }catch(...){/* A failed display path leaves its original call arguments intact. */}
}

// The exact localized call binds its returned string object to a table/key, across await boundaries.
// Repeated Chinese values are never reverse-matched to Japanese strings.
bool localized(O table,O key,O*out,M info){bool result=originalLocalized(table,key,out,info);if(!result||!active||reentry||!out||!*out||GetCurrentThreadId()!=unityThread)return result;try{auto found=originals.find({str(table,512),str(key,2048)});if(found==originals.end())return result;for(auto it=localizedIdentities.begin();it!=localizedIdentities.end();){if(!target(it->first)){auto h=it->first;release(h);it=localizedIdentities.erase(it);}else ++it;}for(auto&identity:localizedIdentities)if(target(identity.first)==*out){if(identity.second!=found->second)identity.second.clear();return result;}if(localizedIdentities.size()<32768)localizedIdentities[root(*out,true)]=found->second;}catch(...){}return result;}
std::wstring sourceIdentity(O value){for(auto&identity:localizedIdentities)if(target(identity.first)==value)return identity.second;auto found=displaySources.find(normalize(str(value)));return found==displaySources.end()?std::wstring():found->second;}

O makeSprite(Picture&p,O original){if(p.png.empty())return nullptr;if(target(p.replacement))return target(p.replacement);auto textureClass=klass("UnityEngine.CoreModule.dll","UnityEngine","Texture2D"),spriteClass=klass("UnityEngine.CoreModule.dll","UnityEngine","Sprite"),byteClass=klass("mscorlib.dll","System","Byte");if(!textureClass||!spriteClass||!byteClass)return nullptr;auto texture=a.object_new(textureClass);int w=2,h=2;invoke(method(textureClass,".ctor",{"System.Int32","System.Int32"},"System.Void",0),texture,{&w,&h});ownUnityObject(texture);auto textureHandle=root(texture);if(!textureHandle)return nullptr;
    try{auto bytes=a.array_new(byteClass,p.png.size());if(!bytes)throw std::runtime_error("PNG bytes");auto byteHandle=root(bytes);if(!byteHandle)throw std::runtime_error("PNG root");
        // IL2CPP array header is two object pointers, bounds pointer and max_length on x64.
        if(a.array_length(bytes)!=p.png.size()){release(byteHandle);throw std::runtime_error("array layout");}memcpy(static_cast<BYTE*>(bytes)+32,p.png.data(),p.png.size());
        auto conversion=klass("UnityEngine.ImageConversionModule.dll","UnityEngine","ImageConversion");bool mark=false;auto loaded=invoke(method(conversion,"LoadImage",{"UnityEngine.Texture2D","System.Byte[]","System.Boolean"},"System.Boolean",1),nullptr,{texture,bytes,&mark});release(byteHandle);if(!boolValue(loaded))throw std::runtime_error("PNG decode");
        auto rect=call(original,"get_rect");Hold heldRect(rect);auto pivot=call(original,"get_pivot");Hold heldPivot(pivot);auto border=call(original,"get_border");Hold heldBorder(border);auto ppu=call(original,"get_pixelsPerUnit");Hold heldPpu(ppu);if(!rect||!pivot||!border||!ppu)throw std::runtime_error("sprite geometry");float*rectData=static_cast<float*>(a.object_unbox(rect)),*pivotData=static_cast<float*>(a.object_unbox(pivot));if(rectData[2]<=0||rectData[3]<=0)throw std::runtime_error("sprite rect");
        // PNG has been reconstructed to the complete logical Sprite rect. Preserve physical size, pivot, border and PPU.
        rectData[0]=rectData[1]=0;pivotData[0]/=rectData[2];pivotData[1]/=rectData[3];unsigned extrude=0;int mesh=1;bool physics=false;
        auto sprite=invoke(method(spriteClass,"Create",{"UnityEngine.Texture2D","UnityEngine.Rect","UnityEngine.Vector2","System.Single","System.UInt32","UnityEngine.SpriteMeshType","UnityEngine.Vector4","System.Boolean"},"UnityEngine.Sprite",1),nullptr,{texture,a.object_unbox(rect),a.object_unbox(pivot),a.object_unbox(ppu),&extrude,&mesh,a.object_unbox(border),&physics});ownUnityObject(sprite);p.replacement=root(sprite);release(textureHandle);return target(p.replacement);
    }catch(...){release(textureHandle);return nullptr;}}

void restoreSprites(){for(auto&r:spriteRecords){auto button=target(r.button);if(button){const char*names[]={"cachedNormal","cachedHighlighted","cachedFlashing"};for(size_t i=0;i<3;i++)if(target(r.ours[i])&&get(button,names[i])==target(r.ours[i]))set(button,field(a.object_get_class(button),names[i]),target(r.original[i]));auto image=get(button,"image");if(image)try{auto current=call(image,"get_sprite");for(size_t i=0;i<3;i++)if(current==target(r.ours[i])&&target(r.original[i]))call(image,"set_sprite",{target(r.original[i])});}catch(...){}}release(r.button);release(r.entry);for(auto&h:r.original)release(h);for(auto&h:r.ours)release(h);}spriteRecords.clear();}
void buttonFrame(hook_context*ctx,HookParam*hp,TextBuffer*b,uintptr_t*){if(reentry||GetCurrentThreadId()!=unityThread)return;auto button=reinterpret_cast<O>(ctx->argof(1,hp));try{imageChoice(button,b);}catch(...){}if(!active)return;auto entry=get(button,"entry","ButtonEntry");if(!entry)return;try{auto name=str(call(entry,"get_name"),2048);const char*fields[]={"cachedNormal","cachedHighlighted","cachedFlashing"};const wchar_t*states[]={L"normal",L"highlighted",L"flashing"};SpriteRecord*record=nullptr;for(auto&r:spriteRecords)if(target(r.button)==button){record=&r;break;}if(record&&target(record->entry)!=entry){
        // Pooled SelectButtons are rebound; restore only values we still own before adopting a new entry.
        for(size_t i=0;i<3;i++){if(get(button,fields[i])==target(record->ours[i])&&target(record->ours[i]))set(button,field(a.object_get_class(button),fields[i]),target(record->original[i]));release(record->original[i]);release(record->ours[i]);}
        release(record->entry);record->entry=root(entry,true);
    }if(!record){if(spriteRecords.size()>=256)return;SpriteRecord r;r.button=root(button,true);r.entry=root(entry,true);spriteRecords.push_back(r);record=&spriteRecords.back();}
    for(size_t i=0;i<3;i++){auto current=get(button,fields[i]);if(!current)continue;auto found=std::find_if(pictures.begin(),pictures.end(),[&](const Picture&p){return p.entry==name&&p.state==states[i];});if(found==pictures.end())continue;if(target(record->ours[i])==current)continue;if(str(call(current,"get_name"),2048)!=found->sprite)continue;auto replacement=makeSprite(*found,current);if(!replacement)continue;release(record->original[i]);release(record->ours[i]);record->original[i]=root(current);record->ours[i]=root(replacement);set(button,field(a.object_get_class(button),fields[i]),replacement);auto image=get(button,"image");if(image&&call(image,"get_sprite")==current)call(image,"set_sprite",{replacement});}
    }catch(...){}
}
void choices(hook_context*ctx,HookParam*hp,TextBuffer*b,uintptr_t*){if(reentry||(unityThread&&unityThread!=GetCurrentThreadId()))return;frame();auto m=reinterpret_cast<M>(hp->user_value);auto result=a.class_from_type(a.method_get_return_type(m));uint32_t align=0;bool hidden=result&&a.class_is_valuetype(result)&&a.class_value_size(result,&align)>8;auto manager=reinterpret_cast<O>(ctx->argof(hidden?2:1));auto list=get(manager,"instances");if(!list)return;try{auto count=call(list,"get_Count");if(!count)return;int n=*static_cast<int*>(a.object_unbox(count));if(n<1||n>64)return;std::wstring text;std::vector<std::wstring>segments;for(int i=0;i<n;i++){auto view=call(list,"get_Item",{&i});auto label=get(view,"text");std::wstring value;if(label){value=displaySource(label);if(active&&!value.empty()){std::wstring translated;applyText(label,nullptr,value,translated,str(call(label,"get_text")));}}
        // Image ChoiceView has a SelectButton component on the same GameObject.
        if(value.empty()&&buttonClass){auto gameObject=call(view,"get_gameObject");auto button=component(gameObject,buttonClass);auto entry=get(button,"entry","ButtonEntry");if(entry){auto entryName=str(call(entry,"get_name"));for(const auto&p:pictures)if(p.entry==entryName&&p.state==L"normal"){value=p.original;break;}}}
        if(value.empty())continue;if(!text.empty())text+=L"\n";text+=value;segments.push_back(value);}page(b,manager,text,L"","options",segments);}catch(...){}
}

bool hook(M m,const char*name,decltype(HookParam::text_fun) fn, bool output=false){if(!m)return false;auto address=*reinterpret_cast<const uintptr_t*>(m);MEMORY_BASIC_INFORMATION mem{};auto module=GetModuleHandleW(L"GameAssembly.dll");if(!VirtualQuery(reinterpret_cast<void*>(address),&mem,sizeof(mem))||mem.AllocationBase!=module||!(mem.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;HookParam h;h.address=address;h.type=NO_CONTEXT|(output?USING_STRING|CODEC_UTF16:0);h.jittype=JITTYPE::UNITY;h.text_fun=fn;h.user_value=reinterpret_cast<uintptr_t>(m);return NewHook(h,name);}
bool install(){if(installed)return true;auto module=GetModuleHandleW(L"GameAssembly.dll");if(!module)return false;
#define NM_API(name) if(!api(module,a.name,"il2cpp_" #name))return false
    NM_API(domain_get);NM_API(domain_get_assemblies);NM_API(assembly_get_image);NM_API(image_get_name);NM_API(class_from_name);NM_API(class_get_parent);NM_API(class_get_name);NM_API(class_get_namespace);NM_API(class_get_methods);NM_API(class_get_fields);NM_API(method_get_name);NM_API(method_get_param_count);NM_API(method_get_param);NM_API(method_get_return_type);NM_API(method_get_flags);NM_API(field_get_name);NM_API(field_get_type);NM_API(field_get_flags);NM_API(field_get_value);NM_API(field_set_value);NM_API(type_get_object);NM_API(class_from_type);NM_API(class_get_type);NM_API(type_get_name);NM_API(free);NM_API(class_value_size);NM_API(class_is_valuetype);NM_API(runtime_invoke);NM_API(object_new);NM_API(object_get_class);NM_API(object_unbox);NM_API(string_length);NM_API(string_chars);NM_API(string_new_utf16);NM_API(string_new);NM_API(gchandle_new);NM_API(gchandle_new_weakref);NM_API(gchandle_get_target);NM_API(gchandle_free);NM_API(thread_attach);NM_API(thread_detach);NM_API(array_new);NM_API(array_length);
#undef NM_API
    auto domain=a.domain_get();if(!domain)return false;auto thread=a.thread_attach(domain);if(!thread)return false;struct Thread{O p;~Thread(){a.thread_detach(p);}}attached{thread};
    printerClass=klass("Novel.dll","Novel","TextPrinter");managerClass=klass("Novel.dll","Novel","NovelManager");nameClass=klass("Novel.dll","Novel","Nameplate");choiceClass=klass("Novel.dll","Novel","ChoiceManager");viewClass=klass("Novel.dll","Novel","ChoiceView");buttonClass=klass("Assembly-CSharp.dll","","SelectButton");
    auto show=method(printerClass,"ShowMessage",{"System.String"},"System.Void",0);if(!show||!field(printerClass,"text","TMPro.TextMeshProUGUI"))return false;
    if(!hook(show,"NemkNovelDialogue",message,true))return false;
    hook(method(managerClass,"Say",{"System.String","System.String","System.Threading.CancellationToken"}),"NemkNovelManager",bindManager);
    hook(method(managerClass,"SayAuto",{"System.String","System.String","System.Threading.CancellationToken"}),"NemkNovelAutoManager",bindManager);
    hook(method(choiceClass,"Show",{"System.Threading.CancellationToken"}),"NemkNovelDialogue",choices,true);
    auto localization=klass("Unity.Localization.dll","UnityEngine.Localization.Settings","LocalizationSettings");
    selectedLocale=method(localization,"get_SelectedLocale",{},"UnityEngine.Localization.Locale",1);stringDatabase=method(localization,"get_StringDatabase",{},"UnityEngine.Localization.Settings.LocalizedStringDatabase",1);
    bool namesReady=hook(method(nameClass,"SetName",{"System.String"},"System.Void",0),"NemkNovelNameDisplay",labelDisplay);
    bool choicesReady=hook(method(viewClass,"Setup",{"System.String","System.Int32"},"System.Void",0),"NemkNovelChoiceDisplay",labelDisplay);
    Msg::Log(namesReady?"NemkNovel name display ready":"NemkNovel name display unavailable");Msg::Log(choicesReady?"NemkNovel choice display ready":"NemkNovel choice display unavailable");
    auto canvas=klass("UnityEngine.UIModule.dll","UnityEngine","Canvas");bool frameReady=hook(method(canvas,"SendWillRenderCanvases",{},"System.Void",1),"NemkNovelFrame",frameHook);
    auto tmpClass=klass("Unity.TextMeshPro.dll","TMPro","TMP_Text");
    livePresentation=frameReady&&method(printerClass,"get_IsPrinting",{},"System.Boolean",0)&&method(printerClass,"get_IsPaused",{},"System.Boolean",0)&&field(printerClass,"parsedText","System.String")&&field(printerClass,"parsedTextLength","System.Int32")&&method(tmpClass,"set_text",{"System.String"},"System.Void",0)&&method(tmpClass,"get_maxVisibleCharacters",{},"System.Int32",0)&&method(tmpClass,"set_maxVisibleCharacters",{"System.Int32"},"System.Void",0)&&hook(method(printerClass,"ClearMessage",{},"System.Void",0),"NemkNovelClear",clearMessage);
    bool resource=frameReady&&method(tmpClass,"set_text",{"System.String"},"System.Void",0)&&method(tmpClass,"get_text",{},"System.String",0);
    auto utility=klass("Assembly-CSharp.dll","","LocalizationUtility");auto getter=method(utility,"TryGetLocalizedString",{"System.String","System.String","System.String&"},"System.Boolean",1);
    if(getter){auto address=*reinterpret_cast<void*const*>(getter);if(MH_CreateHook(address,reinterpret_cast<void*>(localized),reinterpret_cast<void**>(&originalLocalized))==MH_OK){localizedAddress=address;if(MH_EnableHook(address)!=MH_OK){MH_RemoveHook(address);localizedAddress=nullptr;}}}
    if(buttonClass)hook(method(buttonClass,"LateUpdate",{},"System.Void",0),"NemkNovelDialogue",buttonFrame,true);
    presentation=resource;installed=true;Msg::Log(livePresentation?"NemkNovel live ready":"NemkNovel live unavailable");Msg::Log(resource?"NemkNovel presentation ready":"NemkNovel presentation unavailable");Msg::Log("NemkNovel read-only ready");return true;
}

// If Unity is paused, retain the DLL until its main thread has restored owned resources.
bool prepareDetach(){detaching=true;if(!active&&!resourcesOwned&&!needCleanup&&!liveEnabled){if(localizedAddress){MH_DisableHook(localizedAddress);MH_RemoveHook(localizedAddress);localizedAddress=nullptr;}return true;}auto end=GetTickCount64()+2500;while(GetTickCount64()<end){if(!active&&!resourcesOwned&&!needCleanup&&!liveEnabled){if(localizedAddress){MH_DisableHook(localizedAddress);MH_RemoveHook(localizedAddress);localizedAddress=nullptr;}return true;}Sleep(10);}return false;}
}
bool NemkPrepareDetach(){return nemknovel::prepareDetach();}
