#pragma once

#include <wincrypt.h>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#pragma comment(lib, "advapi32.lib")

namespace DDWorksResource
{
struct Display
{
    std::wstring text;
    DWORD kind;
};
struct Script
{
    std::vector<BYTE> original;
    std::map<DWORD, Display> displays;
};
std::vector<Script> scripts;
std::atomic<bool> active{false};
bool requested = false;
uintptr_t currentVMGlobal = 0;
std::mutex fontMutex;
std::vector<std::pair<LOGFONTW, HFONT>> fonts;

// readable 校验访问范围已提交且可读取
bool readable(const void *address, size_t length)
{
    auto cursor = reinterpret_cast<uintptr_t>(address);
    if (!cursor || length > UINTPTR_MAX - cursor) return false;
    const auto end = cursor + length;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

// readUint 有界读取资源中的小端32位字段
bool readUint(const std::vector<BYTE> &data, size_t &cursor, DWORD &value)
{
    if (cursor > data.size() || data.size() - cursor < 4) return false;
    memcpy(&value, data.data() + cursor, 4);
    cursor += 4;
    return true;
}

// validHXB 校验只读快照的脚本签名和24位长度
bool validHXB(const BYTE *data, size_t size)
{
    return size >= 18 && size < (1u << 24) && !memcmp(data, "DDWuHXB\0", 8) &&
           ((size_t(data[8]) << 16) | (size_t(data[9]) << 8) | data[10]) == size;
}

// readDisplay 只接受合法BMP文字及引擎原生换行，限制参数槽容量
bool readDisplay(const BYTE *data, size_t size, DWORD offset, std::wstring &value)
{
    if (offset <= 16 || offset >= size || data[offset - 1] != 0x80) return false;
    size_t cursor = offset;
    while (cursor + 1 < size)
    {
        const wchar_t unit = wchar_t(data[cursor] | (WORD(data[cursor + 1]) << 8));
        cursor += 2;
        if (!unit) return !value.empty() && value.size() <= 510;
        if (unit < 0x20 || (unit >= 0xd800 && unit <= 0xdfff) || unit == 0xfffd || value.size() >= 510 ||
            unit == '@' || unit == '|' || unit == '{' || unit == '}' || unit == '<' || unit == '>') return false;
        if (unit == L'\\')
        {
            if (cursor + 1 >= size || data[cursor] != 'n' || data[cursor + 1] != 0) return false;
        }
        value += unit;
    }
    return false;
}

// verifyDigest 使用原生SHA256核对快照载荷，失败时不激活
bool verifyDigest(const std::vector<BYTE> &data)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[32]{};
    DWORD length = sizeof(digest);
    bool valid = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                 CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
                 CryptHashData(hash, data.data() + 40, DWORD(data.size() - 40), 0) &&
                 CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == 32 &&
                 !memcmp(digest, data.data() + 8, 32);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return valid;
}

// load 从游戏缓存读取本进程固定快照，原始脚本和容器保持只读
bool load()
{
    wchar_t executable[32768]{};
    const auto pathLength = GetModuleFileNameW(nullptr, executable, 32768);
    if (!pathLength || pathLength >= 32768) return false;
    std::wstring path(executable, pathLength);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    path.resize(slash + 1);
    path += L"dango_cache\\DDWorks.active.bin";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    requested = true;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 44 || size.QuadPart > (64 << 20) + 40)
    { CloseHandle(file); return false; }
    std::vector<BYTE> data(static_cast<size_t>(size.QuadPart));
    DWORD received = 0;
    const bool read = ReadFile(file, data.data(), DWORD(data.size()), &received, nullptr) && received == data.size();
    CloseHandle(file);
    if (!read || memcmp(data.data(), "DDWR0001", 8) || !verifyDigest(data)) return false;
    size_t cursor = 40;
    DWORD count = 0;
    if (!readUint(data, cursor, count) || !count || count > 100000) return false;
    std::vector<Script> parsed;
    for (DWORD index = 0; index < count; ++index)
    {
        DWORD originalSize = 0, translatedSize = 0, records = 0;
        if (!readUint(data, cursor, originalSize) || !readUint(data, cursor, translatedSize) || !readUint(data, cursor, records) ||
            originalSize > data.size() - cursor || translatedSize > data.size() - cursor - originalSize || records > 100000) return false;
        const auto *original = data.data() + cursor;
        const auto *translated = original + originalSize;
        if (!validHXB(original, originalSize) || !validHXB(translated, translatedSize)) return false;
        Script script;
        script.original.assign(original, original + originalSize);
        cursor += originalSize + translatedSize;
        for (DWORD item = 0; item < records; ++item)
        {
            DWORD oldOffset = 0, newOffset = 0, kind = 0;
            if (!readUint(data, cursor, oldOffset) || !readUint(data, cursor, newOffset) || !readUint(data, cursor, kind) || kind > 2 ||
                oldOffset <= 16 || oldOffset >= originalSize || original[oldOffset - 1] != 0x80 || script.displays.count(oldOffset)) return false;
            std::wstring text;
            if (!readDisplay(translated, translatedSize, newOffset, text)) return false;
            script.displays.emplace(oldOffset, Display{std::move(text), kind});
        }
        parsed.emplace_back(std::move(script));
    }
    if (cursor != data.size()) return false;
    scripts = std::move(parsed);
    return true;
}

// compatible 核对UTF16字面量来源和VM基址、长度布局，不影响普通监听能力
bool compatible()
{
    BYTE unicodeLiteral[] = {0xa1, XX4, 0x80, 0xb8, 0xdc, 0x06, 0x04, 0x00, 0x00,
                            0x74, 0x16, 0x8b, 0xdf, 0x89, 0x5c, 0x24, 0x10, 0x66, 0x8b, 0x07,
                            0x83, 0xc7, 0x02, 0x66, 0x85, 0xc0};
    BYTE vmStorage[] = {0x8b, 0x55, 0xfc, 0x88, 0x83, 0xdc, 0x06, 0x04, 0x00,
                        0x89, 0x53, 0x08, 0x8d, 0x83, 0xe0, 0x06, 0x04, 0x00, 0x89, 0x43, 0x04};
    const auto literal = MemDbg::findBytes(unicodeLiteral, sizeof(unicodeLiteral), processStartAddress, processStopAddress);
    const auto storage = MemDbg::findBytes(vmStorage, sizeof(vmStorage), processStartAddress, processStopAddress);
    return literal && storage && *reinterpret_cast<const DWORD *>(literal + 1) == currentVMGlobal &&
           !MemDbg::findBytes(unicodeLiteral, sizeof(unicodeLiteral), literal + 1, processStopAddress) &&
           !MemDbg::findBytes(vmStorage, sizeof(vmStorage), storage + 1, processStopAddress);
}

// replace 仅从已核对原脚本的可见字面量地址替换本次参数复制源
const Display *replace(hook_context *context)
{
    if (!active || !readable(reinterpret_cast<void *>(currentVMGlobal), 4)) return nullptr;
    const auto vm = *reinterpret_cast<uintptr_t *>(currentVMGlobal);
    if (!readable(reinterpret_cast<void *>(vm), 12)) return nullptr;
    const auto *base = *reinterpret_cast<const BYTE **>(vm + 4);
    const auto size = *reinterpret_cast<DWORD *>(vm + 8);
    const auto source = context->stack[2];
    const auto start = reinterpret_cast<uintptr_t>(base);
    if (source < start || size < 18 || size >= (1u << 24) || source - start >= size || !readable(base, size)) return nullptr;
    const auto offset = DWORD(source - start);
    for (const auto &script : scripts)
    {
        if (script.original.size() != size) continue;
        const auto display = script.displays.find(offset);
        if (display == script.displays.end() || memcmp(base, script.original.data(), size)) continue;
        context->stack[2] = reinterpret_cast<uintptr_t>(display->second.text.c_str());
        return &display->second;
    }
    return nullptr;
}

// selectDisplayFont 在测量及绘制前为游戏DC选择同尺寸的中文字体
// 字体句柄保留到游戏退出，避免游戏DC仍选中它时因组件分离而失效
void selectDisplayFont(hook_context *context, HookParam *, TextBuffer *, uintptr_t *)
{
    if (!active || context->retaddr < processStartAddress || context->retaddr >= processStopAddress) return;
    const auto dc = reinterpret_cast<HDC>(context->stack[1]);
    const auto original = GetCurrentObject(dc, OBJ_FONT);
    LOGFONTW descriptor{};
    if (!original || GetObjectW(original, sizeof(descriptor), &descriptor) != sizeof(descriptor)) return;
    if (!_wcsicmp(descriptor.lfFaceName, L"Microsoft YaHei")) return;
    descriptor.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(descriptor.lfFaceName, L"Microsoft YaHei");
    std::lock_guard<std::mutex> lock(fontMutex);
    for (const auto &font : fonts)
    {
        if (!memcmp(&font.first, &descriptor, sizeof(descriptor)))
        { SelectObject(dc, font.second); return; }
    }
    if (fonts.size() >= 128) return;
    const auto translated = CreateFontIndirectW(&descriptor);
    if (!translated) return;
    fonts.emplace_back(descriptor, translated);
    SelectObject(dc, translated);
}

// installFonts 同时覆盖字符宽度和绘制，已存在字体的DC也能使用中文
bool installFonts()
{
    for (const auto address : {reinterpret_cast<uintptr_t>(GetCharABCWidthsW),
                               reinterpret_cast<uintptr_t>(TextOutW), reinterpret_cast<uintptr_t>(DrawTextW)})
    {
        HookParam hp{};
        hp.address = address;
        hp.text_fun = selectDisplayFont;
        if (!NewHook(hp, "DDWorksFont")) return false;
    }
    return true;
}
}
