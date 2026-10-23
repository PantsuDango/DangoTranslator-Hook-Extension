#pragma once

#include "LCSEScene.h"
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

namespace LCSESnapshot
{
// Data 保存摘要绑定的不可变显示资源，解析成功本身不表示用户注入已经启用
struct Data
{
    std::array<BYTE, 32> fingerprint{}, digest{};
    std::vector<LCSEGlyph::Entry> glyphs;
    std::vector<WORD> reserved;
    std::wstring face;
    std::shared_ptr<const LCSEScene::TranslationMap> translations;
};

// hash 核对完整载荷摘要，不用程序名称或整份EXE摘要作为引擎能力白名单
inline bool hash(const BYTE *bytes, size_t size, std::array<BYTE, 32> &result)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH value = 0;
    DWORD length = DWORD(result.size());
    const bool valid = bytes && size <= 64 * 1024 * 1024 &&
        CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &value) &&
        CryptHashData(value, bytes, DWORD(size), 0) &&
        CryptGetHashParam(value, HP_HASHVAL, result.data(), &length, 0) && length == result.size();
    if (value) CryptDestroyHash(value);
    if (provider) CryptReleaseContext(provider, 0);
    return valid;
}

// parse 只接受外部预期摘要与资源指纹均一致的有界快照，不公布半成品映射
inline std::shared_ptr<const Data> parse(const std::vector<BYTE> &bytes,
    const std::array<BYTE, 32> &fingerprint, const std::array<BYTE, 32> &digest) noexcept
{
    try
    {
        const std::array<BYTE, 32> empty{};
        std::array<BYTE, 32> computed{};
        if (bytes.size() < 88 || bytes.size() > 64 * 1024 * 1024 || fingerprint == empty || digest == empty ||
            memcmp(bytes.data(), "LCSER001", 8) || memcmp(bytes.data() + 8, digest.data(), 32) ||
            memcmp(bytes.data() + 40, fingerprint.data(), 32) || !hash(bytes.data() + 40, bytes.size() - 40, computed) ||
            computed != digest) return {};
        size_t cursor = 72;
        const auto number = [&](size_t count, DWORD &value)
        {
            if (cursor > bytes.size() || count > bytes.size() - cursor) return false;
            value = 0; memcpy(&value, bytes.data() + cursor, count); cursor += count; return true;
        };
        DWORD glyphCount = 0, reservedCount = 0, mappingCount = 0, faceCount = 0;
        if (!number(4, glyphCount) || !number(4, reservedCount) || !number(4, mappingCount) || !number(4, faceCount) ||
            glyphCount > 60 * 188 || reservedCount > 60 * 188 || mappingCount > 100000 ||
            !faceCount || faceCount >= LF_FACESIZE ||
            uint64_t(glyphCount) * 4 + uint64_t(reservedCount) * 2 + uint64_t(faceCount) * 2 + uint64_t(mappingCount) * 10 > bytes.size() - cursor) return {};
        auto result = std::make_shared<Data>();
        result->fingerprint = fingerprint; result->digest = digest;
        std::set<WORD> codes, reserved;
        std::set<wchar_t> characters;
        WORD priorCode = 0;
        wchar_t priorCharacter = 0;
        for (DWORD index = 0; index < glyphCount; ++index)
        {
            DWORD code = 0, character = 0;
            if (!number(2, code) || !number(2, character) || !LCSEGlyph::vacantCode(WORD(code)) ||
                !LCSEGlyph::safeGlyph(wchar_t(character)) || code <= priorCode || character <= priorCharacter ||
                !codes.insert(WORD(code)).second || !characters.insert(wchar_t(character)).second) return {};
            result->glyphs.push_back({WORD(code), wchar_t(character)});
            priorCode = WORD(code); priorCharacter = wchar_t(character);
        }
        priorCode = 0;
        for (DWORD index = 0; index < reservedCount; ++index)
        {
            DWORD code = 0;
            if (!number(2, code) || !LCSEGlyph::legalCode(WORD(code)) || code <= priorCode || codes.count(WORD(code))) return {};
            reserved.insert(WORD(code)); result->reserved.push_back(WORD(code)); priorCode = WORD(code);
        }
        for (DWORD index = 0; index < faceCount; ++index)
        {
            DWORD unit = 0;
            if (!number(2, unit) || !LCSEGlyph::safeGlyph(wchar_t(unit))) return {};
            result->face += wchar_t(unit);
        }
        auto mapping = std::make_shared<LCSEScene::TranslationMap>();
        std::vector<BYTE> prior;
        const auto controls = [](const std::vector<BYTE> &text)
        {
            std::vector<BYTE> result;
            for (size_t index = 0; index < text.size(); ++index)
                if (text[index] < 0x20) result.push_back(text[index]);
                else if (IsDBCSLeadByteEx(932, text[index])) ++index;
            return result;
        };
        for (DWORD index = 0; index < mappingCount; ++index)
        {
            DWORD originalSize = 0, translatedSize = 0;
            if (!number(4, originalSize) || !number(4, translatedSize) || !originalSize || originalSize >= 0x400 ||
                !translatedSize || translatedSize >= 0x400 || uint64_t(originalSize) + translatedSize > bytes.size() - cursor) return {};
            std::vector<BYTE> original(bytes.begin() + cursor, bytes.begin() + cursor + originalSize); cursor += originalSize;
            std::vector<BYTE> translated(bytes.begin() + cursor, bytes.begin() + cursor + translatedSize); cursor += translatedSize;
            if (!LCSEDisplay::validText(original) || !LCSEDisplay::validText(translated) ||
                (index && !(prior < original)) || controls(original) != controls(translated)) return {};
            // 原文所有相邻合法字位都必须在保留表中，未登记中文字位不能进入译文
            for (size_t at = 0; at + 1 < original.size(); ++at)
            {
                const WORD code = WORD(WORD(original[at]) << 8 | original[at + 1]);
                if (LCSEGlyph::legalCode(code) && (!reserved.count(code) || codes.count(code))) return {};
            }
            for (size_t at = 0; at < translated.size(); ++at)
                if (IsDBCSLeadByteEx(932, translated[at]))
                {
                    const WORD first = translated[at];
                    const WORD code = WORD(first << 8 | translated[at + 1]);
                    ++at;
                    if (LCSEGlyph::vacantCode(code) && !codes.count(code)) return {};
                }
            prior = original;
            if (!mapping->emplace(std::move(original), std::move(translated)).second) return {};
        }
        if (cursor != bytes.size()) return {};
        result->translations = std::move(mapping);
        return result;
    }
    catch (...) { return {}; }
}

// load 通过单一只读句柄读取预先生成的缓存，加载与呈现及用户启用保持分离
inline std::shared_ptr<const Data> load(const std::wstring &path,
    const std::array<BYTE, 32> &fingerprint, const std::array<BYTE, 32> &digest) noexcept
{
    HANDLE file = INVALID_HANDLE_VALUE;
    try
    {
        if (path.empty() || path.size() >= 32768 || path.find(wchar_t(0)) != std::wstring::npos) return {};
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 88 || size.QuadPart > 64 * 1024 * 1024)
        { CloseHandle(file); return {}; }
        std::vector<BYTE> bytes(size_t(size.QuadPart));
        DWORD received = 0;
        const bool valid = ReadFile(file, bytes.data(), DWORD(bytes.size()), &received, nullptr) && received == bytes.size();
        CloseHandle(file); file = INVALID_HANDLE_VALUE;
        return valid ? parse(bytes, fingerprint, digest) : std::shared_ptr<const Data>{};
    }
    catch (...) { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return {}; }
}
}
