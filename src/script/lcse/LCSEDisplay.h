#pragma once

#include <windows.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include "LCSEGlyph.h"

// LCSEDisplayLayout 保存通过独立结构校验的对象与画布入口
struct LCSEDisplayLayout
{
    std::map<std::string, uintptr_t> functions;
    std::map<std::string, std::vector<DWORD>> tables;

    // get 只读取已确认入口，不为缺失角色补猜测地址
    uintptr_t get(const char *name) const
    {
        const auto found = functions.find(name);
        return found == functions.end() ? 0 : found->second;
    }
};

// LCSEPresentationLayout 保存已核对的场景阶段和模块数据槽，不自动安装显示Hook
struct LCSEPresentationLayout
{
    std::map<std::string, uintptr_t> functions;
    std::map<std::string, uintptr_t> globals;
    std::map<std::string, uintptr_t> phases;
    int dcFrameOffset = 0;
    bool refresh = false;
    bool atomicPresentation = false;
    std::map<std::string, uintptr_t> submissionPhases;
    std::map<std::string, uintptr_t> refreshPhases;
    int refreshDCFrameOffset = 0, refreshObjectFrameOffset = 0;
    bool choices = false;
    bool hover = false;
    std::map<std::string, uintptr_t> hoverGlobals;
    std::map<std::string, uintptr_t> hoverPhases;
};

namespace LCSEDisplay
{
// Span 自持一次显示调用的文字和起点，选项换行只恢复原选项横坐标
struct Span
{
    std::vector<BYTE> text;
    int x = 0, y = 0;
    bool resetLineX = false;
    int width = 0, height = 0;
    int initialNesting = -1;
};

// readable 将外部对象读取限制在已提交且可访问的同一内存区域
inline bool readable(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto first = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return address >= first && address - first <= info.RegionSize && size <= info.RegionSize - (address - first);
}

// Canvas 自持与已确认布局相同的DIB，不调用游戏生命周期或借用游戏画布
class Canvas
{
    alignas(4) std::array<BYTE, 0x44> storage{};
    HBITMAP bitmap = nullptr;
    int width = 0, height = 0;
    bool attempted = false, prepared = false;

public:
    Canvas() = default;
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;
    ~Canvas()
    {
        if (bitmap) { GdiFlush(); DeleteObject(bitmap); }
    }

    // initialize 直接创建有界24位DIB，失败时不暴露位图或不完整对象
    bool initialize(int requestedWidth, int requestedHeight)
    {
        if (attempted || requestedWidth <= 0 || requestedHeight <= 0 || requestedWidth > 4096 || requestedHeight > 4096) return false;
        attempted = true;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = requestedWidth;
        info.bmiHeader.biHeight = requestedHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        const DWORD stride = (requestedWidth * 3 + 3) & ~DWORD(3);
        info.bmiHeader.biSizeImage = stride * DWORD(requestedHeight);
        const auto dc = GetDC(nullptr);
        if (!dc) return false;
        void *pixels = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (!bitmap || !pixels) return false;
        memcpy(storage.data(), &info.bmiHeader, sizeof(info.bmiHeader));
        const DWORD fields[] = {DWORD(reinterpret_cast<uintptr_t>(bitmap)), DWORD(reinterpret_cast<uintptr_t>(pixels)), stride, 3};
        memcpy(storage.data() + 0x34, fields, sizeof(fields));
        width = requestedWidth; height = requestedHeight;
        prepared = true;
        return true;
    }

    // fill 仅向自身已确认的24位画布填充完整矩形
    bool fill(int requestedWidth, int requestedHeight, BYTE red, BYTE green, BYTE blue)
    {
        if (!prepared || requestedWidth <= 0 || requestedHeight <= 0 || requestedWidth > width || requestedHeight > height) return false;
        auto pixels = reinterpret_cast<BYTE *>(*reinterpret_cast<DWORD *>(storage.data() + 0x38));
        const auto stride = *reinterpret_cast<DWORD *>(storage.data() + 0x3c);
        for (int y = 0; y < requestedHeight; ++y)
        {
            auto row = pixels + size_t(height - y - 1) * stride;
            for (int x = 0; x < requestedWidth; ++x)
            { row[x * 3] = blue; row[x * 3 + 1] = green; row[x * 3 + 2] = red; }
        }
        return true;
    }

    // rasterMask 按原生单色及64级灰度公式写入私有24位画布，先核对完整写入矩形
    bool rasterMask(int left, int top, const LCSEGlyph::Bitmap &glyph)
    {
        const auto &metrics = glyph.metrics;
        const auto stride = glyph.format == GGO_GRAY8_BITMAP ? (size_t(metrics.gmBlackBoxX) + 3) & ~size_t(3) :
            ((size_t(metrics.gmBlackBoxX) + 31) / 32) * 4;
        if (!prepared || (glyph.format != GGO_BITMAP && glyph.format != GGO_GRAY8_BITMAP) ||
            metrics.gmBlackBoxX > 512 || metrics.gmBlackBoxY > 512 || glyph.stride != stride ||
            glyph.bytes.size() != stride * metrics.gmBlackBoxY) return false;
        if (!metrics.gmBlackBoxX || !metrics.gmBlackBoxY) return true;
        if (left < 0 || top < 0 || metrics.gmBlackBoxX > DWORD(width) || metrics.gmBlackBoxY > DWORD(height) ||
            left > width - int(metrics.gmBlackBoxX) || top > height - int(metrics.gmBlackBoxY)) return false;
        if (glyph.format == GGO_GRAY8_BITMAP)
            for (DWORD y = 0; y < metrics.gmBlackBoxY; ++y)
                for (DWORD x = 0; x < metrics.gmBlackBoxX; ++x)
                    if (glyph.bytes[y * stride + x] > 64) return false;
        const auto pixels = reinterpret_cast<BYTE *>(*reinterpret_cast<DWORD *>(storage.data() + 0x38));
        const auto destinationStride = *reinterpret_cast<DWORD *>(storage.data() + 0x3c);
        for (DWORD y = 0; y < metrics.gmBlackBoxY; ++y)
        {
            auto row = pixels + size_t(height - top - int(y) - 1) * destinationStride + size_t(left) * 3;
            for (DWORD x = 0; x < metrics.gmBlackBoxX; ++x)
            {
                const unsigned coverage = glyph.format == GGO_GRAY8_BITMAP ? glyph.bytes[y * stride + x] :
                    ((glyph.bytes[y * stride + x / 8] & (0x80 >> (x % 8))) ? 64 : 0);
                for (size_t channel = 0; channel < 3; ++channel)
                    row[x * 3 + channel] = BYTE(int(row[x * 3 + channel]) + (0 - int(row[x * 3 + channel])) * int(coverage) / 64);
            }
        }
        return true;
    }

    // copyFrom 验证原生背景DIB的真实容量后复制到私有目标，拒绝把上一帧译文作为背景
    bool copyFrom(const void *source)
    {
        if (!prepared || !readable(reinterpret_cast<uintptr_t>(source), storage.size())) return false;
        std::array<BYTE, 0x44> header;
        memcpy(header.data(), source, header.size());
        const auto value = [&header](size_t offset) { DWORD result; memcpy(&result, header.data() + offset, sizeof(result)); return result; };
        WORD depth, planes;
        memcpy(&depth, header.data() + 0xe, 2); memcpy(&planes, header.data() + 0xc, 2);
        const auto stride = DWORD((width * 3 + 3) & ~3);
        const auto size = size_t(stride) * size_t(height);
        const auto destination = *reinterpret_cast<DWORD *>(storage.data() + 0x38);
        if (value(0) != 40 || planes != 1 || depth != 24 || value(4) != DWORD(width) || value(8) != DWORD(height) ||
            value(0x3c) != stride || value(0x40) != 3 || value(0x14) != size || !value(0x34) ||
            value(0x38) == destination || !readable(value(0x38), size)) return false;
        memcpy(reinterpret_cast<void *>(destination), reinterpret_cast<const void *>(value(0x38)), size);
        return true;
    }

    // assignPixels 只接收与自身容量一致的自持底部向上像素副本
    bool assignPixels(const std::vector<BYTE> &bytes)
    {
        const auto size = size_t((width * 3 + 3) & ~3) * size_t(height);
        if (!prepared || bytes.size() != size) return false;
        memcpy(reinterpret_cast<void *>(*reinterpret_cast<DWORD *>(storage.data() + 0x38)), bytes.data(), size);
        return true;
    }

    // matchesPixels 比较自持画布与不可变快照，不产生额外整帧分配
    bool matchesPixels(const std::vector<BYTE> &bytes) const
    {
        const auto size = size_t((width * 3 + 3) & ~3) * size_t(height);
        return prepared && bytes.size() == size && !memcmp(reinterpret_cast<const void *>(
            *reinterpret_cast<const DWORD *>(storage.data() + 0x38)), bytes.data(), size);
    }

    // mixMask 按原生256分母与向下取整合成已核对矩形，不读写任何外部画布
    bool mixMask(const Canvas &color, const Canvas &mask, int x, int y, int requestedWidth, int requestedHeight,
        int sourceX = 0, int sourceY = 0)
    {
        if (!prepared || !color.prepared || !mask.prepared || x < 0 || y < 0 || requestedWidth < 0 || requestedHeight < 0 ||
            x > width || y > height || requestedWidth > width - x || requestedHeight > height - y ||
            sourceX < 0 || sourceY < 0 || sourceX > color.width || sourceY > color.height || sourceX > mask.width || sourceY > mask.height ||
            requestedWidth > color.width - sourceX || requestedHeight > color.height - sourceY ||
            requestedWidth > mask.width - sourceX || requestedHeight > mask.height - sourceY) return false;
        const auto pixels = [](const Canvas &canvas) { return reinterpret_cast<BYTE *>(*reinterpret_cast<const DWORD *>(canvas.storage.data() + 0x38)); };
        const auto stride = [](const Canvas &canvas) { return *reinterpret_cast<const DWORD *>(canvas.storage.data() + 0x3c); };
        for (int row = 0; row < requestedHeight; ++row)
        {
            auto destination = pixels(*this) + size_t(height - y - row - 1) * stride(*this) + size_t(x) * 3;
            const auto colors = pixels(color) + size_t(color.height - sourceY - row - 1) * stride(color) + size_t(sourceX) * 3;
            const auto masks = pixels(mask) + size_t(mask.height - sourceY - row - 1) * stride(mask) + size_t(sourceX) * 3;
            for (int byte = 0; byte < requestedWidth * 3; ++byte)
            {
                const int value = (int(destination[byte]) - int(colors[byte])) * int(masks[byte]);
                const int scaled = value >= 0 ? value / 256 : -((-value + 255) / 256);
                destination[byte] = BYTE(int(colors[byte]) + scaled);
            }
        }
        return true;
    }

    // present 只呈现已验证的私有像素矩形，恢复目标HDC状态并回收临时源HDC
    bool present(HDC destination, int destinationX, int destinationY, int requestedWidth, int requestedHeight, int sourceX, int sourceY)
    {
        if (!prepared || !destination || sourceX < 0 || sourceY < 0 || requestedWidth <= 0 || requestedHeight <= 0 ||
            sourceX > width || sourceY > height || requestedWidth > width - sourceX || requestedHeight > height - sourceY) return false;
        const auto source = CreateCompatibleDC(destination);
        if (!source) return false;
        const auto previous = SelectObject(source, bitmap);
        bool success = false;
        if (previous && previous != HGDI_ERROR)
        {
            success = BitBlt(destination, destinationX, destinationY, requestedWidth, requestedHeight, source, sourceX, sourceY, SRCCOPY) != FALSE;
            if (!GdiFlush()) success = false;
            const auto restored = SelectObject(source, previous);
            if (!restored || restored == HGDI_ERROR) success = false;
        }
        if (!DeleteDC(source)) success = false;
        return success;
    }

    // object 返回自持对象地址，不返回外部共享画布
    void *object() { return prepared ? storage.data() : nullptr; }
    // object 只读访问自持画布，供同帧像素复制使用
    const void *object() const { return prepared ? storage.data() : nullptr; }
};

// validText 核对原生分类器接受的完整字节边界，扩展字位留给已验证字形表处理
inline bool validText(const std::vector<BYTE> &text)
{
    if (text.empty() || text.size() >= 0x400) return false;
    for (size_t index = 0; index < text.size(); ++index)
    {
        const auto code = text[index];
        if (code == 1 || code == 2 || code == 3 || (code >= 0x20 && code <= 0x7e) || (code >= 0xa1 && code <= 0xdf)) continue;
        if (!((code >= 0x81 && code <= 0x9f) || (code >= 0xe0 && code <= 0xfc)) || ++index == text.size()) return false;
        const auto trail = text[index];
        if (!((trail >= 0x40 && trail <= 0x7e) || (trail >= 0x80 && trail <= 0xfc))) return false;
    }
    return true;
}

// Message 只持有私有显示字段、目标画布和三个子画布，不构造原生消息对象
// 完整页先预检排版与字形后才写入私有遮罩，快照绑定和运行时启用仍由后续层核对
class Message
{
    LCSEDisplayLayout layout;
    Canvas target;
    std::array<Canvas, 3> children;
    alignas(4) std::array<BYTE, 0x550> storage{};
    bool attempted = false, prepared = false;
    bool rendered = false;
    bool composed = false;
    HWND window = nullptr;
    size_t pixelLimit = 0, canvasBytes = 0;

    struct Placement { UINT code; int left, top, mixTop; };
    std::map<UINT, LCSEGlyph::Bitmap> rasterGlyphs;
    std::vector<Placement> rasterPlacements;

    // field 只访问已通过共享复制布局确认的本对象字段
    DWORD &field(size_t offset) { return *reinterpret_cast<DWORD *>(storage.data() + offset); }

    // listed 只查询从实际代码引用核对的禁则和括号表
    bool listed(const char *kind, bool single, UINT code) const
    {
        const auto table = layout.tables.find(std::string(kind) + (single ? "_single" : "_double"));
        if (table == layout.tables.end()) return false;
        const auto value = single ? code << 8 : code;
        return std::find(table->second.begin(), table->second.end(), value) != table->second.end();
    }

public:
    explicit Message(const LCSEDisplayLayout &contract) : layout(contract) {}
    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;

    // initialize 按原生显示复制语义只复制样式和布局，原文、累计文本及游标均不改写
    bool initialize(const void *source, const std::vector<BYTE> &translated, size_t limit = 64 * 1024 * 1024)
    {
        if (attempted || !validText(translated) ||
            !readable(reinterpret_cast<uintptr_t>(source), storage.size())) return false;
        std::array<BYTE, 0x550> original;
        memcpy(original.data(), source, original.size());
        const auto value = [&original](size_t offset) { DWORD result; memcpy(&result, original.data() + offset, sizeof(result)); return result; };
        if (value(0x538) != 0 || !readable(value(0x3c), 0x44) || !readable(value(0x40), 0x118)) return false;
        std::array<BYTE, 0x44> frame;
        memcpy(frame.data(), reinterpret_cast<const void *>(value(0x3c)), frame.size());
        int frameWidth, frameHeight;
        WORD depth;
        memcpy(&frameWidth, frame.data() + 4, 4); memcpy(&frameHeight, frame.data() + 8, 4); memcpy(&depth, frame.data() + 0xe, 2);
        const int x = int(value(0xc4)), y = int(value(0xc8)), width = int(value(0xcc)), height = int(value(0xd0));
        const int innerX = int(value(0xd4)), innerY = int(value(0xd8)), innerWidth = int(value(0xdc)), innerHeight = int(value(0xe0));
        const int shadowX = int(value(0x518)), shadowY = int(value(0x51c)), fontHeight = int(value(0x44));
        if (depth != 24 || frameWidth <= 0 || frameHeight <= 0 || frameWidth > 4096 || frameHeight > 4096 ||
            width <= 0 || height <= 0 || width > frameWidth || height > frameHeight || x < 0 || y < 0 ||
            x > frameWidth - width || y > frameHeight - height || innerWidth <= 0 || innerHeight <= 0 ||
            innerWidth > width || innerHeight > height || innerX < x || innerY < y ||
            innerX - x > width - innerWidth || innerY - y > height - innerHeight || fontHeight <= 0 || fontHeight > 128 ||
            shadowX < 0 || shadowY < 0 || shadowX > 32 || shadowY > 32) return false;
        // 按对齐后的真实步长限制总DIB容量，不能只计算未对齐像素数
        const auto size = [](int w, int h) { return uint64_t((w * 3 + 3) & ~3) * uint64_t(h); };
        const auto bytes = size(frameWidth, frameHeight) + size(width, height) * 3;
        if (limit > 64 * 1024 * 1024 || bytes > limit) return false;
        pixelLimit = limit; canvasBytes = size_t(bytes);
        memcpy(&window, reinterpret_cast<const BYTE *>(uintptr_t(value(0x40))) + 0x114, sizeof(window));
        attempted = true;
        if (!target.initialize(frameWidth, frameHeight)) return false;
        for (auto &child : children) if (!child.initialize(width, height)) return false;
        field(0x3c) = DWORD(reinterpret_cast<uintptr_t>(target.object()));
        for (size_t index = 0; index < children.size(); ++index)
            field(0x30 + index * 4) = DWORD(reinterpret_cast<uintptr_t>(children[index].object()));
        // 只复制原生font-copy的样式字段，排除字形缓存、指针和析构责任
        memcpy(storage.data() + 0x44, original.data() + 0x44, 0x4c);
        memcpy(storage.data() + 0x90, original.data() + 0x90, 3);
        field(0x94) = value(0x94);
        storage[0xb8] = original[0xb8];
        field(0xbc) = value(0xbc); field(0xc0) = value(0xc0);
        memcpy(storage.data() + 0xc4, original.data() + 0xc4, 0x20);
        field(0xe4) = value(0xc4); field(0xe8) = value(0xc8);
        field(0xf4) = value(0xf4); field(0xf8) = value(0xf8);
        field(0x518) = value(0x518); field(0x51c) = value(0x51c); field(0x524) = value(0x524);
        for (const size_t offset : {0x521, 0x522, 0x52c, 0x52f, 0x530, 0x531, 0x532, 0x533, 0x534})
            storage[offset] = original[offset];
        // 按已核对的原生填色语义初始化，不重复创建DIB或调用不检查分配结果的原方法
        target.fill(frameWidth, frameHeight, 0, 0, 0);
        children[0].fill(width, height, original[0x52f], original[0x530], original[0x531]);
        children[1].fill(width, height, original[0x532], original[0x533], original[0x534]);
        children[2].fill(width, height, 255, 255, 255);
        memcpy(storage.data() + 0x108, translated.data(), translated.size());
        field(0xf0) = DWORD(translated.size());
        prepared = true;
        return prepared;
    }

    // initializeSnapshot 从自持样式与窗口值构造显示副本，不保留原生消息或窗口对象指针
    bool initializeSnapshot(const std::array<BYTE, 0x550> &style, const std::vector<BYTE> &text,
        int width, int height, HWND targetWindow, size_t limit = 64 * 1024 * 1024, bool clearedOrigin = false)
    {
        std::array<BYTE, 0x550> source = style;
        std::array<BYTE, 0x44> frame{};
        BITMAPINFOHEADER info{};
        info.biWidth = width; info.biHeight = height; info.biBitCount = 24;
        memcpy(frame.data(), &info, sizeof(info));
        std::array<BYTE, 0x118> owner{};
        memcpy(owner.data() + 0x114, &targetWindow, sizeof(targetWindow));
        const DWORD frameAddress = DWORD(reinterpret_cast<uintptr_t>(frame.data())), ownerAddress = DWORD(reinterpret_cast<uintptr_t>(owner.data()));
        memcpy(source.data() + 0x3c, &frameAddress, sizeof(frameAddress));
        memcpy(source.data() + 0x40, &ownerAddress, sizeof(ownerAddress));
        if (!initialize(source.data(), text, limit)) return false;
        // 原生清页回调从内框起点开始，只有原帧重建已确认该起点时才用于译文
        if (clearedOrigin) { field(0xe4) = field(0xd4); field(0xe8) = field(0xd8); }
        return true;
    }

    // renderPage 用单段原生页面起点复现完整排版，沿用有界分段预检
    bool renderPage(LCSEGlyph::Renderer &renderer)
    {
        if (!prepared || field(0xf0) >= 0x400) return false;
        return renderSpans(renderer, {{std::vector<BYTE>(storage.begin() + 0x108, storage.begin() + 0x108 + field(0xf0)),
            int(field(0xe4)), int(field(0xe8)), false}}, true);
    }

    // renderSpans 按提示语及选项调用顺序预检所有字形，保留跨段括号状态和共享遮罩
    bool renderSpans(LCSEGlyph::Renderer &renderer, const std::vector<Span> &spans, bool terminalClear = false)
    {
        if (!prepared || rendered || field(0xec) != 0 || spans.empty() || spans.size() > 65) return false;
        for (const auto &span : spans)
            if (!validText(span.text) || span.initialNesting < -1 || span.initialNesting > 1024) return false;
        for (const auto name : {"wrap_single", "wrap_double", "open_single", "open_double", "close_single", "close_double"})
            if (!layout.tables.count(name)) return false;
        LOGFONTA font;
        MAT2 matrix;
        memcpy(&font, storage.data() + 0x44, sizeof(font));
        memcpy(&matrix, storage.data() + 0x80, sizeof(matrix));
        MAT2 identity{}; identity.eM11.value = 1; identity.eM22.value = 1;
        const auto format = field(0x94);
        if (font.lfHeight <= 0 || font.lfHeight > 128 || font.lfWidth < 0 || font.lfWidth > 128 || font.lfEscapement || font.lfOrientation ||
            !memchr(font.lfFaceName, 0, LF_FACESIZE) || memcmp(&matrix, &identity, sizeof(matrix)) ||
            (format != GGO_BITMAP && format != GGO_GRAY8_BITMAP)) return false;
        const int spacingY = int(field(0xf4)), spacingX = int(field(0xf8)), shadowX = storage[0x521] ? int(field(0x518)) : 0,
            shadowY = storage[0x521] ? int(field(0x51c)) : 0;
        if (spacingX < -128 || spacingY < -128 || spacingX > 512 || spacingY > 512 ||
            font.lfHeight + shadowY + spacingY <= 0 || int(field(0x524)) < 0 || field(0x524) > 1024) return false;
        const auto dc = GetDC(window);
        if (!dc) return false;
        const auto handle = CreateFontIndirectA(&font);
        if (!handle) { ReleaseDC(window, dc); return false; }
        bool success = false;
        {
            LCSEGlyph::SelectedFont selected(dc, handle);
            if (selected.valid())
            {
                std::map<UINT, LCSEGlyph::Bitmap> glyphs;
                std::vector<Placement> placements;
                size_t glyphBytes = 0;
                int64_t x = int(field(0xe4)), y = int(field(0xe8));
                int nesting = int(field(0x524));
                const int64_t left = int(field(0xc4)), top = int(field(0xc8)), width = int(field(0xcc)), height = int(field(0xd0)),
                    innerX = int(field(0xd4)), innerY = int(field(0xd8)), innerRight = innerX + int(field(0xdc)), innerBottom = innerY + int(field(0xe0));
                const auto newline = [&](const Span &span)
                {
                    x = innerX + ((storage[0x522] && nesting > 0) ? font.lfHeight + shadowX + spacingX : 0);
                    if (span.resetLineX) x = span.x;
                    y += font.lfHeight + shadowY + spacingY;
                    return y + font.lfHeight + shadowY <= (span.height ? (std::min)(innerBottom, int64_t(span.y) + span.height) : innerBottom);
                };
                bool valid = true;
                for (const auto &span : spans)
                {
                    x = span.x; y = span.y;
                    if (span.initialNesting >= 0) nesting = span.initialNesting;
                    const bool bounded = span.width != 0 || span.height != 0;
                    const auto right = bounded ? (std::min)(innerRight, int64_t(span.x) + span.width) : innerRight;
                    const auto bottom = bounded ? (std::min)(innerBottom, int64_t(span.y) + span.height) : innerBottom;
                    if ((bounded && (span.width <= 0 || span.height <= 0 || span.width > 4096 || span.height > 4096)) ||
                        x < left || y < top || x > right || y + font.lfHeight + shadowY > bottom)
                        { valid = false; break; }
                    for (size_t at = 0; valid && at < span.text.size(); ++at)
                    {
                        UINT code = span.text[at];
                        const bool single = !((code >= 0x81 && code <= 0x9f) || (code >= 0xe0 && code <= 0xfc));
                        if (!single) code = code << 8 | span.text[++at];
                        if (code < 0x20)
                        {
                            if (code == 1) valid = newline(span);
                            else if (code != 2 && !(code == 3 && terminalClear && &span == &spans.back() && at + 1 == span.text.size())) valid = false;
                            continue;
                        }
                        const int advance = (single ? font.lfHeight / 2 : font.lfHeight) + shadowX + spacingX;
                        if (advance <= 0) { valid = false; break; }
                        // 当前原生单字节路径在载入字符前测试禁则，传入的零值不能命中ASCII表
                        if (x + advance > right && (single || !storage[0x52c] || !listed("wrap", false, code)))
                        {
                            if (y + 2 * (font.lfHeight + shadowY) + spacingY > bottom || !newline(span) || x + advance > right)
                                { valid = false; break; }
                        }
                        if (bounded && x + advance > right) { valid = false; break; }
                        if (listed("open", single, code)) ++nesting;
                        if (listed("close", single, code) && nesting > 0) --nesting;
                        if (code != 0x20 && code != 0x8140)
                        {
                            auto found = glyphs.find(code);
                            if (found == glyphs.end())
                            {
                                // 两阶段字形读取最多分配1MiB，预留峰值后才访问GDI
                                if (pixelLimit - canvasBytes - glyphBytes < 1024 * 1024) { valid = false; break; }
                                LCSEGlyph::Bitmap bitmap;
                                if (!renderer.bitmap(dc, code, format, matrix, bitmap)) { valid = false; break; }
                                // 有效像素缩短后仍保留GDI缓冲容量，按实际分配计入预算
                                glyphBytes += bitmap.bytes.capacity();
                                found = glyphs.emplace(code, std::move(bitmap)).first;
                            }
                            const auto &metrics = found->second.metrics;
                            const int64_t glyphX = x - left + metrics.gmptGlyphOrigin.x,
                                glyphY = y + font.lfHeight - top - (storage[0xb8] ? int64_t(metrics.gmBlackBoxY) : metrics.gmptGlyphOrigin.y),
                                mixY = y + font.lfHeight - top - metrics.gmptGlyphOrigin.y;
                            if (metrics.gmBlackBoxX && metrics.gmBlackBoxY &&
                                (glyphX < 0 || glyphY < 0 || glyphX + metrics.gmBlackBoxX > width || glyphY + metrics.gmBlackBoxY > height ||
                                mixY < 0 || mixY + metrics.gmBlackBoxY > height ||
                                left + glyphX + shadowX + metrics.gmBlackBoxX > int64_t(*reinterpret_cast<DWORD *>(reinterpret_cast<BYTE *>(target.object()) + 4)) ||
                                top + (std::max)(glyphY, mixY) + shadowY + metrics.gmBlackBoxY > int64_t(*reinterpret_cast<DWORD *>(reinterpret_cast<BYTE *>(target.object()) + 8))))
                                { valid = false; break; }
                            if (bounded && metrics.gmBlackBoxX && metrics.gmBlackBoxY &&
                                (left + glyphX < span.x || top + (std::min)(glyphY, mixY) < span.y ||
                                left + glyphX + shadowX + metrics.gmBlackBoxX > right ||
                                top + (std::max)(glyphY, mixY) + shadowY + metrics.gmBlackBoxY > bottom))
                                { valid = false; break; }
                            placements.push_back({code, int(glyphX), int(glyphY), int(mixY)});
                        }
                        x += advance;
                    }
                    if (!valid) break;
                }
                if (valid)
                {
                    success = true;
                    for (const auto &placement : placements)
                        if (!children[2].rasterMask(placement.left, placement.top, glyphs.at(placement.code))) { success = false; break; }
                    if (success) { rasterGlyphs = std::move(glyphs); rasterPlacements = std::move(placements); }
                }
            }
        }
        DeleteObject(handle);
        ReleaseDC(window, dc);
        if (!success)
        {
            children[2].fill(int(field(0xcc)), int(field(0xd0)), 255, 255, 255);
            return false;
        }
        rendered = true;
        return true;
    }

    // composePage 每次从调用方提供的当前背景重新合成，拒绝累计叠加上一帧译文
    bool composePage(const void *background)
    {
        composed = false;
        if (!rendered || !target.copyFrom(background)) return false;
        const int x = int(field(0xc4)), y = int(field(0xc8)), width = int(field(0xcc)), height = int(field(0xd0));
        if (storage[0x521])
        {
            const int shadowX = x + int(field(0x518)), shadowY = y + int(field(0x51c));
            const auto frame = reinterpret_cast<const BYTE *>(target.object());
            const int availableWidth = int(*reinterpret_cast<const DWORD *>(frame + 4)) - shadowX,
                availableHeight = int(*reinterpret_cast<const DWORD *>(frame + 8)) - shadowY;
            if (availableWidth > 0 && availableHeight > 0 && !target.mixMask(children[1], children[2], shadowX, shadowY,
                (std::min)(width, availableWidth), (std::min)(height, availableHeight))) return false;
        }
        composed = target.mixMask(children[0], children[2], x, y, width, height);
        return composed;
    }

    // composeIncremental 按原生逐字矩形顺序合成，每字先更新遮罩再绘制阴影及正文
    bool composeIncremental(const void *background)
    {
        composed = false;
        if (!rendered || !target.copyFrom(background) || !children[2].fill(int(field(0xcc)), int(field(0xd0)), 255, 255, 255)) return false;
        const int x = int(field(0xc4)), y = int(field(0xc8));
        for (const auto &placement : rasterPlacements)
        {
            const auto &glyph = rasterGlyphs.at(placement.code);
            if (!children[2].rasterMask(placement.left, placement.top, glyph)) return false;
            const int width = int(glyph.metrics.gmBlackBoxX), height = int(glyph.metrics.gmBlackBoxY);
            if (!width || !height) continue;
            if (storage[0x521] && !target.mixMask(children[1], children[2], x + placement.left + int(field(0x518)),
                y + placement.mixTop + int(field(0x51c)), width, height, placement.left, placement.mixTop)) return false;
            if (!target.mixMask(children[0], children[2], x + placement.left, y + placement.mixTop,
                width, height, placement.left, placement.mixTop)) return false;
        }
        composed = true;
        return true;
    }

    // canvas 只在背景与译文完整合成后暴露独立目标画布
    void *canvas() { return composed ? target.object() : nullptr; }
    // drawingLayer 只读复制私有色彩及遮罩，不暴露原生消息或生命周期对象
    const void *drawingLayer(size_t index) const { return composed && index < children.size() ? children[index].object() : nullptr; }
    // present 仅提交成功合成的页面，未知阶段或失败页面不能进入目标HDC
    bool present(HDC destination, int destinationX, int destinationY, int width, int height, int sourceX, int sourceY)
    {
        return composed && target.present(destination, destinationX, destinationY, width, height, sourceX, sourceY);
    }
};
}
