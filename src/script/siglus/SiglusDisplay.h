#pragma once

// 只在排版函数入口替换只读显示参数，不改动VM字符串或游戏分配器拥有的对象
namespace DangoSiglus
{
  constexpr size_t maximumUnits = 8192;
  constexpr size_t maximumPoolBytes = 64 * 1024 * 1024;
  struct DisplayString
  {
    union { const wchar_t *pointer; wchar_t shortText[8]; } data{};
    uint32_t length = 0;
    uint32_t capacity = 7;
  };
  static_assert(sizeof(DisplayString) == 24, "Siglus requires the x86 UTF16 string ABI");
  std::mutex poolMutex;
  std::unordered_map<std::wstring, DisplayString *> pool;
  size_t poolBytes = 0;

  // readDisplay 校验完整对象、UTF16范围和终止符后读取显示参数
  bool readDisplay(uintptr_t address, std::wstring_view &text)
  {
    if (!address || IsBadReadPtr(reinterpret_cast<void *>(address), sizeof(DisplayString))) return false;
    auto object = reinterpret_cast<const DisplayString *>(address);
    if (!object->length || object->length > maximumUnits || object->length > object->capacity) return false;
    auto characters = object->capacity < 8 ? object->data.shortText : object->data.pointer;
    if (!characters || IsBadReadPtr(characters, (object->length + 1) * sizeof(wchar_t)) || characters[object->length]) return false;
    text = {characters, object->length};
    return text.find(L'\0') == std::wstring_view::npos;
  }

  // capture 在共同排版入口采集完整显示参数并按窗口对象分流
  void capture(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
  {
    std::wstring_view original;
    if (!readDisplay(context->stack[1], original)) return;
    *split = context->ecx;
    buffer->from(original);
  }

  // embed 使用组件拥有的稳定对象替换本次调用参数并保留VM原对象
  void embed(hook_context *context, TextBuffer buffer, HookParam *)
  {
    std::wstring_view original;
    if (!readDisplay(context->stack[1], original) || context->stack[1] == context->stack[2]) return;
    auto translated = buffer.viewW();
    if (translated.empty() || translated.size() > maximumUnits || translated == original ||
        original.find(L'#') != std::wstring_view::npos || translated.find(L'#') != std::wstring_view::npos) return;
    for (wchar_t character : translated)
      if (!character || (character < 0x20 && character != L'\n' && character != L'\t')) return;
    std::wstring key(translated);
    std::lock_guard<std::mutex> lock(poolMutex);
    auto found = pool.find(key);
    if (found == pool.end())
    {
      const auto storageBytes = (translated.size() + 1) * sizeof(wchar_t);
      const auto bytes = storageBytes * 2 + sizeof(DisplayString) + 64;
      if (poolBytes + bytes > maximumPoolBytes) return;
      auto object = static_cast<DisplayString *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DisplayString) + storageBytes));
      if (!object) return;
      object->length = static_cast<uint32_t>(translated.size());
      if (translated.size() < 8)
      {
        object->capacity = 7;
        memcpy(object->data.shortText, translated.data(), translated.size() * sizeof(wchar_t));
      }
      else
      {
        auto storage = reinterpret_cast<wchar_t *>(object + 1);
        memcpy(storage, translated.data(), translated.size() * sizeof(wchar_t));
        object->capacity = static_cast<uint32_t>(translated.size());
        object->data.pointer = storage;
      }
      found = pool.emplace(std::move(key), object).first;
      poolBytes += bytes;
    }
    // 游戏进程拥有这些有界显示数据；解除Hook时保留它们，进程退出后统一回收
    context->stack[1] = reinterpret_cast<uintptr_t>(found->second);
  }

  // attach 校验唯一排版特征及参数布局后注册单一Unicode显示入口
  bool attach()
  {
    const BYTE marker[] = {0x81, 0xfe, 0x0c, 0x30, 0, 0};
    auto match = MemDbg::findBytes(marker, sizeof(marker), processStartAddress, processStopAddress);
    if (!match || MemDbg::findBytes(marker, sizeof(marker), match + 1, processStopAddress)) return false;
    const BYTE prologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
    uintptr_t entry = 0;
    const auto begin = match > processStartAddress + 0x500 ? match - 0x500 : processStartAddress;
    for (auto cursor = begin; cursor + sizeof(prologue) < match; ++cursor)
      if (memcmp(reinterpret_cast<void *>(cursor), prologue, sizeof(prologue)) == 0) entry = cursor;
    if (!entry) return false;
    const BYTE parameter[] = {0x8b, 0x7d, 0x08};
    const BYTE capacity[] = {0x83, 0x7f, 0x14, 0x08};
    if (!MemDbg::findBytes(parameter, sizeof(parameter), entry, match) ||
        !MemDbg::findBytes(capacity, sizeof(capacity), entry, match)) return false;
    HookParam hp;
    hp.address = entry;
    hp.type = USING_STRING | CODEC_UTF16 | USING_SPLIT | NO_CONTEXT | EMBED_ABLE;
    hp.text_fun = capture;
    hp.embed_fun = embed;
    hp.embed_hook_font = F_GetGlyphOutlineW;
    return NewHook(hp, "EmbedSiglus");
  }
}
