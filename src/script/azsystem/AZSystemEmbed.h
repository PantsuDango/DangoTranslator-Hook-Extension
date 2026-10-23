#pragma once

namespace
{
// azSystemVisibleTextRole 限定不同解释器版本中的正文、角色名和选项显示参数
unsigned azSystemVisibleTextRole(unsigned opcode, unsigned argument, unsigned slotBase)
{
  const unsigned message = slotBase == 0x40 ? 0x1a : 0x1b;
  if (opcode == message && argument < 2)
    return argument + 1;
  if ((opcode == message - 5 && argument == 1) ||
      (opcode == message - 4 && argument == 4))
    return 3;
  if (opcode == message - 9 && argument == 0)
    return 4;
  return 0;
}

// azSystemDisplayArgument 验证当前命令并返回独立于原始ASB的已解析显示参数
char *azSystemDisplayArgument(hook_context *context, HookParam *hp, unsigned *role)
{
  const auto vm = context->ecx;
  const auto argument = context->edx;
  const auto slotBase = hp->user_value;
  if (!vm || argument >= 16 || (slotBase != 0x40 && slotBase != 0x44) ||
      IsBadReadPtr(reinterpret_cast<void *>(vm), slotBase + 16 * 0x108 + 4))
    return nullptr;
  const auto command = *reinterpret_cast<const uintptr_t *>(vm + 0x34);
  if (!command || IsBadReadPtr(reinterpret_cast<const void *>(command), 12))
    return nullptr;
  const auto length = *reinterpret_cast<const WORD *>(command);
  const auto opcode = *reinterpret_cast<const WORD *>(command + 2);
  const auto count = *reinterpret_cast<const unsigned *>(vm + slotBase + 16 * 0x108);
  if (length < 12 || argument >= count || count > 16)
    return nullptr;
  *role = azSystemVisibleTextRole(opcode, argument, static_cast<unsigned>(slotBase));
  if (!*role)
    return nullptr;
  const auto slot = vm + slotBase + argument * 0x108;
  // 只替换已经求值的字符串参数，变量、表达式和分支编号不进入写入入口
  if (*reinterpret_cast<const unsigned *>(slot) != 7)
    return nullptr;
  auto text = reinterpret_cast<char *>(slot + 8);
  const auto size = strnlen_s(text, 254);
  return size > 0 && size < 254 ? text : nullptr;
}

// readAZSystemDisplayArgument 在解释器读取显示参数之前提交完整原文
void readAZSystemDisplayArgument(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *split)
{
  unsigned role = 0;
  const auto text = azSystemDisplayArgument(context, hp, &role);
  if (!text)
    return;
  *split = role;
  buffer->from(text);
}

// writeAZSystemDisplayArgument 将动态Shift-JIS译文写入有容量边界的临时显示参数
void writeAZSystemDisplayArgument(hook_context *context, TextBuffer buffer, HookParam *hp)
{
  unsigned role = 0;
  auto text = azSystemDisplayArgument(context, hp, &role);
  if (!text || buffer.size <= 0 || buffer.size > 253 ||
      IsBadWritePtr(text, 254) || memchr(buffer.data, 0, buffer.size))
    return;
  // 拒绝引擎不识别的动态字符扩展区，不能让双字节字符被拆成控制码
  const auto bytes = reinterpret_cast<const BYTE *>(buffer.data);
  for (int index = 0; index < buffer.size; ++index)
  {
    const BYTE character = bytes[index];
    if (character < 0x80 || (character >= 0xa1 && character <= 0xdf))
      continue;
    if (!((character >= 0x81 && character <= 0x9f) || (character >= 0xe0 && character <= 0xfc)) ||
        ++index >= buffer.size)
      return;
  }
  memcpy(text, buffer.data, buffer.size);
  text[buffer.size] = 0;
}

// attachAZSystemDisplayArguments 根据已验证的解释器参数布局定位统一字符串读取入口
bool attachAZSystemDisplayArguments()
{
  const BYTE signature[] = {
      0x53, 0x33, 0xdb, 0x33, 0xc0, 0x83, 0xfa, 0x10,
      0x57, 0x0f, 0x83, XX, XX, XX, XX,
      0x69, 0xd2, 0x08, 0x01, 0x00, 0x00,
      0x56, 0x8d, 0x74, 0x0a, XX, 0x8b, 0x16};
  const auto address = MemDbg::findBytes(signature, sizeof(signature), processStartAddress, processStopAddress);
  if (!address || MemDbg::findBytes(signature, sizeof(signature), address + 1, processStopAddress))
    return false;
  const auto slotBase = *reinterpret_cast<const BYTE *>(address + 25);
  if (slotBase != 0x40 && slotBase != 0x44)
    return false;
  HookParam hp;
  hp.address = address;
  hp.codepage = 932;
  hp.user_value = slotBase;
  hp.type = USING_STRING | USING_SPLIT | NO_CONTEXT | EMBED_ABLE | EMBED_DYNA_SJIS;
  hp.text_fun = readAZSystemDisplayArgument;
  hp.embed_fun = writeAZSystemDisplayArgument;
  // 两代引擎都通过TextOutA生成字形，沿用动态字符映射绘制完整简体中文
  hp.embed_hook_font = F_TextOutA | F_CreateFontA;
  return NewHook(hp, "EmbedAZSystem");
}
}
