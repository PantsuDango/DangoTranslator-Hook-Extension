#pragma once

#include <mutex>
#include <unordered_map>
#include "YurisLayout.h"

namespace YurisBackfill
{
constexpr size_t maximumTextBytes = 1024;
constexpr size_t maximumStoredBytes = 64 * 1024 * 1024;
std::mutex textMutex;
std::unordered_map<std::string, const char *> storedTexts;
size_t storedBytes = 0;
uintptr_t menuStackOffset = 0;
YurisLayout::Layout displayLayout;
bool layoutPrepared = false;
thread_local std::unordered_map<uintptr_t, uintptr_t> originalMenuPointers;

// readable 确认字符串和上下文内存范围可读
bool readable(uintptr_t address, size_t size)
{
  return address && size && address + size > address && !IsBadReadPtr(reinterpret_cast<void *>(address), size);
}

// readString 限长读取完整字符串并跳过带注音和内嵌控制符的正文
std::string readString(uintptr_t address, size_t length = 0)
{
  if (length > maximumTextBytes)
    return {};
  if (!length)
  {
    while (length <= maximumTextBytes)
    {
      if (!readable(address + length, 1))
        return {};
      if (!*reinterpret_cast<const char *>(address + length))
        break;
      ++length;
    }
  }
  if (!length || length > maximumTextBytes || !readable(address, length))
    return {};
  std::string text(reinterpret_cast<const char *>(address), length);
  for (size_t index = 0; index < text.size(); ++index)
  {
    const BYTE first = text[index];
    if (first < 0x20 || first == '\\' || first == 0x7f)
      return {};
    if ((first >= 0x81 && first <= 0x9f) || (first >= 0xe0 && first <= 0xfc))
    {
      if (++index >= text.size())
        return {};
      const BYTE second = text[index];
      if (second < 0x40 || second == 0x7f || second > 0xfc ||
          (first == 0xef && second >= 0xf0 && second <= 0xf5) ||
          (first == 0x81 && (second == 0x83 || second == 0xe1)))
        return {};
    }
    else if (first >= 0x80 && !(first >= 0xa1 && first <= 0xdf))
      return {};
  }
  return text;
}

// storeText 为显示期间可能重复使用的译文分配有总量限制的稳定内存
const char *storeText(TextBuffer buffer)
{
  if (buffer.size <= 0 || buffer.size > maximumTextBytes || memchr(buffer.data, 0, buffer.size))
    return nullptr;
  std::lock_guard<std::mutex> lock(textMutex);
  const std::string text(buffer.strA());
  auto found = storedTexts.find(text);
  if (found != storedTexts.end())
    return found->second;
  if (storedBytes + text.size() + 1 > maximumStoredBytes)
    return nullptr;
  auto storage = static_cast<char *>(HeapAlloc(GetProcessHeap(), 0, text.size() + 1));
  if (!storage)
    return nullptr;
  memcpy(storage, text.c_str(), text.size() + 1);
  storedTexts.emplace(text, storage);
  storedBytes += text.size() + 1;
  return storage;
}

// readDisplayString 有界读取显示原文，保留注音与控制符供监听层清洗
std::string readDisplayString(uintptr_t address, size_t length = 0)
{
  constexpr size_t limit = 8192;
  if (!address || length >= limit)
    return {};
  if (!length)
  {
    while (length < limit)
    {
      if (!readable(address + length, 1))
        return {};
      if (!*reinterpret_cast<const char *>(address + length))
        break;
      ++length;
    }
  }
  if (!length || length >= limit || !readable(address, length))
    return {};
  return std::string(reinterpret_cast<const char *>(address), length);
}

// readWord 只读取WORD指令求值完成后即将复制到显示缓冲区的正文
void readWord(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
{
  const auto text = readString(context->ebx, context->esi);
  if (text.empty() || !context->esi)
    return;
  *split = 1;
  buffer->from(text);
}

// writeWord 同步替换复制源和后续排版长度并再次核对8192字节显示缓冲区
void writeWord(hook_context *context, TextBuffer buffer, HookParam *)
{
  if (!displayLayout.word.address || !readable(context->edx, displayLayout.word.characters + 4) || buffer.size <= 0 || buffer.size > maximumTextBytes)
    return;
  const auto used = *reinterpret_cast<const DWORD *>(context->edx + displayLayout.word.length);
  const auto destination = *reinterpret_cast<const DWORD *>(context->edx + displayLayout.word.text);
  if (used != context->eax || used > 0x1ffe || used + buffer.size >= 0x1ffe ||
      IsBadWritePtr(reinterpret_cast<void *>(destination + used), buffer.size + 1))
    return;
  const auto stable = storeText(buffer);
  if (!stable)
    return;
  context->ebx = reinterpret_cast<uintptr_t>(stable);
  context->esi = buffer.size;
}

// readMenu 只在CGACT的TEXT分支开始绘制前读取完整显示参数
void readMenu(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
{
  const auto slot = reinterpret_cast<uintptr_t>(context->stack) + menuStackOffset;
  if (!readable(slot, sizeof(uintptr_t)))
    return;
  const auto text = readString(*reinterpret_cast<const uintptr_t *>(slot));
  if (text.empty())
    return;
  *split = 2;
  buffer->from(text);
}

// writeMenu 暂时替换绘制分支的栈参数，绘制完毕后恢复原选项值
void writeMenu(hook_context *context, TextBuffer buffer, HookParam *)
{
  const auto slot = reinterpret_cast<uintptr_t>(context->stack) + menuStackOffset;
  if (!readable(slot, sizeof(uintptr_t)) || IsBadWritePtr(reinterpret_cast<void *>(slot), sizeof(uintptr_t)) || originalMenuPointers.count(slot))
    return;
  const auto stable = storeText(buffer);
  if (!stable)
    return;
  originalMenuPointers.emplace(slot, *reinterpret_cast<uintptr_t *>(slot));
  *reinterpret_cast<uintptr_t *>(slot) = reinterpret_cast<uintptr_t>(stable);
}

// restoreMenu 在同一个绘制分支的唯一出口恢复参数，避免后续复合命令使用译文
void restoreMenu(hook_context *context, HookParam *, TextBuffer *, uintptr_t *)
{
  const auto slot = reinterpret_cast<uintptr_t>(context->stack) + menuStackOffset;
  const auto found = originalMenuPointers.find(slot);
  if (found != originalMenuPointers.end())
  {
    *reinterpret_cast<uintptr_t *>(slot) = found->second;
    originalMenuPointers.erase(found);
  }
}

// prepareLayout inspects the unmodified main image once, before any legacy Hook.
// Each display consumer is independently unique and has its own CP932 contract.
void prepareLayout()
{
  if (layoutPrepared) return;
  layoutPrepared = true;
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return;
  const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return;
  const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
  if (!readable(reinterpret_cast<uintptr_t>(nt), sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return;
  const auto sections = IMAGE_FIRST_SECTION(nt);
  if (!nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96 ||
      !readable(reinterpret_cast<uintptr_t>(sections), nt->FileHeader.NumberOfSections * sizeof(*sections))) return;
  YurisLayout::Image image;
  for (unsigned n = 0; n < nt->FileHeader.NumberOfSections; ++n) {
    const auto &s = sections[n];
    if (!(s.Characteristics & IMAGE_SCN_MEM_READ)) continue;
    const auto size = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
    if (!size || uint64_t(s.VirtualAddress) + size > nt->OptionalHeader.SizeOfImage ||
        uint64_t(base) + s.VirtualAddress + size > 0x100000000ULL || !readable(base + s.VirtualAddress, size)) continue;
    image.push_back({static_cast<uint32_t>(base + s.VirtualAddress), reinterpret_cast<const unsigned char *>(base + s.VirtualAddress), size,
                    (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0, (s.Characteristics & IMAGE_SCN_MEM_WRITE) != 0});
  }
  displayLayout = YurisLayout::find(image);
  menuStackOffset = displayLayout.menu.stack;
}

// Preserve the independent complete-page listeners; embedding is a separate capability.
uintptr_t prepareRead()
{
  prepareLayout();
  return displayLayout.word.address ? displayLayout.word.address + 37 : 0;
}
uintptr_t prepareMenuRead()
{
  prepareLayout();
  return displayLayout.menu.address ? displayLayout.menu.address + 6 : 0;
}

// readMenuPage 在TEXT循环计数前读取完整文字，排除正文逐字绘制共用的单字参数
void readMenuPage(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
{
  const auto text = readDisplayString(context->edi);
  if (text.empty() || text.size() == 1 ||
      (text.size() == 2 && IsDBCSLeadByteEx(932, static_cast<BYTE>(text[0]))))
    return;
  *split = context->edi;
  buffer->from(text);
}

// readPage 只读取复制完成并已终止的页面，不修改游戏缓冲或脚本变量
void readPage(hook_context *context, HookParam *, TextBuffer *buffer, uintptr_t *split)
{
  const auto length = context->edx;
  if (!length || length >= 0x1ffe || !displayLayout.word.address || !readable(context->eax, displayLayout.word.characters + 4) ||
      *reinterpret_cast<const DWORD *>(context->eax + displayLayout.word.length) != length ||
      *reinterpret_cast<const DWORD *>(context->eax + displayLayout.word.text) != context->ecx ||
      !readable(context->ecx, length + 1) ||
      *reinterpret_cast<const char *>(context->ecx + length))
    return;
  const auto text = readDisplayString(context->ecx, length);
  if (text.empty())
    return;
  *split = 1;
  buffer->from(text);
}

// attachRead 安装独立只读页面Hook，不要求全量解析或注入布局通过
bool attachRead(uintptr_t address, uintptr_t menuAddress)
{
  HookParam hp;
  hp.codepage = 932;
  hp.type = USING_STRING | USING_SPLIT | NO_CONTEXT | FULL_STRING;
  bool ok = false;
  if (address)
  {
    hp.address = address;
    hp.text_fun = readPage;
    ok = NewHook(hp, "YurisDialogue");
  }
  if (menuAddress)
  {
    hp.address = menuAddress;
    hp.text_fun = readMenuPage;
    ok = NewHook(hp, "YurisChoices") || ok;
  }
  return ok;
}

// attach installs every independently confirmed display path. A failed menu
// install leaves dialogue embedding intact, and unsupported text stays original.
bool attach()
{
  prepareLayout();
  wchar_t executable[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return false;
  std::wstring configuration(executable);
  const auto slash = configuration.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return false;
  configuration.resize(slash + 1); configuration += L"uif_config.json";
  if (GetFileAttributesW(configuration.c_str()) != INVALID_FILE_ATTRIBUTES ||
      !Util::FindImportEntry(processStartAddress, reinterpret_cast<DWORD>(TextOutA)) ||
      !Util::FindImportEntry(processStartAddress, reinterpret_cast<DWORD>(CreateFontIndirectA))) return false;
  HookParam hp;
  hp.codepage = 932;
  hp.type = USING_STRING | USING_SPLIT | NO_CONTEXT | EMBED_ABLE | EMBED_DYNA_SJIS;
  hp.embed_hook_font = F_TextOutA | F_CreateFontIndirectA;
  bool word = false, menu = false;
  if (displayLayout.word.address) {
    hp.address = displayLayout.word.address; hp.text_fun = readWord; hp.embed_fun = writeWord;
    word = NewHook(hp, "EmbedYuris");
  }
  if (displayLayout.menu.address) {
    HookParam restore;
    restore.address = displayLayout.menu.restore; restore.type = NO_CONTEXT; restore.text_fun = restoreMenu;
    if (NewHook(restore, "YurisDisplayRestore")) {
      hp.address = displayLayout.menu.address; hp.text_fun = readMenu; hp.embed_fun = writeMenu;
      menu = NewHook(hp, "EmbedYuris");
    }
  }

  return word || menu;
}
}
