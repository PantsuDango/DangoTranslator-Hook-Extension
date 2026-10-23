#pragma once

#include "LCSEDisplay.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <set>
#include <functional>

namespace LCSEScene
{
// CaptureLock 用原子标记保护快照状态，同线程重入和跨线程争用均不等待
class CaptureLock
{
    std::atomic_flag &flag;
    bool held;
public:
    // CaptureLock 只尝试一次取得状态所有权，不递归获取标准互斥锁
    explicit CaptureLock(std::atomic_flag &value) : flag(value), held(!flag.test_and_set(std::memory_order_acquire)) {}
    CaptureLock(const CaptureLock &) = delete;
    CaptureLock &operator=(const CaptureLock &) = delete;
    // ~CaptureLock 仅由实际持有者释放标记，拒绝的重入不会提前解锁
    ~CaptureLock() { if (held) flag.clear(std::memory_order_release); }
    // ownsLock 返回本次是否可以访问快照状态
    bool ownsLock() const { return held; }
};

// copyReadable 只复制有界已提交内存，原生对象在读取期间失效时拒绝结果
inline bool copyReadable(void *destination, uintptr_t source, size_t size)
{
    if (!LCSEDisplay::readable(source, size)) return false;
#ifdef _MSC_VER
    __try { memcpy(destination, reinterpret_cast<const void *>(source), size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    memcpy(destination, reinterpret_cast<const void *>(source), size);
#endif
    return true;
}

// value 只读取已经复制到自身存储的四字节字段
inline DWORD value(const BYTE *storage, size_t offset)
{
    DWORD result;
    memcpy(&result, storage + offset, sizeof(result));
    return result;
}

// matchesBytes 分块核对原生字节及终止符，不借用字符串或分配整份像素副本
inline bool matchesBytes(uintptr_t address, const std::vector<BYTE> &expected, bool terminated)
{
    if (!LCSEDisplay::readable(address, expected.size() + (terminated ? 1 : 0))) return false;
    std::array<BYTE, 4096> block{};
    for (size_t at = 0; at < expected.size();)
    {
        const auto size = (std::min)(block.size(), expected.size() - at);
        if (!copyReadable(block.data(), address + at, size) || memcmp(block.data(), expected.data() + at, size)) return false;
        at += size;
    }
    BYTE end = 0;
    return !terminated || (copyReadable(&end, address + expected.size(), 1) && !end);
}

// displayStyle 只保留已审计显示字段，排除原生字符串、缓存、音频及回调所有权
inline std::array<BYTE, 0x550> displayStyle(const std::array<BYTE, 0x550> &message, bool choice)
{
    std::array<BYTE, 0x550> result{};
    const auto raw = message.data();
    memcpy(result.data() + 0x44, raw + 0x44, 0x4c);
    memcpy(result.data() + 0x90, raw + 0x90, 3);
    memcpy(result.data() + 0x94, raw + 0x94, 4);
    result[0xb8] = raw[0xb8];
    memcpy(result.data() + 0xc4, raw + 0xc4, 0x20);
    memcpy(result.data() + 0xf4, raw + 0xf4, 8);
    memcpy(result.data() + 0x518, raw + 0x518, 8);
    for (const size_t offset : {0x521, 0x522, 0x52c, 0x52f, 0x530, 0x531, 0x532, 0x533, 0x534}) result[offset] = raw[offset];
    memcpy(result.data() + 0x524, raw + 0x524, 4);
    if (choice) memset(result.data() + 0x524, 0, 4);
    return result;
}

// callbackMatches 只接受已确认默认回调或有界可执行跳转桩，不调用未知消息方法
inline bool callbackMatches(uintptr_t address, uintptr_t expected)
{
    if (!expected) return false;
    for (size_t depth = 0; depth < 4; ++depth)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        if (address == expected) return true;
        std::array<BYTE, 5> jump{};
        int displacement;
        if (!copyReadable(jump.data(), address, jump.size()) || jump[0] != 0xe9) return false;
        memcpy(&displacement, jump.data() + 1, sizeof(displacement));
        address = uintptr_t(int64_t(address) + 5 + displacement);
    }
    return false;
}

// Surface 自持24位画布像素，原始位图和像素地址仅作为同次绘制的身份标记
struct Surface
{
    int width = 0, height = 0;
    size_t stride = 0;
    uintptr_t object = 0, bitmap = 0, address = 0;
    std::vector<BYTE> pixels;

    // validPixels 核对自持24位像素的真实步长和容量，不相信外部构造的尺寸字段
    bool validPixels() const
    {
        return width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
            stride == ((size_t(width) * 3 + 3) & ~size_t(3)) && pixels.size() == stride * size_t(height);
    }

    // rectangle 拒绝会越过自持画布边界的区域，不依赖原生方法裁剪
    bool rectangle(int x, int y, int w, int h) const
    {
        return validPixels() && x >= 0 && y >= 0 && w > 0 && h > 0 && x <= width && y <= height &&
            w <= width - x && h <= height - y;
    }

    // restoreRect 从不可变背景恢复同坐标矩形，保留区域外像素及行尾填充
    bool restoreRect(const Surface &source, int x, int y, int w, int h)
    {
        if (this == &source || !rectangle(x, y, w, h) || !source.rectangle(x, y, w, h)) return false;
        for (int row = 0; row < h; ++row)
            memcpy(pixels.data() + size_t(height - y - row - 1) * stride + size_t(x) * 3,
                source.pixels.data() + size_t(source.height - y - row - 1) * source.stride + size_t(x) * 3, size_t(w) * 3);
        return true;
    }

    // highlightBlue 复现原选项高亮的蓝色通道饱和加128，其余通道及区域不改写
    bool highlightBlue(int x, int y, int w, int h)
    {
        if (!rectangle(x, y, w, h)) return false;
        for (int row = 0; row < h; ++row)
        {
            auto first = pixels.data() + size_t(height - y - row - 1) * stride + size_t(x) * 3;
            for (int column = 0; column < w; ++column) first[column * 3] = BYTE((std::min)(255, int(first[column * 3]) + 128));
        }
        return true;
    }

    // capture 核对真实DIB对象、容量和重复头部，不保留可供异步解引用的原生指针
    bool capture(uintptr_t source, size_t &remaining)
    {
        *this = {};
        std::array<BYTE, 0x44> header{}, repeated{};
        if (!copyReadable(header.data(), source, header.size())) return false;
        BITMAPINFOHEADER info;
        memcpy(&info, header.data(), sizeof(info));
        if (info.biSize != sizeof(info) || info.biWidth <= 0 || info.biHeight <= 0 || info.biWidth > 4096 || info.biHeight > 4096 ||
            info.biPlanes != 1 || info.biBitCount != 24 || info.biCompression != BI_RGB || value(header.data(), 0x40) != 3) return false;
        const size_t row = (size_t(info.biWidth) * 3 + 3) & ~size_t(3), size = row * size_t(info.biHeight);
        if (value(header.data(), 0x3c) != row || info.biSizeImage != size || size > remaining) return false;
        const auto handle = reinterpret_cast<HBITMAP>(uintptr_t(value(header.data(), 0x34)));
        const auto bytes = uintptr_t(value(header.data(), 0x38));
        DIBSECTION dib{};
        if (!handle || !bytes || GetObjectW(handle, sizeof(dib), &dib) != sizeof(dib) ||
            dib.dsBm.bmWidth != info.biWidth || dib.dsBm.bmHeight != info.biHeight || dib.dsBm.bmWidthBytes != row ||
            dib.dsBm.bmPlanes != 1 || dib.dsBm.bmBitsPixel != 24 || reinterpret_cast<uintptr_t>(dib.dsBm.bmBits) != bytes) return false;
        std::vector<BYTE> candidate(size);
        if (!copyReadable(candidate.data(), bytes, size) || !copyReadable(repeated.data(), source, repeated.size()) || header != repeated) return false;
        width = info.biWidth; height = info.biHeight; stride = row;
        object = source; bitmap = reinterpret_cast<uintptr_t>(handle); address = bytes;
        pixels = std::move(candidate);
        remaining -= size;
        return true;
    }

    // sameIdentity 核对同一画布及同一份DIB，而不读取已经记录的原始地址
    bool sameIdentity(const Surface &other) const
    {
        return object == other.object && bitmap == other.bitmap && address == other.address &&
            width == other.width && height == other.height && stride == other.stride;
    }

    // matchesNative 核对当前DIB身份及全部像素，拒绝旧画布或其他图层改写的输出
    bool matchesNative() const
    {
        if (!validPixels() || !object || !bitmap || !address) return false;
        std::array<BYTE, 0x44> header{}, repeated{};
        if (!copyReadable(header.data(), object, header.size())) return false;
        BITMAPINFOHEADER info{};
        memcpy(&info, header.data(), sizeof(info));
        if (info.biSize != sizeof(info) || info.biWidth != width || info.biHeight != height ||
            info.biPlanes != 1 || info.biBitCount != 24 || info.biCompression != BI_RGB || info.biSizeImage != pixels.size() ||
            value(header.data(), 0x34) != bitmap || value(header.data(), 0x38) != address ||
            value(header.data(), 0x3c) != stride || value(header.data(), 0x40) != 3) return false;
        DIBSECTION dib{};
        if (GetObjectW(reinterpret_cast<HBITMAP>(bitmap), sizeof(dib), &dib) != sizeof(dib) ||
            dib.dsBm.bmWidth != width || dib.dsBm.bmHeight != height || dib.dsBm.bmWidthBytes != stride ||
            dib.dsBm.bmPlanes != 1 || dib.dsBm.bmBitsPixel != 24 || reinterpret_cast<uintptr_t>(dib.dsBm.bmBits) != address ||
            !matchesBytes(address, pixels, false)) return false;
        return copyReadable(repeated.data(), object, repeated.size()) && header == repeated;
    }

    // mix 按已验证原生256分母混合自持矩形，不访问游戏画布
    bool mix(const Surface &color, const Surface &mask, int x, int y, int w, int h, int sourceX = 0, int sourceY = 0)
    {
        if (x < 0 || y < 0 || w < 0 || h < 0 || x > width || y > height || w > width - x || h > height - y ||
            sourceX < 0 || sourceY < 0 || sourceX > color.width || sourceY > color.height || sourceX > mask.width || sourceY > mask.height ||
            w > color.width - sourceX || h > color.height - sourceY || w > mask.width - sourceX || h > mask.height - sourceY ||
            !validPixels() || !color.validPixels() || !mask.validPixels()) return false;
        for (int row = 0; row < h; ++row)
        {
            auto destination = pixels.data() + size_t(height - y - row - 1) * stride + size_t(x) * 3;
            const auto colors = color.pixels.data() + size_t(color.height - sourceY - row - 1) * color.stride + size_t(sourceX) * 3;
            const auto masks = mask.pixels.data() + size_t(mask.height - sourceY - row - 1) * mask.stride + size_t(sourceX) * 3;
            for (int byte = 0; byte < w * 3; ++byte)
            {
                const int product = (int(destination[byte]) - int(colors[byte])) * int(masks[byte]);
                destination[byte] = BYTE(int(colors[byte]) + (product >= 0 ? product / 256 : -((-product + 255) / 256)));
            }
        }
        return true;
    }
};

// Layer 自持实际绘制像素与原文字节，显示样式排除原生对象、缓存、音频和回调指针
struct Layer
{
    size_t index = 0;
    bool choice = false;
    bool boundCallbacks = false;
    bool clearedOrigin = false;
    size_t reflowPrefix = 0;
    std::array<DWORD, 2> reflowPosition{};
    DWORD reflowNesting = 0;
    DWORD cursor = 0;
    std::array<DWORD, 2> position{};
    std::array<BYTE, 0x550> style{};
    std::vector<BYTE> current, displayed;
    Surface color, shadow, mask;

    // currentPage 只允许默认正文回调与精确当前页前缀关联整句译文
    bool currentPage() const
    {
        return !choice && boundCallbacks && cursor && cursor == displayed.size() && cursor <= current.size() &&
            std::equal(displayed.begin(), displayed.end(), current.begin());
    }

    // reflowSpans 仅复现已确认重排前缀与原逐字坐标续写，非法边界保留拒绝
    std::vector<LCSEDisplay::Span> reflowSpans() const
    {
        if (!reflowPrefix || reflowPrefix >= displayed.size() || reflowNesting > 1024) return {};
        return {{std::vector<BYTE>(displayed.begin(), displayed.begin() + reflowPrefix),
            int(value(style.data(), 0xc4)), int(value(style.data(), 0xc8)), false, 0, 0, int(reflowNesting)},
            {std::vector<BYTE>(displayed.begin() + reflowPrefix, displayed.end()),
            int(reflowPosition[0]), int(reflowPosition[1]), false, 0, 0, int(reflowNesting)}};
    }

    // matchesDrawing 逐字节核对独立重建的三份画布，未知或歧义起点不能用于译文
    bool matchesDrawing(const LCSEDisplay::Message &message) const
    {
        const Surface *surfaces[] = {&color, &shadow, &mask};
        for (size_t index = 0; index < 3; ++index)
        {
            const auto canvas = static_cast<const BYTE *>(message.drawingLayer(index));
            const auto &surface = *surfaces[index];
            if (!canvas || value(canvas, 4) != DWORD(surface.width) || value(canvas, 8) != DWORD(surface.height) ||
                value(canvas, 0x3c) != DWORD(surface.stride) || value(canvas, 0x14) != surface.pixels.size() ||
                !matchesBytes(value(canvas, 0x38), surface.pixels, false)) return false;
        }
        return true;
    }

    // matchesMessage 拒绝原文、游标、显示样式或遮罩在快照之后改变的消息槽
    bool matchesMessage(uintptr_t source, uintptr_t output, uintptr_t owner, const LCSEPresentationLayout &layout) const
    {
        std::array<BYTE, 0x550> message{}, repeated{};
        if (!copyReadable(message.data(), source, message.size()) || current.size() >= 0x400 ||
            value(message.data(), 0xec) != cursor || value(message.data(), 0xf0) != current.size() ||
            value(message.data(), 0x100) != displayed.size() || message[0x108 + current.size()] ||
            value(message.data(), 0xe4) != position[0] || value(message.data(), 0xe8) != position[1] ||
            value(message.data(), 0x3c) != output || value(message.data(), 0x40) != owner || value(message.data(), 0x538) ||
            value(message.data(), 0x30) != color.object || value(message.data(), 0x34) != shadow.object ||
            value(message.data(), 0x38) != mask.object || displayStyle(message, choice) != style ||
            !std::equal(current.begin(), current.end(), message.begin() + 0x108) ||
            !matchesBytes(value(message.data(), 0xfc), displayed, true)) return false;
        if (boundCallbacks && (!callbackMatches(value(message.data(), 0x540), layout.functions.at("display_step")) ||
            !callbackMatches(value(message.data(), 0x544), layout.functions.at("display_newline")) ||
            !callbackMatches(value(message.data(), 0x548), layout.functions.at("display_clear")))) return false;
        return color.matchesNative() && shadow.matchesNative() && mask.matchesNative() &&
            copyReadable(repeated.data(), source, repeated.size()) && message == repeated;
    }

    // composeRect 按原生局部合成参数先阴影后正文，来源使用相对消息外框的同一遮罩矩形
    bool composeRect(Surface &frame, int x, int y, int width, int height) const
    {
        if (!frame.rectangle(x, y, width, height) || style[0x521] > 1 || value(style.data(), 0xc4) > DWORD(frame.width) ||
            value(style.data(), 0xc8) > DWORD(frame.height) || value(style.data(), 0x518) > 32 || value(style.data(), 0x51c) > 32) return false;
        const int sourceX = x - int(value(style.data(), 0xc4)), sourceY = y - int(value(style.data(), 0xc8));
        if (style[0x521] && !frame.mix(shadow, mask, x + int(value(style.data(), 0x518)),
            y + int(value(style.data(), 0x51c)), width, height, sourceX, sourceY)) return false;
        return frame.mix(color, mask, x, y, width, height, sourceX, sourceY);
    }

    // compose 复现该槽位的原生整页合成，阴影右下边界按画布裁剪
    bool compose(Surface &frame) const
    {
        const int x = int(value(style.data(), 0xc4)), y = int(value(style.data(), 0xc8)),
            w = int(value(style.data(), 0xcc)), h = int(value(style.data(), 0xd0));
        if (style[0x521])
        {
            const int sx = x + int(value(style.data(), 0x518)), sy = y + int(value(style.data(), 0x51c));
            if (sx < frame.width && sy < frame.height && !frame.mix(shadow, mask, sx, sy,
                (std::min)(w, frame.width - sx), (std::min)(h, frame.height - sy))) return false;
        }
        return frame.mix(color, mask, x, y, w, h);
    }
};

// Identity 将背景与消息数组绑定到同一窗口及同一绘制线程，字段只用来比较当前身份
struct Identity
{
    uintptr_t background = 0, output = 0, messages = 0, owner = 0, choice = 0;
    HWND window = nullptr;
    DWORD count = 0, choices = 0, thread = 0;
    int width = 0, height = 0;
    std::array<DWORD, 4> sceneObjects{};

    // read 只从已验证数据槽复制当前身份，未知容量或外部窗口拒绝读取
    static bool read(const LCSEPresentationLayout &layout, Identity &result)
    {
        const auto readSlot = [&layout](const char *name, DWORD &value)
        {
            const auto slot = layout.globals.find(name);
            return slot != layout.globals.end() && copyReadable(&value, slot->second, sizeof(value));
        };
        DWORD background, output, messages, owner, width, height, choices, choice = 0;
        result = {};
        if (!readSlot("background", background) || !readSlot("output", output) || !readSlot("messages", messages) || !readSlot("window", owner) ||
            !readSlot("width", width) || !readSlot("height", height) || !readSlot("message_count", result.count) || !readSlot("choice_count", choices)) return false;
        if (layout.functions.count("scene_redraw"))
        {
            size_t index = 0;
            for (const auto name : {"base_image", "sprites", "widget_images", "menu_images"})
                if (!readSlot(name, result.sceneObjects[index++])) return false;
        }
        const auto controller = layout.globals.find("choice_renderer");
        if (controller == layout.globals.end() || !copyReadable(&choice, controller->second, sizeof(choice)) ||
            !background || !output || background == output || !owner || width == 0 || height == 0 || width > 4096 || height > 4096 ||
            result.count > 64 || choices > 64 || (result.count && !LCSEDisplay::readable(messages, size_t(result.count) * 0x550))) return false;
        if (!copyReadable(&result.window, uintptr_t(owner) + 0x114, sizeof(result.window)) || !IsWindow(result.window)) return false;
        DWORD process = 0;
        result.thread = GetCurrentThreadId();
        if (GetWindowThreadProcessId(result.window, &process) != result.thread || process != GetCurrentProcessId()) return false;
        if (choices && (!result.count || choice < messages || size_t(choice - messages) % 0x550 ||
            size_t(choice - messages) / 0x550 >= result.count)) return false;
        result.background = background; result.output = output; result.messages = messages; result.owner = owner;
        result.width = int(width); result.height = int(height); result.choices = choices; result.choice = choices ? choice : 0;
        return true;
    }

    // matches 防止换窗口、换消息数组、换选项或跨线程结果进入同一次绘制
    bool matches(const Identity &other) const
    {
        return background == other.background && output == other.output && messages == other.messages && owner == other.owner &&
            choice == other.choice && window == other.window && count == other.count && choices == other.choices && thread == other.thread &&
            width == other.width && height == other.height && sceneObjects == other.sceneObjects;
    }
};

// Choice 自持选择层全部显示段和原点击矩形，地址字段只用于同次菜单身份比较
struct Choice
{
    std::array<BYTE, 0x1c> metadata{};
    std::vector<BYTE> records, prompt, displayed;
    std::vector<LCSEDisplay::Span> spans;

    // text 核对原生字符串容量和终止字节，返回自持副本而不借用原文指针
    static bool text(uintptr_t source, DWORD length, bool empty, std::vector<BYTE> &result)
    {
        if (length >= 0x400 || (!length && !empty)) return false;
        if (!source && !length && empty) { result.clear(); return true; }
        std::vector<BYTE> bytes(size_t(length) + 1), repeated(bytes.size());
        if (!copyReadable(bytes.data(), source, bytes.size()) || bytes.back() != 0 ||
            !copyReadable(repeated.data(), source, repeated.size()) || repeated != bytes) return false;
        bytes.pop_back();
        if (!bytes.empty() && !LCSEDisplay::validText(bytes)) return false;
        result = std::move(bytes);
        return true;
    }

    // capture 核对同一消息槽、选项数组和原位置，保留提示语与逐项换行语义
    bool capture(uintptr_t controller, const Identity &state)
    {
        *this = {};
        std::array<BYTE, 0x1c> repeated{};
        std::array<BYTE, 0xe4> message{};
        if (!state.choices || state.choices > 64 || !copyReadable(metadata.data(), controller, metadata.size()) ||
            value(metadata.data(), 0) != state.choice || value(metadata.data(), 8) != state.choices || metadata[0x18] > 1 ||
            (!metadata[0x18] && !value(metadata.data(), 16)) ||
            (value(metadata.data(), 12) != DWORD(-1) && value(metadata.data(), 12) >= state.choices) ||
            !copyReadable(message.data(), state.choice, message.size())) return false;
        const auto array = uintptr_t(value(metadata.data(), 4));
        records.resize(size_t(state.choices) * 0x1c);
        if (!copyReadable(records.data(), array, records.size()) ||
            !text(value(metadata.data(), 16), value(metadata.data(), 20), true, prompt)) return false;
        if (!metadata[0x18] && !prompt.empty())
        {
            spans.push_back({prompt, int(value(message.data(), 0xd4)), int(value(message.data(), 0xd8)), false});
            displayed = prompt;
        }
        for (size_t index = 0; index < state.choices; ++index)
        {
            const auto record = records.data() + index * 0x1c;
            const int x = int(value(record, 0)), y = int(value(record, 4)), width = int(value(record, 8)), height = int(value(record, 12));
            std::vector<BYTE> bytes;
            if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > state.width || height > state.height ||
                x > state.width - width || y > state.height - height || !text(value(record, 16), value(record, 20), false, bytes)) return false;
            displayed.insert(displayed.end(), bytes.begin(), bytes.end());
            spans.push_back({std::move(bytes), x, y, true});
        }
        std::vector<BYTE> repeatedRecords(records.size());
        return copyReadable(repeated.data(), controller, repeated.size()) && repeated == metadata &&
            copyReadable(repeatedRecords.data(), array, repeatedRecords.size()) && repeatedRecords == records;
    }

    // matches 拒绝选择编号、记录身份、分段长度或原文字节在同帧内变化
    bool matches(const Choice &other) const
    {
        if (metadata != other.metadata || records != other.records || prompt != other.prompt || displayed != other.displayed ||
            spans.size() != other.spans.size()) return false;
        for (size_t index = 0; index < spans.size(); ++index)
            if (spans[index].x != other.spans[index].x || spans[index].y != other.spans[index].y ||
                spans[index].resetLineX != other.spans[index].resetLineX || spans[index].text != other.spans[index].text) return false;
        return true;
    }
};

// Snapshot 只公布通过实际原帧重建对照的不可变整场景数据
struct Snapshot
{
    unsigned long long generation = 0;
    bool incremental = false;
    bool liveRefresh = false;
    Identity identity;
    Surface background, output;
    Choice selection;
    std::vector<Layer> layers;

    // matchesState 同线程复查完整场景与全部槽位，只允许明确声明的选中编号变化
    bool matchesState(const LCSEPresentationLayout &layout, const Surface &expectedOutput, bool selectedChanges = false) const noexcept
    {
        try
        {
            Identity current;
            if (!generation || layers.size() > identity.count || !GdiFlush() || !Identity::read(layout, current) ||
                !identity.matches(current) || background.object != current.background || output.object != current.output ||
                background.width != current.width || background.height != current.height ||
                expectedOutput.width != current.width || expectedOutput.height != current.height ||
                !output.sameIdentity(expectedOutput)) return false;
            Choice choice;
            const auto sameChoice = [&]()
            {
                if (!choice.capture(layout.globals.at("choice_renderer"), current)) return false;
                if (selectedChanges) memcpy(choice.metadata.data() + 12, selection.metadata.data() + 12, 4);
                return selection.matches(choice);
            };
            if (current.choices && (!layout.choices || !sameChoice())) return false;
            size_t visible = 0;
            for (size_t index = 0; index < current.count; ++index)
            {
                const auto source = current.messages + index * 0x550;
                DWORD length = 0;
                if (!copyReadable(&length, source + 0x100, sizeof(length))) return false;
                if (!length)
                {
                    if (visible < layers.size() && layers[visible].index <= index) return false;
                    continue;
                }
                if (visible >= layers.size() || layers[visible].index != index ||
                    layers[visible].choice != (current.choices && source == current.choice) ||
                    !layers[visible].matchesMessage(source, current.output, current.owner, layout)) return false;
                ++visible;
            }
            if (visible != layers.size() || !background.matchesNative() || !expectedOutput.matchesNative() ||
                !Identity::read(layout, current) || !identity.matches(current)) return false;
            return !current.choices || sameChoice();
        }
        catch (...) { return false; }
    }

    // matchesLive 核对原始完整帧，悬停更新须显式提供已跟踪的原生输出
    bool matchesLive(const LCSEPresentationLayout &layout) const noexcept { return matchesState(layout, output); }
};

// TranslationMap 按显示原始字节匹配译文，不用翻页游标或脚本分支值作为替换目标
using TranslationMap = std::map<std::vector<BYTE>, std::vector<BYTE>>;

// PresentationTarget 只在已验证呈现栈帧内借用本次窗口HDC，不保存异步显示目标
class PresentationTarget
{
    HDC dc = nullptr;
    Identity identity;

    // valid 拒绝外部窗口、跨线程、映射缩放和坐标偏移，不修改目标HDC状态
    bool valid() const
    {
        DWORD process = 0;
        POINT window{}, viewport{};
        RECT client{}, clip{};
        return dc && identity.thread == GetCurrentThreadId() && IsWindow(identity.window) &&
            GetWindowThreadProcessId(identity.window, &process) == identity.thread && process == GetCurrentProcessId() &&
            GetObjectType(dc) == OBJ_DC && WindowFromDC(dc) == identity.window &&
            GetMapMode(dc) == MM_TEXT && GetGraphicsMode(dc) == GM_COMPATIBLE && GetLayout(dc) == 0 &&
            GetWindowOrgEx(dc, &window) && !window.x && !window.y &&
            GetViewportOrgEx(dc, &viewport) && !viewport.x && !viewport.y &&
            GetClientRect(identity.window, &client) && client.left == 0 && client.top == 0 &&
            client.right == identity.width && client.bottom == identity.height && GetClipBox(dc, &clip) != ERROR;
    }

    // draw 使用已自持的24位像素直接提交，不创建额外整帧副本或借用原生DIB
    bool draw(const Surface &surface) const
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = surface.width; info.bmiHeader.biHeight = surface.height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 24; info.bmiHeader.biCompression = BI_RGB;
        info.bmiHeader.biSizeImage = DWORD(surface.pixels.size());
        return SetDIBitsToDevice(dc, 0, 0, surface.width, surface.height, 0, 0, 0, surface.height,
            surface.pixels.data(), &info, DIB_RGB_COLORS) == surface.height && GdiFlush();
    }

public:
    // read 核对完整呈现阶段声明的EBP局部变量及真实窗口DC来源
    bool read(uintptr_t frame, const LCSEPresentationLayout &layout, const Identity &source, int frameOffset = 0)
    {
        dc = nullptr; identity = {};
        const auto offset = frameOffset ? frameOffset : layout.dcFrameOffset;
        if (!frame || frame % 4 || !offset || offset < -4096 || offset > 4096 || offset % 4 ||
            (frameOffset && (!layout.refresh || frameOffset != layout.refreshDCFrameOffset))) return false;
        const auto stack = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
        const auto first = reinterpret_cast<uintptr_t>(stack->StackLimit), last = reinterpret_cast<uintptr_t>(stack->StackBase);
        const auto slot64 = int64_t(frame) + offset;
        if (frame < first || frame >= last || sizeof(uintptr_t) > last - frame || slot64 < int64_t(first) ||
            slot64 >= int64_t(last) || sizeof(uintptr_t) > last - uintptr_t(slot64)) return false;
        HDC candidate = nullptr, repeated = nullptr;
        const auto slot = uintptr_t(slot64);
        if (!copyReadable(&candidate, slot, sizeof(candidate))) return false;
        dc = candidate; identity = source;
        if (!valid() || !copyReadable(&repeated, slot, sizeof(repeated)) || repeated != candidate) { dc = nullptr; return false; }
        return true;
    }

    // matchesDC 在局部呈现返回时确认仍是入口所借用的同一窗口HDC
    bool matchesDC(HDC expected) const { return dc && dc == expected; }

    // present 提交前后复核代次，失败时只恢复仍匹配的当前原帧并请求原生重绘
    bool present(const Surface &surface, const Snapshot &source, const LCSEPresentationLayout &layout,
        const std::function<bool()> &current, const Surface *hoverOriginal = nullptr) const noexcept
    {
        bool attempted = false;
        try
        {
            if (!surface.validPixels() || surface.width != identity.width || surface.height != identity.height ||
                !source.identity.matches(identity) || !current || !valid() || !current()) return false;
            attempted = true;
            if (draw(surface) && valid() && current()) return true;
            // 原生对象已换页时不能把旧原文像素覆盖到新页，只排队请求当前窗口重绘
            const auto &original = hoverOriginal ? *hoverOriginal : source.output;
            if (valid() && source.matchesState(layout, original, hoverOriginal != nullptr) && valid()) draw(original);
        }
        catch (...) {}
        if (attempted && identity.thread == GetCurrentThreadId() && IsWindow(identity.window))
            InvalidateRect(identity.window, nullptr, FALSE);
        return false;
    }
};

// PendingBlit 将原生这一次窗口复制的源换成私有DIB，原始DC/画布及消息不改。
// 生命周期独立于会话，必须在精确BitBlt返回点Flush后才能回收。
class PendingBlit
{
    LCSEDisplay::Canvas canvas;
    HDC dc = nullptr;
    HGDIOBJ previous = nullptr;
    uintptr_t done = 0;
    std::array<uintptr_t, 9> original{};
    std::function<bool()> current;
public:
    // ~PendingBlit 只回收私有DC和位图，绝不释放游戏对象
    ~PendingBlit()
    {
        if (dc) { GdiFlush(); if (previous && previous != HGDI_ERROR) SelectObject(dc, previous); DeleteDC(dc); }
    }
    // returnAddress 绑定必须完成回收的精确原生返回指令
    uintptr_t returnAddress() const noexcept { return done; }
    // initialize 先创建并校验私有源，全部成功后才替换本次调用参数
    bool initialize(const Surface &surface, uintptr_t returnPoint, uintptr_t *arguments, const std::function<bool()> &valid, const std::function<bool()> &after)
    {
        if (!arguments || !returnPoint || !surface.validPixels() || !valid || !valid() ||
            !canvas.initialize(surface.width, surface.height) || !canvas.assignPixels(surface.pixels)) return false;
        dc = CreateCompatibleDC(reinterpret_cast<HDC>(arguments[0]));
        if (!dc) return false;
        previous = SelectObject(dc, reinterpret_cast<HBITMAP>(value(static_cast<const BYTE *>(canvas.object()), 0x34)));
        if (!previous || previous == HGDI_ERROR || !GdiFlush() || !valid()) return false;
        std::copy(arguments, arguments + original.size(), original.begin());
        current = after; done = returnPoint;
        // 局部原文字形矩形无法覆盖整句译文，中文从同帧背景组成完整场景一次提交。
        arguments[1] = arguments[2] = arguments[6] = arguments[7] = 0;
        arguments[3] = surface.width; arguments[4] = surface.height;
        arguments[5] = reinterpret_cast<uintptr_t>(dc);
        return true;
    }
    // finish 等待GDI完成后复查当前页，失败只恢复仍有效的原画面
    bool finish(bool success) noexcept
    {
        try
        {
            const bool flushed = GdiFlush() != FALSE;
            if (success && flushed && current && current()) return true;
            // 只在仍可证明为当前页时使用原复制参数；换页后请求游戏自行重绘。
            if (current && current())
                BitBlt(reinterpret_cast<HDC>(original[0]), int(original[1]), int(original[2]), int(original[3]), int(original[4]),
                    reinterpret_cast<HDC>(original[5]), int(original[6]), int(original[7]), DWORD(original[8]));
            const auto window = WindowFromDC(reinterpret_cast<HDC>(original[0]));
            if (IsWindow(window)) InvalidateRect(window, nullptr, FALSE);
            GdiFlush();
        }
        catch (...) {}
        return false;
    }
};

// TranslatedFrame 从不可变原帧生成独立译文像素和遮罩，不修改原消息或游戏画布
class TranslatedFrame
{
    std::shared_ptr<const Snapshot> original;
    Surface frame;
    std::vector<Layer> visualLayers;
    size_t translated = 0, fallback = 0;

    // replacement 拒绝未知控制或改变原分页控制的译文，编码字位仍由字形表验证
    static bool replacement(const std::vector<BYTE> &source, const std::vector<BYTE> &text)
    {
        if (!LCSEDisplay::validText(source) || !LCSEDisplay::validText(text)) return false;
        const auto controls = [](const std::vector<BYTE> &bytes)
        {
            std::vector<BYTE> result;
            for (size_t at = 0; at < bytes.size(); ++at)
            {
                if (bytes[at] < 0x20) result.push_back(bytes[at]);
                else if (IsDBCSLeadByteEx(932, bytes[at])) ++at;
            }
            return result;
        };
        return controls(source) == controls(text);
    }

    // renderLayer 临时绘制完整图层，全部像素复制成功后才更新独立合成画布
    static bool renderLayer(const Snapshot &source, const Layer &layer, const std::vector<BYTE> &text,
        const std::vector<LCSEDisplay::Span> &spans, const LCSEDisplayLayout &layout,
        LCSEGlyph::Renderer &renderer, LCSEDisplay::Canvas &canvas, size_t remaining, Layer &result, bool wholePage = false)
    {
        const auto copies = layer.color.pixels.size() + layer.shadow.pixels.size() + layer.mask.pixels.size();
        if (copies > remaining) return false;
        LCSEDisplay::Message message(layout);
        auto style = layer.style;
        // 整页译文从页初括号状态开始，不能继承已显示前缀的括号深度再读一次
        if ((source.liveRefresh || wholePage) && !layer.choice) memset(style.data() + 0x524, 0, 4);
        const bool replay = !layer.choice && layer.reflowPrefix && text == layer.displayed;
        const auto replaySpans = replay ? layer.reflowSpans() : std::vector<LCSEDisplay::Span>{};
        if (!message.initializeSnapshot(style, text, source.identity.width, source.identity.height,
            source.identity.window, remaining - copies, layer.clearedOrigin) ||
            !(layer.choice ? message.renderSpans(renderer, spans) :
                replay ? (!replaySpans.empty() && message.renderSpans(renderer, replaySpans, true)) : message.renderPage(renderer)) ||
            !message.composeIncremental(canvas.object())) return false;
        Layer candidate;
        candidate.index = layer.index; candidate.choice = layer.choice; candidate.style = layer.style;
        candidate.clearedOrigin = layer.clearedOrigin;
        candidate.current = layer.choice ? spans.back().text : text;
        candidate.cursor = DWORD(candidate.current.size());
        if (layer.choice)
            for (const auto &span : spans) candidate.displayed.insert(candidate.displayed.end(), span.text.begin(), span.text.end());
        else candidate.displayed = text;
        size_t copyLimit = copies;
        size_t index = 0;
        for (auto surface : {&candidate.color, &candidate.shadow, &candidate.mask})
        {
            if (!surface->capture(reinterpret_cast<uintptr_t>(message.drawingLayer(index++)), copyLimit)) return false;
            surface->object = surface->bitmap = surface->address = 0;
        }
        if (!canvas.copyFrom(message.canvas())) return false;
        result = std::move(candidate);
        return true;
    }

public:
    // initialize 逐层精确匹配并从同帧原背景合成，失败正文或不安全整组选项保留原文
    bool initialize(std::shared_ptr<const Snapshot> source, const TranslationMap &translations,
        const LCSEDisplayLayout &layout, LCSEGlyph::Renderer &renderer, size_t limit = 64 * 1024 * 1024) noexcept
    {
        try
        {
            if (original || !source || !source->incremental || limit > 64 * 1024 * 1024 ||
                source->layers.size() > 64 || !source->background.validPixels() || !source->output.validPixels()) return false;
            size_t remaining = limit;
            const auto reserve = [&remaining](size_t bytes) { if (bytes > remaining) return false; remaining -= bytes; return true; };
            if (!reserve(source->background.pixels.size()) || !reserve(source->output.pixels.size())) return false;
            for (const auto &layer : source->layers)
                for (auto surface : {&layer.color, &layer.shadow, &layer.mask})
                    if (!surface->validPixels() || !reserve(surface->pixels.size())) return false;
            if (!reserve(source->background.pixels.size())) return false;
            LCSEDisplay::Canvas canvas;
            if (!canvas.initialize(source->identity.width, source->identity.height) || !canvas.assignPixels(source->background.pixels)) return false;
            std::vector<Layer> layers;
            size_t mapped = 0, retained = 0;
            for (const auto &layer : source->layers)
            {
                auto text = layer.displayed;
                auto spans = layer.choice ? source->selection.spans : std::vector<LCSEDisplay::Span>{};
                bool found = false;
                bool wholePage = false;
                const auto entries = layer.choice ? spans.size() : size_t(1);
                if (layer.choice)
                {
                    if (spans.empty() || spans.size() > 65 || !source->identity.choices ||
                        source->selection.records.size() != size_t(source->identity.choices) * 0x1c) return false;
                    found = true;
                    size_t index = 0;
                    std::set<std::vector<BYTE>> labels;
                    for (auto &span : spans)
                    {
                        const auto at = translations.find(span.text);
                        if (at == translations.end() || !replacement(span.text, at->second)) { found = false; break; }
                        span.text = at->second;
                        if (span.resetLineX)
                        {
                            if (index >= source->identity.choices || !labels.insert(span.text).second) { found = false; break; }
                            const auto record = source->selection.records.data() + index++ * 0x1c;
                            span.width = int(value(record, 8)); span.height = int(value(record, 12));
                        }
                    }
                    if (index != source->identity.choices) found = false;
                    if (found) text = spans.front().text;
                }
                else
                {
                    // 仅在原生已绘制字节严格等于当前页游标前缀时，允许整页译文提前显示
                    wholePage = layer.currentPage();
                    const auto &originalText = wholePage ? layer.current : text;
                    const auto at = translations.find(originalText);
                    if (at != translations.end() && replacement(originalText, at->second)) { text = at->second; found = true; }
                }
                Layer rendered;
                if (!found || !renderLayer(*source, layer, text, spans, layout, renderer, canvas, remaining, rendered, wholePage))
                {
                    found = false;
                    const auto &originalSpans = source->selection.spans;
                    const auto &originalText = layer.choice ? originalSpans.front().text : layer.displayed;
                    if (!renderLayer(*source, layer, originalText, originalSpans, layout, renderer, canvas, remaining, rendered)) return false;
                }
                if (!reserve(rendered.color.pixels.size()) || !reserve(rendered.shadow.pixels.size()) || !reserve(rendered.mask.pixels.size())) return false;
                (found ? mapped : retained) += entries;
                layers.push_back(std::move(rendered));
            }
            Surface result;
            if (!result.capture(reinterpret_cast<uintptr_t>(canvas.object()), remaining)) return false;
            result.object = result.bitmap = result.address = 0;
            frame = std::move(result); visualLayers = std::move(layers); translated = mapped; fallback = retained; original = std::move(source);
            return true;
        }
        catch (...) { return false; }
    }

    // source 返回需要另行核对当前代次的不可变原帧，不能仅凭持有该指针呈现译文
    std::shared_ptr<const Snapshot> source() const { return original; }
    // pixels 只暴露已完整合成的自持译文像素，不提供游戏画布地址
    const Surface *pixels() const { return original ? &frame : nullptr; }
    // layers 返回自持显示遮罩供后续悬停合成，原生图层身份由source独立保存
    const std::vector<Layer> &layers() const { return visualLayers; }
    // translatedCount 返回真正通过字形和容量检查的显示参数数
    size_t translatedCount() const { return translated; }
    // fallbackCount 返回未命中或不安全而实际保留原文的显示参数数
    size_t fallbackCount() const { return fallback; }
};

// ChoiceFrame 自持选择层刷新后的画面，逐项重现恢复背景、高亮和遮罩合成
class ChoiceFrame
{
    std::shared_ptr<const Snapshot> scene;
    std::shared_ptr<const TranslatedFrame> translation;
    Surface frame;
    size_t pixelLimit = 0, retainedBytes = 0;

public:
    // initialize 只接受已逐像素验证的选择场景，并为当前帧及原子刷新预留完整容量
    bool initialize(std::shared_ptr<const Snapshot> source, size_t limit = 64 * 1024 * 1024)
    {
        if (scene || !source || !source->incremental || !source->identity.choices || limit > 64 * 1024 * 1024 ||
            source->identity.choices > 64 || source->layers.size() > 64 ||
            !source->background.validPixels() || !source->output.validPixels() ||
            source->selection.records.size() != size_t(source->identity.choices) * 0x1c) return false;
        size_t bytes = 0;
        const auto reserve = [&](size_t size) { if (size > limit - bytes) return false; bytes += size; return true; };
        if (!reserve(source->background.pixels.size()) || !reserve(source->output.pixels.size())) return false;
        size_t choices = 0;
        for (const auto &layer : source->layers)
        {
            if (!layer.color.validPixels() || !layer.shadow.validPixels() || !layer.mask.validPixels()) return false;
            if (!reserve(layer.color.pixels.size()) || !reserve(layer.shadow.pixels.size()) || !reserve(layer.mask.pixels.size())) return false;
            if (layer.choice) ++choices;
        }
        if (choices != 1 || bytes > limit || source->output.pixels.size() > (limit - bytes) / 2) return false;
        frame = source->output;
        retainedBytes = bytes + frame.pixels.size(); pixelLimit = limit;
        scene = std::move(source);
        return true;
    }

    // initializeTranslation 借用不可变译文图层并自持当前像素，不复制完整原场景或改变点击区域
    bool initializeTranslation(std::shared_ptr<const TranslatedFrame> source, size_t limit = 64 * 1024 * 1024)
    {
        if (scene || !source || !source->pixels() || !source->pixels()->validPixels()) return false;
        ChoiceFrame candidate;
        if (!candidate.initialize(source->source(), limit)) return false;
        size_t bytes = source->pixels()->pixels.size();
        for (const auto &layer : source->layers())
            for (auto surface : {&layer.color, &layer.shadow, &layer.mask})
            {
                if (!surface->validPixels() || surface->pixels.size() > limit - bytes) return false;
                bytes += surface->pixels.size();
            }
        if (bytes > limit - candidate.retainedBytes || candidate.frame.pixels.size() > limit - candidate.retainedBytes - bytes ||
            candidate.frame.width != source->pixels()->width || candidate.frame.height != source->pixels()->height ||
            candidate.frame.pixels.size() != source->pixels()->pixels.size()) return false;
        std::copy(source->pixels()->pixels.begin(), source->pixels()->pixels.end(), candidate.frame.pixels.begin());
        candidate.frame.object = candidate.frame.bitmap = candidate.frame.address = 0;
        candidate.retainedBytes += bytes; candidate.translation = std::move(source);
        *this = std::move(candidate);
        return true;
    }

    // apply 在有界临时帧中完成一次选择高亮，任一非法位置或像素容量均保留上一帧
    bool apply(size_t index, bool highlighted) noexcept
    {
        try
        {
            if (!scene || index >= scene->identity.choices || frame.pixels.size() > pixelLimit - retainedBytes) return false;
            const auto record = scene->selection.records.data() + index * 0x1c;
            const int x = int(value(record, 0)), y = int(value(record, 4)), w = int(value(record, 8)), h = int(value(record, 12));
            if (x < 1 || y < 0 || y > frame.height - 3 || !frame.rectangle(x, y + 3, w, h)) return false;
            Surface candidate = frame;
            if (!candidate.restoreRect(scene->background, x, y + 3, w, h) ||
                (highlighted && !candidate.highlightBlue(x, y + 3, w, h))) return false;
            for (const auto &layer : translation ? translation->layers() : scene->layers)
                if (layer.choice && !layer.composeRect(candidate, x - 1, y + 2, w, h)) return false;
            frame = std::move(candidate);
            return true;
        }
        catch (...) { return false; }
    }

    // matchesLive 只接受同一线程窗口、选择消息槽及原始图层，允许合法选中编号改变
    bool matchesLive(const LCSEPresentationLayout &layout) const noexcept
    {
        try
        {
            if (!scene || !layout.choices || !layout.hover || !GdiFlush()) return false;
            const auto &identity = scene->identity;
            std::array<BYTE, 12> state{}, repeated{};
            const auto stateSlot = layout.hoverGlobals.at("choice_state");
            if (!copyReadable(state.data(), stateSlot, state.size())) return false;
            const auto index = value(state.data(), 8);
            if (index >= identity.count || identity.messages + size_t(index) * 0x550 != identity.choice) return false;
            return !translation && scene->matchesState(layout, frame, true) &&
                copyReadable(repeated.data(), stateSlot, repeated.size()) && state == repeated;
        }
        catch (...) { return false; }
    }

    // pixels 只暴露自持画面的只读副本视图，不提供游戏画布或消息对象
    const Surface *pixels() const { return scene ? &frame : nullptr; }

    // source 保留经过完整重建的基线身份，当前悬停像素必须另行校验
    const std::shared_ptr<const Snapshot> &source() const { return scene; }
};

// Recorder 在背景与消息完成两个阶段捕获同次绘制，失败或嵌套绘制丢弃整帧
class Recorder
{
    // ReflowBasis 只保存已确认整场景重排的少量布局与原文，不持有像素或游戏对象
    struct ReflowBasis
    {
        size_t index = 0;
        DWORD cursor = 0;
        std::array<DWORD, 2> position{};
        std::array<BYTE, 0x550> style{};
        std::vector<BYTE> current;
    };
    std::atomic_flag captureLock = ATOMIC_FLAG_INIT;
    std::shared_ptr<Snapshot> pending;
    std::shared_ptr<const Snapshot> completed;
    unsigned long long generation = 0;
    std::atomic<unsigned long long> invalidations{0};
    unsigned long long pendingEpoch = 0, completedEpoch = 0;
    size_t remaining = 0;
    std::atomic<const char *> failureCode{"rejected capture_unknown"};
    Identity reflowIdentity{};
    std::vector<ReflowBasis> reflowBases;
    unsigned long long reflowEpoch = 0;

    // reflowBytes 将当前保存的重排元数据实际容量计入同一捕获预算
    size_t reflowBytes() const
    {
        size_t bytes = reflowBases.capacity() * sizeof(ReflowBasis);
        for (const auto &basis : reflowBases) bytes += basis.current.capacity();
        return bytes;
    }

    // rememberReflow 只记录已完整重建的当前页前缀，累计旧页不能成为续写基线
    bool rememberReflow(const Snapshot &scene, unsigned long long epoch)
    {
        size_t estimate = scene.layers.size() * sizeof(ReflowBasis);
        for (const auto &layer : scene.layers) estimate += layer.current.size();
        if (estimate > 256 * 1024 || estimate > remaining) return false;
        std::vector<ReflowBasis> bases;
        bases.reserve(scene.layers.size());
        if (!scene.identity.choices)
            for (const auto &layer : scene.layers)
                if (layer.currentPage() && value(layer.style.data(), 0x524) <= 1024)
                {
                    ReflowBasis basis;
                    basis.index = layer.index; basis.cursor = layer.cursor; basis.position = layer.position;
                    basis.style = layer.style; basis.current = layer.current;
                    bases.push_back(std::move(basis));
                }
        size_t bytes = bases.capacity() * sizeof(ReflowBasis);
        for (const auto &basis : bases) bytes += basis.current.capacity();
        if (bytes > 256 * 1024 || bytes > remaining) return false;
        remaining -= bytes;
        reflowBases = std::move(bases); reflowIdentity = scene.identity; reflowEpoch = epoch;
        return true;
    }

    // identity 只从已验证的数据槽读取当前对象，未知容量或外部窗口拒绝捕获
    bool identity(const LCSEPresentationLayout &layout, Identity &result)
    {
        return Identity::read(layout, result);
    }

    // layer 复制已有消息层，不调用原生消息方法，也不修改原文和字节游标
    bool layer(uintptr_t source, size_t index, const Identity &state, Layer &result,
        const Choice *choice = nullptr, const LCSEDisplayLayout *display = nullptr)
    {
        std::array<BYTE, 0x550> message{}, repeated{};
        if (!copyReadable(message.data(), source, message.size())) return false;
        const auto raw = message.data();
        const auto length = value(raw, 0x100), currentLength = value(raw, 0xf0), cursor = value(raw, 0xec);
        if (!length || length > (choice ? size_t(65) * 1023 : 1023) || currentLength >= 0x400 || message[0x108 + currentLength] != 0 ||
            (cursor > currentLength && (cursor != DWORD(-1) || currentLength != 0)) || value(raw, 0x538) ||
            value(raw, 0x3c) != state.output || value(raw, 0x40) != state.owner) return false;
        const int x = int(value(raw, 0xc4)), y = int(value(raw, 0xc8)), w = int(value(raw, 0xcc)), h = int(value(raw, 0xd0));
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > state.width || h > state.height || x > state.width - w || y > state.height - h ||
            value(raw, 0x518) > 32 || value(raw, 0x51c) > 32 || message[0x521] > 1) return false;
        std::vector<BYTE> displayed(size_t(length) + 1);
        if (!copyReadable(displayed.data(), value(raw, 0xfc), displayed.size()) || displayed.back() != 0) return false;
        displayed.pop_back();
        const std::vector<BYTE> current(raw + 0x108, raw + 0x108 + currentLength);
        if ((!choice && !LCSEDisplay::validText(displayed)) || (!current.empty() && !LCSEDisplay::validText(current))) return false;
        if (choice && (!display || displayed != choice->displayed || current != choice->spans.back().text || cursor != currentLength)) return false;
        if (display && (!callbackMatches(value(raw, 0x540), display->get("display_step")) ||
            !callbackMatches(value(raw, 0x544), display->get("display_newline")) ||
            !callbackMatches(value(raw, 0x548), display->get("display_clear")))) return false;
        // 字节游标不能位于CP932双字节字符中间
        if (cursor != DWORD(-1))
            for (size_t at = 0; at < cursor;)
            {
                at += IsDBCSLeadByteEx(932, current[at]) ? 2 : 1;
                if (at > cursor) return false;
            }
        if (!result.color.capture(value(raw, 0x30), remaining) || !result.shadow.capture(value(raw, 0x34), remaining) ||
            !result.mask.capture(value(raw, 0x38), remaining) ||
            result.color.width != w || result.color.height != h || result.shadow.width != w || result.shadow.height != h ||
            result.mask.width != w || result.mask.height != h ||
            !copyReadable(repeated.data(), source, repeated.size()) || message != repeated) return false;
        result.index = index; result.cursor = cursor; result.current = current; result.displayed = std::move(displayed);
        result.choice = choice != nullptr;
        result.boundCallbacks = display != nullptr;
        result.position = {value(raw, 0xe4), value(raw, 0xe8)};
        result.style = displayStyle(message, choice);
        return true;
    }

    // reconstruct 从同帧背景复现全部显示层并逐像素比较，不调用原生消息绘制
    bool reconstruct(Snapshot &candidate, const LCSEDisplayLayout *display, LCSEGlyph::Renderer *renderer)
    {
        if (candidate.background.pixels.size() > remaining) return false;
        if (display)
        {
            LCSEDisplay::Canvas rebuilt;
            const auto &state = candidate.identity;
            if (!renderer || !rebuilt.initialize(state.width, state.height) || !rebuilt.assignPixels(candidate.background.pixels)) return false;
            remaining -= candidate.background.pixels.size();
            for (auto &item : candidate.layers)
            {
                const auto &text = item.choice ? candidate.selection.spans.front().text : item.displayed;
                auto style = item.style;
                if (candidate.liveRefresh && !item.choice) memset(style.data() + 0x524, 0, 4);
                const bool distinctOrigins = !item.choice && (value(style.data(), 0xc4) != value(style.data(), 0xd4) ||
                    value(style.data(), 0xc8) != value(style.data(), 0xd8));
                size_t matched = 0;
                // 原始三层像素须唯一确认首次外框或清页内框起点，不能从当前游标猜测
                for (size_t origin = 0; origin < (distinctOrigins ? 2 : 1); ++origin)
                {
                    LCSEDisplay::Message message(*display);
                    if (!message.initializeSnapshot(style, text, state.width, state.height, state.window, remaining, origin == 1) ||
                        !(item.choice ? message.renderSpans(*renderer, candidate.selection.spans) : message.renderPage(*renderer)) ||
                        !message.composeIncremental(rebuilt.object()) || !item.matchesDrawing(message)) continue;
                    if (++matched != 1 || !rebuilt.copyFrom(message.canvas())) return false;
                    item.clearedOrigin = origin == 1;
                }
                if (!matched && candidate.liveRefresh && !item.choice && reflowEpoch == invalidations.load() &&
                    reflowIdentity.matches(state))
                    for (const auto &basis : reflowBases)
                    {
                        auto originalStyle = item.style;
                        memcpy(originalStyle.data() + 0x524, basis.style.data() + 0x524, 4);
                        if (basis.index != item.index || basis.current != item.current || originalStyle != basis.style ||
                            basis.cursor >= item.cursor || item.cursor != item.displayed.size() || item.cursor > item.current.size() ||
                            !std::equal(item.displayed.begin(), item.displayed.end(), item.current.begin())) continue;
                        item.reflowPrefix = basis.cursor; item.reflowPosition = basis.position;
                        item.reflowNesting = value(basis.style.data(), 0x524);
                        const auto spans = item.reflowSpans();
                        LCSEDisplay::Message message(*display);
                        if (spans.empty() || !message.initializeSnapshot(originalStyle, text, state.width, state.height,
                            state.window, remaining) || !message.renderSpans(*renderer, spans, true) ||
                            !message.composeIncremental(rebuilt.object()) || !item.matchesDrawing(message) ||
                            !rebuilt.copyFrom(message.canvas())) return false;
                        item.clearedOrigin = false; matched = 1;
                        break;
                    }
                if (matched != 1) return false;
            }
            return rebuilt.matchesPixels(candidate.output.pixels);
        }
        Surface rebuilt = candidate.background;
        for (const auto &item : candidate.layers) if (!item.compose(rebuilt)) return false;
        return rebuilt.pixels == candidate.output.pixels;
    }

public:
    // failure 返回捕获失败阶段，不包含游戏文字或用户信息
    const char *failure() const noexcept { return failureCode.load(); }

    // begin 仅在基础画布复制完成的已验证阶段创建新代次，拒绝借用旧帧或嵌套帧
    bool begin(const LCSEPresentationLayout &layout, uintptr_t phase) noexcept
    {
        try
        {
            CaptureLock lock(captureLock);
            if (!lock.ownsLock()) { invalidations.fetch_add(1); return false; }
            const auto epoch = invalidations.load();
            completed.reset(); std::vector<ReflowBasis>().swap(reflowBases); ++generation;
            if (pending) { pending.reset(); return false; }
            const auto expected = layout.phases.find("background_ready");
            if (expected == layout.phases.end() || phase != expected->second || !GdiFlush()) return false;
            auto candidate = std::make_shared<Snapshot>();
            candidate->generation = generation;
            remaining = 64 * 1024 * 1024;
            failureCode.store("rejected capture_identity_or_canvas");
            if (!identity(layout, candidate->identity) || !candidate->background.capture(candidate->identity.background, remaining) ||
                candidate->background.width != candidate->identity.width || candidate->background.height != candidate->identity.height) return false;
            if (candidate->identity.choices && (!layout.choices ||
                !candidate->selection.capture(layout.globals.at("choice_renderer"), candidate->identity))) return false;
            auto &output = candidate->output;
            if (!output.capture(candidate->identity.output, remaining) || output.width != candidate->identity.width ||
                output.height != candidate->identity.height || output.pixels != candidate->background.pixels ||
                output.address == candidate->background.address || output.bitmap == candidate->background.bitmap) return false;
            const auto released = output.pixels.size();
            std::vector<BYTE>().swap(output.pixels);
            remaining += released;
            if (invalidations.load() != epoch) return false;
            pendingEpoch = epoch;
            pending = std::move(candidate);
            return true;
        }
        catch (...) { return false; }
    }

    // complete 只有同次背景、消息层和实际原帧逐像素一致时才公布快照
    bool complete(const LCSEPresentationLayout &layout, uintptr_t phase,
        const LCSEDisplayLayout *display = nullptr, LCSEGlyph::Renderer *renderer = nullptr) noexcept
    {
        try
        {
            CaptureLock lock(captureLock);
            if (!lock.ownsLock()) { invalidations.fetch_add(1); return false; }
            const auto epoch = invalidations.load();
            completed.reset();
            auto candidate = std::move(pending);
            if (!candidate || pendingEpoch != epoch) return false;
            const auto expected = layout.phases.find("messages_complete");
            Identity current;
            if ((display == nullptr) != (renderer == nullptr) || expected == layout.phases.end() || phase != expected->second || !GdiFlush() ||
                !identity(layout, current) || !candidate->identity.matches(current)) return false;
            Choice selection;
            if (current.choices && (!display || !layout.choices || !selection.capture(layout.globals.at("choice_renderer"), current) ||
                !candidate->selection.matches(selection))) return false;
            Surface background;
            if (!background.capture(current.background, remaining) || !candidate->background.sameIdentity(background) ||
                background.pixels != candidate->background.pixels) return false;
            const auto released = background.pixels.size();
            std::vector<BYTE>().swap(background.pixels);
            remaining += released;
            Surface output;
            if (!output.capture(current.output, remaining) || !candidate->output.sameIdentity(output)) return false;
            candidate->output = std::move(output);
            for (size_t index = 0; index < current.count; ++index)
            {
                const auto address = current.messages + index * 0x550;
                DWORD length;
                if (!copyReadable(&length, address + 0x100, sizeof(length))) return false;
                const auto choice = current.choices && address == current.choice ? &candidate->selection : nullptr;
                if (choice && !length) return false;
                if (!length) continue;
                Layer item;
                failureCode.store("rejected capture_message_layout");
                if (!layer(address, index, current, item, choice, display)) return false;
                candidate->layers.push_back(std::move(item));
            }
            failureCode.store("rejected native_pixel_reconstruction");
            if (!reconstruct(*candidate, display, renderer)) return false;
            if (!identity(layout, current) || !candidate->identity.matches(current)) return false;
            if (current.choices && (!selection.capture(layout.globals.at("choice_renderer"), current) ||
                !candidate->selection.matches(selection))) return false;
            candidate->incremental = display != nullptr;
            if (invalidations.load() != epoch) return false;
            if (display && !rememberReflow(*candidate, epoch)) return false;
            completedEpoch = epoch;
            completed = std::move(candidate);
            return true;
        }
        catch (...) { return false; }
    }

    // refresh 只在已验证局部呈现入口捕获当前正文，重建一致后才公布新代次
    bool refresh(const LCSEPresentationLayout &layout, uintptr_t output, const LCSEDisplayLayout &display,
        LCSEGlyph::Renderer &renderer) noexcept
    {
        try
        {
            CaptureLock lock(captureLock);
            if (!lock.ownsLock()) { invalidations.fetch_add(1); return false; }
            const auto epoch = invalidations.load();
            completed.reset(); pending.reset(); ++generation;
            if (!layout.refresh || !GdiFlush()) return false;
            auto candidate = std::make_shared<Snapshot>();
            candidate->generation = generation; candidate->incremental = candidate->liveRefresh = true;
            remaining = 64 * 1024 * 1024;
            const auto metadataBytes = reflowBytes();
            if (metadataBytes > remaining) return false;
            remaining -= metadataBytes;
            failureCode.store("rejected capture_identity_or_canvas");
            if (!identity(layout, candidate->identity) || candidate->identity.output != output || candidate->identity.choices ||
                !candidate->background.capture(candidate->identity.background, remaining) ||
                !candidate->output.capture(output, remaining) ||
                candidate->background.width != candidate->identity.width || candidate->background.height != candidate->identity.height ||
                candidate->output.width != candidate->identity.width || candidate->output.height != candidate->identity.height ||
                candidate->background.bitmap == candidate->output.bitmap || candidate->background.address == candidate->output.address) return false;
            const auto &state = candidate->identity;
            for (size_t index = 0; index < state.count; ++index)
            {
                const auto address = state.messages + index * 0x550;
                DWORD length = 0;
                if (!copyReadable(&length, address + 0x100, sizeof(length))) return false;
                if (!length) continue;
                Layer item;
                failureCode.store("rejected capture_message_layout");
                if (!layer(address, index, state, item, nullptr, &display)) return false;
                candidate->layers.push_back(std::move(item));
            }
            failureCode.store("rejected native_pixel_reconstruction");
            if (!reconstruct(*candidate, &display, &renderer) || !candidate->matchesLive(layout) || invalidations.load() != epoch) return false;
            completedEpoch = epoch; completed = std::move(candidate);
            return true;
        }
        catch (...) { return false; }
    }

    // invalidate 非阻塞淘汰旧帧，重入或跨线程取消由当前回调在提交前核对
    void invalidate(bool discardReflow = true) noexcept
    {
        const auto epoch = invalidations.fetch_add(1) + 1;
        try
        {
            CaptureLock lock(captureLock);
            if (!lock.ownsLock()) return;
            pending.reset(); completed.reset(); ++generation;
            if (discardReflow || reflowEpoch + 1 != epoch) std::vector<ReflowBasis>().swap(reflowBases);
            else reflowEpoch = epoch;
        }
        catch (...) {}
    }

    // current 核对调用方仍持有当前代次，禁止失效或正在更新的快照进入后续呈现
    bool current(const std::shared_ptr<const Snapshot> &snapshot) noexcept
    {
        try
        {
            CaptureLock lock(captureLock);
            return lock.ownsLock() && snapshot && snapshot == completed && !pending && completedEpoch == invalidations.load();
        }
        catch (...) { return false; }
    }

    // latest 返回独立且不可变的已验证快照，调用方不能借此写入游戏对象
    std::shared_ptr<const Snapshot> latest() noexcept
    {
        try
        {
            CaptureLock lock(captureLock);
            if (!lock.ownsLock() || completedEpoch != invalidations.load()) return {};
            return completed;
        }
        catch (...) { return {}; }
    }
};

// CaptureSession 按真实阶段组织捕获及可选整页呈现，关闭状态不读取游戏画布
class CaptureSession : public std::enable_shared_from_this<CaptureSession>
{
    LCSEPresentationLayout contract;
    LCSEDisplayLayout display;
    std::shared_ptr<LCSEGlyph::Renderer> renderer;
    std::shared_ptr<const TranslationMap> translations;
    std::atomic<bool> submission{false};
    std::function<void(const char *)> diagnostics;
    ULONGLONG lastDiagnostic = 0;
    bool drawingFailed = false;
    // report 对失败限频，恢复只报告一次，防止逐字刷新刷屏
    void report(const char *reason, bool recovered = false) noexcept
    {
        try
        {
        if (!submission || !diagnostics) return;
        if (recovered && !drawingFailed) return;
        const auto now = GetTickCount64();
        if (!recovered && lastDiagnostic && now - lastDiagnostic < 5000) return;
        drawingFailed = !recovered; lastDiagnostic = now; diagnostics(reason);
        } catch (...) {}
    }
    Recorder recorder;
    std::atomic_flag callbackLock = ATOMIC_FLAG_INIT;
    std::atomic<bool> enabled{false};
    std::atomic<unsigned long long> invalidations{1};
    std::atomic<uint64_t> repaintTarget{0};
    unsigned long long epoch = 0;
    unsigned depth = 0, stage = 0;
    DWORD thread = 0;
    std::shared_ptr<const Snapshot> completed;
    std::shared_ptr<const TranslatedFrame> translatedFrame;
    std::shared_ptr<const Snapshot> refreshPending;
    HDC refreshDC = nullptr;
    uintptr_t refreshRectangleAddress = 0;
    std::array<DWORD, 4> refreshRectangle{};
    DWORD refreshThread = 0;
    unsigned refreshDepth = 0;
    std::unique_ptr<ChoiceFrame> hoverNative, hoverTranslation;
    DWORD hoverThread = 0, hoverIndex = 0, hoverHighlight = 0;
    bool hoverPending = false, hoverReady = false;

    // rememberTarget 只记录已经校验的窗口线程，不保留游戏像素或可变对象所有权
    void rememberTarget(const Identity &identity) noexcept
    {
        repaintTarget.store(uint64_t(uintptr_t(identity.window)) | (uint64_t(identity.thread) << 32));
    }

    // requestRepaint 控制线程取消显示后只请求原生重绘，禁止向新页面写回旧原帧
    void requestRepaint() noexcept
    {
        const uint64_t target = repaintTarget.exchange(0);
        if (!target) return;
        const auto window = reinterpret_cast<HWND>(uintptr_t(DWORD(target)));
        const DWORD thread = DWORD(target >> 32);
        DWORD process = 0, owner = 0;
        HWND current = nullptr;
        const auto global = contract.globals.find("window");
        if (global != contract.globals.end() && copyReadable(&owner, global->second, sizeof(owner)) && owner &&
            copyReadable(&current, uintptr_t(owner) + 0x114, sizeof(current)) && current == window &&
            IsWindow(window) && GetWindowThreadProcessId(window, &process) == thread && process == GetCurrentProcessId())
            InvalidateRect(window, nullptr, FALSE);
    }

    // clearHover 撤销未闭合操作和跟踪画面，不更改原生菜单或消息
    void clearHover()
    {
        hoverNative.reset(); hoverTranslation.reset(); hoverThread = hoverIndex = hoverHighlight = 0;
        hoverPending = hoverReady = false;
    }

    // beginHover 为原生与译文连续操作预留共同峰值预算，禁止复制整份场景
    bool beginHover()
    {
        if (hoverNative) return hoverNative->matchesLive(contract);
        if (!completed || !completed->identity.choices || !completed->matchesState(contract, completed->output, true)) return false;
        size_t remaining = 64 * 1024 * 1024;
        const auto reserve = [&](size_t bytes) { if (bytes > remaining) return false; remaining -= bytes; return true; };
        // 最终原生复制额外持有一份私有DIB，计入同一峰值预算
        if (!reserve(completed->output.pixels.size())) return false;
        if (!reserve(completed->background.pixels.size()) || !reserve(completed->output.pixels.size())) return false;
        for (const auto &layer : completed->layers)
            for (auto surface : {&layer.color, &layer.shadow, &layer.mask}) if (!reserve(surface->pixels.size())) return false;
        if (translatedFrame)
        {
            if (translatedFrame->source() != completed || !reserve(translatedFrame->pixels()->pixels.size())) return false;
            for (const auto &layer : translatedFrame->layers())
                for (auto surface : {&layer.color, &layer.shadow, &layer.mask}) if (!reserve(surface->pixels.size())) return false;
        }
        // 两份跟踪帧和各自的原子临时帧按共同64MiB预算校验
        for (size_t index = 0; index < (translatedFrame ? 4 : 2); ++index)
            if (!reserve(completed->output.pixels.size())) return false;
        auto native = std::make_unique<ChoiceFrame>();
        if (!native->initialize(completed)) return false;
        std::unique_ptr<ChoiceFrame> translated;
        if (translatedFrame)
        {
            translated = std::make_unique<ChoiceFrame>();
            if (!translated->initializeTranslation(translatedFrame)) return false;
        }
        hoverNative = std::move(native); hoverTranslation = std::move(translated);
        return true;
    }

    // clear 淘汰当前阶段和公布结果，已返回快照仍由调用方独立持有
    void clear()
    {
        recorder.invalidate(); completed.reset(); translatedFrame.reset(); depth = stage = 0; thread = 0;
        refreshPending.reset(); refreshDC = nullptr; refreshRectangleAddress = 0; refreshRectangle = {};
        refreshThread = 0; refreshDepth = 0;
        clearHover();
    }

public:
    // submissionSupported 用户资源呈现要求整页、正文增量、菜单和悬停全部成功挂接
    bool submissionSupported() const noexcept { return contract.atomicPresentation && contract.refresh && contract.choices && contract.hover && contract.dcFrameOffset; }

    // refreshPresentation 资源就绪后请求游戏线程自行重绘，不从控制线程执行游戏绘制函数
    void refreshPresentation() const noexcept
    {
        const auto global = contract.globals.find("window");
        DWORD owner = 0, process = 0;
        HWND window = nullptr;
        if (global != contract.globals.end() && copyReadable(&owner, global->second, sizeof(owner)) && owner &&
            copyReadable(&window, uintptr_t(owner) + 0x114, sizeof(window)) && IsWindow(window) &&
            GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId())
            InvalidateRect(window, nullptr, FALSE);
    }

    // CaptureSession 保存挂接前的不可变布局，不在后续回调重新扫描已修改代码
    CaptureSession(const LCSEPresentationLayout &presentation, const LCSEDisplayLayout &drawing,
        std::function<void(const char *)> log = {})
        : contract(presentation), display(drawing), diagnostics(std::move(log)) {}

    // configure 只在无活动回调时启用完整绘制契约，取消无需等待游戏线程
    bool configure(bool active, std::shared_ptr<LCSEGlyph::Renderer> glyphs = {},
        std::shared_ptr<const TranslationMap> mapping = {}, bool submit = false) noexcept
    {
        enabled.store(false); invalidations.fetch_add(1); recorder.invalidate();
        requestRepaint();
        try
        {
            CaptureLock lock(callbackLock);
            if (!lock.ownsLock()) return !active;
            clear(); epoch = invalidations.load(); submission = false;
            if (!active) { renderer.reset(); translations.reset(); return true; }
            if (contract.functions.count("scene_redraw") != 1 || contract.phases.size() != 3 ||
                !contract.phases.count("background_ready") || !contract.phases.count("messages_complete") ||
                !contract.phases.count("scene_present_return") || display.tables.size() != 8 ||
                (submit && (!submissionSupported() || !mapping))) return false;
            renderer = glyphs ? std::move(glyphs) : std::make_shared<LCSEGlyph::Renderer>();
            translations = std::move(mapping);
            submission = submit;
            enabled.store(true);
            return true;
        }
        catch (...) { return false; }
    }

    // invalidate 在翻页、读档或菜单更换时立即撤销旧代次，不阻塞绘制回调
    void invalidate() noexcept
    {
        invalidations.fetch_add(1); recorder.invalidate();
        try { CaptureLock lock(callbackLock); if (lock.ownsLock()) clear(); }
        catch (...) {}
    }

    // blitFailed 在原生复制或提交后校验失败时限频报告，不卸载已就绪资源
    void blitFailed() noexcept
    {
        CaptureLock lock(callbackLock);
        if (lock.ownsLock()) report("rejected native_blit_or_changed_frame");
    }

    // blitCurrent 前后复查同次快照/悬停/阶段，设置界面或取消后不能使用旧中文
    bool blitCurrent(const std::shared_ptr<const Snapshot> &candidate, unsigned long long stamp, bool hover,
        bool publish = false, const std::shared_ptr<const TranslatedFrame> &translated = {}) noexcept
    {
        CaptureLock lock(callbackLock);
        if (!lock.ownsLock() || !enabled.load() || invalidations.load() != stamp) return false;
        const bool valid = hover ? hoverReady && !hoverPending && hoverNative && hoverNative->source() == candidate && hoverNative->matchesLive(contract) :
            recorder.current(candidate) && candidate->matchesLive(contract);
        if (valid)
        {
            if (publish) { completed = candidate; translatedFrame = translated; }
            report("recovered atomic_blit", true);
        }
        return valid;
    }

    // prepareBlit 在已核对的BitBlt调用指令执行前完成中文合成，禁止原文先上屏再覆盖
    std::unique_ptr<PendingBlit> prepareBlit(uintptr_t address, uintptr_t frame, uintptr_t *arguments) noexcept
    {
        if (!enabled.load() || !submission || !contract.atomicPresentation) return {};
        CaptureLock lock(callbackLock);
        if (!lock.ownsLock()) return {};
        try
        {
            const auto stamp = invalidations.load();
            if (!enabled.load()) return {};
            const bool rect = address == contract.submissionPhases.at("rect_blit");
            const bool full = address == contract.submissionPhases.at("full_blit");
            if ((!rect && !full) || !frame || !arguments) return {};
            // 设置或恢复正文撤销旧页后，直接整帧入口也负责接纳新代次。
            // 资源映射仍有效，不能一直等待可能不再执行的scene/rect阶段。
            if (epoch != stamp) { clear(); epoch = stamp; }
            const auto tid = GetCurrentThreadId();
            const bool hover = rect && hoverNative && hoverReady && !hoverPending;
            const bool direct = full && !depth && !refreshDepth && !hoverPending;
            if ((rect && (depth || refreshDepth != 1 || refreshThread != tid)) ||
                (full && !direct && (depth != 1 || stage != 3 || thread != tid))) return {};
            if (direct)
            {
                // 配置/窗口重绘会直接呈现完整输出，不一定经过scene_redraw或rect入口。
                // 每次重新捕获并逐像素证明，不依赖原先的呈现路径或旧中文帧。
                DWORD output = 0;
                completed.reset(); translatedFrame.reset(); recorder.invalidate(false);
                if (!copyReadable(&output, frame - 8, 4) || !renderer || !recorder.refresh(contract, output, display, *renderer))
                { report(recorder.failure()); return {}; }
            }
            auto candidate = hover ? hoverNative->source() : rect ? refreshPending : recorder.latest();
            if (!candidate || (hover ? !hoverNative->matchesLive(contract) : !recorder.current(candidate) || !candidate->matchesLive(contract)))
            { report("rejected source_state"); return {}; }
            DWORD object = 0, sourceDC = 0;
            PresentationTarget target;
            // 两种呈现函数具有同一已核对EBP布局：+8窗口DC、-8对象、-4原源DC。
            if (!copyReadable(&object, frame - 8, 4) || object != candidate->identity.output ||
                !copyReadable(&sourceDC, frame - 4, 4) || arguments[5] != sourceDC ||
                GetObjectType(reinterpret_cast<HDC>(sourceDC)) != OBJ_MEMDC ||
                reinterpret_cast<uintptr_t>(GetCurrentObject(reinterpret_cast<HDC>(sourceDC), OBJ_BITMAP)) != candidate->output.bitmap ||
                !target.read(frame, contract, candidate->identity, contract.refreshDCFrameOffset) ||
                !target.matchesDC(reinterpret_cast<HDC>(arguments[0])) || (rect && !target.matchesDC(refreshDC)) || arguments[8] != SRCCOPY)
            { report("rejected destination_or_source_dc"); return {}; }
            const std::array<uintptr_t, 4> area = rect ? std::array<uintptr_t, 4>{refreshRectangle[0],refreshRectangle[1],refreshRectangle[2],refreshRectangle[3]} :
                std::array<uintptr_t, 4>{0,0,uintptr_t(candidate->identity.width),uintptr_t(candidate->identity.height)};
            if (arguments[1] != area[0] || arguments[2] != area[1] || arguments[3] != area[2] || arguments[4] != area[3] ||
                arguments[6] != area[0] || arguments[7] != area[1]) { report("rejected blit_rectangle"); return {}; }
            const Surface *pixels = nullptr;
            std::shared_ptr<TranslatedFrame> translated;
            if (hover) { if (hoverTranslation) pixels = hoverTranslation->pixels(); }
            else if (translations && !translations->empty())
            {
                translated = std::make_shared<TranslatedFrame>();
                const auto stagingBytes = candidate->output.pixels.size();
                if (stagingBytes >= 64 * 1024 * 1024 || !renderer ||
                    !translated->initialize(candidate, *translations, display, *renderer, 64 * 1024 * 1024 - stagingBytes))
                { report("rejected translation_composition"); return {}; }
                if (translated->translatedCount()) pixels = translated->pixels();
            }
            if (!pixels) { report("fallback no_visible_mapping_or_glyph"); return {}; }
            auto self = shared_from_this();
            const auto valid = [self,candidate,stamp,hover,direct,translated]() { return self->blitCurrent(candidate, stamp, hover, direct, translated); };
            // initialize内部的最后复查仍持有本锁，此处用无锁同线程校验，返回后改用blitCurrent。
            const auto inside = [&]() { return enabled.load() && invalidations.load() == stamp &&
                (hover ? hoverNative->matchesLive(contract) : recorder.current(candidate) && candidate->matchesLive(contract)); };
            auto pending = std::make_unique<PendingBlit>();
            if (!pending->initialize(*pixels, contract.submissionPhases.at(rect ? "rect_done" : "full_done"), arguments, inside, valid))
            { report("rejected private_bitmap"); return {}; }
            if (!hover && !direct) translatedFrame = std::move(translated);
            rememberTarget(candidate->identity);
            return pending;
        }
        catch (...) { report("rejected blit_exception"); return {}; }
    }

    // observe 接收已安装Hook的准确地址，仅在同线程阶段闭合后公布真实呈现帧
    bool observe(uintptr_t address, uintptr_t frame = 0, uintptr_t object = 0, HDC destination = nullptr,
        uintptr_t rectangle = 0) noexcept
    {
        if (!enabled.load()) return false;
        if (!contract.refresh)
            for (const auto &phase : contract.refreshPhases) if (address == phase.second) return false;
        if (!contract.hover)
            for (const auto &phase : contract.hoverPhases) if (address == phase.second) return false;
        CaptureLock lock(callbackLock);
        if (!lock.ownsLock()) { invalidations.fetch_add(1); recorder.invalidate(); return false; }
        try
        {
            const auto stamp = invalidations.load();
            if (!enabled.load()) return false;
            if (epoch != stamp) { clear(); epoch = stamp; }
            bool accepted = false;
            const auto currentThread = GetCurrentThreadId();
            if (contract.hover && contract.refresh && contract.hoverPhases.size() == 2 &&
                (address == contract.hoverPhases.at("hover_enter") || address == contract.hoverPhases.at("hover_complete")))
            {
                if (depth || refreshDepth) { clear(); return false; }
                if (address == contract.hoverPhases.at("hover_enter"))
                {
                    const auto index = uintptr_t(destination);
                    accepted = !hoverPending && index <= 63 && rectangle <= 1 && beginHover() &&
                        index < hoverNative->source()->identity.choices;
                    if (accepted)
                    {
                        hoverIndex = DWORD(index); hoverHighlight = DWORD(rectangle); hoverThread = currentThread;
                        hoverPending = true; hoverReady = false;
                        completed.reset(); translatedFrame.reset(); recorder.invalidate();
                    }
                }
                else if (hoverPending && hoverThread == currentThread && frame && hoverNative)
                {
                    DWORD index = 0, highlight = 0;
                    accepted = copyReadable(&index, frame + 8, 4) && copyReadable(&highlight, frame + 12, 4) &&
                        index == hoverIndex && (highlight & 0xff) == hoverHighlight &&
                        hoverNative->apply(index, hoverHighlight != 0) && hoverNative->matchesLive(contract) &&
                        (!hoverTranslation || hoverTranslation->apply(index, hoverHighlight != 0));
                    hoverPending = false; hoverReady = accepted;
                }
                if (!accepted || !enabled.load() || invalidations.load() != stamp) { clear(); return false; }
                return true;
            }
            if (contract.refresh && (address == contract.refreshPhases.at("refresh_enter") ||
                address == contract.refreshPhases.at("refresh_present_return")))
            {
                // 整页内部的局部调用仍由整页生命周期负责，不在其中公布独立刷新结果
                if (depth) return false;
                if (hoverNative)
                {
                    const auto &source = hoverNative->source();
                    if (address == contract.refreshPhases.at("refresh_enter"))
                    {
                        accepted = !hoverPending && hoverReady && !refreshDepth && object == source->identity.output &&
                            destination && rectangle && copyReadable(refreshRectangle.data(), rectangle, sizeof(refreshRectangle)) &&
                            hoverNative->pixels()->rectangle(int(refreshRectangle[0]), int(refreshRectangle[1]),
                                int(refreshRectangle[2]), int(refreshRectangle[3])) && hoverNative->matchesLive(contract);
                        if (accepted)
                        {
                            refreshDepth = 1; refreshThread = currentThread; refreshDC = destination; refreshRectangleAddress = rectangle;
                        }
                    }
                    else
                    {
                        const auto parameters = [&]()
                        {
                            DWORD pointer = 0, liveObject = 0;
                            std::array<DWORD, 4> repeated{};
                            return frame && copyReadable(&pointer, frame + 12, 4) && pointer == refreshRectangleAddress &&
                                copyReadable(repeated.data(), pointer, sizeof(repeated)) && repeated == refreshRectangle &&
                                copyReadable(&liveObject, uintptr_t(int64_t(frame) + contract.refreshObjectFrameOffset), 4) &&
                                liveObject == source->identity.output;
                        };
                        const auto current = [&]()
                        {
                            return enabled.load() && invalidations.load() == stamp && hoverReady && !hoverPending &&
                                parameters() && hoverNative->matchesLive(contract) && enabled.load() && invalidations.load() == stamp;
                        };
                        PresentationTarget target;
                        accepted = refreshDepth == 1 && refreshThread == currentThread && current() &&
                            target.read(frame, contract, source->identity, contract.refreshDCFrameOffset) && target.matchesDC(refreshDC);
                        if (accepted && submission && hoverTranslation)
                        {
                            rememberTarget(source->identity);
                            accepted = current();
                        }
                        refreshDepth = 0; refreshThread = 0; refreshDC = nullptr; refreshRectangleAddress = 0; refreshRectangle = {};
                    }
                    if (!accepted || !enabled.load() || invalidations.load() != stamp) { clear(); return false; }
                    return true;
                }
                if (address == contract.refreshPhases.at("refresh_enter"))
                {
                    completed.reset(); translatedFrame.reset(); recorder.invalidate(false);
                    if (refreshDepth)
                    {
                        if (refreshThread == currentThread && refreshDepth < 32) ++refreshDepth;
                        refreshPending.reset(); return false;
                    }
                    refreshDepth = 1; refreshThread = currentThread; refreshDC = destination;
                    refreshRectangleAddress = rectangle;
                    accepted = destination && rectangle && copyReadable(refreshRectangle.data(), rectangle, sizeof(refreshRectangle)) &&
                        renderer && recorder.refresh(contract, object, display, *renderer);
                    refreshPending = accepted ? recorder.latest() : std::shared_ptr<const Snapshot>{};
                    accepted = refreshPending && refreshPending->output.rectangle(int(refreshRectangle[0]), int(refreshRectangle[1]),
                        int(refreshRectangle[2]), int(refreshRectangle[3]));
                    if (!accepted) { refreshPending.reset(); report(recorder.failure()); }
                }
                else
                {
                    auto candidate = refreshPending;
                    DWORD liveObject = 0;
                    const auto parameters = [&]()
                    {
                        DWORD pointer = 0;
                        std::array<DWORD, 4> repeated{};
                        return frame && copyReadable(&pointer, frame + 12, sizeof(pointer)) && pointer == refreshRectangleAddress &&
                            copyReadable(repeated.data(), pointer, sizeof(repeated)) && repeated == refreshRectangle;
                    };
                    if (refreshDepth == 1 && refreshThread == currentThread && frame && candidate &&
                        copyReadable(&liveObject, uintptr_t(int64_t(frame) + contract.refreshObjectFrameOffset), sizeof(liveObject)) &&
                        liveObject == candidate->identity.output && parameters() && recorder.current(candidate) && candidate->matchesLive(contract))
                    {
                        auto translated = translatedFrame;
                        accepted = !translated || translated->source() == candidate;
                        PresentationTarget target;
                        const auto current = [&]()
                        {
                            return enabled.load() && invalidations.load() == stamp && recorder.current(candidate) &&
                                candidate->matchesLive(contract) && parameters() && enabled.load() && invalidations.load() == stamp;
                        };
                        accepted = accepted && target.read(frame, contract, candidate->identity, contract.refreshDCFrameOffset) &&
                            target.matchesDC(refreshDC) && current();
                        if (accepted && submission && translated && translated->translatedCount())
                        {
                            rememberTarget(candidate->identity);
                            accepted = current();
                        }
                        if (accepted) { completed = std::move(candidate); translatedFrame = std::move(translated); }
                    }
                    if (refreshDepth) --refreshDepth;
                    if (!refreshDepth) { refreshThread = 0; refreshDC = nullptr; refreshRectangleAddress = 0; refreshRectangle = {}; refreshPending.reset(); }
                    if (!accepted) { completed.reset(); translatedFrame.reset(); recorder.invalidate(); }
                }
                if (!enabled.load() || invalidations.load() != stamp) { clear(); return false; }
                return accepted;
            }
            if (address == contract.functions.at("scene_redraw"))
            {
                if (refreshDepth) { clear(); return false; }
                clearHover();
                completed.reset(); translatedFrame.reset(); recorder.invalidate();
                if (!depth) { depth = 1; thread = currentThread; stage = 1; accepted = true; }
                else if (thread == currentThread && depth < 32) { ++depth; stage = 4; }
                else { clear(); }
            }
            else if (depth && thread == currentThread && address == contract.phases.at("scene_present_return"))
            {
                if (depth == 1 && stage == 3)
                {
                    auto candidate = recorder.latest();
                    accepted = candidate && recorder.current(candidate) && candidate->matchesLive(contract);
                    auto translated = translatedFrame;
                    accepted = accepted && (!translated || translated->source() == candidate);
                    if (accepted && submission && translated && translated->translatedCount())
                    {
                        PresentationTarget target;
                        const auto current = [&]()
                        {
                            return enabled.load() && invalidations.load() == stamp && recorder.current(candidate) &&
                                candidate->matchesLive(contract) && enabled.load() && invalidations.load() == stamp;
                        };
                        accepted = target.read(frame, contract, candidate->identity);
                        if (accepted)
                        {
                            rememberTarget(candidate->identity);
                            accepted = current();
                        }
                    }
                    if (accepted) { completed = std::move(candidate); translatedFrame = std::move(translated); }
                }
                --depth;
                if (!depth) { stage = 0; thread = 0; }
                if (!accepted) { completed.reset(); translatedFrame.reset(); recorder.invalidate(); }
            }
            else if (depth == 1 && thread == currentThread && stage == 1 && address == contract.phases.at("background_ready"))
            {
                accepted = renderer && recorder.begin(contract, address); stage = accepted ? 2 : 4;
            }
            else if (depth == 1 && thread == currentThread && stage == 2 && address == contract.phases.at("messages_complete"))
            {
                accepted = renderer && recorder.complete(contract, address, &display, renderer.get()); stage = accepted ? 3 : 4;
                if (!accepted) report(recorder.failure());
            }
            else { completed.reset(); translatedFrame.reset(); recorder.invalidate(); stage = 4; }
            if (!enabled.load() || invalidations.load() != stamp) { clear(); return false; }
            return accepted;
        }
        catch (...) { clear(); return false; }
    }

    // latest 仅返回已通过真实呈现阶段并仍处于当前代次的只读快照
    std::shared_ptr<const Snapshot> latest() noexcept
    {
        try
        {
            CaptureLock lock(callbackLock);
            if (!lock.ownsLock() || !enabled.load() || epoch != invalidations.load() || depth || refreshDepth || !recorder.current(completed)) return {};
            return completed;
        }
        catch (...) { return {}; }
    }

    // current 在绘制线程复查已公布代次和实时场景，GDI回调取消后也不能接受旧帧
    bool current(const std::shared_ptr<const Snapshot> &snapshot) noexcept
    {
        try
        {
            CaptureLock lock(callbackLock);
            if (!lock.ownsLock() || !snapshot || snapshot != completed || !enabled.load() ||
                epoch != invalidations.load() || depth || refreshDepth || !recorder.current(snapshot)) return false;
            if (!snapshot->matchesLive(contract)) return false;
            return enabled.load() && epoch == invalidations.load() && recorder.current(snapshot);
        }
        catch (...) { return false; }
    }

    // translation 返回与当前公布原帧绑定的独立译文，实际呈现前仍须核对current
    std::shared_ptr<const TranslatedFrame> translation() noexcept
    {
        try
        {
            CaptureLock lock(callbackLock);
            if (!lock.ownsLock() || !enabled.load() || epoch != invalidations.load() || depth || refreshDepth ||
                !translatedFrame || translatedFrame->source() != completed || !recorder.current(completed)) return {};
            return translatedFrame;
        }
        catch (...) { return {}; }
    }
};
}
