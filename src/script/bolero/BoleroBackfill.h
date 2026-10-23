#pragma once

#include <mutex>
#include <unordered_map>

namespace BoleroBackfill
{
constexpr size_t maximumTextBytes = 4096;
constexpr size_t maximumStoredBytes = 64 * 1024 * 1024;
std::mutex textMutex;
std::unordered_map<std::string, const char *> storedTexts;
size_t storedBytes = 0;

// readString 按已提交内存页边界读取有长度限制的显示字符串
std::string readString(const char *text)
{
  if (!text)
    return {};
  std::string result;
  while (result.size() <= maximumTextBytes)
  {
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(text, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !memory.Protect)
      return {};
    const auto remaining = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize - reinterpret_cast<uintptr_t>(text);
    const auto count = (std::min)(static_cast<size_t>(remaining), maximumTextBytes + 1 - result.size());
    if (!count || IsBadReadPtr(text, count))
      return {};
    const auto end = static_cast<const char *>(memchr(text, 0, count));
    if (end)
    {
      result.append(text, end - text);
      return result.size() <= maximumTextBytes ? result : std::string{};
    }
    result.append(text, count);
    text += count;
  }
  return {};
}

// readText 只在正文排版和菜单绘制入口提交完整显示参数
void readText(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
{
  const auto text = readString(reinterpret_cast<const char *>(context->stack[2]));
  if (text.empty())
    return;
  *split = 1;
  buffer->from(text);
}

// writeText 使用进程内稳定存储替换显示参数，不覆盖脚本或选项判断值
void writeText(hook_context *context, TextBuffer buffer, HookParam *)
{
  if (buffer.size <= 0 || static_cast<size_t>(buffer.size) > maximumTextBytes || memchr(buffer.data, 0, buffer.size))
    return;
  const auto bytes = reinterpret_cast<const BYTE *>(buffer.data);
  for (int index = 0; index < buffer.size; ++index)
  {
    const BYTE character = bytes[index];
    if ((character >= 0x20 && character < 0x7f) || (character >= 0xa1 && character <= 0xdf))
      continue;
    if (!((character >= 0x81 && character <= 0x9f) || (character >= 0xe0 && character <= 0xfc)) ||
        ++index >= buffer.size || !bytes[index])
      return;
  }
  std::lock_guard<std::mutex> lock(textMutex);
  const std::string translation(buffer.strA());
  auto existing = storedTexts.find(translation);
  if (existing == storedTexts.end())
  {
    if (storedBytes + translation.size() + 1 > maximumStoredBytes)
      return;
    auto stableText = static_cast<char *>(HeapAlloc(GetProcessHeap(), 0, translation.size() + 1));
    if (!stableText)
      return;
    memcpy(stableText, translation.c_str(), translation.size() + 1);
    existing = storedTexts.emplace(translation, stableText).first;
    storedBytes += translation.size() + 1;
  }
  // 菜单对象会继续持有指针，使用进程堆使Helper关闭或DLL卸载后字符串仍然有效
  context->stack[2] = reinterpret_cast<uintptr_t>(existing->second);
}

// uniqueAddress 仅接受唯一匹配的已分析函数布局
uintptr_t uniqueAddress(const BYTE *signature, size_t size)
{
  const auto address = MemDbg::findBytes(signature, size, processStartAddress, processStopAddress);
  return address && !MemDbg::findBytes(signature, size, address + 1, processStopAddress) ? address : 0;
}

// attach 同时核对正文和菜单入口后注册独立的全量缓存写入Hook
bool attach()
{
  const BYTE dialogue[] = {
      0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, XX4,
      0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xec, 0x0c, 0x53, 0x56, 0x57,
      0xa1, XX4, 0x33, 0xc5, 0x50, 0x8d, 0x45, 0xf4, 0x64, 0xa3, 0x00, 0x00, 0x00, 0x00,
      0x8b, 0x75, 0x0c, 0x8a, 0x1e, 0x88, 0x5d, 0xf0, 0x8b, 0x45, 0xf0, 0x46, 0x50};
  const BYTE menu[] = {
      0x55, 0x8b, 0xec, 0x8b, 0x45, 0x0c, 0x8b, 0x55, 0x18, 0x83, 0xec, 0x08,
      0x53, 0x8b, 0x5d, 0x10, 0x56, 0x8b, 0xf1, 0x8b, 0x4d, 0x14, 0x57, 0x8b, 0x7d, 0x08,
      0x89, 0x46, 0x08, 0x8b, 0x45, 0x1c};
  const auto dialogueAddress = uniqueAddress(dialogue, sizeof(dialogue));
  const auto menuAddress = uniqueAddress(menu, sizeof(menu));
  if (!dialogueAddress || !menuAddress ||
      !Util::FindImportEntry(processStartAddress, reinterpret_cast<DWORD>(GetGlyphOutlineA)))
    return false;
  HookParam hp;
  hp.address = dialogueAddress;
  hp.codepage = 932;
  hp.type = USING_STRING | USING_SPLIT | NO_CONTEXT | EMBED_ABLE | EMBED_DYNA_SJIS;
  hp.text_fun = readText;
  hp.embed_fun = writeText;
  hp.embed_hook_font = F_GetGlyphOutlineA | F_CreateFontA;
  const bool dialogueReady = NewHook(hp, "EmbedBolero");
  hp.address = menuAddress;
  const bool menuReady = NewHook(hp, "EmbedBolero");
  return dialogueReady && menuReady;
}
}
