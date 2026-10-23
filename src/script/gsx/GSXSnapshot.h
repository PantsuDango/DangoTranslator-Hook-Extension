#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <wincrypt.h>
#include <windows.h>
#pragma comment(lib, "advapi32.lib")

namespace gsxsnapshot {
constexpr DWORD MaximumBytes = 64 * 1024 * 1024;
using Texts = std::map<std::wstring, std::wstring>;
// digest 核对实际加载字节，不相信文件内部声明的摘要
inline bool digest(const BYTE *bytes, DWORD size, std::array<BYTE, 32> &out) {
  HCRYPTPROV provider = 0;
  HCRYPTHASH hash = 0;
  DWORD n = 32;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                            CRYPT_VERIFYCONTEXT))
    return false;
  bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
            CryptHashData(hash, bytes, size, 0) &&
            CryptGetHashParam(hash, HP_HASHVAL, out.data(), &n, 0) && n == 32;
  if (hash)
    CryptDestroyHash(hash);
  CryptReleaseContext(provider, 0);
  return ok;
}
// whitespace 与Go缓存键的首尾空白规则一致
inline bool whitespace(wchar_t c) {
  return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
         c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
// normalize 只整理换行及首尾空白，禁止模糊匹配
inline std::wstring normalize(const std::wstring &text) {
  std::wstring out;
  for (auto c : text)
    if (c != L'\r')
      out += c;
  size_t a = 0, b = out.size();
  while (a < b && whitespace(out[a]))
    a++;
  while (b > a && whitespace(out[b - 1]))
    b--;
  return out.substr(a, b - a);
}
// safe 核对有界Unicode译文，不兼容控制和换行由构建器逐句回退
inline bool safe(const std::wstring &original,
                 const std::wstring &translation) {
  if (original.empty() || normalize(original) != original ||
      translation.empty() || original.size() > 4095 ||
      translation.size() > 4095)
    return false;
  for (const auto *value : {&original, &translation})
    for (auto c : *value)
      if ((c < 32 && c != 10 && c != 9) || (c >= 0x7f && c < 0xa0) ||
          (c >= 0xd800 && c <= 0xdfff) || (c >= 0xe000 && c <= 0xf8ff) ||
          (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069) ||
          c == 0xfffd)
        return false;
  return true;
}
// parse 拒绝重复键、非法UTF16、越界及尾随字节，全部核对成功才发布
inline bool parse(const BYTE *bytes, DWORD size, Texts &texts) {
  if (!bytes || size < 24 || size > MaximumBytes ||
      memcmp(bytes, "DGGSBF01", 8))
    return false;
  DWORD version = 0, count = 0;
  uint64_t reserved = 0;
  memcpy(&version, bytes + 8, 4);
  memcpy(&count, bytes + 12, 4);
  memcpy(&reserved, bytes + 16, 8);
  if (version != 1 || reserved || count > 100000)
    return false;
  size_t offset = 24;
  Texts next;
  for (DWORD i = 0; i < count; i++) {
    std::wstring pair[2];
    for (auto &v : pair) {
      if (size - offset < 4)
        return false;
      DWORD n = 0;
      memcpy(&n, bytes + offset, 4);
      offset += 4;
      if (!n || n > 4095 || n > (size - offset) / 2)
        return false;
      v.resize(n);
      memcpy(v.data(), bytes + offset, n * 2);
      offset += n * 2;
      for (auto c : v)
        if (!c || c >= 0xd800 && c <= 0xdfff)
          return false;
    }
    if (!safe(pair[0], pair[1]) || !next.emplace(pair[0], pair[1]).second)
      return false;
  }
  if (offset != size)
    return false;
  texts = std::move(next);
  return true;
}
// load 持有禁止写入及替换的句柄读取已核对快照
inline bool load(const std::wstring &path, const std::array<BYTE, 32> &hash,
                 std::vector<BYTE> &bytes) {
  if (path.empty() || path.size() > 32760 ||
      path.find(L'\0') != std::wstring::npos)
    return false;
  auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 24 &&
            size.QuadPart <= MaximumBytes;
  if (ok) {
    bytes.resize(size_t(size.QuadPart));
    DWORD n = 0;
    ok = ReadFile(file, bytes.data(), DWORD(bytes.size()), &n, nullptr) &&
         n == bytes.size();
  }
  CloseHandle(file);
  std::array<BYTE, 32> actual{};
  Texts next;
  return ok && digest(bytes.data(), DWORD(bytes.size()), actual) &&
         actual == hash && parse(bytes.data(), DWORD(bytes.size()), next);
}
} // namespace gsxsnapshot
