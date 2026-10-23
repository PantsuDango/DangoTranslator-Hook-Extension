#pragma once
#include <atomic>

namespace MidareagehaBackfill
{
std::atomic<bool> installed{false};

struct TextSpan
{
  char *text = nullptr;
  size_t size = 0;
};

// currentText 读取脚本位置计算结束后的显示缓冲，不修改脚本游标
TextSpan currentText(hook_context *context, const HookParam *hp)
{
  TextSpan span;
  if (!installed)
    return span;
  const auto base = hp->user_value == 1 ? context->ebx : context->ebp;
  if (!base)
    return span;
  auto text = reinterpret_cast<char *>(base + hp->offset);
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(text, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    return span;
  const size_t capacity = hp->user_value == 2 ? 0x40 : hp->user_value == 3 ? 0x200 : 0x400;
  const auto available = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize - reinterpret_cast<uintptr_t>(text);
  const auto limit = std::min<size_t>(available, capacity);
  size_t length = 0;
  while (length < limit && text[length])
    ++length;
  if (!length || length == limit)
    return span;
  span.text = text;
  span.size = length;
  return span;
}

// readText 将正文、角色名和选项分别送入全量缓存查询
void readText(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *role)
{
  const auto span = currentText(context, hp);
  if (!span.text)
    return;
  size_t first = 0;
  while (first < span.size && (span.text[first] == ' ' || span.text[first] == '\t'))
    ++first;
  if (first == span.size || span.text[first] == '$')
    return;
  // 只提交合法原文，防止控制行或已经写入的动态编码被当作新日文再次翻译
  for (size_t position = 0; position < span.size; ++position)
  {
    const auto ch = static_cast<unsigned char>(span.text[position]);
    if (IsDBCSLeadByteEx(932, ch))
    {
      if (++position >= span.size)
        return;
      const auto trail = static_cast<unsigned char>(span.text[position]);
      if (trail < 0x40 || trail == 0x7f || trail > 0xfc)
        return;
    }
    else if ((ch < 0x20 && ch != '\t' && ch != '\r' && ch != '\n') || ch == 0x7f ||
             (ch >= 0x80 && (ch < 0xa1 || ch > 0xdf)))
      return;
  }
  *role = hp->user_value;
  buffer->from(span.text, span.size);
}

// writeText 严格限制原有容量，保留原始剧情资源和选项编号
void writeText(hook_context *context, TextBuffer buffer, HookParam *hp)
{
  const auto span = currentText(context, hp);
  if (!span.text || buffer.size <= 0 || static_cast<size_t>(buffer.size) > span.size ||
      memchr(buffer.data, 0, buffer.size) || IsBadWritePtr(span.text, span.size + 1))
    return;
  memcpy(span.text, buffer.data, buffer.size);
  span.text[buffer.size] = 0;
}

// uniqueAddress 只接受唯一的完整指令布局，未知版本保持原有监听
uintptr_t uniqueAddress(const BYTE *pattern, size_t size)
{
  const auto address = MemDbg::findBytes(pattern, size, processStartAddress, processStopAddress);
  if (!address || MemDbg::findBytes(pattern, size, address + 1, processStopAddress))
    return 0;
  return address;
}

// attach 同时确认三类可见文本入口，全部安装成功才允许写入
bool attach(uintptr_t textOffset)
{
  const BYTE bodyPattern[] = {
      0x8d, 0xbb, XX4, 0x57, 0x8b, 0x83, XX4, 0x50, 0xe8, XX4,
      0x83, 0xc4, 0x08, 0x53, 0xe8, XX4, 0x59};
  const BYTE namePattern[] = {
      0x8d, 0x45, 0xc0, 0x50, 0xe8, XX4, 0x59, 0x8b, 0x93, XX4,
      0x03, 0xc2, 0x89, 0x83, XX4, 0x8b, 0x4b, 0x1c, 0x8b, 0xb1, XX4};
  const BYTE choicePattern[] = {
      0x8b, 0x93, XX4, 0x33, 0xc0, 0x8d, 0xbd, 0x00, 0xfc, 0xff, 0xff,
      0x8b, 0x8c, 0x93, XX4, 0x8b, 0xf1, 0x83, 0xc9, 0xff, 0xf2, 0xae,
      0xf7, 0xd1, 0x2b, 0xf9, 0x8b, 0xd1, 0x87, 0xf7};
  const auto body = uniqueAddress(bodyPattern, sizeof(bodyPattern));
  const auto name = uniqueAddress(namePattern, sizeof(namePattern));
  const auto choice = uniqueAddress(choicePattern, sizeof(choicePattern));
  if (!body || !name || !choice || *reinterpret_cast<const uint32_t *>(body + 2) != textOffset ||
      *reinterpret_cast<const uint32_t *>(name + 12) != *reinterpret_cast<const uint32_t *>(name + 20))
    return false;
  // 选项缓冲来自明确的512字节分配，不能按读取时的1024字节栈缓冲放大容量
  const BYTE allocation[] = {0x68, 0x00, 0x02, 0x00, 0x00, 0xe8, XX4, 0x59};
  if (choice < processStartAddress + 0x100 || !MemDbg::findBytes(allocation, sizeof(allocation), choice - 0x100, choice))
    return false;
  const uintptr_t addresses[] = {body, name + 24, choice};
  const int offsets[] = {static_cast<int>(textOffset), -0x40, -0x400};
  for (size_t index = 0; index < 3; ++index)
  {
    HookParam hp;
    hp.address = addresses[index];
    hp.offset = offsets[index];
    hp.user_value = index + 1;
    hp.type = USING_STRING | FULL_STRING | USING_SPLIT | NO_CONTEXT | EMBED_ABLE | EMBED_DYNA_SJIS;
    hp.codepage = 932;
    hp.text_fun = readText;
    hp.embed_fun = writeText;
    hp.embed_hook_font = F_GetGlyphOutlineA | F_CreateFontIndirectA | F_TextOutA;
    if (!NewHook(hp, "EmbedMIDAREAGEHA"))
    {
      for (size_t previous = 0; previous < index; ++previous)
        RemoveHook(addresses[previous], 0);
      return false;
    }
  }
  installed = true;
  return true;
}
}
