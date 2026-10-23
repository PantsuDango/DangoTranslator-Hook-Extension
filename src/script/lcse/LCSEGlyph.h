#pragma once

#include <windows.h>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace LCSEGlyph
{
struct Entry { WORD code; wchar_t character; };

// Bitmap 保存经过两阶段API核对的有界字形，像素层不接收裸外部位图指针
struct Bitmap
{
    GLYPHMETRICS metrics{};
    UINT format = 0;
    size_t stride = 0;
    std::vector<BYTE> bytes;
};

// legalCode 只接受经过原生分类器确认的双字节字位
inline bool legalCode(WORD code)
{
    const auto lead = code >> 8, trail = code & 255;
    return ((lead >= 0x81 && lead <= 0x9f) || (lead >= 0xe0 && lead <= 0xfc)) &&
        ((trail >= 0x40 && trail <= 0x7e) || (trail >= 0x80 && trail <= 0xfc));
}

// safeGlyph 拒绝私用字形、代理字符、低控制及不可见格式控制
inline bool safeGlyph(wchar_t value)
{
    return value >= 0x20 && value != 0x7f && !(value >= 0x80 && value <= 0x9f) &&
        !(value >= 0xd800 && value <= 0xf8ff) && value != 0xfffd && value != 0xffff && value != 0xfffe &&
        value != 0x00ad && !(value >= 0x0600 && value <= 0x0605) && value != 0x061c && value != 0x06dd &&
        value != 0x070f && !(value >= 0x0890 && value <= 0x0891) && value != 0x08e2 && value != 0x180e &&
        !(value >= 0x200b && value <= 0x200f) && !(value >= 0x202a && value <= 0x202e) &&
        !(value >= 0x2060 && value <= 0x206f) && value != 0xfeff && !(value >= 0xfff9 && value <= 0xfffb);
}

// vacantCode 只借用原生CP932未定义或私用字位，正常日文字形始终不改向
inline bool vacantCode(WORD code)
{
    if (!legalCode(code)) return false;
    const char raw[] = {char(code >> 8), char(code & 255)};
    wchar_t character = 0;
    const auto count = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw, 2, &character, 1);
    return !count || (count == 1 && character >= 0xe000 && character <= 0xf8ff);
}

// resolveFace 通过字体枚举确认请求字体存在，并解析系统本地化名称而不接受静默替代
inline std::wstring resolveFace(HDC dc, const std::wstring &requested)
{
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    requested.copy(query.lfFaceName, LF_FACESIZE - 1);
    std::wstring actual;
    EnumFontFamiliesExW(dc, &query, [](const LOGFONTW *font, const TEXTMETRICW *, DWORD type, LPARAM context) -> int
    {
        if (!(type & TRUETYPE_FONTTYPE)) return 1;
        *reinterpret_cast<std::wstring *>(context) = font->lfFaceName;
        return 0;
    }, reinterpret_cast<LPARAM>(&actual), 0);
    return actual;
}

// matchesFace 只接受当前选中字体名称表中的同族Unicode别名，拒绝静默替换为其他字体
inline bool matchesFace(HDC dc, const std::wstring &expected, const std::wstring &actual)
{
    if (expected.empty() || actual.empty()) return false;
    if (_wcsicmp(expected.c_str(), actual.c_str()) == 0) return true;
    constexpr DWORD nameTable = 0x656d616e; // GetFontData使用小端tag表示name
    const DWORD size = GetFontData(dc, nameTable, 0, nullptr, 0);
    if (size == GDI_ERROR || size < 6 || size > 1024 * 1024) return false;
    std::vector<BYTE> bytes(size);
    if (GetFontData(dc, nameTable, 0, bytes.data(), size) != size) return false;
    const auto word = [&](size_t at) { return WORD(WORD(bytes[at]) << 8 | bytes[at + 1]); };
    const auto format = word(0), count = word(2), storage = word(4);
    if (format > 1 || count > 4096 || size_t(count) * 12 + 6 > bytes.size() ||
        storage < size_t(count) * 12 + 6 || storage > bytes.size()) return false;
    bool expectedFound = false, actualFound = false;
    for (size_t index = 0; index < count; ++index)
    {
        const size_t at = 6 + index * 12;
        const auto platform = word(at), encoding = word(at + 2), name = word(at + 6);
        const auto length = word(at + 8), offset = word(at + 10);
        if ((platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10))) ||
            (name != 1 && name != 4 && name != 16)) continue;
        const size_t start = size_t(storage) + offset;
        if (!length || length % 2 || length / 2 >= LF_FACESIZE || start > bytes.size() || length > bytes.size() - start) continue;
        std::wstring alias;
        for (size_t cursor = start; cursor < start + length; cursor += 2)
        {
            const auto unit = word(cursor);
            if (!safeGlyph(wchar_t(unit))) { alias.clear(); break; }
            alias += wchar_t(unit);
        }
        if (!alias.empty())
        {
            expectedFound |= _wcsicmp(expected.c_str(), alias.c_str()) == 0;
            actualFound |= _wcsicmp(actual.c_str(), alias.c_str()) == 0;
        }
    }
    return expectedFound && actualFound;
}

// releaseRestoredFailureFont 仅释放原生失败分支已从HDC移除的新字体，调用方必须确认失败返回地址
inline bool releaseRestoredFailureFont(HDC dc, HGDIOBJ removed)
{
    if (!dc || !removed || removed == HGDI_ERROR || GetObjectType(removed) != OBJ_FONT ||
        !GetCurrentObject(dc, OBJ_FONT) || GetCurrentObject(dc, OBJ_FONT) == removed) return false;
    return DeleteObject(removed) != FALSE;
}

// SelectedFont 为本次测量和位图调用恢复调用者原有字体，不取得HDC所有权
class SelectedFont
{
    HDC dc = nullptr;
    HGDIOBJ previous = nullptr;
public:
    SelectedFont(HDC target, HFONT font) : dc(target)
    {
        if (dc && font)
        {
            previous = SelectObject(dc, font);
            if (previous == HGDI_ERROR) previous = nullptr;
        }
    }
    SelectedFont(const SelectedFont &) = delete;
    SelectedFont &operator=(const SelectedFont &) = delete;
    ~SelectedFont() { if (previous) SelectObject(dc, previous); }
    bool valid() const { return previous != nullptr; }
};

// Renderer 为完整快照提供不可变字位、中文缺字检查及有界字体缓存
class Renderer
{
    struct Font { LOGFONTW spec; HFONT handle; };
    std::mutex mutex;
    std::map<WORD, wchar_t> glyphs;
    std::wstring face;
    std::vector<Font> fonts;
    bool prepared = false;
    std::string failure;

    // cachedFont 复用至多64种原始字号样式，只核对字体身份，不让其他句子的缺字阻断当前绘制
    HFONT cachedFont(HDC dc, const LOGFONTW &original)
    {
        LOGFONTW spec = original;
        spec.lfCharSet = DEFAULT_CHARSET;
        ZeroMemory(spec.lfFaceName, sizeof(spec.lfFaceName));
        face.copy(spec.lfFaceName, LF_FACESIZE - 1);
        for (const auto &font : fonts)
            if (!memcmp(&font.spec, &spec, sizeof(spec))) return font.handle;
        if (fonts.size() >= 64) { failure = "font_cache_limit"; return nullptr; }
        auto font = CreateFontIndirectW(&spec);
        if (!font) { failure = "font_create_failed"; return nullptr; }
        bool valid = false;
        {
            SelectedFont selected(dc, font);
            wchar_t actual[LF_FACESIZE]{};
            valid = selected.valid() && GetTextFaceW(dc, LF_FACESIZE, actual) > 0 && matchesFace(dc, face, actual);
            if (!valid) failure = "font_identity_mismatch";
        }
        if (!valid) { DeleteObject(font); return nullptr; }
        fonts.push_back({spec, font});
        return font;
    }

public:
    Renderer() = default;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    ~Renderer()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &font : fonts) DeleteObject(font.handle);
    }

    // preparationFailure 只报告资源准备阶段原因，不输出剧情或用户文字
    std::string preparationFailure()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return failure;
    }

    // prepare 只验证不可变字位及原生来源，字体与缺字在实际显示时逐句处理，不作为开启门槛
    bool prepare(const std::vector<Entry> &entries, const std::vector<std::vector<BYTE>> &sources,
        const std::vector<WORD> &reserved, const std::wstring &fontFace, const LOGFONTW &)
    {
        std::lock_guard<std::mutex> lock(mutex);
        failure = "invalid_glyph_table";
        if (prepared || entries.empty() || entries.size() > 60 * 188 || fontFace.empty() || fontFace.size() >= LF_FACESIZE) return false;
        std::set<WORD> used(reserved.begin(), reserved.end());
        for (const auto &source : sources)
            for (size_t i = 0; i + 1 < source.size(); ++i)
            {
                const auto code = WORD(WORD(source[i]) << 8 | source[i + 1]);
                if (legalCode(code)) used.insert(code);
            }
        std::map<WORD, wchar_t> candidate;
        std::set<wchar_t> characters;
        for (const auto &entry : entries)
        {
            if (!vacantCode(entry.code) || used.count(entry.code) || !safeGlyph(entry.character) ||
                !candidate.emplace(entry.code, entry.character).second || !characters.insert(entry.character).second) return false;
        }
        glyphs.swap(candidate);
        face = fontFace;
        prepared = true;
        failure.clear();
        return true;
    }

    // contains 区分扩展字位与应继续走原生API的正常CP932字形
    bool contains(UINT code)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return prepared && code <= 0xffff && glyphs.count(WORD(code)) != 0;
    }

    // outline 保持原生两阶段缓冲协议和矩阵，失败或短缓冲也恢复原字体
    DWORD outline(HDC dc, UINT code, UINT format, LPGLYPHMETRICS metrics, DWORD size, LPVOID buffer, const MAT2 *matrix)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!prepared || code > 0xffff || !dc || !metrics || !matrix ||
            (format != GGO_BITMAP && format != GGO_GRAY8_BITMAP)) return GDI_ERROR;
        const auto glyph = glyphs.find(WORD(code));
        if (glyph == glyphs.end()) return GDI_ERROR;
        LOGFONTW style{};
        const auto current = GetCurrentObject(dc, OBJ_FONT);
        if (!current || GetObjectW(current, sizeof(style), &style) != sizeof(style)) return GDI_ERROR;
        const auto font = cachedFont(dc, style);
        if (!font) return GDI_ERROR;
        SelectedFont selected(dc, font);
        if (!selected.valid()) return GDI_ERROR;
        // 只检查当前字符，缺字或查询失败由既有整句回退处理，不撤销已加载资源
        WORD index = 0xffff;
        if (GetGlyphIndicesW(dc, &glyph->second, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR)
        { failure = "glyph_query_failed"; return GDI_ERROR; }
        if (index == 0xffff || index == 0) { failure = "glyph_missing"; return GDI_ERROR; }
        return GetGlyphOutlineW(dc, glyph->second, format, metrics, size, buffer, matrix);
    }

    // bitmap 同时检查指标、真实步长和二次读取结果，失败时不暴露半成品字形
    bool bitmap(HDC dc, UINT code, UINT format, const MAT2 &matrix, Bitmap &result)
    {
        result = {};
        if (!dc || (format != GGO_BITMAP && format != GGO_GRAY8_BITMAP) || code > 0xffff) return false;
        const bool mapped = contains(code);
        if (!mapped && code > 255 && vacantCode(WORD(code))) return false;
        Bitmap candidate;
        candidate.format = format;
        const auto fetch = [&](GLYPHMETRICS *metrics, DWORD size, void *buffer)
        {
            return mapped ? outline(dc, code, format, metrics, size, buffer, &matrix) :
                GetGlyphOutlineA(dc, code, format, metrics, size, buffer, &matrix);
        };
        const auto size = fetch(&candidate.metrics, 0, nullptr);
        if (size == GDI_ERROR || size > 1024 * 1024 || candidate.metrics.gmBlackBoxX > 512 || candidate.metrics.gmBlackBoxY > 512) return false;
        candidate.stride = format == GGO_GRAY8_BITMAP ? (candidate.metrics.gmBlackBoxX + 3) & ~size_t(3) :
            ((candidate.metrics.gmBlackBoxX + 31) / 32) * 4;
        // GDI返回的容量可能包含额外尾行，原生光栅只读取指标指定的有效行
        const auto rasterBytes = candidate.stride * candidate.metrics.gmBlackBoxY;
        if (rasterBytes > size) return false;
        candidate.bytes.resize(size);
        if (size)
        {
            GLYPHMETRICS repeated{};
            if (fetch(&repeated, size, candidate.bytes.data()) != size || memcmp(&repeated, &candidate.metrics, sizeof(repeated))) return false;
        }
        candidate.bytes.resize(rasterBytes);
        if (format == GGO_GRAY8_BITMAP)
            for (DWORD y = 0; y < candidate.metrics.gmBlackBoxY; ++y)
                for (DWORD x = 0; x < candidate.metrics.gmBlackBoxX; ++x)
                    if (candidate.bytes[y * candidate.stride + x] > 64) return false;
        result = std::move(candidate);
        return true;
    }
};
}
