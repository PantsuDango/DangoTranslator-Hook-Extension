#pragma once
#include "PJADVLayout.h"
std::wstring g_pjadvStaticGlyphs;
std::vector<unsigned char> g_pjadvStaticFont;
HANDLE g_pjadvStaticFontHandle = nullptr;
void solvefont(HookParam hp);
namespace
{
std::mutex pjadvTextMutex;
std::map<std::string, char *> pjadvTexts;
size_t pjadvTextBytes = 0;
void pjadvEmbedText(hook_context *context, TextBuffer buffer, HookParam *param)
{
  const auto text = buffer.strA();
  if (text.empty() || text.size() > 200 || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) return;
  std::lock_guard<std::mutex> lock(pjadvTextMutex);
  auto found = pjadvTexts.find(text);
  if (found == pjadvTexts.end())
  {
    const auto size = text.size() + 1;
    if (size > 64u * 1024u * 1024u - pjadvTextBytes) return;
    auto stored = new (std::nothrow) char[size];
    if (!stored) return;
    memcpy(stored, text.c_str(), size);
    found = pjadvTexts.emplace(text, stored).first;
    pjadvTextBytes += size;
  }
  context->offset(param->offset, *param) = reinterpret_cast<uintptr_t>(found->second);
}
bool pjadvReadFile(const std::wstring &path, std::vector<unsigned char> &bytes, size_t limit)
{
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  const bool valid = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(limit);
  bytes.assign(valid ? static_cast<size_t>(size.QuadPart) : 0, 0);
  DWORD count = 0;
  const bool read = valid && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
  CloseHandle(file);
  return read;
}
bool loadPJADVResources()
{
  wchar_t executable[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  std::wstring directory(executable, length);
  directory.resize(directory.find_last_of(L"\\/") + 1);
  const auto glyphPath = directory + L"dango_cache\\pjadv_pua.bin";
  const DWORD attributes = GetFileAttributesW(glyphPath.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
  {
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    if (GetFileAttributesW((directory + L"dango_cache\\pjadv_resource_install.json").c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD manifestError = GetLastError();
    return manifestError == ERROR_FILE_NOT_FOUND || manifestError == ERROR_PATH_NOT_FOUND;
  }
  std::vector<unsigned char> bytes;
  if (!pjadvReadFile(glyphPath, bytes, 8 + 1880 * 2) || bytes.size() != 8 + 1880 * 2 || memcmp(bytes.data(), "DGPJPUA1", 8)) return false;
  for (size_t offset = 8; offset < bytes.size(); offset += 2)
  {
    wchar_t glyph = static_cast<wchar_t>(bytes[offset] | bytes[offset + 1] << 8);
    if (glyph >= 0xd800 && glyph <= 0xdfff) return false;
    g_pjadvStaticGlyphs.push_back(glyph);
  }
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&loadPJADVResources), &module)) return false;
  const DWORD moduleLength = GetModuleFileNameW(module, executable, MAX_PATH);
  if (!moduleLength || moduleLength >= MAX_PATH) return false;
  std::wstring fontPath(executable, moduleLength);
  for (int count = 0; count < 3; ++count)
  {
    const auto separator = fontPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return false;
    fontPath.resize(separator);
  }
  fontPath += L"\\config\\SourceHanSansSC-Regular.otf";
  if (!pjadvReadFile(fontPath, g_pjadvStaticFont, 32u * 1024u * 1024u)) return false;
  DWORD fontCount = 0;
  g_pjadvStaticFontHandle = AddFontMemResourceEx(g_pjadvStaticFont.data(), static_cast<DWORD>(g_pjadvStaticFont.size()), nullptr, &fontCount);
  if (!g_pjadvStaticFontHandle || !fontCount) return false;
  HookParam font;
  font.embed_hook_font = F_GetGlyphOutlineA | F_GetGlyphOutlineW | F_CreateFontIndirectA | F_CreateFontIndirectW | F_MultiByteToWideChar;
  solvefont(font);
  return true;
}
}
