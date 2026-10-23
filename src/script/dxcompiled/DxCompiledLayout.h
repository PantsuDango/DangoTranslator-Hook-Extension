#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include "DxCompiledSnapshot.h"

namespace dxlayout {
inline int failure=0;
inline bool reject(int line){failure=line;return false;}
enum Field {Body,Accepted,Step,Render,Draw,DrawDefault,DrawWide,FontCreate,Buffer,Capacity,Cursor,Active,Wait,Opening,Rows,Highlight,Row,Column,Font,Choice,YesNo,ChoiceBuffer,ChoiceCount,RenderEnd,ChoiceEnd,YesNoEnd,FontSize,FontThickness,FontType};
struct Subtitle {uint32_t site,destination,capacity,owner,end;};
struct Literal {uint32_t site,text,owner,end,group,capacity,role;std::wstring original;std::string raw;};
struct ItemText {uint32_t text,capacity;std::string raw;std::wstring original;uint32_t storage=0,group=0;std::wstring key;std::vector<size_t> widths;size_t line=0;};
struct ItemCall {uint32_t site,owner,end,domain;bool byValue=false;};
struct Layout {bool display=true;std::array<uint32_t,32> fields{};std::vector<Subtitle> subtitles;std::vector<Literal> literals;std::vector<ItemText> itemTexts;std::vector<std::vector<uint32_t>> itemDomains;std::vector<ItemCall> itemCalls;};
constexpr size_t mappingSize=2*1024*1024;
inline uint64_t created(){FILETIME c{},e{},k{},u{};return GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)?uint64_t(c.dwHighDateTime)<<32|c.dwLowDateTime:0;}
inline bool readable(uintptr_t p,size_t n){
 if(!p||!n||p>UINTPTR_MAX-n)return false;auto end=p+n;
 while(p<end){MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery((void*)p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;auto protection=m.Protect&0xff;if(protection!=PAGE_READONLY&&protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&protection!=PAGE_EXECUTE_READ&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;auto next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=p)return false;p=next<end?next:end;}return true;
}
// load revalidates every discovered entry against the actual image before any
// hook is installed. Base relocations are explicitly applied to code guards.
inline bool load(Layout &out){
 auto name=L"Local\\Dango.DxCompiled.Layout."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(created());
 HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());if(!map)return reject(__LINE__);
 auto view=(const BYTE*)MapViewOfFile(map,FILE_MAP_READ,0,0,mappingSize);if(!view){CloseHandle(map);return reject(__LINE__);}
 std::vector<BYTE> raw(mappingSize);memcpy(raw.data(),view,raw.size());UnmapViewOfFile(view);CloseHandle(map);
 size_t at=8;auto word=[&](uint32_t &v){if(at+4>raw.size())return reject(__LINE__);memcpy(&v,raw.data()+at,4);at+=4;return true;};
 uint32_t version=0,size=0,base=0,count=0;
 bool ok=!memcmp(raw.data(),"DGDXLY06",8)&&word(version)&&version==6&&word(size)&&word(base)&&word(count)&&count==32;
 auto module=(uintptr_t)GetModuleHandleW(nullptr);auto dos=(const IMAGE_DOS_HEADER*)module;
 if(!readable(module,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000)return reject(__LINE__);
 auto nt=(const IMAGE_NT_HEADERS32*)(module+dos->e_lfanew);
 if(!readable((uintptr_t)nt,sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.SizeOfImage!=size||(nt->OptionalHeader.ImageBase!=base&&nt->OptionalHeader.ImageBase!=uint32_t(module)))return reject(__LINE__);
 Layout next;
 for(auto &v:next.fields)ok=ok&&word(v);
 if(!ok||!word(count)||count>32)return reject(__LINE__);
 for(uint32_t n=0;n<count;n++){Subtitle s{};if(!word(s.site)||!word(s.destination)||!word(s.capacity)||!word(s.owner)||!word(s.end)||s.capacity<32||s.capacity>512)return reject(__LINE__);next.subtitles.push_back(s);}
 if(!word(count)||count>4096)return reject(__LINE__);
 for(uint32_t n=0;n<count;n++){Literal s{};uint32_t length=0;if(!word(s.site)||!word(s.text)||!word(s.owner)||!word(s.end)||!word(s.group)||!word(s.capacity)||!word(s.role)||s.capacity<2||s.capacity>512||s.role>2||!word(length)||!length||length>512||at+length*2>raw.size())return reject(__LINE__);s.original.resize(length);memcpy(s.original.data(),raw.data()+at,length*2);at+=length*2;if(!dxcompiledsnapshot::safe(s.original,L"中文"))return reject(__LINE__);next.literals.push_back(std::move(s));}
 if(!word(count)||count>32768)return reject(__LINE__);
 for(uint32_t n=0;n<count;n++){ItemText s{};if(!word(s.text)||!word(s.capacity)||!word(s.storage)||!word(s.group)||s.storage>1||(s.group&&!s.storage)||s.capacity<1||s.capacity>512)return reject(__LINE__);next.itemTexts.push_back(std::move(s));}
 if(!word(count)||count>4096)return reject(__LINE__);
 for(uint32_t n=0;n<count;n++){uint32_t length=0;if(!word(length)||!length||length>4096)return reject(__LINE__);std::vector<uint32_t> values;for(uint32_t k=0;k<length;k++){uint32_t index=0;if(!word(index)||index>=next.itemTexts.size())return reject(__LINE__);values.push_back(index);}next.itemDomains.push_back(std::move(values));}
 if(!word(count)||count>4096)return reject(__LINE__);
 for(uint32_t n=0;n<count;n++){ItemCall s{};uint32_t flags=0;if(!word(s.site)||!word(s.owner)||!word(s.end)||!word(s.domain)||!word(flags)||flags>1||s.domain>=next.itemDomains.size())return reject(__LINE__);s.byValue=flags==1;next.itemCalls.push_back(s);}
 if(!word(count)||count<2||count>256)return reject(__LINE__);bool subtitleValid=true,literalValid=true,itemValid=true;
 for(uint32_t n=0;n<count;n++){uint32_t rva=0,length=0,group=0,fixups=0;if(!word(rva)||!word(length)||!word(group)||!word(fixups)||group>4||!length||length>(group>=3?65536u:16384u)||fixups>4096||at+length>raw.size()||rva>size||length>size-rva)return reject(__LINE__);std::vector<BYTE> guard(raw.begin()+at,raw.begin()+at+length);at+=length;for(unsigned k=0;k<fixups;k++){uint32_t off=0;if(!word(off)||off>length||4>length-off)return reject(__LINE__);uint32_t v;memcpy(&v,guard.data()+off,4);v+=uint32_t(module)-base;memcpy(guard.data()+off,&v,4);}if(!readable(module+rva,length)||memcmp((void*)(module+rva),guard.data(),length)){if(group==0)return reject(__LINE__);if(group==1)next.display=false;if(group==2)subtitleValid=false;if(group==3)literalValid=false;if(group==4)itemValid=false;}}
 if(!subtitleValid)next.subtitles.clear();
 if(!literalValid)next.literals.clear();
 if(!itemValid)next.itemCalls.clear();
 // Bind the discovery to the file actually backing this process. SHA is an
 // integrity check, never a game compatibility whitelist.
 if(at+32>raw.size())return reject(__LINE__);wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return reject(__LINE__);auto file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return reject(__LINE__);LARGE_INTEGER length{};bool digestOK=GetFileSizeEx(file,&length)&&length.QuadPart>=1024&&length.QuadPart<=64*1024*1024;std::vector<BYTE> source;if(digestOK){source.resize(size_t(length.QuadPart));DWORD read=0;digestOK=ReadFile(file,source.data(),DWORD(source.size()),&read,nullptr)&&read==source.size();}CloseHandle(file);std::array<BYTE,32> actual{};if(!digestOK||!dxcompiledsnapshot::digest(source.data(),DWORD(source.size()),actual)||memcmp(actual.data(),raw.data()+at,32))return reject(__LINE__);
 auto section=IMAGE_FIRST_SECTION(nt);if(nt->FileHeader.NumberOfSections>96||!readable((uintptr_t)section,sizeof(*section)*nt->FileHeader.NumberOfSections))return reject(__LINE__);
 auto range=[&](uint32_t rva,size_t n,bool executable){for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){auto &s=section[j];if(((s.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0)!=executable)continue;if(!executable&&!(s.Characteristics&IMAGE_SCN_MEM_WRITE))continue;if(rva>=s.VirtualAddress&&rva-s.VirtualAddress<s.Misc.VirtualSize&&n<=s.Misc.VirtualSize-(rva-s.VirtualAddress))return readable(module+rva,n);}return reject(__LINE__);};
 for(unsigned n=0;n<32;n++){auto v=next.fields[n];if(!v||n==Capacity||(n>=FontSize&&n<=FontType))continue;bool code=n<8||n==Choice||n==YesNo||(n>=RenderEnd&&n<=YesNoEnd);if(!range(v,code?5:4,code)){if(n==Body||n==Accepted||n==Step||n==Buffer||n==Cursor||n==Active||n==Wait||n==Opening)return reject(__LINE__);next.display=false;}}
 if(next.fields[Capacity]<512||next.fields[Capacity]>65535||!range(next.fields[Buffer],next.fields[Capacity],false))return reject(__LINE__);
 if(!range(next.fields[Rows],980,false)||!range(next.fields[Highlight],980,false))next.display=false;
 for(auto &s:next.subtitles)if(!range(s.site,5,true)||!range(s.owner,5,true)||!range(s.end,5,true)||!range(s.destination,s.capacity,false)){next.subtitles.clear();break;}
 for(auto &s:next.literals)if(!range(s.site-5,5,true)||!range(s.owner,5,true)||!range(s.end,5,true)||s.text>size||s.capacity>size-s.text||!readable(module+s.text,s.capacity)){next.literals.clear();break;}
 for(auto &s:next.literals){bool matched=false;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){auto &part=section[j];if(part.Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE))continue;if(s.text<part.VirtualAddress||s.text-part.VirtualAddress>=part.SizeOfRawData||s.capacity>part.SizeOfRawData-(s.text-part.VirtualAddress))continue;auto offset=uint64_t(part.PointerToRawData)+s.text-part.VirtualAddress;if(offset+s.capacity>source.size()||source[offset+s.capacity-1]!=0||memcmp((void*)(module+s.text),source.data()+offset,s.capacity))continue;s.raw.assign((const char*)source.data()+offset,s.capacity-1);matched=s.raw.find('\0')==std::string::npos;break;}if(!matched){next.literals.clear();break;}}
 // 物品表可写，但显示来源只接受文件与进程中一致的只读原串，不修改表项或属性。
 for(auto &s:next.itemTexts){bool matched=false;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){auto &part=section[j];if((part.Characteristics&IMAGE_SCN_MEM_EXECUTE)||(!s.storage&&(part.Characteristics&IMAGE_SCN_MEM_WRITE)))continue;if(s.text<part.VirtualAddress||s.text-part.VirtualAddress>=part.SizeOfRawData||s.capacity>part.SizeOfRawData-(s.text-part.VirtualAddress))continue;auto offset=uint64_t(part.PointerToRawData)+s.text-part.VirtualAddress;if(offset+s.capacity>source.size()||source[offset+s.capacity-1]!=0||!readable(module+s.text,s.capacity)||memcmp((void*)(module+s.text),source.data()+offset,s.capacity))continue;s.raw.assign((const char*)source.data()+offset,s.capacity-1);int length=s.raw.empty()?0:MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,s.raw.data(),int(s.raw.size()),nullptr,0);if(!s.raw.empty()&&length<=0)continue;s.original.resize(length);if(length&&MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,s.raw.data(),int(s.raw.size()),s.original.data(),length)!=length)continue;matched=s.raw.find('\0')==std::string::npos;break;}if(!matched){s.capacity=0;s.raw.clear();s.original.clear();}}
 // 固定数组按同记录原行组合精确缓存键，空白行仍保留绘制容量。
 std::map<uint32_t,std::vector<size_t>> groups;for(size_t n=0;n<next.itemTexts.size();n++)if(next.itemTexts[n].group)groups[next.itemTexts[n].group].push_back(n);
 for(auto &entry:groups){auto &members=entry.second;std::sort(members.begin(),members.end(),[&](size_t a,size_t b){return next.itemTexts[a].text<next.itemTexts[b].text;});bool valid=members.size()>=2&&members.size()<=8&&next.itemTexts[members.front()].text==entry.first;size_t width=members.size()>1?next.itemTexts[members[1]].text-entry.first:0;valid=valid&&width>=2&&width<=128;std::wstring key;std::vector<size_t> widths;
  for(size_t line=0;line<members.size();line++){auto &part=next.itemTexts[members[line]];valid=valid&&part.capacity&&part.storage==1&&part.text==entry.first+line*width&&part.capacity<=width&&part.original.size()<=128;if(line)key+=L'\n';key+=dxcompiledsnapshot::normalize(part.original);widths.push_back(part.original.size());}key=dxcompiledsnapshot::normalize(key);valid=valid&&!key.empty();
  for(size_t line=0;line<members.size();line++){auto &part=next.itemTexts[members[line]];if(!valid){part.capacity=0;continue;}part.key=key;part.widths=widths;part.line=line;}
 }
 for(auto &s:next.itemCalls)if(s.site<5||!range(s.site-5,5,true)||!range(s.owner,5,true)||!range(s.end,5,true)){next.itemCalls.clear();break;}
 out=std::move(next);return true;
}
}
