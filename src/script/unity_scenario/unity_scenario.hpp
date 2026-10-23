#pragma once

#include <wincrypt.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include "scenario_display.generated.h"
#include "scenario_hde64.generated.h"
#pragma comment(lib, "advapi32.lib")

// unityscenario 独立监听与显示副本，只允许已确认剧情窗口读取或使用激活快照
namespace unityscenario
{
    struct MethodHeader;
    inline MethodHeader *(*methodHeader)(MonoMethod *) = nullptr;
    inline const unsigned char *(*headerCode)(MethodHeader *, uint32_t *, uint32_t *) = nullptr;
    inline void (*freeHeader)(MethodHeader *) = nullptr;
    inline uint32_t (*fieldToken)(MonoClassField *) = nullptr;
    inline MonoMethod *(*getMethod)(MonoImage *, uint32_t, MonoClass *) = nullptr;
    inline MonoString *(*loadString)(MonoDomain *, MonoImage *, uint32_t) = nullptr;
    inline int (*stringLength)(MonoString *) = nullptr;
    inline const wchar_t *(*stringChars)(MonoString *) = nullptr;
    inline MonoClass *scenarioClass = nullptr;
    inline MonoDomain *scenarioDomain = nullptr;
    inline void (*domainForeach)(void (*)(MonoDomain *, void *), void *) = nullptr;
    inline void (*domainAssemblyForeach)(MonoDomain *, GFunc, void *) = nullptr;
    inline int (*domainUnloading)(MonoDomain *) = nullptr;
    inline MonoDomain *(*objectDomain)(MonoObject *) = nullptr;
    inline MonoClassField *indexField = nullptr;
    inline MonoMethod *textMethod = nullptr;
    inline MonoMethod *nameMethod = nullptr;
    inline std::atomic<uint64_t> sequence{0};
    inline std::atomic<bool> installed{false};
    inline thread_local bool reading = false;
    inline MonoMethod *(*jitMethod)(MonoJitInfo *) = nullptr;
    inline void *(*jitCodeStart)(MonoJitInfo *) = nullptr;
    inline int (*jitCodeSize)(MonoJitInfo *) = nullptr;
    inline const char *(*imageFilename)(MonoImage *) = nullptr;
    inline uint32_t (*methodFlags)(MonoMethod *, uint32_t *) = nullptr;
    inline MonoType **(*headerLocals)(MethodHeader *, uint32_t *, int *) = nullptr;
    inline int (*headerClauses)(MethodHeader *) = nullptr;
    inline MonoMethod *displayMethods[2]{};
    inline std::map<std::wstring,std::wstring> translations;
    inline bool backfillRequested = false;
    inline std::string displayDigest;
    inline std::atomic<bool> displayActive{false};
    inline std::atomic<unsigned> displayTraceMask{0};
    inline MonoMethod *fontCreate = nullptr, *fontSet = nullptr, *fontGet = nullptr, *fontHas = nullptr;
    inline std::mutex displayMutex;
    inline DWORD displayThread = 0;
    inline uint32_t fontHandle = 0, stringHandle = 0;
    inline uint32_t currentWindow = 0;
    inline MonoClass *windowClass = nullptr;
    inline MonoClassField *windowScenario = nullptr, *windowText = nullptr, *windowName = nullptr;
    struct FontRecord { uint32_t target, original; };
    inline std::vector<FontRecord> fontRecords;
    struct WindowRecord { uint32_t window, scenario; bool pending; std::vector<BYTE> variables; };
    inline std::vector<WindowRecord> windowRecords;
    // Roots 释放本组件持有的GC根，不调用Unity对象或改写原始剧情
    struct Roots { ~Roots() { if (!GetModuleHandleW(L"mono-2.0-bdwgc.dll") || !mono_gchandle_free || !mono_get_root_domain || !mono_get_root_domain()) return; for (auto record : fontRecords) { mono_gchandle_free(record.target); mono_gchandle_free(record.original); } for(auto &record : windowRecords){mono_gchandle_free(record.window);if(record.scenario)mono_gchandle_free(record.scenario);} if(currentWindow)mono_gchandle_free(currentWindow); if (fontHandle) mono_gchandle_free(fontHandle); if (stringHandle) mono_gchandle_free(stringHandle); } };
    inline Roots roots;

    // traceDisplay 每种显示结果只记录一次，不记录剧情文字或在快进时刷日志
    void traceDisplay(unsigned bit, const char *reason)
    {
        if(!(displayTraceMask.fetch_or(bit)&bit))Msg::Log("UnityScenario display %s",reason);
    }

    // resolve 加载本组件所需的原版 Mono API，缺失时不安装入口
    template <class T> bool resolve(HMODULE module, T &target, const char *name)
    {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        return target != nullptr;
    }

    // typeName 通过运行时类型核对参数及字段语义
    std::string typeName(MonoType *type)
    {
        auto klass = type ? mono_class_from_mono_type(type) : nullptr;
        const char *name = klass ? mono_class_get_name(klass) : nullptr;
        const char *space = klass ? mono_class_get_namespace(klass) : nullptr;
        if (!name || !space) return {};
        return *space ? std::string(space) + "." + name : std::string(name);
    }

    // instanceField 拒绝重名、静态或类型不兼容的字段
    MonoClassField *instanceField(MonoClass *klass, const char *name, const char *type)
    {
        MonoClassField *result = nullptr;
        void *iterator = nullptr;
        while (auto field = mono_class_get_fields(klass, &iterator))
        {
            if (strcmp(mono_field_get_name(field), name)) continue;
            if (result || (mono_field_get_flags(field) & 0x10) || typeName(mono_field_get_type(field)) != type) return nullptr;
            result = field;
        }
        return result;
    }

    // getterMethod 验证唯一的实例字符串取值方法
    MonoMethod *getterMethod(MonoClass *klass, const char *name)
    {
        MonoMethod *result = nullptr;
        void *iterator = nullptr;
        while (auto candidate = mono_class_get_methods(klass, &iterator))
        {
            if (strcmp(mono_method_get_name(candidate), name)) continue;
            if (result) return nullptr;
            auto signature = mono_method_signature(candidate);
            if (!signature || !mono_signature_is_instance(signature) || mono_signature_get_param_count(signature) ||
                typeName(mono_signature_get_return_type(signature)) != "System.String") return nullptr;
            result = candidate;
        }
        return result;
    }

    // tokenAt 读取已经过长度检查的 IL 引用
    uint32_t tokenAt(const unsigned char *code, size_t offset)
    {
        uint32_t value = 0;
        memcpy(&value, code + offset, sizeof(value));
        return value;
    }

    // listMember 核对集合访问的方法、返回值和参数
    bool listMember(MonoImage *image, uint32_t token, const char *name, const char *resultType, uint32_t count)
    {
        if ((token >> 24) != 10) return false;
        auto method = getMethod(image, token, nullptr);
        auto klass = method ? mono_method_get_class(method) : nullptr;
        auto signature = method ? mono_method_signature(method) : nullptr;
        if (!klass || !signature || strcmp(mono_class_get_name(klass), "List`1") ||
            strcmp(mono_class_get_namespace(klass), "System.Collections.Generic") || strcmp(mono_method_get_name(method), name) ||
            !mono_signature_is_instance(signature) || mono_signature_get_param_count(signature) != count ||
            typeName(mono_signature_get_return_type(signature)) != resultType) return false;
        if (count)
        {
            void *iterator = nullptr;
            if (typeName(mono_signature_get_params(signature, &iterator)) != "System.Int32") return false;
        }
        return true;
    }

    // safeGetter 复查实际加载的完整取值指令，未知布局不调用托管方法
    bool safeGetter(MonoMethod *method, MonoImage *image, MonoClassField *texts, MonoClassField *value)
    {
        auto header = methodHeader(method);
        if (!header) return false;
        struct HeaderGuard { MethodHeader *value; ~HeaderGuard() { freeHeader(value); } } guard{header};
        uint32_t size = 0, stack = 0;
        const auto code = headerCode(header, &size, &stack);
        if (!code || size != 48) return false;
        const unsigned char expected[] = {2,0x7b,0,0,0,0,2,0x7b,0,0,0,0,0x6f,0,0,0,0,0x32,6,0x72,0,0,0,0,0x2a,2,0x7b,0,0,0,0,2,0x7b,0,0,0,0,0x6f,0,0,0,0,0x7b,0,0,0,0,0x2a};
        unsigned char shape[48]; memcpy(shape, code, sizeof(shape));
        for (size_t offset : {2,8,13,20,27,33,38,43}) memset(shape + offset, 0, 4);
        if (memcmp(shape, expected, sizeof(shape)) || tokenAt(code,2) != fieldToken(indexField) ||
            tokenAt(code,8) != fieldToken(texts) || tokenAt(code,27) != fieldToken(texts) ||
            tokenAt(code,33) != fieldToken(indexField) || tokenAt(code,43) != fieldToken(value)) return false;
        auto literal = tokenAt(code,20);
        if ((literal >> 24) != 0x70) return false;
        auto empty = loadString(scenarioDomain, image, literal & 0xffffff);
        return empty && stringLength(empty) == 0 && listMember(image,tokenAt(code,13),"get_Count","System.Int32",0) &&
            listMember(image,tokenAt(code,38),"get_Item","NameStory",1);
    }

    // stringValue 只复制有界的真实托管字符串，拒绝嵌入零和异常字符
    std::wstring stringValue(MonoObject *object, int limit)
    {
        if (!object || typeName(mono_class_get_type(mono_object_get_class(object))) != "System.String") return {};
        auto text = reinterpret_cast<MonoString *>(object);
        int length = stringLength(text);
        const auto chars = stringChars(text);
        if (!chars || length < 0 || length > limit) return {};
        std::wstring result(chars, length);
        if (result.find(L'\0') != std::wstring::npos) return {};
        return result;
    }

    // jsonString 保留原文换行和引号，不清除游戏已处理的变量文字
    std::wstring jsonString(std::wstring_view text)
    {
        std::wstring result = L"\"";
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (wchar_t ch : text)
        {
            if (ch == L'"' || ch == L'\\') { result += L'\\'; result += ch; }
            else if (ch < 32) { result += L"\\u00"; result += hex[(ch >> 4) & 15]; result += hex[ch & 15]; }
            else result += ch;
        }
        return result + L"\"";
    }

    // capture 在原版 getter 执行前读取同一页的正文和姓名，重入不产生重复快照
    void capture(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        if (reading || mono_domain_get() != scenarioDomain) return;
        auto owner = reinterpret_cast<MonoObject *>(context->argof(1,hp));
        if (!owner || objectDomain(owner) != scenarioDomain || mono_object_get_class(owner) != scenarioClass) return;
        struct ReadingGuard { ReadingGuard() { reading = true; } ~ReadingGuard() { reading = false; } } guard;
        int index = -1; mono_field_get_value(owner,indexField,&index);
        if (index < 0 || index > 1000000) return;
        MonoObject *exception = nullptr;
        auto text = stringValue(mono_runtime_invoke(textMethod,owner,nullptr,&exception),16384);
        if (exception || text.empty()) return;
        auto speaker = stringValue(mono_runtime_invoke(nameMethod,owner,nullptr,&exception),1024);
        if (exception) return;
        int current = -1; mono_field_get_value(owner,indexField,&current);
        if (index != current) return;
        auto payload = L"{\"sequence\":" + std::to_wstring(sequence.fetch_add(1)+1) + L",\"owner\":" +
            std::to_wstring(reinterpret_cast<uintptr_t>(owner)) + L",\"index\":" + std::to_wstring(index) +
            L",\"text\":" + jsonString(text) + L",\"speaker\":" + jsonString(speaker) +
            L",\"backfill\":" + (displayActive.load()?L"true":L"false") + L"}";
        if (payload.size() * sizeof(wchar_t) >= TEXT_BUFFER_SIZE) return;
        buffer->from(payload);
    }

    // hashFile 对单次只读句柄流式计算摘要，不将大型Unity资源加载到内存
    bool hashFile(const std::wstring &path, BYTE digest[32])
    {
        HANDLE file = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        HCRYPTPROV provider = 0; HCRYPTHASH hash = 0; DWORD count = 0, size = 32;
        bool valid = CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider,CALG_SHA_256,0,0,&hash);
        BYTE buffer[65536];
        while (valid) { if (!ReadFile(file,buffer,sizeof(buffer),&count,nullptr)) { valid = false; break; } if (!count) break; valid = CryptHashData(hash,buffer,count,0); }
        valid = valid && CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0) && size == 32;
        if (hash) CryptDestroyHash(hash); if (provider) CryptReleaseContext(provider,0); CloseHandle(file); return valid;
    }

    // readFile 限制启动缓存容量并拒绝读取中的替换，所有绘制回调不访问磁盘
    bool readFile(const std::wstring &path, size_t limit, std::vector<BYTE> &data)
    {
        HANDLE file = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{}; DWORD received = 0;
        bool ok = GetFileSizeEx(file,&size) && size.QuadPart > 0 && size.QuadPart <= limit;
        if (ok) { data.resize(size_t(size.QuadPart)); ok = ReadFile(file,data.data(),DWORD(data.size()),&received,nullptr) && received == data.size(); }
        CloseHandle(file); return ok;
    }

    // uintValue 有界读取小端缓存长度
    bool uintValue(const std::vector<BYTE> &data, size_t &pos, uint32_t &value)
    {
        if (pos > data.size() || data.size()-pos < 4) return false;
        memcpy(&value,data.data()+pos,4); pos += 4; return true;
    }

    // resourceString 读取有界BMP码元，非法控制和代理项整句拒绝
    bool resourceString(const std::vector<BYTE> &data, size_t &pos, std::wstring &value, uint32_t limit)
    {
        uint32_t size = 0; if (!uintValue(data,pos,size) || !size || size > limit || size > (data.size()-pos)/2) return false;
        value.resize(size); memcpy(value.data(),data.data()+pos,size*2); pos += size*2;
        for (auto unit : value) if (!unit || unit == 0xfffd || (unit >= 0xd800 && unit <= 0xdfff) || (unit < 32 && unit != L'\r' && unit != L'\n' && unit != L'\t')) return false;
        return true;
    }

    // safeRelative 不允许快照引用游戏目录以外或具有Windows特殊语义的路径
    bool safeRelative(const std::wstring &path)
    {
        if (path.empty() || path.front() == L'/' || path.find_first_of(L"\\:\r\n\t") != std::wstring::npos) return false;
        size_t pos = 0;
        while (pos < path.size()) { auto end = path.find(L'/',pos); if (end == std::wstring::npos) end = path.size(); auto part = path.substr(pos,end-pos); if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false; pos = end+1; }
        return true;
    }

    // hashBytes 校验同一次读取的缓存字节，不用二次文件打开代替完整性检查
    bool hashBytes(const std::vector<BYTE> &data, BYTE digest[32])
    {
        HCRYPTPROV provider=0;HCRYPTHASH hash=0;DWORD size=32;
        bool valid=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)&&CryptHashData(hash,data.data(),DWORD(data.size()),0)&&CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)&&size==32;
        if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);return valid;
    }

    // loadDisplay 先校验外部授权摘要、原始资源及映射，再公布本进程不可变快照
    bool loadDisplay(MonoImage *image)
    {
        wchar_t executable[32768]{}; DWORD length = GetModuleFileNameW(nullptr,executable,32768);
        if (!length || length >= 32768) return false;
        std::wstring path(executable,length); auto slash = path.find_last_of(L"\\/"); auto dot = path.find_last_of(L'.');
        if (slash == std::wstring::npos || dot <= slash) return false;
        const auto root = path.substr(0,slash+1); const auto dataRoot = path.substr(0,dot)+L"_Data\\";
        std::vector<BYTE> ticket, data;
        const auto ticketPath = root+L"dango_cache\\UnityScenario.active.bin";
        backfillRequested = GetFileAttributesW(ticketPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!backfillRequested) return false;
        if (!readFile(ticketPath,4096,ticket) || ticket.size() < 142 || memcmp(ticket.data(),"USCA0002",8)) return false;
        size_t pos = 136; std::wstring file;
        if (!resourceString(ticket,pos,file,128) || pos != ticket.size() || !safeRelative(file) || file.find(L'/') != std::wstring::npos || file.size() < 6 || file.substr(file.size()-5) != L".uscn") return false;
        const auto dataPath = root+L"dango_cache\\"+file;
        // 校验读取到的同一份字节，防止两次打开之间发生资源替换
        if (!readFile(dataPath,64<<20,data) || data.size() < 80 || memcmp(data.data(),"USCN0001",8)) return false;
        BYTE digest[32]{};
        if (!hashBytes(data,digest) || memcmp(digest,ticket.data()+8,32) || memcmp(data.data()+8,ticket.data()+40,64)) return false;
        std::vector<BYTE> activation;
        if (!readFile(root+L"dango_cache\\backfill.active.json",65536,activation) || !hashBytes(activation,digest) || memcmp(digest,ticket.data()+104,32)) return false;
        pos = 72; uint32_t count = 0; if (!uintValue(data,pos,count) || !count || count > 4096) return false;
        std::set<std::wstring> paths;
        for (uint32_t i = 0; i < count; ++i) {
            std::wstring relative; if (!resourceString(data,pos,relative,1024) || !safeRelative(relative) || !paths.insert(relative).second || data.size()-pos < 32) return false;
            if (!hashFile(dataRoot+relative,digest) || memcmp(digest,data.data()+pos,32)) return false;
            if(relative==L"Managed/Assembly-CSharp.dll"){
                if(!resolve(GetModuleHandleW(L"mono-2.0-bdwgc.dll"),imageFilename,"mono_image_get_filename"))return false;
                const char *loaded=imageFilename(image);if(!loaded||strlen(loaded)>131070)return false;
                int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,loaded,-1,nullptr,0);if(length<=1||length>32768)return false;
                std::wstring actual(size_t(length),L'\0');if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,loaded,-1,actual.data(),length))return false;actual.pop_back();
                if(!hashFile(actual,digest)||memcmp(digest,data.data()+pos,32))return false;
            }
            pos += 32;
        }
        if (!paths.count(L"Managed/Assembly-CSharp.dll") || !uintValue(data,pos,count) || count > 100000) return false;
        std::map<std::wstring,std::wstring> values;
        for (uint32_t i = 0; i < count; ++i) {
            std::wstring original, translated;
            if (!resourceString(data,pos,original,16384) || !resourceString(data,pos,translated,16384) || original.find_first_of(L"<>") != std::wstring::npos || translated.find_first_of(L"<>") != std::wstring::npos || original.find(L"%n") != std::wstring::npos || translated.find(L"%n") != std::wstring::npos || !values.emplace(original,translated).second) return false;
        }
        if (pos != data.size()) return false;
        constexpr char hex[]="0123456789abcdef";
        // 前面的来源校验复用了摘要缓冲，确认身份必须使用授权保存的整份快照摘要
        displayDigest.clear(); for(size_t i=8;i<40;++i){auto byte=ticket[i];displayDigest+=hex[byte>>4];displayDigest+=hex[byte&15];}
        translations = std::move(values); return true;
    }

    // displaySignature 校验实例性及完整参数数量，泛型参数由静态契约与来源摘要进一步绑定
    bool displaySignature(MonoMethod *method, std::string expected)
    {
        auto signature = mono_method_signature(method); if (!signature) return false;
        auto separator = expected.find('|'); if (separator == std::string::npos) return false;
        bool instance = expected.substr(0,separator) == "20";
        if (bool(mono_signature_is_instance(signature)) != instance) return false;
        expected.erase(0,separator+1); std::vector<std::string> names;
        size_t start = 0;
        while (true) { auto end = expected.find('|',start); names.push_back(expected.substr(start,end-start)); if (end == std::string::npos) break; start=end+1; }
        if (mono_signature_get_param_count(signature)+1 != names.size()) return false;
        auto matches = [](MonoType *type, std::string name) {
            if (name.substr(0,3) == "12:" || name.substr(0,3) == "11:") name.erase(0,3);
            auto generic = name.find('<'); if (generic != std::string::npos) name.resize(generic);
            if (name == "void") name="System.Void"; else if (name == "string") name="System.String"; else if (name == "bool") name="System.Boolean"; else if (name == "int") name="System.Int32"; else if (name == "char") name="System.Char";
            return typeName(type) == name;
        };
        if (!matches(mono_signature_get_return_type(signature),names[0])) return false;
        void *iterator = nullptr; for (size_t i=1;i<names.size();++i) if (!matches(mono_signature_get_params(signature,&iterator),names[i])) return false;
        return true;
    }

    // displayMethod 拒绝同名重载及静态或参数不兼容的显示方法
    MonoMethod *displayMethod(MonoClass *klass, const char *name, const char *signature)
    {
        MonoMethod *found = nullptr; void *iterator = nullptr;
        while (auto method = mono_class_get_methods(klass,&iterator)) { if (strcmp(mono_method_get_name(method),name)) continue; if (found || !displaySignature(method,signature)) return nullptr; found=method; }
        return found;
    }

    // overloadMethod 按完整签名选择Unity公开重载，存在多个兼容候选仍拒绝
    MonoMethod *overloadMethod(MonoClass *klass, const char *name, const char *signature)
    {
        MonoMethod *found=nullptr; void *iterator=nullptr;
        while(auto method=mono_class_get_methods(klass,&iterator)){if(strcmp(mono_method_get_name(method),name)||!displaySignature(method,signature))continue;if(found)return nullptr;found=method;}
        return found;
    }

    // displayContract 校验完整窗口IL及每个字段与调用的语义，失败只限制显示能力
    bool displayContract(MonoImage *image, MonoClass *window, const ScenarioDisplaySpec &spec, MonoMethod *&method)
    {
        method = displayMethod(window,spec.name,!strcmp(spec.name,"init") ? "20|void|12:UnityEngine.TextAsset" : "20|void");
        if (!method) return false; uint32_t impl = 0; const auto flags = methodFlags(method,&impl); if (impl || (flags&0x10)) return false;
        auto header = methodHeader(method); if (!header) return false;
        struct Guard { MethodHeader *header; ~Guard(){ freeHeader(header); } } guard{header};
        uint32_t size=0, stack=0, locals=0; int initialized=0;
        headerLocals(header,&locals,&initialized); if (locals || headerClauses(header)) return false;
        auto code = headerCode(header,&size,&stack); if (!code || strlen(spec.code) != size*2) return false;
        std::vector<BYTE> shape(code,code+size);
        for (size_t i=0;i<spec.count;++i) {
            const auto &token = spec.tokens[i]; if (token.offset+4>size) return false;
            std::string semantic(token.semantic); auto first=semantic.find(':'); auto second=semantic.find(':',first+1); auto third=semantic.find(':',second+1);
            if (first==std::string::npos || second==std::string::npos || third==std::string::npos) return false;
            auto owner=semantic.substr(first+1,second-first-1); auto name=semantic.substr(second+1,third-second-1); auto signature=semantic.substr(third+1);
            const auto value=tokenAt(code,token.offset);
            if (semantic.substr(0,first)=="field") {
                auto klass=mono_class_from_name(image,"",owner.c_str()); auto field=klass ? instanceField(klass,name.c_str(),signature.substr(0,6)=="06:12:" ? signature.substr(6).c_str() : signature=="06:bool" ? "System.Boolean" : signature=="06:string" ? "System.String" : "") : nullptr;
                if (!field || fieldToken(field)!=value) return false;
            } else {
                auto called=getMethod(image,value,nullptr); if (!called || typeName(mono_class_get_type(mono_method_get_class(called)))!=owner || name!=mono_method_get_name(called) || !displaySignature(called,signature)) return false;
            }
            memset(shape.data()+token.offset,0,4);
        }
        constexpr char hex[]="0123456789abcdef"; std::string actual;
        for (auto byte : shape) { actual+=hex[byte>>4]; actual+=hex[byte&15]; }
        return actual==spec.code;
    }

    // normalized 与SQLite精确键保持同样的CRLF及Unicode空白规则
    std::wstring normalized(std::wstring value)
    {
        std::wstring text; for (size_t i=0;i<value.size();++i) { if (value[i]==L'\r') { text+=L'\n'; if (i+1<value.size() && value[i+1]==L'\n') ++i; } else text+=value[i]; }
        auto space=[](wchar_t c){ return (c>=9&&c<=13)||c==32||c==0x85||c==0xa0||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000; };
        size_t begin=0,end=text.size(); while(begin<end&&space(text[begin])) ++begin; while(end>begin&&space(text[end-1])) --end; return text.substr(begin,end-begin);
    }

    // enterWindow 记录本次窗口及原始模板页，不改变构造参数、剧情对象或显示状态
    void enterWindow(hook_context *context, HookParam *hp, TextBuffer *, uintptr_t *)
    {
        if(reading||!displayActive.load()||mono_domain_get()!=scenarioDomain)return;
        std::unique_lock<std::mutex> lock(displayMutex,std::try_to_lock);if(!lock.owns_lock())return;
        if(displayThread&&displayThread!=GetCurrentThreadId())return;displayThread=GetCurrentThreadId();
        if(currentWindow){mono_gchandle_free(currentWindow);currentWindow=0;}
        auto window=reinterpret_cast<MonoObject *>(context->argof(1,hp));if(!window||mono_object_get_class(window)!=windowClass)return;
        for(auto it=windowRecords.begin();it!=windowRecords.end();){if(!mono_gchandle_get_target(it->window)){mono_gchandle_free(it->window);if(it->scenario)mono_gchandle_free(it->scenario);it=windowRecords.erase(it);}else ++it;}
        WindowRecord *record=nullptr;for(auto &candidate:windowRecords)if(mono_gchandle_get_target(candidate.window)==window){record=&candidate;break;}
        const bool initializing=hp->user_value==1;
        if(!record){if(!initializing||windowRecords.size()>=256){traceDisplay(1,"window_untracked");return;}auto handle=mono_gchandle_new_weakref(window,0);if(!handle)return;windowRecords.push_back({handle,0,false,{}});record=&windowRecords.back();}
        if(initializing){
            if(record->scenario){mono_gchandle_free(record->scenario);record->scenario=0;}
            record->pending=true;record->variables.clear();
            auto asset=reinterpret_cast<MonoObject *>(context->argof(2,hp));if(!asset)return;
            auto klass=mono_object_get_class(asset);if(typeName(mono_class_get_type(klass))!="UnityEngine.TextAsset")return;
            auto getter=getterMethod(klass,"get_text");if(!getter)return;
            struct Guard{Guard(){reading=true;}~Guard(){reading=false;}}guard;
            MonoObject *error=nullptr;auto raw=stringValue(mono_runtime_invoke(getter,asset,nullptr,&error),1<<20);if(error||raw.empty()){traceDisplay(2,"asset_read_failed");return;}
            size_t begin=0;while(begin<raw.size()&&record->variables.size()<100000){auto end=raw.find(L"\r\n",begin);if(end==std::wstring::npos)break;auto row=raw.substr(begin,end-begin);auto comma=row.find(L',');auto header=row.substr(0,comma);header.erase(std::remove(header.begin(),header.end(),L' '),header.end());auto event=header.find(L'(');if(event!=std::wstring::npos)header.resize(event);auto id=header.find(L'_');if(id!=std::wstring::npos)header.resize(id);
                BYTE flags=header.find(L"%n")!=std::wstring::npos?1:0;
                if(comma!=std::wstring::npos){auto body=row.substr(comma+1,row.find(L',',comma+1)-comma-1);if(body.find(L"%n")!=std::wstring::npos)flags|=2;}
                record->variables.push_back(flags);begin=end+2;
            }
        }
        currentWindow=mono_gchandle_new_weakref(window,0);
        traceDisplay(4,"window_entered");
    }

    // currentDisplay 核对同一窗口、Scenario、当前页及显示槽，动态模板不按同名字面量替换
    bool currentDisplay(MonoMethod *caller, MonoObject *target, const std::wstring &value)
    {
        auto window=currentWindow?mono_gchandle_get_target(currentWindow):nullptr;if(!window)return false;
        WindowRecord *record=nullptr;for(auto &candidate:windowRecords)if(mono_gchandle_get_target(candidate.window)==window){record=&candidate;break;}
        if(!record)return false;
        MonoObject *scenario=nullptr,*text=nullptr,*name=nullptr;mono_field_get_value(window,windowScenario,&scenario);mono_field_get_value(window,windowText,&text);mono_field_get_value(window,windowName,&name);
        if(!scenario||mono_object_get_class(scenario)!=scenarioClass||text==name||(target!=text&&target!=name))return false;
        if(record->pending&&caller==displayMethods[0]){record->scenario=mono_gchandle_new_weakref(scenario,0);record->pending=false;}
        if(!record->scenario||mono_gchandle_get_target(record->scenario)!=scenario)return false;
        int index=-1;mono_field_get_value(scenario,indexField,&index);
        if(index<0||size_t(index)>=record->variables.size()||(record->variables[index]&(target==name?1:2)))return false;
        MonoObject *error=nullptr;auto actual=stringValue(mono_runtime_invoke(target==name?nameMethod:textMethod,scenario,nullptr,&error),16384);int after=-1;mono_field_get_value(scenario,indexField,&after);
        return !error&&after==index&&value==actual;
    }

    // replaceDisplay 只替换已确认窗口的临时显示参数，不修改Scenario、ID、事件或存档
    void replaceDisplay(hook_context *context, HookParam *hp, TextBuffer *, uintptr_t *)
    {
        if (reading || !displayActive.load() || mono_domain_get()!=scenarioDomain) return;
        // 调用点直接绑定已核对方法，公共set_text被重新编译或重定向时仍替换实际传入参数
        auto caller=reinterpret_cast<MonoMethod *>(hp->user_value); if (caller!=displayMethods[0] && caller!=displayMethods[1]) return;
        auto target=reinterpret_cast<MonoObject *>(context->argof(1,hp));
        auto original=reinterpret_cast<MonoObject *>(context->argof(2,hp)); if (!target || !original || objectDomain(target)!=scenarioDomain || objectDomain(original)!=scenarioDomain) return;
        auto value=stringValue(original,16384); auto found=translations.find(normalized(value));
        std::unique_lock<std::mutex> lock(displayMutex,std::try_to_lock); if (!lock.owns_lock()) return;
        if (displayThread && displayThread!=GetCurrentThreadId()) return; displayThread=GetCurrentThreadId();
        struct ReadingGuard { ReadingGuard(){reading=true;} ~ReadingGuard(){reading=false;} } guard;
        const bool bound=currentDisplay(caller,target,value);
        if (stringHandle) { mono_gchandle_free(stringHandle); stringHandle=0; }
        // 弱引用避免保留已销毁窗口，原字体根只覆盖有界的实际显示对象
        FontRecord *record=nullptr;
        for (auto it=fontRecords.begin();it!=fontRecords.end();) { if (!mono_gchandle_get_target(it->target)) { mono_gchandle_free(it->target); mono_gchandle_free(it->original); it=fontRecords.erase(it); } else ++it; }
        for(auto &candidate : fontRecords) if(mono_gchandle_get_target(candidate.target)==target){record=&candidate;break;}
        MonoObject *exception=nullptr;
        auto restore=[&](){ if (record) { auto font=mono_gchandle_get_target(record->original); void *args[]={font}; if(font) mono_runtime_invoke(fontSet,target,args,&exception); } };
        if (!bound || found==translations.end()) { traceDisplay(bound?16:8,bound?"translation_missing":"page_unbound");restore(); return; }
        if (!fontHandle) {
            auto family=mono_string_new_wrapper("Microsoft YaHei"); int size=24; void *args[]={family,&size};
            auto font=mono_runtime_invoke(fontCreate,nullptr,args,&exception); if (exception || !font) {traceDisplay(32,"font_create_failed");return;} fontHandle=mono_gchandle_new(font,0); if(!fontHandle) return;
        }
        auto font=mono_gchandle_get_target(fontHandle); if(!font) return;
        for (auto ch : found->second) { if(ch==L'\n'||ch==L'\r'||ch==L'\t') continue; void *args[]={&ch}; auto result=mono_runtime_invoke(fontHas,font,args,&exception); if(exception||!result||!*reinterpret_cast<BYTE *>(mono_object_unbox(result))) { traceDisplay(64,"font_character_missing");restore(); return; } }
        if (!record) {
            if (fontRecords.size()>=256) return; auto old=mono_runtime_invoke(fontGet,target,nullptr,&exception); if(exception||!old) return;
            uint32_t weak=mono_gchandle_new_weakref(target,0), strong=mono_gchandle_new(old,0); if(!weak||!strong){if(weak)mono_gchandle_free(weak);if(strong)mono_gchandle_free(strong);return;}
            fontRecords.push_back({weak,strong}); record=&fontRecords.back();
        }
        auto replacement=mono_string_new_utf16(scenarioDomain,reinterpret_cast<const gunichar2 *>(found->second.data()),int(found->second.size())); if(!replacement){restore();return;}
        stringHandle=mono_gchandle_new(reinterpret_cast<MonoObject *>(replacement),0); if(!stringHandle){restore();return;}
        void *args[]={font}; mono_runtime_invoke(fontSet,target,args,&exception); if(exception){traceDisplay(128,"font_set_failed");restore();return;}
        context->argof(2,hp)=reinterpret_cast<uintptr_t>(mono_gchandle_get_target(stringHandle));
        traceDisplay(256,"applied");
    }

    // displayCallSites 按完整指令边界确认两个同槽虚调用及参数恢复布局，不扫描共享文字函数
    bool displayCallSites(const BYTE *code, size_t size, std::vector<uintptr_t> &sites, int32_t &slot)
    {
        if(!code||size<16||size>16384||IsBadReadPtr(code,size))return false;
        size_t previous=0, beforePrevious=0, offset=0;
        std::vector<uintptr_t> branches;
        std::vector<std::pair<uintptr_t,size_t>> spans;
        unsigned indirect=0;
        while(offset<size){
            BYTE bytes[16]{};memcpy(bytes,code+offset,(std::min)(size-offset,sizeof(bytes)));
            hde64s instruction{};hde64_disasm(bytes,&instruction);
            if(!instruction.len||(instruction.flags&F_ERROR)||instruction.len>size-offset)return false;
            if(instruction.flags&F_RELATIVE){
                const int64_t delta=(instruction.flags&F_IMM8)?int64_t(int8_t(instruction.imm.imm8)):int64_t(int32_t(instruction.imm.imm32));
                branches.push_back(reinterpret_cast<uintptr_t>(code+offset+instruction.len)+delta);
            }
            if(instruction.opcode==0xff&&instruction.modrm_reg==2&&instruction.modrm_mod!=3){
                ++indirect;
                // mov rcx,rax / mov rax,[rax] / call [rax+slot]；Hook从vtable读取前开始，参数已就位
                if(previous+3!=offset||beforePrevious+3!=previous||
                   memcmp(code+beforePrevious,"\x48\x8b\xc8\x48\x8b\x00",6)||
                   instruction.p_rep||instruction.p_lock||instruction.p_seg||instruction.p_66||instruction.p_67||instruction.rex||
                   instruction.modrm_rm!=0||(instruction.modrm_mod!=1&&instruction.modrm_mod!=2))return false;
                const int32_t displacement=instruction.modrm_mod==1?int32_t(int8_t(instruction.disp.disp8)):int32_t(instruction.disp.disp32);
                if(displacement<0||displacement>16384||(displacement%sizeof(void *))||(slot>=0&&slot!=displacement))return false;
                slot=displacement;sites.push_back(reinterpret_cast<uintptr_t>(code+previous));spans.push_back({sites.back(),3+instruction.len});
            }
            beforePrevious=previous;previous=offset;offset+=instruction.len;
        }
        for(auto target:branches)for(const auto &span:spans)if(target>=span.first&&target<span.first+span.second)return false;
        return indirect==2&&sites.size()==2;
    }

    // installDisplay 独立安装已核对窗口的实际显示调用点，普通监听不启用或加载映射
    bool installDisplay(HMODULE runtime, MonoImage *image, MonoClass *window)
    {
        if (!loadDisplay(image)) return false;
        if (!resolve(runtime,jitMethod,"mono_jit_info_get_method") || !resolve(runtime,jitCodeStart,"mono_jit_info_get_code_start") || !resolve(runtime,jitCodeSize,"mono_jit_info_get_code_size") || !resolve(runtime,methodFlags,"mono_method_get_flags") || !resolve(runtime,headerLocals,"mono_method_header_get_locals") || !resolve(runtime,headerClauses,"mono_method_header_get_num_clauses") ||
            !resolve(runtime,mono_jit_info_table_find,"mono_jit_info_table_find") || !mono_string_new_utf16 || !mono_string_new_wrapper || !mono_gchandle_new || !mono_gchandle_new_weakref || !mono_gchandle_get_target || !mono_gchandle_free || !mono_object_unbox) return false;
        for (size_t i=0;i<2;++i) if(!displayContract(image,window,scenarioDisplaySpecs[i],displayMethods[i])) return false;
        // Unity原生类型只从已加载程序集查找，不加载任何第三方翻译插件
        struct Types { MonoClass *text=nullptr; bool ambiguous=false; } types;
        domainAssemblyForeach(scenarioDomain,[](void *assembly,void *state){ auto &types=*reinterpret_cast<Types *>(state); auto image=mono_assembly_get_image(reinterpret_cast<MonoAssembly *>(assembly)); if(!image)return;
            if(auto type=mono_class_from_name(image,"UnityEngine.UI","Text")){if(types.text&&types.text!=type)types.ambiguous=true;types.text=type;}
        },&types);
        if(!types.text||types.ambiguous)return false;
        auto setter=displayMethod(types.text,"set_text","20|void|string");
        fontGet=displayMethod(types.text,"get_font","20|12:UnityEngine.Font"); if(!fontGet)return false;
        auto fontClass=mono_class_from_mono_type(mono_signature_get_return_type(mono_method_signature(fontGet))); if(!fontClass)return false;
        fontCreate=overloadMethod(fontClass,"CreateDynamicFontFromOSFont","00|12:UnityEngine.Font|string|int");
        fontSet=displayMethod(types.text,"set_font","20|void|12:UnityEngine.Font"); fontHas=overloadMethod(fontClass,"HasCharacter","20|bool|char");
        if(!setter||!fontCreate||!fontSet||!fontGet||!fontHas)return false;
        windowClass=window;windowScenario=instanceField(window,"sc","Scenario");windowText=instanceField(window,"textwindow","UnityEngine.UI.Text");windowName=instanceField(window,"namewindow","UnityEngine.UI.Text");
        if(!windowScenario||!windowText||!windowName)return false;
        uintptr_t entries[2]{};std::vector<uintptr_t> sites[2];int32_t slot=-1;
        for(size_t i=0;i<2;++i){
            entries[i]=reinterpret_cast<uintptr_t>(mono_compile_method(displayMethods[i]));if(!entries[i])return false;
            auto info=mono_jit_info_table_find(scenarioDomain,reinterpret_cast<char *>(entries[i]));
            if(!info||jitMethod(info)!=displayMethods[i]||jitCodeStart(info)!=reinterpret_cast<void *>(entries[i])||
               !displayCallSites(reinterpret_cast<const BYTE *>(entries[i]),jitCodeSize(info),sites[i],slot))return false;
        }
        HookParam hook;
        hook.type=NO_CONTEXT; hook.offset=1; hook.text_fun=replaceDisplay; hook.jittype=JITTYPE::UNITY;
        const char *names[2][2]={{"UnityScenarioDisplayInitName","UnityScenarioDisplayInitText"},{"UnityScenarioDisplayNextName","UnityScenarioDisplayNextText"}};
        for(size_t i=0;i<2;++i)for(size_t j=0;j<2;++j){hook.address=sites[i][j];hook.user_value=reinterpret_cast<uintptr_t>(displayMethods[i]);if(!NewHook(hook,names[i][j]))return false;}
        for(size_t i=0;i<2;++i){hook.address=entries[i];hook.text_fun=enterWindow;hook.user_value=i==0?1:2;if(!NewHook(hook,i==0?"UnityScenarioWindowInit":"UnityScenarioWindowNext"))return false;}
        displayActive.store(true);return true;
    }

    // findImage 只接受唯一加载的原版主程序集，不打开文件或加载翻译插件
    void findImage(void *assembly, void *state)
    {
        auto image = mono_assembly_get_image(reinterpret_cast<MonoAssembly *>(assembly));
        const char *name = image ? mono_image_get_name(image) : nullptr;
        if (!name || strcmp(name,"Assembly-CSharp")) return;
        auto found = reinterpret_cast<std::pair<MonoImage *, unsigned> *>(state);
        found->first = image; found->second++;
    }

    struct DomainImage { MonoDomain *domain=nullptr; MonoImage *image=nullptr; unsigned count=0; };

    // findDomain 按已加载程序集选择唯一执行域，不在根域另编译游戏从未调用的副本
    void findDomain(MonoDomain *domain, void *state)
    {
        if(!domain||domainUnloading(domain))return;
        std::pair<MonoImage *,unsigned> images{nullptr,0};
        domainAssemblyForeach(domain,findImage,&images);
        if(!images.second)return;
        auto &found=*reinterpret_cast<DomainImage *>(state);
        found.domain=domain;found.image=images.first;found.count+=images.second;
    }

    // install 先安装只读监听，再独立核对激活快照及显示入口
    bool install()
    {
        if (installed.load()) return true;
        HMODULE runtime = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!runtime || GetModuleHandleW(L"GameAssembly.dll")) return false;
        monofunctions::init(runtime);
        if (!resolve(runtime,methodHeader,"mono_method_get_header") || !resolve(runtime,headerCode,"mono_method_header_get_code") ||
            !resolve(runtime,freeHeader,"mono_metadata_free_mh") || !resolve(runtime,fieldToken,"mono_class_get_field_token") ||
            !resolve(runtime,getMethod,"mono_get_method") || !resolve(runtime,loadString,"mono_ldstr") ||
            !resolve(runtime,domainForeach,"mono_domain_foreach") || !resolve(runtime,domainAssemblyForeach,"mono_domain_assembly_foreach") ||
            !resolve(runtime,domainUnloading,"mono_domain_is_unloading") || !resolve(runtime,objectDomain,"mono_object_get_domain") ||
            !resolve(runtime,mono_domain_get,"mono_domain_get") || !resolve(runtime,mono_domain_set,"mono_domain_set") ||
            !resolve(runtime,stringLength,"mono_string_length") || !resolve(runtime,stringChars,"mono_string_chars") ||
            !resolve(runtime,mono_field_get_value,"mono_field_get_value") || !resolve(runtime,mono_field_get_type,"mono_field_get_type") ||
            !resolve(runtime,mono_class_get_fields,"mono_class_get_fields") || !resolve(runtime,mono_field_get_name,"mono_field_get_name") ||
            !resolve(runtime,mono_method_get_class,"mono_method_get_class") || !resolve(runtime,mono_method_get_name,"mono_method_get_name") ||
            !mono_get_root_domain || !mono_thread_attach || !mono_thread_detach || !mono_class_get_fields || !mono_field_get_flags ||
            !mono_class_get_methods || !mono_runtime_invoke || !mono_method_signature || !mono_signature_is_instance ||
            !mono_signature_get_param_count || !mono_signature_get_params || !mono_signature_get_return_type ||
            !mono_object_get_class || !mono_class_get_type || !mono_class_from_mono_type || !mono_class_get_name ||
            !mono_class_get_namespace || !mono_field_get_name || !mono_method_get_name || !mono_method_get_class ||
            !mono_assembly_foreach || !mono_assembly_get_image || !mono_image_get_name || !mono_class_from_name || !mono_compile_method) return false;
        auto domain = mono_get_root_domain();
        auto thread = domain ? mono_thread_attach(domain) : nullptr;
        if (!thread) return false;
        struct Attachment { MonoThread *thread; ~Attachment() { mono_thread_detach(thread); } } attachment{thread};
        DomainImage found;
        domainForeach(findDomain,&found);
        if(found.count!=1||!found.domain||!found.image||domainUnloading(found.domain)||!mono_domain_set(found.domain,0))return false;
        scenarioDomain=found.domain;
        auto image = found.image;
        scenarioClass = mono_class_from_name(image,"","Scenario");
        auto story = mono_class_from_name(image,"","NameStory");
        auto window = mono_class_from_name(image,"","TDScenarioWindow");
        if (!scenarioClass || !story || !window) return false;
        indexField = instanceField(scenarioClass,"index","System.Int32");
        // 泛型实际参数还会通过 getter 的 get_Item 返回类型校验
        auto texts = instanceField(scenarioClass,"Texts","System.Collections.Generic.List`1");
        auto text = instanceField(story,"text","System.String");
        auto name = instanceField(story,"name","System.String");
        textMethod = getterMethod(scenarioClass,"getCurrentText");
        nameMethod = getterMethod(scenarioClass,"getCurrentName");
        if (!indexField || !texts || !text || !name || !textMethod || !nameMethod ||
            !instanceField(window,"sc","Scenario") || !instanceField(window,"textwindow","UnityEngine.UI.Text") ||
            !instanceField(window,"namewindow","UnityEngine.UI.Text") || !safeGetter(textMethod,image,texts,text) || !safeGetter(nameMethod,image,texts,name)) return false;
        HookParam hook;
        hook.address = reinterpret_cast<uintptr_t>(mono_compile_method(textMethod));
        if (!hook.address) return false;
        hook.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT;
        hook.text_fun = capture;
        hook.jittype = JITTYPE::UNITY;
        strcpy_s(hook.function,"Assembly-CSharp::Scenario:getCurrentText:0");
        installed.store(NewHook(hook,"UnityScenarioDialogue"));
        if (installed.load()) {
            if (installDisplay(runtime,image,window)) Msg::Log("UnityScenario resources ready %s",displayDigest.c_str());
            else if (backfillRequested) Msg::Log("UnityScenario resources failed");
        }
        return installed.load();
    }
}
