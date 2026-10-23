#include "LCSE.h"
#include "LCSELayout.generated.h"
#include "LCSEGlyph.h"
#include <atomic>
#include <memory>
#include <sstream>

namespace
{
struct CodeRange { uintptr_t first, last; };
std::vector<CodeRange> ranges;
uintptr_t messageReturn = 0, messageRestoreReturn = 0, choiceCallerReturn = 0;
std::atomic<unsigned long long> pageSequence{0};
std::shared_ptr<const LCSEPresentationLayout> presentationContract;
std::shared_ptr<LCSEScene::CaptureSession> presentationSession;
LCSEControl::State presentationControl;
// 每次原生BitBlt独立持有私有画布，off或重入不能提前释放已交给GDI的源DC。
thread_local std::unique_ptr<LCSEScene::PendingBlit> pendingBlit;

// readable 检查本次读取不会越过已提交的内存区域
bool readable(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto last = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= last && size <= last - address;
}

// codeAddress 只接受当前引擎镜像的可执行节
bool codeAddress(uintptr_t address, size_t size)
{
    for (const auto &range : ranges)
        if (address >= range.first && address <= range.last && size <= range.last - address) return true;
    return false;
}

// collectCodeRanges 按PE节属性排除资源和数据中的偶然特征
bool collectCodeRanges()
{
    ranges.clear();
    std::vector<CodeRange> collected;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
    const auto ntAddress = base + dos->e_lfanew;
    if (!readable(ntAddress, sizeof(IMAGE_NT_HEADERS32))) return false;
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->FileHeader.NumberOfSections > 96) return false;
    auto *sections = IMAGE_FIRST_SECTION(nt);
    if (!readable(reinterpret_cast<uintptr_t>(sections), sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections)) return false;
    for (size_t i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto &section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t size = section.Misc.VirtualSize;
        if (!size || size > 32 * 1024 * 1024 || section.VirtualAddress > nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress || !readable(base + section.VirtualAddress, size)) return false;
        collected.push_back({base + section.VirtualAddress, base + section.VirtualAddress + size});
    }
    ranges.swap(collected);
    return !ranges.empty();
}

// patternForName 从同一份共享定义读取严格字节特征
std::vector<int> patternForName(const char *name)
{
    const DangoLCSEFunctionSpec *spec = nullptr;
    for (const auto &entry : dangoLCSEFunctions) if (strcmp(entry.name, name) == 0) spec = &entry;
    if (!spec) return {};
    std::istringstream input(spec->pattern);
    std::string token;
    std::vector<int> pattern;
    while (input >> token) pattern.push_back(token == "??" ? -1 : int(strtoul(token.c_str(), nullptr, 16)));
    return pattern;
}

// patternAt 区分真正的通配符和字面CC，不能吞掉字段位移变化
bool patternAt(uintptr_t address, const std::vector<int> &pattern)
{
    if (pattern.empty() || !codeAddress(address, pattern.size())) return false;
    const auto *data = reinterpret_cast<const BYTE *>(address);
    for (size_t index = 0; index < pattern.size(); ++index)
        if (pattern[index] >= 0 && data[index] != pattern[index]) return false;
    return true;
}

// findUnique 使用共享布局定义匹配唯一机器码入口
uintptr_t findUnique(const char *name)
{
    const auto pattern = patternForName(name);
    if (pattern.empty()) return 0;
    uintptr_t result = 0;
    for (const auto &range : ranges)
    {
        if (pattern.size() > range.last - range.first) continue;
        for (auto address = range.first; address <= range.last - pattern.size(); ++address)
        {
            if (!patternAt(address, pattern)) continue;
            if (result) return 0;
            result = address;
        }
    }
    return result;
}

// findCallOffset 读取共享定义中的调用偏移以避免两端布局漂移
size_t findCallOffset(const char *name)
{
    for (const auto &entry : dangoLCSEFunctions)
        if (strcmp(entry.name, name) == 0) return entry.callOffset;
    return 0;
}

// resolveTarget 只解析已确认代码范围内的有界跳转桩
uintptr_t resolveTarget(uintptr_t target)
{
    std::vector<uintptr_t> visited;
    for (int i = 0; i < 8; ++i)
    {
        if (!codeAddress(target, 5) || std::find(visited.begin(), visited.end(), target) != visited.end()) return 0;
        visited.push_back(target);
        if (*reinterpret_cast<const BYTE *>(target) != 0xe9) return target;
        target = target + 5 + *reinterpret_cast<const int32_t *>(target + 1);
    }
    return 0;
}

// callTarget 校验相对调用及有界跳转桩与已确认函数相连
uintptr_t callTarget(uintptr_t call)
{
    if (!codeAddress(call, 5) || *reinterpret_cast<const BYTE *>(call) != 0xe8) return 0;
    return resolveTarget(call + 5 + *reinterpret_cast<const int32_t *>(call + 1));
}

// readText 读取完整CP932快照并拒绝未知低控制字节和超长字符串
bool readText(uintptr_t address, std::wstring &result)
{
    std::string raw;
    for (size_t i = 0; i < 0x400; ++i)
    {
        if (!readable(address + i, 1)) return false;
        const unsigned char c = *reinterpret_cast<const unsigned char *>(address + i);
        if (!c)
        {
            if (raw.empty()) return false;
            const int length = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), int(raw.size()), nullptr, 0);
            if (length <= 0) return false;
            result.resize(length);
            return MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), int(raw.size()), result.data(), length) == length;
        }
        if (c < 0x20 && c != 1 && c != 2 && c != 3 && c != '\r' && c != '\n' && c != '\t') return false;
        raw.push_back(char(c));
    }
    return false;
}

// clean 保留正文换行并移除经过确认的显示控制
std::wstring clean(const std::wstring &text)
{
    std::wstring result;
    for (wchar_t c : text)
    {
        if (c == 2 || c == 3) continue;
        result.push_back(c == 1 ? L'\n' : c);
    }
    return result;
}

// jsonString 完整转义原生UTF16快照中的引号和控制字符
std::wstring jsonString(const std::wstring &value)
{
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result = L"\"";
    for (auto c : value)
    {
        if (c == L'"' || c == L'\\') { result += L'\\'; result += c; }
        else if (c < 0x20) { result += L"\\u00"; result += hex[(c >> 4) & 15]; result += hex[c & 15]; }
        else result += c;
    }
    return result + L"\"";
}

// packet 用原生页面序号区分真实相邻同文并保留姓名独立字段
std::wstring packet(int kind, const std::wstring &text, const std::wstring &speaker)
{
    return L"{\"lcse\":1,\"kind\":" + std::to_wstring(kind) + L",\"page\":" +
        std::to_wstring(++pageSequence) + L",\"text\":" + jsonString(text) + L",\"speaker\":" + jsonString(speaker) + L"}";
}
}

// locateGlyphLayout 独立验证字形调用与实际GDI导入，不安装绘制或写入Hook
bool LCSE::locateGlyphLayout(LCSEGlyphLayout &result)
{
    result = {};
    if (ranges.empty() && !collectCodeRanges()) return false;
    const auto measure = findUnique("glyph_measure"), draw = findUnique("glyph_draw"), classifier = findUnique("dbcs_classifier");
    if (!measure || !draw || !classifier || callTarget(draw + findCallOffset("glyph_draw")) != measure) return false;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    struct Import { uintptr_t call; const char *name; uintptr_t *slot; };
    LCSEGlyphLayout candidate;
    const Import imports[] = {
        {measure + 42, "GetGlyphOutlineA", &candidate.outlineSlot}, {measure + 170, "GetGlyphOutlineA", &candidate.outlineSlot},
        {draw + 35, "CreateFontIndirectA", &candidate.createFontSlot}, {draw + 65, "SelectObject", &candidate.selectSlot},
        {draw + 105, "SelectObject", &candidate.selectSlot}, {draw + 174, "SelectObject", &candidate.selectSlot},
        {draw + 184, "DeleteObject", &candidate.deleteSlot}
    };
    const auto gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return false;
    for (const auto &entry : imports)
    {
        if (!codeAddress(entry.call, 6) || *reinterpret_cast<const WORD *>(entry.call) != 0x15ff) return false;
        const auto slot = uintptr_t(*reinterpret_cast<const DWORD *>(entry.call + 2));
        const auto function = reinterpret_cast<uintptr_t>(GetProcAddress(gdi, entry.name));
        if (slot < base || slot - base > nt->OptionalHeader.SizeOfImage ||
            sizeof(uintptr_t) > nt->OptionalHeader.SizeOfImage - (slot - base) || !readable(slot, sizeof(uintptr_t)) ||
            !function || *reinterpret_cast<const uintptr_t *>(slot) != function ||
            (*entry.slot && *entry.slot != slot)) return false;
        *entry.slot = slot;
    }
    candidate.measure = measure;
    candidate.draw = draw;
    candidate.classifier = classifier;
    candidate.failure = draw + 111;
    result = candidate;
    return true;
}

// locateDisplayLayout 核对完整对象布局与调用图，不安装显示替换Hook
bool LCSE::locateDisplayLayout(LCSEDisplayLayout &result)
{
    result = {};
    LCSEGlyphLayout glyph;
    if (!locateGlyphLayout(glyph)) return false;
    LCSEDisplayLayout candidate;
    candidate.functions["glyph_draw"] = glyph.draw;
    candidate.functions["dbcs_classifier"] = glyph.classifier;
    for (const auto name : dangoLCSEDisplayNames)
    {
        const auto address = findUnique(name);
        if (!address) return false;
        candidate.functions[name] = address;
    }
    for (const auto &entry : dangoLCSEDisplayDerived)
    {
        const auto from = candidate.get(entry.from);
        const auto address = callTarget(from + entry.offset);
        if (!from || !address || !patternAt(address, patternForName(entry.to))) return false;
        candidate.functions[entry.to] = address;
    }
    for (const auto &entry : dangoLCSEDisplayCalls)
    {
        const auto from = candidate.get(entry.from), to = candidate.get(entry.to);
        if (!from || !to || callTarget(from + entry.offset) != to) return false;
    }
    for (const auto &entry : dangoLCSEDisplayPointers)
    {
        const auto from = candidate.get(entry.from);
        if (!from || !codeAddress(from + entry.offset, 4)) return false;
        const auto target = resolveTarget(*reinterpret_cast<const DWORD *>(from + entry.offset));
        if (!target || (*entry.to && target != candidate.get(entry.to))) return false;
        if (*entry.pattern)
        {
            std::istringstream input(entry.pattern);
            std::string token;
            std::vector<int> pattern;
            while (input >> token) pattern.push_back(token == "??" ? -1 : int(strtoul(token.c_str(), nullptr, 16)));
            if (!patternAt(target, pattern)) return false;
        }
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    for (const auto &entry : dangoLCSEDisplayImports)
    {
        const auto from = candidate.get(entry.from), call = from + entry.offset;
        if (!from || !codeAddress(call, 6) || *reinterpret_cast<const WORD *>(call) != 0x15ff) return false;
        const auto slot = uintptr_t(*reinterpret_cast<const DWORD *>(call + 2));
        const auto function = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA(entry.module), entry.symbol));
        if (slot < base || slot - base > nt->OptionalHeader.SizeOfImage ||
            sizeof(uintptr_t) > nt->OptionalHeader.SizeOfImage - (slot - base) || !readable(slot, sizeof(uintptr_t)) ||
            !function || *reinterpret_cast<const uintptr_t *>(slot) != function) return false;
    }
    for (const auto &entry : dangoLCSEDisplayData)
    {
        const auto from = candidate.get(entry.from);
        if (!from || !codeAddress(from + entry.offset, 4) || candidate.tables.count(entry.name)) return false;
        const auto address = uintptr_t(*reinterpret_cast<const DWORD *>(from + entry.offset));
        std::istringstream input(entry.pattern);
        std::string token;
        std::vector<BYTE> expected;
        while (input >> token)
        {
            if (token == "??") return false;
            expected.push_back(BYTE(strtoul(token.c_str(), nullptr, 16)));
        }
        if (expected.empty() || expected.size() > 64 || expected.size() % 4 || address < base ||
            address - base > nt->OptionalHeader.SizeOfImage || expected.size() > nt->OptionalHeader.SizeOfImage - (address - base) ||
            !readable(address, expected.size()) || memcmp(reinterpret_cast<void *>(address), expected.data(), expected.size())) return false;
        auto &table = candidate.tables[entry.name];
        for (size_t offset = 0; offset < expected.size(); offset += 4)
        {
            DWORD value;
            memcpy(&value, expected.data() + offset, sizeof(value));
            table.push_back(value);
        }
    }
    const auto setter = findUnique("message_setter");
    if (!setter) return false;
    candidate.functions["message_setter"] = setter;
    result = std::move(candidate);
    return true;
}

// locatePresentationChoices 独立核对选择层重绘，未知布局不撤销普通场景及正文读取
bool locatePresentationChoices(LCSEPresentationLayout &candidate)
{
    if (std::size(dangoLCSEChoiceNames) != 2 || std::size(dangoLCSEChoiceCalls) != 4) return false;
    auto functions = candidate.functions;
    for (const auto name : dangoLCSEChoiceNames)
    {
        const auto address = findUnique(name);
        if (!address || functions.count(name)) return false;
        functions[name] = address;
    }
    for (const auto &call : dangoLCSEChoiceCalls)
    {
        const auto from = functions.find(call.from), to = functions.find(call.to);
        if (from == functions.end() || to == functions.end() || callTarget(from->second + call.offset) != to->second) return false;
    }
    candidate.functions = std::move(functions);
    candidate.choices = true;
    return true;
}

// hoverWritableSlot 核对悬停数据位于模块可写且不可执行区域，不读取未初始化对象
bool hoverWritableSlot(uintptr_t base, const IMAGE_NT_HEADERS32 *nt, uintptr_t slot, size_t size)
{
    if (!size || size > 64 || slot < base || slot % 4 || slot - base > nt->OptionalHeader.SizeOfImage ||
        size > nt->OptionalHeader.SizeOfImage - (slot - base) || !readable(slot, size)) return false;
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (size_t index = 0; index < nt->FileHeader.NumberOfSections; ++index)
    {
        const auto &section = sections[index];
        const auto rva = slot - base;
        if ((section.Characteristics & 0xe0000000) != 0xc0000000 || rva < section.VirtualAddress) continue;
        const auto offset = rva - section.VirtualAddress;
        if (offset <= section.Misc.VirtualSize && size <= section.Misc.VirtualSize - offset) return true;
    }
    return false;
}

// locatePresentationHover 独立核对悬停恢复与选择状态身份，不撤销已验证选择层或正文
bool locatePresentationHover(LCSEPresentationLayout &candidate)
{
    if (!candidate.choices || std::size(dangoLCSEHoverNames) != 2 || std::size(dangoLCSEHoverCalls) != 2 ||
        std::size(dangoLCSEHoverImports) != 12 || std::size(dangoLCSEHoverGlobals) != 9) return false;
    auto functions = candidate.functions;
    for (const auto name : dangoLCSEHoverNames)
    {
        const auto address = findUnique(name);
        if (!address || functions.count(name)) return false;
        functions[name] = address;
    }
    for (const auto &call : dangoLCSEHoverCalls)
    {
        const auto from = functions.find(call.from), to = functions.find(call.to);
        if (from == functions.end() || to == functions.end() || callTarget(from->second + call.offset) != to->second) return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    for (const auto &api : dangoLCSEHoverImports)
    {
        const auto from = functions.find(api.from);
        if (from == functions.end()) return false;
        const auto call = from->second + api.offset;
        if (!codeAddress(call, 6) || *reinterpret_cast<const WORD *>(call) != 0x15ff) return false;
        const auto slot = uintptr_t(*reinterpret_cast<const DWORD *>(call + 2));
        const auto expected = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA(api.module), api.symbol));
        if (!expected || slot < base || slot - base > nt->OptionalHeader.SizeOfImage ||
            sizeof(uintptr_t) > nt->OptionalHeader.SizeOfImage - (slot - base) || !readable(slot, sizeof(uintptr_t)) ||
            *reinterpret_cast<const uintptr_t *>(slot) != expected) return false;
    }
    std::map<std::string, uintptr_t> bound;
    for (const auto &reference : dangoLCSEHoverGlobals)
    {
        const auto from = functions.find(reference.from);
        if (from == functions.end() || !*reference.name || !codeAddress(from->second + reference.offset, 4)) return false;
        const auto slot = uintptr_t(*reinterpret_cast<const DWORD *>(from->second + reference.offset));
        if (!hoverWritableSlot(base, nt, slot, reference.size) || (bound.count(reference.name) && bound.at(reference.name) != slot)) return false;
        bound[reference.name] = slot;
    }
    if (bound.size() != 5 || !bound.count("choice_records") || !bound.count("choice_state") ||
        bound.at("choice_records") != candidate.globals.at("choice_renderer") + 4) return false;
    for (const auto role : {"background", "output", "messages"})
        if (!bound.count(role) || bound.at(role) != candidate.globals.at(role)) return false;
    const auto state = uint64_t(bound.at("choice_state"));
    for (const auto &slot : candidate.globals)
    {
        const auto size = slot.first == "choice_renderer" ? uint64_t(28) : uint64_t(4);
        if (state < uint64_t(slot.second) + size && uint64_t(slot.second) < state + 12) return false;
    }
    // 完整331字节悬停契约的合成后尾声，额外填充须允许安全挂接最短跳转
    const auto hover = functions.at("choice_hover");
    if (!codeAddress(hover + 327, 5) || memcmp(reinterpret_cast<const void *>(hover + 327), "\x8b\xe5\x5d\xc3\xcc", 5)) return false;
    candidate.functions = std::move(functions);
    candidate.hoverGlobals = std::move(bound);
    candidate.hoverPhases = {{"hover_enter", hover}, {"hover_complete", hover + 327}};
    candidate.hover = true;
    return true;
}

// locatePresentationLayout 分离场景存储与绘制阶段校验，不因失败撤销已确认的正文读取
bool LCSE::locatePresentationLayout(LCSEPresentationLayout &result)
{
    result = {};
    LCSEDisplayLayout display;
    if (!locateDisplayLayout(display)) return false;
    LCSEPresentationLayout candidate;
    candidate.functions = std::move(display.functions);
    for (const auto name : dangoLCSEPresentationNames)
    {
        const auto address = findUnique(name);
        if (!address || candidate.functions.count(name)) return false;
        candidate.functions[name] = address;
    }
    for (const auto name : {"message_copy", "message_restore"})
    {
        const auto address = findUnique(name), offset = findCallOffset(name);
        if (!address || !offset || callTarget(address + offset) != candidate.functions.at("message_setter")) return false;
        candidate.functions[name] = address;
    }
    const auto function = [&candidate](const char *name)
    {
        const auto found = candidate.functions.find(name);
        return found == candidate.functions.end() ? uintptr_t(0) : found->second;
    };
    for (const auto &entry : dangoLCSEPresentationCalls)
    {
        const auto from = function(entry.from), to = function(entry.to);
        if (!from || !to || callTarget(from + entry.offset) != to) return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    for (const auto &entry : dangoLCSEPresentationImports)
    {
        const auto from = function(entry.from), call = from + entry.offset;
        if (!from || !codeAddress(call, 6) || *reinterpret_cast<const WORD *>(call) != 0x15ff) return false;
        const auto slot = uintptr_t(*reinterpret_cast<const DWORD *>(call + 2));
        const auto expected = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA(entry.module), entry.symbol));
        if (!expected || slot < base || slot - base > nt->OptionalHeader.SizeOfImage ||
            sizeof(uintptr_t) > nt->OptionalHeader.SizeOfImage - (slot - base) || !readable(slot, sizeof(uintptr_t)) ||
            *reinterpret_cast<const uintptr_t *>(slot) != expected) return false;
    }
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (const auto &entry : dangoLCSEPresentationGlobals)
    {
        const auto from = function(entry.from);
        if (!from || !codeAddress(from + entry.offset, 4) || !*entry.name || !entry.size || entry.size > 64) return false;
        const auto slot = uintptr_t(*reinterpret_cast<const DWORD *>(from + entry.offset));
        if (slot < base || slot % 4 || slot - base > nt->OptionalHeader.SizeOfImage ||
            entry.size > nt->OptionalHeader.SizeOfImage - (slot - base) || !readable(slot, entry.size)) return false;
        bool writable = false;
        for (size_t index = 0; index < nt->FileHeader.NumberOfSections; ++index)
        {
            const auto &section = sections[index];
            const auto rva = slot - base;
            if ((section.Characteristics & 0xe0000000) != 0xc0000000 || rva < section.VirtualAddress) continue;
            const auto offset = rva - section.VirtualAddress;
            if (offset <= section.Misc.VirtualSize && entry.size <= section.Misc.VirtualSize - offset) writable = true;
        }
        if (!writable || (candidate.globals.count(entry.name) && candidate.globals.at(entry.name) != slot)) return false;
        candidate.globals[entry.name] = slot;
    }
    if (candidate.globals.size() != 13 || !candidate.globals.count("choice_count") || !candidate.globals.count("choice_renderer") ||
        candidate.globals.at("choice_count") != candidate.globals.at("choice_renderer") + 8) return false;
    for (const auto name : {"output", "background", "messages", "message_count", "window", "width", "height", "choice_renderer", "choice_count", "sprites", "base_image", "widget_images", "menu_images"})
        if (!candidate.globals.count(name)) return false;
    for (const auto &first : candidate.globals)
        for (const auto &second : candidate.globals)
            if (first.first != second.first && first.second == second.second) return false;
    for (const auto &entry : dangoLCSEPresentationPhases)
    {
        const auto from = function(entry.from);
        if (!from || !*entry.name || entry.objectFrameOffset || candidate.phases.count(entry.name)) return false;
        std::istringstream input(entry.pattern);
        std::string token;
        std::vector<int> pattern;
        while (input >> token) pattern.push_back(token == "??" ? -1 : int(strtoul(token.c_str(), nullptr, 16)));
        if (!patternAt(from + entry.offset, pattern)) return false;
        candidate.phases[entry.name] = from + entry.offset;
        if (!strcmp(entry.name, "scene_present_return"))
        {
            if (entry.offset < 24 || entry.dcFrameOffset >= 0 || entry.dcFrameOffset < -4096 || entry.dcFrameOffset % 4) return false;
            for (const auto &instruction : std::vector<std::pair<int, WORD>>{{-24, 0x8589}, {-18, 0x958b}, {11, 0x8d8b}})
            {
                const auto at = uintptr_t(int64_t(from + entry.offset) + instruction.first);
                if (!codeAddress(at, 6) || *reinterpret_cast<const WORD *>(at) != instruction.second ||
                    *reinterpret_cast<const int32_t *>(at + 2) != entry.dcFrameOffset) return false;
            }
            candidate.dcFrameOffset = entry.dcFrameOffset;
        }
        else if (entry.dcFrameOffset) return false;
    }
    if (!candidate.dcFrameOffset || candidate.phases.size() != 3 || !candidate.phases.count("background_ready") || !candidate.phases.count("messages_complete") ||
        !candidate.phases.count("scene_present_return") || candidate.phases.at("background_ready") >= candidate.phases.at("messages_complete") ||
        candidate.phases.at("messages_complete") >= candidate.phases.at("scene_present_return")) return false;
    locatePresentationChoices(candidate);
    locatePresentationHover(candidate);
    // 局部呈现契约单独判定，失败不撤销已通过的整页或读取能力
    std::map<std::string, uintptr_t> refreshPhases;
    bool refresh = true;
    int refreshDC = 0, refreshObject = 0;
    for (const auto &entry : dangoLCSERefreshPhases)
    {
        const auto from = function(entry.from);
        std::istringstream input(entry.pattern);
        std::string token;
        std::vector<int> pattern;
        while (input >> token) pattern.push_back(token == "??" ? -1 : int(strtoul(token.c_str(), nullptr, 16)));
        if (!from || strcmp(entry.from, "canvas_present_rect") || refreshPhases.count(entry.name) || !patternAt(from + entry.offset, pattern)) { refresh = false; break; }
        if (!strcmp(entry.name, "refresh_enter"))
        {
            if (entry.offset || entry.dcFrameOffset || entry.objectFrameOffset) { refresh = false; break; }
        }
        else if (!strcmp(entry.name, "refresh_present_return"))
        {
            if (entry.offset != 107 || entry.dcFrameOffset != 8 || entry.objectFrameOffset != -8 ||
                !patternAt(from + 6, {0x89,0x4d,0xf8,0x8b,0x45,0x08}) ||
                !patternAt(from + 22, {0x8b,0x4d,0xf8,0x8b,0x51,0x34}) ||
                !patternAt(from + 91, {0x8b,0x55,0x08,0x52,0xff,0x15,-1,-1,-1,-1,0xff,0x15,-1,-1,-1,-1})) { refresh = false; break; }
            refreshDC = entry.dcFrameOffset; refreshObject = entry.objectFrameOffset;
        }
        else { refresh = false; break; }
        refreshPhases[entry.name] = from + entry.offset;
    }
    if (refresh && refreshPhases.size() == 2 && refreshDC && refreshObject)
    {
        candidate.refresh = true; candidate.refreshPhases = std::move(refreshPhases);
        candidate.refreshDCFrameOffset = refreshDC; candidate.refreshObjectFrameOffset = refreshObject;
    }
    // 两个最终窗口复制点沿用完整函数及具名导入契约，不扫描全局GDI调用。
    for (const auto &entry : dangoLCSEDisplayImports)
        if (!strcmp(entry.symbol, "BitBlt") && (!strcmp(entry.from, "canvas_present") || !strcmp(entry.from, "canvas_present_rect")))
        {
            const auto at = function(entry.from) + entry.offset;
            const auto name = !strcmp(entry.from, "canvas_present") ? "full" : "rect";
            if (!patternAt(at, {0xff,0x15,-1,-1,-1,-1,0xff,0x15,-1,-1,-1,-1})) return false;
            candidate.submissionPhases[std::string(name) + "_blit"] = at;
            candidate.submissionPhases[std::string(name) + "_done"] = at + 6;
        }
    result = std::move(candidate);
    return true;
}

// installPresentationCapture 安装不输出文本的绘制阶段Hook，全部成功后才公布关闭状态的会话
bool installPresentationCapture(const LCSEPresentationLayout &contract, const LCSEDisplayLayout &display)
{
    std::atomic_store(&presentationSession, std::shared_ptr<LCSEScene::CaptureSession>{});
    try
    {
        auto captured = contract;
        std::set<uintptr_t> installedAddresses;
        for (const auto &entry : std::vector<std::pair<const char *, uintptr_t>>{
            {"LCSESceneEnter", contract.functions.at("scene_redraw")},
            {"LCSESceneBackground", contract.phases.at("background_ready")},
            {"LCSESceneMessages", contract.phases.at("messages_complete")},
            {"LCSEScenePresented", contract.phases.at("scene_present_return")}})
        {
            if (!entry.second || !installedAddresses.insert(entry.second).second) return false;
            HookParam hp{};
            hp.address = entry.second;
            hp.type = NO_CONTEXT;
            hp.text_fun = [](hook_context *context, HookParam *hook, TextBuffer *, uintptr_t *)
            {
                const auto error = GetLastError();
                {
                    auto current = std::atomic_load(&presentationSession);
                    if (current) current->observe(uintptr_t(hook->address), context ? context->ebp : 0,
                        context ? context->ecx : 0, context ? reinterpret_cast<HDC>(context->stack[1]) : nullptr,
                        context ? context->stack[2] : 0);
                }
                SetLastError(error);
            };
            if (!NewHook(hp, entry.first)) return false;
        }
        captured.refresh = false;
        if (contract.refresh)
        {
            bool complete = true;
            for (const auto &entry : std::vector<std::pair<const char *, uintptr_t>>{
                {"LCSERefreshEnter", contract.refreshPhases.at("refresh_enter")},
                {"LCSERefreshPresented", contract.refreshPhases.at("refresh_present_return")}})
            {
                if (!entry.second || !installedAddresses.insert(entry.second).second) { complete = false; break; }
                HookParam hp{};
                hp.address = entry.second; hp.type = NO_CONTEXT;
                hp.text_fun = [](hook_context *context, HookParam *hook, TextBuffer *, uintptr_t *)
                {
                    const auto error = GetLastError();
                    {
                        auto current = std::atomic_load(&presentationSession);
                        if (current) current->observe(uintptr_t(hook->address), context ? context->ebp : 0,
                            context ? context->ecx : 0, context ? reinterpret_cast<HDC>(context->stack[1]) : nullptr,
                            context ? context->stack[2] : 0);
                    }
                    SetLastError(error);
                };
                if (!NewHook(hp, entry.first)) { complete = false; break; }
            }
            captured.refresh = complete;
        }
        captured.hover = false;
        if (contract.hover && captured.refresh && contract.hoverPhases.size() == 2)
        {
            bool complete = true;
            for (const auto &entry : std::vector<std::pair<const char *, uintptr_t>>{
                {"LCSEHoverEnter", contract.hoverPhases.at("hover_enter")},
                {"LCSEHoverComplete", contract.hoverPhases.at("hover_complete")}})
            {
                if (!entry.second || !installedAddresses.insert(entry.second).second) { complete = false; break; }
                HookParam hp{};
                hp.address = entry.second; hp.type = NO_CONTEXT;
                hp.text_fun = [](hook_context *context, HookParam *hook, TextBuffer *, uintptr_t *)
                {
                    const auto error = GetLastError();
                    {
                        auto current = std::atomic_load(&presentationSession);
                        if (current) current->observe(uintptr_t(hook->address), context ? context->ebp : 0, 0,
                            context ? reinterpret_cast<HDC>(context->stack[1]) : nullptr,
                            context ? context->stack[2] & 0xff : 0);
                    }
                    SetLastError(error);
                };
                if (!NewHook(hp, entry.first)) { complete = false; break; }
            }
            captured.hover = complete;
        }
        captured.atomicPresentation = contract.submissionPhases.size() == 4;
        for (const auto &entry : contract.submissionPhases)
        {
            if (!entry.second || !installedAddresses.insert(entry.second).second) { captured.atomicPresentation = false; break; }
            HookParam hp{};
            hp.address = entry.second; hp.type = NO_CONTEXT;
            hp.text_fun = [](hook_context *context, HookParam *hook, TextBuffer *, uintptr_t *)
            {
                if (!context) return;
                const auto error = GetLastError();
                auto current = std::atomic_load(&presentationSession);
                if (pendingBlit && pendingBlit->returnAddress() == uintptr_t(hook->address))
                {
                    // 此回调位于原BitBlt返回后、原DC还原/释放前，关闭资源也必须回收。
                    if (!pendingBlit->finish(context->eax != 0) && current) current->blitFailed();
                    pendingBlit.reset();
                }
                else if (current && !pendingBlit)
                    pendingBlit = current->prepareBlit(uintptr_t(hook->address), context->ebp, context->stack);
                SetLastError(error);
            };
            if (!NewHook(hp, (std::string("LCSEPresent_") + entry.first).c_str())) { captured.atomicPresentation = false; break; }
        }
        auto session = std::make_shared<LCSEScene::CaptureSession>(captured, display,
            [](const char *reason) { Msg::Log(CP_UTF8, "LCSE drawing %s", reason); });
        std::atomic_store(&presentationSession, std::move(session));
        return true;
    }
    catch (...) { return false; }
}

// configurePresentationCapture 不重新扫描已挂接代码，缺少完整阶段Hook时拒绝启用
bool LCSE::configurePresentationCapture(bool enabled, std::shared_ptr<LCSEGlyph::Renderer> renderer,
    std::shared_ptr<const LCSEScene::TranslationMap> translations, bool submit)
{
    auto session = std::atomic_load(&presentationSession);
    return session && session->configure(enabled, std::move(renderer), std::move(translations), submit);
}

// configurePresentationResource 在控制调用中准备中文字位，绘制回调不读取文件或切换资源
bool LCSE::configurePresentationResource(std::shared_ptr<const LCSESnapshot::Data> resource, bool submit)
{
    try
    {
        auto session = std::atomic_load(&presentationSession);
        const std::array<BYTE, 32> empty{};
        if (!session) { Msg::Log(CP_UTF8, "LCSE resource failed session_missing"); return false; }
        session->configure(false);
        if (!resource || !resource->translations ||
            resource->fingerprint == empty || resource->digest == empty)
        { Msg::Log(CP_UTF8, "LCSE resource failed identity_invalid"); return false; }
        auto renderer = std::make_shared<LCSEGlyph::Renderer>();
        LOGFONTW style{}; style.lfHeight = 24;
        if (!resource->glyphs.empty() && !renderer->prepare(resource->glyphs, {}, resource->reserved, resource->face, style))
        { Msg::Log(CP_UTF8, "LCSE resource failed %s", renderer->preparationFailure().c_str()); return false; }
        const bool ready = session->configure(true, std::move(renderer), resource->translations, submit);
        Msg::Log(CP_UTF8, "LCSE resource %s mappings=%zu glyphs=%zu font_checks=per_sentence", ready ? "configured" : "failed capture_configuration", resource->translations->size(), resource->glyphs.size());
        return ready;
    }
    catch (...) { Msg::Log(CP_UTF8, "LCSE resource failed configuration_exception"); return false; }
}

// processPresentationControl 区分资源加载确认和普通文本监听，未知绘制能力拒绝提交
std::string LCSE::processPresentationControl(const LCSEControl::Request &request)
{
    return presentationControl.apply(request, [](const LCSEControl::Request &value)
    {
        auto session = std::atomic_load(&presentationSession);
        if (!session || (value.action == 1 && !session->submissionSupported()))
        { Msg::Log(CP_UTF8, "LCSE resource failed presentation_unavailable"); return false; }
        auto resource = LCSESnapshot::load(value.path, value.fingerprint, value.digest);
        if (!resource) { Msg::Log(CP_UTF8, "LCSE resource failed snapshot_load"); return false; }
        if (!configurePresentationResource(std::move(resource), value.action == 1)) return false;
        if (value.action == 1) session->refreshPresentation();
        return true;
    }, []() { configurePresentationCapture(false); });
}

// resetPresentationControl 管道断开和卸载前取消旧代次，绘制线程无需等待控制锁
void LCSE::resetPresentationControl()
{
    presentationControl.reset([]() { configurePresentationCapture(false); });
}

// presentationSnapshot 仅暴露当前只读呈现结果，不建立文本线程或调用翻译源
std::shared_ptr<const LCSEScene::Snapshot> LCSE::presentationSnapshot()
{
    auto session = std::atomic_load(&presentationSession);
    return session ? session->latest() : std::shared_ptr<const LCSEScene::Snapshot>{};
}

// presentationSnapshotCurrent 拒绝换页、取消或未观察刷新后的旧快照，不写入游戏对象
bool LCSE::presentationSnapshotCurrent(const std::shared_ptr<const LCSEScene::Snapshot> &snapshot)
{
    auto session = std::atomic_load(&presentationSession);
    return session && session->current(snapshot);
}

// presentationTranslation 只返回绑定当前原帧的独立译文像素，不创建实时翻译线程
std::shared_ptr<const LCSEScene::TranslatedFrame> LCSE::presentationTranslation()
{
    auto session = std::atomic_load(&presentationSession);
    return session ? session->translation() : std::shared_ptr<const LCSEScene::TranslatedFrame>{};
}

// attach_function 安装正文与菜单监听，绘制阶段契约失败只关闭可选捕获
bool LCSE::attach_function()
{
    if (!collectCodeRanges()) return false;
    const auto setter = findUnique("message_setter");
    const auto message = findUnique("message_copy");
    const auto messageOffset = findCallOffset("message_copy");
    if (!setter || !message || !messageOffset || callTarget(message + messageOffset) != setter) return false;
    // 在监听Hook改写入口之前保存不可变呈现契约，失败只关闭后续显示接入
    auto presentation = std::make_shared<LCSEPresentationLayout>();
    if (!locatePresentationLayout(*presentation)) presentation.reset();
    LCSEDisplayLayout display;
    const bool captureReady = presentation && locateDisplayLayout(display);
    std::atomic_store(&presentationSession, std::shared_ptr<LCSEScene::CaptureSession>{});
    std::atomic_store(&presentationContract, std::shared_ptr<const LCSEPresentationLayout>(presentation));
    messageReturn = message + messageOffset + 5;
    const auto restore = findUnique("message_restore"), restoreOffset = findCallOffset("message_restore");
    if (restore && restoreOffset && callTarget(restore + restoreOffset) == setter)
        messageRestoreReturn = restore + restoreOffset + 5;
    // 正文Hook会改写setter入口，选项调用关系必须在安装前核对
    const auto choice = findUnique("choice_draw");
    const auto choiceCaller = findUnique("choice_copy");
    const auto drawOffset = findCallOffset("choice_draw"), callerOffset = findCallOffset("choice_copy");
    const bool choiceReady = choice && choiceCaller && drawOffset && callerOffset &&
        callTarget(choice + drawOffset) == setter && callTarget(choiceCaller + callerOffset) == choice;
    if (choiceReady) choiceCallerReturn = choiceCaller + callerOffset + 5;
    HookParam hp{};
    hp.address = setter;
    hp.type = CODEC_UTF16 | USING_STRING | FULL_STRING | NO_CONTEXT;
    hp.text_fun = [](hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
    {
        if (context->retaddr != messageReturn && (!messageRestoreReturn || context->retaddr != messageRestoreReturn)) return;
        if (auto session = std::atomic_load(&presentationSession)) session->invalidate();
        std::wstring text;
        if (!readText(context->stack[1], text)) return;
        std::wstring speaker;
        const auto separator = text.find(wchar_t(1));
        if (separator != std::wstring::npos) { speaker = clean(text.substr(0, separator)); text = text.substr(separator + 1); }
        text = clean(text);
        if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
        *split = 0;
        buffer->from(packet(0, text, speaker));
    };
    if (!NewHook(hp, "LCSEDialogue")) return false;
    if (captureReady) installPresentationCapture(*presentation, display);
    // 安装完成通知与首句文本分离，呈现就绪只代表入口完整而非资源已加载
    const auto reportInstalled = []()
    {
        const auto session = std::atomic_load(&presentationSession);
        Msg::Log("LCSE installed 1 1 %u", session && session->submissionSupported() ? 1u : 0u);
    };
    if (!choiceReady) { reportInstalled(); return true; }
    HookParam menu{};
    menu.address = choice;
    menu.type = CODEC_UTF16 | USING_STRING | FULL_STRING | NO_CONTEXT;
    menu.text_fun = [](hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
    {
        if (context->retaddr != choiceCallerReturn || context->stack[1] != 0 || !readable(context->ecx, 12)) return;
        if (auto session = std::atomic_load(&presentationSession)) session->invalidate();
        const auto *object = reinterpret_cast<const DWORD *>(context->ecx);
        const auto data = object[1];
        const auto count = object[2];
        if (!count || count > 64 || !readable(data, count * 0x1c)) return;
        std::wstring text;
        for (DWORD i = 0; i < count; ++i)
        {
            std::wstring option;
            const auto address = *reinterpret_cast<const DWORD *>(data + i * 0x1c + 0x10);
            if (!readText(address, option)) return;
            if (!text.empty()) text += L'\n';
            text += clean(option);
        }
        *split = 0;
        buffer->from(packet(1, text, L""));
    };
    NewHook(menu, "LCSEChoices");
    reportInstalled();
    return true;
}
