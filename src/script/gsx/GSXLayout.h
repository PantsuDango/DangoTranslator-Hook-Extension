#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
namespace gsxlayout {
enum Field {
  Setter,
  Render,
  Frame,
  Objects,
  Dirty,
  Dirty2,
  Buffer,
  Length,
  Type,
  Stride,
  RenderEnd,
  FontCapture
};
struct Layout {
  std::array<uint32_t, 12> fields{};
  bool display = true;
};
// memory 逐区核对内存权限和容量，不接受溢出或保护页
inline bool memory(uintptr_t p, size_t n, bool writable = false) {
  if (!p || !n || p > UINTPTR_MAX - n)
    return false;
  auto end = p + n;
  while (p < end) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery((void *)p, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
      return false;
    auto prot = m.Protect & 255;
    bool write = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                 prot == PAGE_EXECUTE_READWRITE ||
                 prot == PAGE_EXECUTE_WRITECOPY;
    if (!write &&
        (writable || (prot != PAGE_READONLY && prot != PAGE_EXECUTE_READ)))
      return false;
    auto next = (uintptr_t)m.BaseAddress + m.RegionSize;
    if (next <= p)
      return false;
    p = next < end ? next : end;
  }
  return true;
}
// created 取得进程创建身份，避免PID复用
inline uint64_t created() {
  FILETIME c{}, e{}, k{}, u{};
  return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)
             ? uint64_t(c.dwHighDateTime) << 32 | c.dwLowDateTime
             : 0;
}
// load 重放当前镜像证据，独立降级显示能力
inline bool load(Layout &out) {
  auto name = L"Local\\Dango.GSX.Layout." +
              std::to_wstring(GetCurrentProcessId()) + L"." +
              std::to_wstring(created());
  HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (!map)
    return false;
  auto view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 65536);
  if (!view) {
    CloseHandle(map);
    return false;
  }
  std::array<BYTE, 65536> raw{};
  memcpy(raw.data(), view, raw.size());
  UnmapViewOfFile(view);
  CloseHandle(map);
  size_t at = 8;
  // word 有界解码协议整数
  auto word = [&](uint32_t &v) {
    if (at + 4 > raw.size())
      return false;
    memcpy(&v, raw.data() + at, 4);
    at += 4;
    return true;
  };
  uint32_t version = 0, size = 0, base = 0, count = 0;
  if (memcmp(raw.data(), "DGGSXL01", 8) || !word(version) || version != 2 ||
      !word(size) || size > 128 * 1024 * 1024 || !word(base) || !word(count) ||
      count != 12)
    return false;
  auto module = (uintptr_t)GetModuleHandleW(nullptr);
  auto dos = (IMAGE_DOS_HEADER *)module;
  if (!memory(module, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
      dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
    return false;
  auto nt = (IMAGE_NT_HEADERS32 *)(module + dos->e_lfanew);
  if (!memory((uintptr_t)nt, sizeof(*nt)) ||
      nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.SizeOfImage != size ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
    return false;
  Layout next;
  for (auto &v : next.fields)
    if (!word(v))
      return false;
  if (next.fields[Buffer] != 0x20c || next.fields[Length] != 0x210 ||
      next.fields[Type] != 0x334 ||
      (next.fields[Stride] && next.fields[Stride] != 0x41c))
    return false;
  if (!word(count) || count < 3 || count > 32)
    return false;
  std::array<bool, 3> readGuards{};
  bool fontGuard = false;
  for (unsigned n = 0; n < count; n++) {
    uint32_t rva = 0, len = 0, fix = 0, group = 0;
    if (!word(rva) || !word(len) || !word(fix) || !word(group) || group > 1 ||
        !len || len > 16384 || fix > 4096 || at + len > raw.size() ||
        rva >= size || len > size - rva)
      return false;
    std::vector<BYTE> guard(raw.begin() + at, raw.begin() + at + len);
    at += len;
    for (unsigned k = 0; k < fix; k++) {
      uint32_t off = 0;
      if (!word(off) || off > len || 4 > len - off)
        return false;
      uint32_t v;
      memcpy(&v, guard.data() + off, 4);
      v += uint32_t(module) - base;
      memcpy(guard.data() + off, &v, 4);
    }
    for (unsigned k = 0; k < 3; k++) {
      auto key = k == 0 ? Setter : k == 1 ? Render : RenderEnd;
      if (rva == next.fields[key] && !group &&
          len >= (key == RenderEnd ? 5u : 16u))
        readGuards[k] = true;
    }
    if (group && next.fields[FontCapture] >= rva + 6 &&
        uint64_t(next.fields[FontCapture]) + 6 <= uint64_t(rva) + len)
      fontGuard = true;
    if (!memory(module + rva, len) ||
        memcmp((void *)(module + rva), guard.data(), len)) {
      if (!group)
        return false;
      next.display = false;
    }
  }
  if (!readGuards[0] || !readGuards[1] || !readGuards[2])
    return false;
  if (!fontGuard || !next.fields[Stride] || !next.fields[Frame] ||
      !next.fields[FontCapture] || !next.fields[Dirty] || !next.fields[Dirty2])
    next.display = false;
  auto sections = IMAGE_FIRST_SECTION(nt);
  if (nt->FileHeader.NumberOfSections > 96 ||
      !memory((uintptr_t)sections,
              sizeof(*sections) * nt->FileHeader.NumberOfSections))
    return false;
  for (auto key : {Setter, Render, RenderEnd, Frame, FontCapture}) {
    auto v = next.fields[key];
    bool executable = false;
    for (unsigned n = 0; n < nt->FileHeader.NumberOfSections; n++) {
      auto &s = sections[n];
      if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
          v >= s.VirtualAddress && v - s.VirtualAddress < s.Misc.VirtualSize)
        executable = true;
    }
    if (!executable) {
      if (key != Frame && key != FontCapture)
        return false;
      next.display = false;
    }
  }
  out = next;
  return true;
}
} // namespace gsxlayout
