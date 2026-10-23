#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include "PJADVLayout.generated.h"

namespace DangoPJADV
{
struct Pattern
{
  std::vector<unsigned char> bytes;
  std::vector<unsigned char> masks;
};

// parsePattern 解析由共享布局定义生成的掩码特征
inline Pattern parsePattern(const char *definition)
{
  Pattern result;
  std::istringstream input(definition);
  std::string token;
  while (input >> token)
  {
    if (token == "??")
    {
      result.bytes.push_back(0);
      result.masks.push_back(0);
    }
    else
    {
      if (token.size() != 2) return {};
      char *end = nullptr;
      const auto value = strtoul(token.c_str(), &end, 16);
      if (!end || *end || value > 255) return {};
      result.bytes.push_back(static_cast<unsigned char>(value));
      result.masks.push_back(255);
    }
  }
  return result;
}

// matchPattern 只比较已核对的指令字节，不比较游戏内容或重定位地址
inline bool matchPattern(const unsigned char *data, size_t size, const Pattern &pattern)
{
  if (pattern.bytes.size() > size) return false;
  for (size_t index = 0; index < pattern.bytes.size(); ++index)
    if ((data[index] & pattern.masks[index]) != pattern.bytes[index]) return false;
  return true;
}

// readableRange 拒绝未提交或受保护的模块区域，避免扫描不可读代码页
inline bool readableRange(const unsigned char *begin, size_t size)
{
  const auto end = reinterpret_cast<uintptr_t>(begin) + size;
  auto address = reinterpret_cast<uintptr_t>(begin);
  if (end < address) return false;
  while (address < end)
  {
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= address) return false;
    address = next;
  }
  return true;
}

// findEntry 在模块的可执行节中唯一定位入口，与Go预检使用同一份结构特征
inline uintptr_t findEntry(HMODULE module, const char *name)
{
  const DangoPJADVFunctionSpec *spec = nullptr;
  for (const auto &candidate : dangoPJADVFunctions)
    if (strcmp(candidate.name, name) == 0) spec = &candidate;
  if (!spec || !module) return 0;
  const auto base = reinterpret_cast<const unsigned char *>(module);
  if (!readableRange(base, sizeof(IMAGE_DOS_HEADER))) return 0;
  const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 1024 * 1024) return 0;
  const auto header = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
  if (!readableRange(reinterpret_cast<const unsigned char *>(header), sizeof(*header)) ||
      header->Signature != IMAGE_NT_SIGNATURE || header->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      header->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || header->FileHeader.NumberOfSections > 96) return 0;
  const auto sections = IMAGE_FIRST_SECTION(header);
  if (!readableRange(reinterpret_cast<const unsigned char *>(sections), sizeof(IMAGE_SECTION_HEADER) * header->FileHeader.NumberOfSections)) return 0;
  const auto pattern = parsePattern(spec->pattern);
  const auto check = parsePattern(spec->check);
  if (pattern.bytes.empty() || (*spec->check && check.bytes.empty())) return 0;
  uintptr_t result = 0;
  for (size_t sectionIndex = 0; sectionIndex < header->FileHeader.NumberOfSections; ++sectionIndex)
  {
    const auto &section = sections[sectionIndex];
    if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    const size_t size = (std::min)(section.Misc.VirtualSize, section.SizeOfRawData);
    if (section.SizeOfRawData > 32u * 1024u * 1024u ||
        static_cast<uint64_t>(section.VirtualAddress) + section.SizeOfRawData > header->OptionalHeader.SizeOfImage) return 0;
    const auto data = base + section.VirtualAddress;
    if (!readableRange(data, size)) return 0;
    for (size_t offset = 0; offset + pattern.bytes.size() <= size; ++offset)
    {
      if (!matchPattern(data + offset, size - offset, pattern) || spec->checkOffset > size - offset ||
          !matchPattern(data + offset + spec->checkOffset, size - offset - spec->checkOffset, check)) continue;
      if (result) return 0;
      result = reinterpret_cast<uintptr_t>(data + offset);
    }
  }
  return result;
}
}
