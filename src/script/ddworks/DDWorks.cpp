#include "DDWorks.h"
#include "DDWorksResource.h"

namespace
{
uintptr_t dialogueCopyReturn = 0;

bool readable(const void *address, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start <= end && size <= end - start;
}

// jsonString 保留原参数和实际译文中的引号、换行及反斜杠
std::wstring jsonString(const std::wstring &value)
{
    std::wstring result = L"\"";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto unit : value)
    {
        if (unit == L'"' || unit == L'\\')
        {
            result += L'\\';
            result += unit;
        }
        else if (unit < 0x20)
        {
            result += L"\\u00";
            result += hex[(unit >> 4) & 15];
            result += hex[unit & 15];
        }
        else result += unit;
    }
    result += L'"';
    return result;
}

}

bool DDWorks::attach_function()
{
    // The VM's full UTF-16 operand is copied through lstrcpyW. Hook the API,
    // then accept only calls from its uniquely identified VM copy site.
    BYTE signature[] = {
        0xe8, XX4, 0x8b, 0x15, XX4,
        0x81, 0xfe, 0xc7, 0x00, 0x00, 0x00, 0x77, 0x0c,
        0x8d, 0x04, 0x76, 0x8d, 0x04, 0x85, XX4,
        0xeb, 0x18, 0x81, 0xfe, 0x00, 0x01, 0x00, 0x00};
    const auto address = MemDbg::findBytes(signature, sizeof(signature), processStartAddress, processStopAddress);
    if (!address || MemDbg::findBytes(signature, sizeof(signature), address + 1, processStopAddress))
        return false;
    BYTE copySignature[] = {
        0x83, 0xff, 0x13, 0x75, XX, 0xc1, 0xe6, 0x0a,
        0x03, 0x35, XX4, 0x53, 0x56, 0xff, 0x15, XX4,
        0x5b, 0x5f, 0x5e, 0x5d, 0xc3};
    const auto copyBranch = MemDbg::findBytes(copySignature, sizeof(copySignature), processStartAddress, processStopAddress);
    if (!copyBranch || MemDbg::findBytes(copySignature, sizeof(copySignature), copyBranch + 1, processStopAddress))
        return false;
    dialogueCopyReturn = copyBranch + 22;
    DDWorksResource::currentVMGlobal = *reinterpret_cast<const DWORD *>(address + 7);
    auto *kernel = GetModuleHandleW(L"kernel32.dll");
    auto *copy = kernel ? GetProcAddress(kernel, "lstrcpyW") : nullptr;
    if (!copy)
        return false;

    HookParam hp{};
    hp.address = reinterpret_cast<uintptr_t>(copy);
    // Each lstrcpyW call is a complete VM operand, never a character fragment.
    // FULL_STRING with the Host's zero flush delay preserves packet boundaries.
    hp.type = CODEC_UTF16 | USING_STRING | FULL_STRING | NO_CONTEXT;
    hp.text_fun = [](hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
    {
        if (context->retaddr != dialogueCopyReturn)
            return;
        const auto *storage = reinterpret_cast<const wchar_t *>(context->stack[2]);
        // 替换仅修改复制源指针，原参数仍可供独立显示事件读取
        const bool backfill = DDWorksResource::active.load();
        const auto *display = backfill ? DDWorksResource::replace(context) : nullptr;
        if (!readable(storage, sizeof(wchar_t)))
            return;
        size_t length = 0;
        while (length < 0x1ff && readable(storage + length, sizeof(wchar_t)) && storage[length])
            ++length;
        if (length == 0 || length == 0x1ff)
            return;
        // This site also copies names and resources. Go admits unbroken text
        // only when it is an exact body or choice operand in this game archive.
        *split = 0;
        if (backfill)
        {
            if (display && display->kind == 2) return;
            const std::wstring original(storage, length);
            const auto packet = L"{\"ddworks_display\":1,\"kind\":" +
                std::to_wstring(display ? int(display->kind) : -1) +
                L",\"original\":" + jsonString(original) + L",\"translation\":" +
                jsonString(display ? display->text : original) + L"}";
            buffer->from(packet);
        }
        else buffer->from(reinterpret_cast<const char *>(storage), length * sizeof(wchar_t));
    };
    const bool installed = NewHook(hp, "DDWorksDialogue");
    if (installed)
    {
        if (DDWorksResource::load() && DDWorksResource::compatible() && DDWorksResource::installFonts())
        {
            DDWorksResource::active.store(true);
            Msg::Log("DDWorks resource snapshot ready");
        }
        else if (DDWorksResource::requested)
        {
            DDWorksResource::active = false;
            Msg::Log("DDWorks resource snapshot failed");
        }
    }
    return installed;
}
