#pragma once

namespace CatSystem2Backfill
{
struct TextSpan
{
  char *text = nullptr;
  size_t size = 0;
  unsigned role = 1;
};

// visibleSpan 保留选项编号和跳转标签，只定位其后的可见文字
TextSpan visibleSpan(hook_context *context)
{
  TextSpan span;
  auto raw = reinterpret_cast<char *>(context->eax);
  if (!raw || IsBadReadPtr(raw, 1))
    return span;
  const auto length = strnlen_s(raw, 4097);
  if (!length || length > 4096 || IsBadReadPtr(raw, length + 1))
    return span;
  size_t begin = 0;
  if (raw[0] >= '0' && raw[0] <= '9')
  {
    size_t cursor = 0;
    while (cursor < length && raw[cursor] >= '0' && raw[cursor] <= '9')
      ++cursor;
    if (cursor < length && (raw[cursor] == ' ' || raw[cursor] == '\t'))
    {
      while (cursor < length && (raw[cursor] == ' ' || raw[cursor] == '\t'))
        ++cursor;
      const auto label = cursor;
      while (cursor < length && (isalnum(static_cast<unsigned char>(raw[cursor])) || raw[cursor] == '_'))
        ++cursor;
      if (cursor > label && cursor < length && (raw[cursor] == ' ' || raw[cursor] == '\t'))
      {
        while (cursor < length && (raw[cursor] == ' ' || raw[cursor] == '\t'))
          ++cursor;
        begin = cursor;
        span.role = 3;
      }
    }
  }
  size_t end = length;
  if (length > 6 && memcmp(raw, "scene", 5) == 0 && (raw[5] == ' ' || raw[5] == '\t'))
  {
    begin = 6;
    span.role = 0;
  }
  // 换行和段落前缀属于游戏控制码，替换时保持原样
  for (;;)
  {
    if (begin < end && (raw[begin] == ' ' || raw[begin] == '\t'))
      ++begin;
    else if (begin + 1 < end && static_cast<BYTE>(raw[begin]) == 0x81 && static_cast<BYTE>(raw[begin + 1]) == 0x40)
      begin += 2;
    else if (begin + 2 < end && memcmp(raw + begin, "\\pc", 3) == 0)
      begin += 3;
    else if (begin + 1 < end && raw[begin] == '\\' && (raw[begin + 1] == 'n' || raw[begin + 1] == 'p' || raw[begin + 1] == '@'))
      begin += 2;
    else
      break;
  }
  while (end >= begin + 2 && raw[end - 2] == '\\' && raw[end - 1] == '@')
    end -= 2;
  if (end > begin)
  {
    span.text = raw + begin;
    span.size = end - begin;
  }
  return span;
}

// readText 提交完整可见原文，不过滤英文正文或依赖跨线程的临时指针
void readText(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
{
  const auto span = visibleSpan(context);
  if (!span.text)
    return;
  *split = span.role;
  buffer->from(span.text, span.size);
}

// writeText 在原有容量内回填并保留控制后缀，不能使用无边界strcpy覆盖脚本内存
void writeText(hook_context *context, TextBuffer buffer, HookParam *)
{
  const auto span = visibleSpan(context);
  if (!span.text || buffer.size <= 0 || static_cast<size_t>(buffer.size) > span.size || memchr(buffer.data, 0, buffer.size))
    return;
  const auto suffix = std::string(span.text + span.size);
  if (IsBadWritePtr(span.text, span.size + suffix.size() + 1))
    return;
  std::string replacement(buffer.strA());
  replacement += suffix;
  memcpy(span.text, replacement.data(), replacement.size());
  span.text[replacement.size()] = 0;
}
}
