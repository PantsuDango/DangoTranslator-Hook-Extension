#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#pragma comment(lib,"advapi32.lib")

namespace csystemsnapshot {
constexpr DWORD MaximumBytes=64*1024*1024;
using Texts=std::map<std::wstring,std::wstring>;

// digest 对实际加载的不可变字节计算SHA256
inline bool digest(const BYTE *bytes,DWORD size,std::array<BYTE,32> &out) {
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;DWORD length=DWORD(out.size());
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))return false;
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)&&CryptHashData(hash,bytes,size,0)&&CryptGetHashParam(hash,HP_HASHVAL,out.data(),&length,0)&&length==out.size();
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);return ok;
}

// whitespace 与Go缓存键的Unicode首尾空白规则一致
inline bool whitespace(wchar_t c) {return (c>=9&&c<=13)||c==32||c==0x85||c==0xa0||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000;}

// normalize 只统一换行和首尾空白，不模糊匹配剧情
inline std::wstring normalize(const std::wstring &text) {
    std::wstring value;value.reserve(text.size());
    for(size_t i=0;i<text.size();i++){if(text[i]==L'\r'){value+=L'\n';if(i+1<text.size()&&text[i+1]==L'\n')i++;}else value+=text[i];}
    size_t first=0,last=value.size();while(first<last&&whitespace(value[first]))first++;while(last>first&&whitespace(value[last-1]))last--;
    return value.substr(first,last-first);
}

// safe 与Go准备端共用相同控制码和私有T容量边界
inline bool safe(const std::wstring &original,std::wstring text) {
    while(!text.empty()&&(text.back()==L'\n'||text.back()==L'\r'||text.back()==L' '))text.pop_back();
    if(text.empty()||text.find(L"e%")!=std::wstring::npos)return false;
    size_t n=0,placeholders=0,images=0;for(auto c:original)if(c==0xfffc)images++;
    for(auto c:text){if(c==0xfe||c==0xff||c==0xffee||c==0xfffd||c==L'/'||(c>=0xd800&&c<=0xdfff)||(c<32&&c!=L'\n'&&c!=L'\r'&&c!=L'\t'))return false;if(c!=L'\r')n++;if(c==0xfffc)placeholders++;}
    if(placeholders&&placeholders!=images)return false;n+=images*(placeholders?3:4);
    if(original.size()>=2&&original.front()==L'【'&&original.back()==L'】'&&!(text.front()==L'【'&&text.back()==L'】'))n+=2;
    return n&&n+1<=2046;
}

// parse 严格消费快照，拒绝重复键、非法UTF16、越界和尾随字节
inline bool parse(const BYTE *bytes,DWORD size,Texts &texts) {
    if(!bytes||size<24||size>MaximumBytes||memcmp(bytes,"DGCSBF01",8)||*reinterpret_cast<const DWORD *>(bytes+8)!=1||*reinterpret_cast<const uint64_t *>(bytes+16)!=0)return false;
    const auto count=*reinterpret_cast<const DWORD *>(bytes+12);if(count>100000)return false;
    size_t offset=24;Texts result;
    for(DWORD i=0;i<count;i++) {
        std::wstring values[2];
        for(auto &value:values){
            if(size-offset<4)return false;DWORD n=0;memcpy(&n,bytes+offset,4);offset+=4;
            if(!n||n>2046||n>(size-offset)/2)return false;
            value.assign(reinterpret_cast<const wchar_t *>(bytes+offset),n);offset+=n*2;
            for(size_t p=0;p<value.size();p++){auto c=value[p];if(!c)return false;if(c>=0xd800&&c<=0xdbff){if(++p>=value.size()||value[p]<0xdc00||value[p]>0xdfff)return false;}else if(c>=0xdc00&&c<=0xdfff)return false;}
        }
        if(normalize(values[0])!=values[0]||!safe(values[0],values[1])||!result.emplace(values[0],values[1]).second)return false;
    }
    if(offset!=size)return false;texts=std::move(result);return true;
}

// load 以固定摘要读取有界文件，文件句柄持有期间禁止写入和替换
inline bool load(const std::wstring &path,const std::array<BYTE,32> &expected,std::vector<BYTE> &bytes) {
    if(path.empty()||path.size()>32760)return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};
    bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>=24&&size.QuadPart<=MaximumBytes;
    if(ok){bytes.resize(size_t(size.QuadPart));DWORD read=0;ok=ReadFile(file,bytes.data(),DWORD(bytes.size()),&read,nullptr)&&read==bytes.size();}
    CloseHandle(file);std::array<BYTE,32> actual{};Texts checked;
    return ok&&digest(bytes.data(),DWORD(bytes.size()),actual)&&actual==expected&&parse(bytes.data(),DWORD(bytes.size()),checked);
}
}
