#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "YurisLayout.generated.h"

// Pure, bounded layout analysis shared by the runtime and isolated native probes.
// Addresses are image addresses, never game identities or fixed RVAs.
namespace YurisLayout
{
struct Section { uint32_t address; const unsigned char *data; size_t size; bool executable; bool writable; };
using Image = std::vector<Section>;
inline uint32_t u32(const unsigned char *p) { uint32_t n; std::memcpy(&n, p, 4); return n; }
inline const unsigned char *range(const Image &image, uint32_t address, size_t size, bool code = false, bool writable = false)
{
  const unsigned char *result = nullptr;
  if (!size || uint64_t(address) + size > 0x100000000ULL) return nullptr;
  for (const auto &s : image)
    if (address >= s.address && uint64_t(address) + size <= uint64_t(s.address) + s.size) {
      if (result || (code && !s.executable) || (writable && !s.writable)) return nullptr;
      result = s.data + (address - s.address);
    }
  return result;
}
inline bool sjis(const Image &image, uint32_t address)
{
  const auto p = range(image, address, 256);
  if (!p) return false;
  for (int n = 0; n < 256; ++n)
    if (p[n] != ((n >= 0x81 && n <= 0x9f) || (n >= 0xe0 && n <= 0xfc))) return false;
  return true;
}
struct Word { uint32_t address = 0, object = 0, table = 0; unsigned text = 0, length = 0, characters = 0; };
inline Word word(const Image &image, uint32_t address, const unsigned char *p, size_t size)
{
  if (size < sizeof(wordCode)) return {};
  auto text = p[2], length = p[24], characters = p[54];
  if (text < 0x10 || text > 0x70 || length < text + 4 || length > 0x74 || characters != length + 4 || p[29] != length || p[32] != text) return {};
  unsigned char copy[sizeof(wordCode)]; std::memcpy(copy, p, sizeof(copy));
  copy[2] = copy[32] = 0x38; copy[24] = copy[29] = 0x60; copy[54] = 0x64;
  for (size_t n = 41; n < 45; ++n) copy[n] = wordCode[n];
  for (size_t n = 0; n < sizeof(copy); ++n) if (wordMask[n] && copy[n] != wordCode[n]) return {};
  const auto object = u32(p + 18), table = u32(p + 77), slot = u32(p + 10);
  if (object != u32(p + 48) || object == slot || !range(image, object, 4, false, true) || !range(image, slot, 4) || !sjis(image, table)) return {};
  // Follow the empty-input edge to the same object's character count and the
  // common terminated-buffer epilogue. Its distance depends on layout code size.
  const int64_t target = int64_t(address) + 45 + int32_t(u32(p + 41));
  if (target < int64_t(address) + sizeof(copy) || target > int64_t(address) + 1024 || target > UINT32_MAX) return {};
  const auto tail = range(image, uint32_t(target), 10, true);
  if (!tail || tail[0] != 0xa1 || u32(tail + 1) != object || tail[5] != 0x8b || tail[6] != 0x50 || tail[7] != characters || tail[8] != 0xeb) return {};
  const int64_t end = target + 10 + int8_t(tail[9]);
  if (end < int64_t(address) + sizeof(copy) || end >= target) return {};
  const auto ep = range(image, uint32_t(end), 15, true);
  const unsigned char cleanup[] = {0x8b,0x48,0,0xc6,0x04,0x0a,0,0x33,0xc0,0x83,0xc4,0x10,0x5b,0x5e,0xc3};
  if (!ep) return {};
  for (size_t n = 0; n < sizeof(cleanup); ++n) if (ep[n] != (n == 2 ? text + 4 : cleanup[n])) return {};
  return {address, object, table, text, length, characters};
}

// Decode only the integer instructions needed for the display loop. Unknown
// instructions stop this path; they never broaden the set of writable memory.
struct Ins { unsigned op = 0, reg = 0, rm = 0, base = 8, index = 8, scale = 0, size = 0; int32_t disp = 0; bool memory = false; };
inline Ins instruction(const unsigned char *p, size_t size)
{
  Ins i; if (!size) return i;
  i.op = p[0]; unsigned pos = 1;
  if (i.op == 0x0f) { if (size < 2) return {}; i.op = 0x100 | p[pos++]; }
  if (i.op == 0xe8 || i.op == 0x182 || i.op == 0x186) {
    if (size < pos + 4) return {}; i.disp = int32_t(u32(p + pos)); i.size = pos + 4; return i;
  }
  if (i.op >= 0x40 && i.op <= 0x47) { i.reg = i.op - 0x40; i.size = 1; return i; }
  switch (i.op) { case 0x8b: case 0x89: case 0x88: case 0x8d: case 0x33: case 0x03: case 0x01: case 0x3b: case 0x1b6: case 0x1be: break; default: return {}; }
  if (pos >= size) return {};
  auto m = p[pos++]; unsigned mod = m >> 6; i.reg = (m >> 3) & 7; i.rm = m & 7;
  i.memory = mod != 3;
  if (i.memory) {
    i.base = i.rm;
    if (i.rm == 4) {
      if (pos >= size) return {}; auto sib = p[pos++]; i.base = sib & 7; i.index = (sib >> 3) & 7; i.scale = sib >> 6;
      if (i.index == 4) i.index = 8;
    }
    unsigned displacement = mod == 1 ? 1 : (mod == 2 || (mod == 0 && i.base == 5) ? 4 : 0);
    if (mod == 0 && i.base == 5) i.base = 8;
    if (pos + displacement > size) return {};
    if (displacement == 1) i.disp = int8_t(p[pos]);
    if (displacement == 4) i.disp = int32_t(u32(p + pos));
    pos += displacement;
  }
  i.size = pos; return i;
}
struct Menu { uint32_t address = 0, restore = 0, table = 0; unsigned stack = 0; };
inline Menu menu(const Image &image, uint32_t address, const unsigned char *p, size_t size)
{
  const unsigned char prefix[] = {0x8b,0x7c,0x24,0,0x33,0xc0,0x8a,0x37,0x8b,0xcf,0x84,0xf6,0x74,8,0x41,0x40,0x8a,0x11,0x84,0xd2,0x75,0xf8,0x85,0xc0,0x0f,0x86};
  if (size < 40 || p[3] < 0x40 || p[3] > 0x7f) return {};
  for (size_t n = 0; n < sizeof(prefix); ++n) if (n != 3 && p[n] != prefix[n]) return {};
  const int64_t exit = 30 + int32_t(u32(p + 26));
  if (exit < 80 || exit > 768 || uint64_t(exit) + 7 > size) return {};
  const auto end = size_t(exit); unsigned pos = 30;
  auto next = [&]() { return pos < end ? instruction(p + pos, end - pos) : Ins{}; };
  auto zero = next();
  if (zero.op != 0x33 || zero.memory || zero.reg != zero.rm || (zero.reg != 3 && zero.reg != 5 && zero.reg != 6)) return {};
  const auto counter = zero.reg; pos += zero.size;
  // Prove the save/metadata setup is straight-line, preserves the text and
  // counter, and only writes scratch stack slots below the incoming parameters.
  auto setup = [&](const Ins &i) {
    if (!i.size || !i.memory || (i.op != 0x8b && i.op != 0x89 && i.op != 0x88 && i.op != 0x1b6 && i.op != 0x1be)) return false;
    if (i.op == 0x89 || i.op == 0x88) return i.base == 4 && i.index == 8 && i.disp >= 0 && i.disp <= 0x3c;
    if (i.reg == 0 || i.reg == 4 || i.reg == 7 || i.reg == counter) return false;
    if (i.base == 4 && i.index == 8) return i.disp >= 0 && i.disp <= 0x200 && i.disp != int32_t(p[3]);
    return uint32_t(i.disp) != 0 && range(image, uint32_t(i.disp), i.op == 0x8b ? 4 : 1) != nullptr;
  };
  while (pos < end && !(p[pos] == 0x8b && end - pos >= 4 && p[pos+1] == 0x7c && p[pos+2] == 0x24 && p[pos+3] == p[3])) {
    auto i = next(); if (!setup(i)) return {}; pos += i.size;
  }
  const auto loop = pos;
  if (end - pos < 4) return {}; pos += 4;
  auto lea = next();
  if (lea.op != 0x8d || lea.reg != 0 || !lea.memory || lea.scale || lea.disp || !((lea.base == 7 && lea.index == counter) || (lea.base == counter && lea.index == 7))) return {};
  pos += lea.size;
  while (pos < end) {
    auto i = next(); if (i.op == 0xe8) break;
    if (!setup(i)) return {}; pos += i.size;
  }
  auto call = next();
  if (call.op != 0xe8) return {};
  auto draw = int64_t(address) + pos + call.size + call.disp;
  if (draw <= 0 || draw > UINT32_MAX || !range(image, uint32_t(draw), 1, true) || (draw >= address && draw <= int64_t(address) + end)) return {};
  pos += call.size;
  auto width = next();
  if (!((width.op == 0x01 && width.reg == 0 && width.memory && width.base == 4 && width.index == 8 && width.disp >= 0 && width.disp <= 0x3c) ||
        (width.op == 0x03 && !width.memory && width.rm == 0 && width.reg != counter && width.reg != 4 && width.reg != 7 && width.reg != 0))) return {};
  pos += width.size;
  auto lead = next();
  if (lead.op != 0x1b6 || lead.reg == counter || lead.reg == 4 || lead.reg == 7 || !lead.memory || lead.scale || lead.disp || !((lead.base == 7 && lead.index == counter) || (lead.base == counter && lead.index == 7))) return {};
  pos += lead.size; auto step = next();
  if (step.op != 0x1b6 || step.reg == counter || !step.memory || step.base != lead.reg || step.index != 8 || !sjis(image, uint32_t(step.disp))) return {};
  const auto table = uint32_t(step.disp); pos += step.size;
  auto inc = next(); if (inc.op != 0x40 + step.reg || step.reg > 3) return {}; pos += inc.size;
  auto extend = next(); if (extend.op != 0x1be || extend.reg == counter || extend.reg == 4 || extend.reg == 7 || extend.memory || extend.rm != step.reg) return {}; pos += extend.size;
  auto add = next(); if (add.op != 0x03 || add.memory || add.reg != counter || add.rm != extend.reg) return {}; pos += add.size;
  const unsigned char count[] = {0x33,0xc0,0x8a,0x37,0x8b,0xcf,0x84,0xf6,0x74,8,0x41,0x40,0x8a,0x11,0x84,0xd2,0x75,0xf8};
  if (pos + sizeof(count) > end || std::memcmp(p + pos, count, sizeof(count))) return {}; pos += sizeof(count);
  auto cmp = next(); if (cmp.op != 0x3b || cmp.memory || cmp.reg != counter || cmp.rm != 0) return {}; pos += cmp.size;
  auto back = next(); if (back.op != 0x182 || int64_t(pos) + back.size + back.disp != loop) return {}; pos += back.size;
  while (pos < end) { auto i = next(); if (i.op != 0x8b || !i.memory || i.base != 4 || i.index != 8 || i.disp < 0 || i.disp > 0x3c || i.reg == 4 || i.reg == 7) return {}; pos += i.size; }
  if (pos != end || p[end] != 0x0f || p[end+1] != 0xb6 || (p[end+2] & 0xc7) != 5 || !range(image, u32(p+end+3), 1)) return {};
  return {address, uint32_t(uint64_t(address) + end), table, p[3]};
}
struct Layout { Word word; Menu menu; };
inline Layout find(const Image &image)
{
  Layout result; unsigned words = 0, menus = 0;
  for (const auto &s : image) if (s.executable) for (size_t n = 0; n + 40 <= s.size; ++n) {
    const auto p = s.data + n; const auto a = uint64_t(s.address) + n;
    if (a > UINT32_MAX) continue;
    if (p[0] == 0x8b && p[1] == 0x52) { auto w = word(image, uint32_t(a), p, s.size-n); if (w.address) { result.word = w; ++words; } }
    if (p[0] == 0x8b && p[1] == 0x7c && p[2] == 0x24) { auto m = menu(image, uint32_t(a), p, s.size-n); if (m.address) { result.menu = m; ++menus; } }
  }
  if (words != 1) result.word = {};
  if (menus != 1) result.menu = {};
  return result;
}
}
