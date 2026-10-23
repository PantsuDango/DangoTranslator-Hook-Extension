#pragma once
#include <windows.h>
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>

namespace ssflayout {
enum Field {VM,Body,Choices,Line,Draw,Device,DrawEnd,ObjectLocal,TextLocal,SourceField,BodyLines,HistoryLines,ChoiceLabels,ChoiceCount,ChoiceTargets,FieldCount};
struct Layout {std::array<uint32_t,FieldCount> fields{};bool display=true;};
inline bool readable(uintptr_t p,size_t n){if(!p||!n||p>UINTPTR_MAX-n)return false;auto end=p+n;while(p<end){MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery((void*)p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;auto prot=m.Protect&255;if(prot!=PAGE_READONLY&&prot!=PAGE_READWRITE&&prot!=PAGE_WRITECOPY&&prot!=PAGE_EXECUTE_READ&&prot!=PAGE_EXECUTE_READWRITE&&prot!=PAGE_EXECUTE_WRITECOPY)return false;auto next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=p)return false;p=next<end?next:end;}return true;}
inline uint64_t created(){FILETIME c{},e{},k{},u{};return GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)?uint64_t(c.dwHighDateTime)<<32|c.dwLowDateTime:0;}
// The evidence is generated for the actual disk image and tied to process birth.
// Apply PE relocations before comparing; a display-only mismatch keeps reading.
inline bool load(Layout &out){
 auto name=L"Local\\Dango.ConstructorSSF.Layout."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(created());
 HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());if(!map)return false;auto view=(const BYTE*)MapViewOfFile(map,FILE_MAP_READ,0,0,65536);if(!view){CloseHandle(map);return false;}std::array<BYTE,65536> raw{};memcpy(raw.data(),view,raw.size());UnmapViewOfFile(view);CloseHandle(map);
 size_t at=8;auto word=[&](uint32_t &v){if(at+4>raw.size())return false;memcpy(&v,raw.data()+at,4);at+=4;return true;};uint32_t version=0,size=0,base=0,count=0;
 if(memcmp(raw.data(),"DGCSLY01",8)||!word(version)||version!=1||!word(size)||size>128*1024*1024||!word(base)||!word(count)||count!=FieldCount)return false;
 auto module=(uintptr_t)GetModuleHandleW(nullptr);auto dos=(const IMAGE_DOS_HEADER*)module;if(!readable(module,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000)return false;auto nt=(const IMAGE_NT_HEADERS32*)(module+dos->e_lfanew);if(!readable((uintptr_t)nt,sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.SizeOfImage!=size)return false;
 Layout next;for(auto &v:next.fields)if(!word(v))return false;if(!word(count)||count<3||count>64)return false;
 for(uint32_t n=0;n<count;n++){uint32_t rva=0,length=0,fixups=0,group=0;if(!word(rva)||!word(length)||!word(fixups)||!word(group)||group>1||!length||length>16384||fixups>4096||at+length>raw.size()||rva>size||length>size-rva)return false;std::vector<BYTE> guard(raw.begin()+at,raw.begin()+at+length);at+=length;for(unsigned k=0;k<fixups;k++){uint32_t off=0;if(!word(off)||off>length||4>length-off)return false;uint32_t v;memcpy(&v,guard.data()+off,4);v+=uint32_t(module)-base;memcpy(guard.data()+off,&v,4);}if(!readable(module+rva,length)||memcmp((void*)(module+rva),guard.data(),length)){if(group==0)return false;next.display=false;}}
 auto section=IMAGE_FIRST_SECTION(nt);if(nt->FileHeader.NumberOfSections>96||!readable((uintptr_t)section,sizeof(*section)*nt->FileHeader.NumberOfSections))return false;
 for(unsigned n=0;n<FieldCount;n++){auto v=next.fields[n];if(n==ObjectLocal||n==TextLocal){auto offset=int32_t(v);if(offset>=0||offset< -4096)return false;continue;}if(n>=SourceField){if(!v){if(n>=HistoryLines)continue;if(n==BodyLines){next.display=false;continue;}return false;}if(v>65536)return false;continue;}if(!v){if(n==Choices)continue;if(n==Line||n==Draw||n==Device||n==DrawEnd){next.display=false;continue;}return false;}bool ok=false;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){auto &s=section[j];if(!(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)||!(s.Characteristics&IMAGE_SCN_CNT_CODE))continue;if(v>=s.VirtualAddress&&v-s.VirtualAddress<s.Misc.VirtualSize&&5<=s.Misc.VirtualSize-(v-s.VirtualAddress))ok=readable(module+v,5);}if(!ok){if(n==Draw||n==Device||n==DrawEnd||n==Line){next.display=false;continue;}return false;}}
 out=next;return true;
}
}
