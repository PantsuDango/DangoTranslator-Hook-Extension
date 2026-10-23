#include "GSX.h"
#include "GSXControl.h"
#include "GSXLayout.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace {
gsxlayout::Layout layout;
uintptr_t module = 0;
std::recursive_mutex mutex;
gsxcontrol::Client client;
struct Snapshot {
  gsxcontrol::Mode mode{};
  gsxsnapshot::Texts texts;
};
std::shared_ptr<const Snapshot> snapshot;
std::atomic<bool> quiescing{false}, stopping{false}, stopped{true},
    restored{true};
std::atomic<unsigned> active{0};
std::thread loader;
DWORD renderThread = 0;
uint64_t page = 0, serial = 0;
size_t budget = 0;
struct Active { // Active 对每次回调计数以保护卸载
  Active() { active.fetch_add(1); }
  ~Active() { active.fetch_sub(1); }
};
struct Surface {
  uintptr_t bitmap = 0, pixels = 0, alpha = 0;
  uint32_t width = 0, height = 0, stride = 0, alphaStride = 0;
};
struct Style {
  LOGFONTW font{};
  uint32_t alignment = 0, color = 0;
};
struct Slot {
  Surface surface;
  std::vector<BYTE> baseline, alphaBaseline, shown, alphaShown;
  std::wstring source, translated;
  uint64_t generation = 0, page = 0, lastUse = 0;
  Style style;
  Style capturedStyle;
  bool injected = false, invalid = false, hasStyle = false;
};
std::map<uintptr_t, Slot> slots;
struct Pending {
  std::wstring source, translated;
  uint64_t generation = 0;
};
std::map<uintptr_t, Pending> pending;
bool framed = false, batchChanged = false;
uint64_t batchPage = 0, boundary = 0, lastBoundary = 0;
std::wstring lastBatch;
struct MenuSlot {
  HMENU menu = nullptr;
  UINT position = 0;
  std::wstring source, shown;
};
std::vector<MenuSlot> menus;
// number 有界读取对齐或非对齐的原生整数
bool number(uintptr_t p, uint32_t &v) {
  if (!gsxlayout::memory(p, 4))
    return false;
  memcpy(&v, (void *)p, 4);
  return true;
}
// current 每次回调核对Helper身份及资源代次
std::shared_ptr<const Snapshot> current() {
  auto s = std::atomic_load(&snapshot);
  return !quiescing.load() && s && client.currentBackfill(s->mode) ? s
                                                                   : nullptr;
}
// loadSnapshots 只在线程中加载不可变资源，不操作游戏对象
void loadSnapshots() {
  uint64_t epoch = 0, helper = 0;
  while (!stopping.load()) {
    gsxcontrol::Mode m{};
    if (client.mode(m) && m.enabled == 2) {
      if (m.epoch != epoch || m.helperCreated != helper) {
        epoch = m.epoch;
        helper = m.helperCreated;
        bool ok = false;
        std::shared_ptr<Snapshot> next;
        try {
          next = std::make_shared<Snapshot>();
          next->mode = m;
          ok = client.loadSnapshot(m, next->texts);
        } catch (...) {
          ok = false;
        }
        if (client.currentBackfill(m) && !stopping.load()) {
          std::shared_ptr<const Snapshot> value = ok ? next : nullptr;
          std::atomic_store(&snapshot, value);
          client.acknowledge(m, ok);
        }
      }
    } else {
      std::atomic_store(&snapshot, std::shared_ptr<const Snapshot>{});
      epoch = helper = 0;
    }
    Sleep(20);
  }
  stopped.store(true);
}
// objectText 读取完整UTF16源对象，输入框保留原来的可编辑值
bool objectText(uintptr_t object, std::wstring &text) {
  uint32_t type = 0, pointer = 0, length = 0;
  if (!gsxlayout::memory(object, 0x41c) ||
      !number(object + layout.fields[gsxlayout::Type], type) ||
      (type != 2 && type != 6) ||
      !number(object + layout.fields[gsxlayout::Buffer], pointer) ||
      !number(object + layout.fields[gsxlayout::Length], length) || !pointer ||
      length > 8192)
    return false;
  for (unsigned n = 0; n <= length; n++) {
    if (!gsxlayout::memory(pointer + n * 2, 2))
      return false;
    wchar_t c = *(wchar_t *)(pointer + n * 2);
    if (!c) {
      text = gsxsnapshot::normalize(text);
      return !text.empty();
    }
    if (c >= 0xd800 && c <= 0xdfff)
      return false;
    text += c;
  }
  return false;
}
// surface 独立核对文字位图的拥有者、像素格式、步长和可写容量
bool surface(uintptr_t object, Surface &s) {
  uint32_t bits = 0, mode = 0;
  if (!number(object + 0x218, s.bitmap) || !number(object + 0x34c, s.width) ||
      !number(object + 0x350, s.height) || !s.width || s.width > 4096 ||
      !s.height || s.height > 4096 || !number(s.bitmap + 0x23c, bits) ||
      bits != 32 || !number(s.bitmap + 0x264, mode) || mode != 4 ||
      !number(s.bitmap + 0x248, s.stride) ||
      !number(s.bitmap + 0x24c, s.alphaStride) ||
      !number(s.bitmap + 0x308, s.pixels) ||
      !number(s.bitmap + 0x30c, s.alpha) || s.stride < s.width * 4 ||
      s.stride > 32768 || s.alphaStride < s.width || s.alphaStride > 8192)
    return false;
  uint64_t bytes = uint64_t(s.stride) * s.height,
           alpha = uint64_t(s.alphaStride) * s.height;
  if (bytes + alpha > 16 * 1024 * 1024)
    return false;
  return gsxlayout::memory(s.pixels, size_t(bytes), true) &&
         gsxlayout::memory(s.alpha, size_t(alpha), true);
}
// sameSurface 防止对象及位图重建后误用旧页像素
bool sameSurface(const Surface &a, const Surface &b) {
  return a.bitmap == b.bitmap && a.pixels == b.pixels && a.alpha == b.alpha &&
         a.width == b.width && a.height == b.height && a.stride == b.stride &&
         a.alphaStride == b.alphaStride;
}
// dirty 只通知原生重绘，不修改脚本、游标或命中区域
void dirty(uintptr_t object) {
  if (gsxlayout::memory(object + 0x38, 1, true))
    *(BYTE *)(object + 0x38) = 1;
  for (auto key : {gsxlayout::Dirty, gsxlayout::Dirty2}) {
    auto rva = layout.fields[key];
    if (rva && gsxlayout::memory(module + rva, 1, true))
      *(BYTE *)(module + rva) = 1;
  }
}
// restore 仅恢复仍与本适配器提交结果精确相同的像素
bool restore(uintptr_t object, Slot &slot) {
  if (!slot.injected)
    return true;
  Surface actual;
  if (!surface(object, actual) || !sameSurface(actual, slot.surface)) {
    slot.injected = false;
    return false;
  }
  auto &s = slot.surface;
  if (memcmp((void *)s.pixels, slot.shown.data(), slot.shown.size()) ||
      memcmp((void *)s.alpha, slot.alphaShown.data(), slot.alphaShown.size())) {
    slot.injected = false;
    return false;
  }
  memcpy((void *)s.pixels, slot.baseline.data(), slot.baseline.size());
  memcpy((void *)s.alpha, slot.alphaBaseline.data(), slot.alphaBaseline.size());
  slot.injected = false;
  dirty(object);
  return true;
}
// erase 释放受预算控制的私有基线，不释放游戏拥有的对象
void erase(std::map<uintptr_t, Slot>::iterator it) {
  auto &v = it->second;
  budget -= v.baseline.size() + v.alphaBaseline.size() + v.shown.size() +
            v.alphaShown.size();
  slots.erase(it);
}
// ensureSlot 为当前对象取得有界状态，淘汰时先恢复仍归属自身的显示
Slot &ensureSlot(uintptr_t object) {
  auto it = slots.find(object);
  if (it == slots.end()) {
    if (slots.size() >= 128) {
      auto oldest = std::min_element(
          slots.begin(), slots.end(), [](const auto &a, const auto &b) {
            return a.second.lastUse < b.second.lastUse;
          });
      restore(oldest->first, oldest->second);
      pending.erase(oldest->first);
      erase(oldest);
    }
    it = slots.emplace(object, Slot{}).first;
  }
  it->second.lastUse = GetTickCount64();
  return it->second;
}
// quote 转义共享文本协议中的控制字符
std::wstring quote(const std::wstring &s) {
  std::wstring out = L"\"";
  for (auto c : s) {
    if (c == L'"')
      out += L"\\\"";
    else if (c == L'\\')
      out += L"\\\\";
    else if (c == L'\n')
      out += L"\\n";
    else if (c == L'\r')
      out += L"\\r";
    else if (c == L'\t')
      out += L"\\t";
    else if (c < 32) {
      wchar_t b[7]{};
      swprintf_s(b, L"\\u%04x", unsigned(c));
      out += b;
    } else
      out += c;
  }
  return out + L'"';
}
// packet 携带原文身份及实际呈现结果，不让注入中文再次进入实时翻译
std::wstring packet(uint64_t id, const std::wstring &source,
                    const std::wstring &translated, uint64_t generation,
                    int kind) {
  return L"{\"nscripter\":1,\"kind\":" + std::to_wstring(kind) + L",\"page\":" +
         std::to_wstring(id) + L",\"revision\":" + std::to_wstring(++serial) +
         L",\"tick\":" + std::to_wstring(GetTickCount64()) +
         L",\"end\":true,\"segment\":false,\"backfillGeneration\":" +
         std::to_wstring(generation) + L",\"text\":" + quote(source) +
         L",\"original\":" + quote(source) + L",\"translation\":" +
         quote(translated) + L",\"speaker\":\"\"}";
}
// readStyle 从实际HFONT恢复有符号字体高度，不能把半字号字段当作像素高度
bool readStyle(uintptr_t object, Style &style) {
  uint32_t font = 0;
  if (!number(object + 0x260, font) || !font ||
      GetObjectW(reinterpret_cast<HFONT>(uintptr_t(font)), sizeof(style.font),
                 &style.font) != sizeof(style.font) ||
      !number(object + 0x274, style.alignment) ||
      !number(object + 0x2d8, style.color) || style.alignment > 2)
    return false;
  auto height = style.font.lfHeight;
  if (!height || height < -192 || height > 192 || style.font.lfEscapement ||
      style.font.lfOrientation || style.font.lfUnderline ||
      style.font.lfStrikeOut)
    return false;
  // 保留原生正负高度、粗斜体及字号，中文字体和字形质量由私有显示控制
  style.font.lfWidth = 0;
  style.font.lfCharSet = DEFAULT_CHARSET;
  style.font.lfOutPrecision = OUT_DEFAULT_PRECIS;
  style.font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
  style.font.lfQuality = NONANTIALIASED_QUALITY;
  style.font.lfPitchAndFamily = DEFAULT_PITCH;
  memset(style.font.lfFaceName, 0, sizeof(style.font.lfFaceName));
  wcscpy_s(style.font.lfFaceName, L"Microsoft YaHei");
  return true;
}
// sameStyle 比较实际字体描述，句中换字或重用HFONT句柄不能误复用旧中文字形
bool sameStyle(const Style &a, const Style &b) {
  return a.alignment == b.alignment && a.color == b.color &&
         memcmp(&a.font, &b.font, sizeof(a.font)) == 0;
}
// displayDiagnostic 每个资源代次仅记录一次各类结果，保留局部回退的可定位原因
void displayDiagnostic(uint64_t epoch, unsigned reason, uintptr_t object) {
  static uint64_t previous = 0;
  static uint32_t reasons = 0;
  if (previous != epoch) {
    previous = epoch;
    reasons = 0;
  }
  if (reason > 6 || (reasons & (1u << reason)))
    return;
  reasons |= 1u << reason;
  const char *names[] = {
      "applied", "missing_translation", "surface",        "shared_surface",
      "font",    "layout_or_glyph",     "budget_or_epoch"};
  uint32_t half = 0, width = 0, height = 0;
  number(object + 0x26c, half);
  number(object + 0x34c, width);
  number(object + 0x350, height);
  char message[192]{};
  sprintf_s(message, "GSX display result=%s half_size=%u surface=%ux%u",
            names[reason], half, width, height);
  Msg::Log(message);
}
// chinese 在私有DIB中按真实字体完整排版并检查字形，超长和缺字只回退当前对象
bool chinese(uintptr_t object, const Surface &s, const std::wstring &text,
             std::vector<BYTE> &pixels, std::vector<BYTE> &alpha,
             const Style *checked = nullptr) {
  if (text.empty() || text.size() > 4095)
    return false;
  Style style;
  if (checked)
    style = *checked;
  else if (!readStyle(object, style))
    return false;
  int size =
      style.font.lfHeight < 0 ? -style.font.lfHeight : style.font.lfHeight;
  // 中文字体的同名字号字面偏小，先放大到原高度的1.5倍，再按表面有界回缩
  int preferredSize = (std::min)(192, (size * 3 + 1) / 2);
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc)
    return false;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = LONG(s.width);
  info.bmiHeader.biHeight = -LONG(s.height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap) {
    DeleteDC(dc);
    return false;
  }
  auto oldBitmap = SelectObject(dc, bitmap);
  HFONT font = nullptr;
  HGDIOBJ oldFont = nullptr;
  RECT box{2, 1, LONG(s.width) - 2, LONG(s.height) - 2};
  UINT flags = DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL;
  if (style.alignment == 1)
    flags |= DT_CENTER;
  if (style.alignment == 2)
    flags |= DT_RIGHT;
  bool ok = false;
  for (int n = preferredSize; n >= (std::min)(size, 10); n--) {
    auto description = style.font;
    description.lfHeight = style.font.lfHeight < 0 ? -n : n;
    font = CreateFontIndirectW(&description);
    if (!font)
      break;
    oldFont = SelectObject(dc, font);
    std::vector<WORD> glyphs(text.size());
    bool coverage =
        GetGlyphIndicesW(dc, text.data(), int(text.size()), glyphs.data(),
                         GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR;
    for (size_t i = 0; i < text.size(); i++)
      if (text[i] != L'\n' && text[i] != L'\t' && glyphs[i] == 0xffff)
        coverage = false;
    RECT measured = box;
    int height = coverage ? DrawTextW(dc, text.data(), int(text.size()),
                                      &measured, flags | DT_CALCRECT)
                          : 0;
    ok = height > 0 && height <= box.bottom - box.top &&
         measured.right <= box.right;
    if (ok)
      break;
    SelectObject(dc, oldFont);
    DeleteObject(font);
    font = nullptr;
    if (!coverage)
      break;
  }
  if (ok) {
    memset(bits, 0, size_t(s.width) * s.height * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    ok = DrawTextW(dc, text.data(), int(text.size()), &box, flags) > 0;
    GdiFlush();
    if (ok) {
      pixels.assign(size_t(s.stride) * s.height, 0);
      alpha.assign(size_t(s.alphaStride) * s.height, 0);
      auto mask = (const DWORD *)bits;
      DWORD rgb = ((style.color & 255) << 16) | (style.color & 0xff00) |
                  ((style.color >> 16) & 255);
      for (unsigned y = 0; y < s.height; y++)
        for (unsigned x = 0; x < s.width; x++)
          if (mask[y * s.width + x] & 0xffffff) {
            memcpy(pixels.data() + y * s.stride + x * 4, &rgb, 4);
            alpha[y * s.alphaStride + x] = 128;
          }
    }
  }
  if (font) {
    SelectObject(dc, oldFont);
    DeleteObject(font);
  }
  SelectObject(dc, oldBitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
  return ok;
}
// applyTranslation 只替换拥有关系明确的文字位图，重复绘制复用已核对的中文字形
bool applyTranslation(uintptr_t object, Slot &slot,
                      const std::shared_ptr<const Snapshot> &resource) {
  if (!resource)
    return false;
  auto found = resource->texts.find(slot.source);
  Surface s;
  if (found == resource->texts.end()) {
    displayDiagnostic(resource->mode.epoch, 1, object);
    return false;
  }
  if (!surface(object, s)) {
    displayDiagnostic(resource->mode.epoch, 2, object);
    return false;
  }
  for (auto &v : slots)
    if (v.first != object && v.second.surface.bitmap) {
      auto &other = v.second.surface;
      auto overlaps = [](uintptr_t a, size_t an, uintptr_t b, size_t bn) {
        return a < b + bn && b < a + an;
      };
      if (other.bitmap == s.bitmap ||
          overlaps(other.pixels, size_t(other.stride) * other.height, s.pixels,
                   size_t(s.stride) * s.height) ||
          overlaps(other.alpha, size_t(other.alphaStride) * other.height,
                   s.alpha, size_t(s.alphaStride) * s.height)) {
        restore(v.first, v.second);
        slot.surface = s;
        displayDiagnostic(resource->mode.epoch, 3, object);
        return false;
      }
    }
  if (!slot.hasStyle) {
    displayDiagnostic(resource->mode.epoch, 4, object);
    return false;
  }
  const auto &style = slot.capturedStyle;
  bool reusable = sameSurface(slot.surface, s) &&
                  slot.translated == found->second &&
                  sameStyle(slot.style, style) && !slot.shown.empty();
  std::vector<BYTE> pixels, alpha;
  if (!reusable && !chinese(object, s, found->second, pixels, alpha, &style)) {
    displayDiagnostic(resource->mode.epoch, 5, object);
    return false;
  }
  size_t bytes = reusable ? slot.shown.size() : pixels.size(),
         alphaBytes = reusable ? slot.alphaShown.size() : alpha.size();
  size_t next = 2 * (bytes + alphaBytes),
         old = slot.baseline.size() + slot.alphaBaseline.size() +
               slot.shown.size() + slot.alphaShown.size();
  if (next > 64 * 1024 * 1024 || budget - old > 64 * 1024 * 1024 - next ||
      !client.currentBackfill(resource->mode)) {
    displayDiagnostic(resource->mode.epoch, 6, object);
    return false;
  }
  slot.baseline.assign((BYTE *)s.pixels, (BYTE *)s.pixels + bytes);
  slot.alphaBaseline.assign((BYTE *)s.alpha, (BYTE *)s.alpha + alphaBytes);
  if (!reusable) {
    slot.shown = std::move(pixels);
    slot.alphaShown = std::move(alpha);
  }
  slot.surface = s;
  slot.style = style;
  slot.translated = found->second;
  slot.generation = resource->mode.generation;
  memcpy((void *)s.pixels, slot.shown.data(), bytes);
  memcpy((void *)s.alpha, slot.alphaShown.data(), alphaBytes);
  slot.injected = true;
  budget = budget - old + next;
  restored.store(false);
  dirty(object);
  displayDiagnostic(resource->mode.epoch, 0, object);
  return true;
}
// queueText 将同帧姓名、正文和选项合为有界副本，不按绘制对象制造连续快进
void queueText(uintptr_t object, Slot &slot,
               const std::shared_ptr<const Snapshot> &resource) {
  auto generation = resource ? resource->mode.generation : 0;
  auto translated = slot.injected ? slot.translated : L"";
  auto &v = pending[object];
  if (v.source != slot.source || v.translated != translated ||
      v.generation != generation)
    batchChanged = true;
  v = {slot.source, translated, generation};
}
// flushText 由游戏帧边界发布完整显示组，只有实际成功绘制的译文被确认
void flushText(TextBuffer *buffer) {
  if (!buffer)
    return;
  if (pending.empty()) {
    if (boundary != lastBoundary) {
      lastBoundary = boundary;
      lastBatch.clear();
      auto resource = current();
      auto text =
          packet(++page, L"", L"", resource ? resource->mode.generation : 0, 3);
      text.pop_back();
      text += L",\"reset\":true,\"segments\":[]}";
      buffer->from(text);
    }
    return;
  }
  std::wstring source, translated, segments = L"[";
  uint64_t generation = 0;
  bool any = false, all = true;
  for (auto &v : pending) {
    if (!source.empty()) {
      source += L'\n';
      translated += L'\n';
      segments += L',';
    }
    source += v.second.source;
    segments += quote(v.second.source);
    translated +=
        v.second.translated.empty() ? v.second.source : v.second.translated;
    any |= !v.second.translated.empty();
    all &= v.second.generation != 0;
    if (v.second.generation)
      generation = v.second.generation;
  }
  auto signature = source + L"\x1f" + translated + L"\x1f" +
                   std::to_wstring(all ? generation : 0);
  if (lastBatch == signature && lastBoundary == boundary) {
    pending.clear();
    batchChanged = false;
    return;
  }
  lastBatch = signature;
  lastBoundary = boundary;
  batchPage = ++page;
  auto text = packet(batchPage, source, any ? translated : L"",
                     all ? generation : 0, 3);
  text.pop_back();
  text += L",\"segments\":" + segments + L"]}";
  if (text.size() < TEXT_BUFFER_SIZE / 2 && source.size() < 10000 &&
      translated.size() < 10000)
    buffer->from(text);
  pending.clear();
  batchChanged = false;
}
// renderBegin 在原生逐字绘制前恢复原基线，不修改对象字符串或排版状态
void renderBegin(hook_context *c, HookParam *, TextBuffer *, uintptr_t *) {
  Active guard;
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (renderThread && renderThread != GetCurrentThreadId())
    return;
  renderThread = GetCurrentThreadId();
  auto object = c->stack[1];
  auto it = slots.find(object);
  if (it != slots.end()) {
    if (!restore(object, it->second) || it->second.invalid)
      erase(it);
    else
      it->second.hasStyle = false;
  }
}
// captureStyle 在原生删除字体前复制样式，绘制结束后不再访问失效HFONT
void captureStyle(hook_context *c, HookParam *, TextBuffer *, uintptr_t *) {
  Active guard;
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (renderThread != GetCurrentThreadId() || quiescing.load())
    return;
  auto object = c->ebx;
  uint32_t font = 0;
  std::wstring source;
  if (!objectText(object, source) || !number(object + 0x260, font) ||
      font != c->stack[0])
    return;
  auto &slot = ensureSlot(object);
  slot.hasStyle = readStyle(object, slot.capturedStyle);
}
// setter 仅在游戏显示线程恢复旧页，其他线程的赋值交由下一次显示回调处理
void setter(hook_context *c, HookParam *, TextBuffer *, uintptr_t *) {
  Active guard;
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto it = slots.find(c->eax);
  uint32_t type = 0;
  if (number(c->eax + layout.fields[gsxlayout::Type], type) &&
      (type == 2 || type == 6))
    ++boundary;
  if (it != slots.end()) {
    if (renderThread == GetCurrentThreadId()) {
      restore(it->first, it->second);
      erase(it);
    } else
      it->second.invalid = true;
  }
  pending.erase(c->eax);
  batchChanged = true;
}
// renderEnd 在原生绘制完成后提交独立中文像素，并发布实际显示通知
void renderEnd(hook_context *c, HookParam *, TextBuffer *buffer, uintptr_t *) {
  Active guard;
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (renderThread != GetCurrentThreadId() || quiescing.load() ||
      (c->eax & 255) != 1)
    return;
  uint32_t object = 0;
  if (!number(c->ebp + 8, object))
    return;
  std::wstring source;
  if (!objectText(object, source))
    return;
  auto resource = current();
  auto &slot = ensureSlot(object);
  if (slot.source != source) {
    slot.page = ++page;
    slot.source = source;
    slot.translated.clear();
    batchChanged = true;
  }
  if (!slot.page)
    slot.page = ++page;
  applyTranslation(object, slot, resource);
  if (framed)
    queueText(object, slot, resource);
  else {
    auto text = packet(slot.page, source, slot.injected ? slot.translated : L"",
                       resource ? resource->mode.generation : 0, 0);
    if (text.size() < TEXT_BUFFER_SIZE / 2 && buffer)
      buffer->from(text);
  }
}
// menuText 有界读取真实Win32菜单名称，保留命令ID及状态
bool menuText(HMENU menu, UINT position, std::wstring &text) {
  wchar_t value[4096]{};
  MENUITEMINFOW info{sizeof(info)};
  info.fMask = MIIM_STRING | MIIM_FTYPE;
  info.dwTypeData = value;
  info.cch = 4095;
  if (!GetMenuItemInfoW(menu, position, TRUE, &info) ||
      info.fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR) ||
      info.cch >= 4095)
    return false;
  text.assign(value, info.cch);
  return true;
}
// setMenuText 只替换文字字段，不重建菜单和命令关系
bool setMenuText(HMENU menu, UINT position, const std::wstring &text) {
  MENUITEMINFOW info{sizeof(info)};
  info.fMask = MIIM_STRING;
  info.dwTypeData = const_cast<wchar_t *>(text.c_str());
  return SetMenuItemInfoW(menu, position, TRUE, &info) != FALSE;
}
// restoreMenus 只撤回仍属于当前适配器的菜单改动
void restoreMenus() {
  for (auto &v : menus) {
    std::wstring actual;
    if (IsMenu(v.menu) && menuText(v.menu, v.position, actual) &&
        actual == v.shown)
      setMenuText(v.menu, v.position, v.source);
  }
  menus.clear();
}
// translateMenu 有界递归处理菜单，图片及自绘项目继续保留
void translateMenu(HMENU menu, const Snapshot &resource, unsigned depth = 0) {
  if (!menu || depth > 8 || menus.size() > 512)
    return;
  int count = GetMenuItemCount(menu);
  if (count < 0 || count > 512)
    return;
  for (int i = 0; i < count; i++) {
    std::wstring source;
    if (menuText(menu, UINT(i), source)) {
      auto found = resource.texts.find(gsxsnapshot::normalize(source));
      if (found != resource.texts.end() && found->second != source &&
          setMenuText(menu, UINT(i), found->second)) {
        menus.push_back({menu, UINT(i), source, found->second});
        restored.store(false);
      }
    }
    translateMenu(GetSubMenu(menu, i), resource, depth + 1);
  }
}
// windowMenu 只在拥有窗口的游戏线程中更新本进程菜单
BOOL CALLBACK windowMenu(HWND window, LPARAM value) {
  DWORD pid = 0;
  DWORD tid = GetWindowThreadProcessId(window, &pid);
  if (pid == GetCurrentProcessId() && tid == GetCurrentThreadId() &&
      IsWindowVisible(window)) {
    translateMenu(GetMenu(window), *reinterpret_cast<const Snapshot *>(value));
    DrawMenuBar(window);
  }
  return TRUE;
}
// frame 在真实游戏线程刷新当前页、动态菜单及失效资源，不在加载线程写游戏内存
void frame(hook_context *, HookParam *, TextBuffer *buffer, uintptr_t *) {
  Active guard;
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (renderThread && renderThread != GetCurrentThreadId())
    return;
  renderThread = GetCurrentThreadId();
  auto resource = current();
  uint64_t epoch = resource ? resource->mode.epoch : 0;
  static uint64_t previous = 0, lastMenus = 0;
  bool changed = epoch != previous;
  if (changed || quiescing.load()) {
    pending.clear();
    batchChanged = true;
    for (auto &v : slots)
      restore(v.first, v.second);
    restoreMenus();
    previous = epoch;
  }
  for (auto it = slots.begin(); it != slots.end();) {
    auto &slot = it->second;
    std::wstring source;
    Surface actual;
    if (slot.invalid || !objectText(it->first, source) ||
        source != slot.source || !surface(it->first, actual) ||
        (slot.injected && !sameSurface(actual, slot.surface))) {
      restore(it->first, slot);
      pending.erase(it->first);
      auto old = it++;
      erase(old);
      continue;
    }
    if (slot.injected &&
        (!resource || slot.generation != resource->mode.generation))
      restore(it->first, slot);
    if (changed) {
      applyTranslation(it->first, slot, resource);
      queueText(it->first, slot, resource);
    }
    ++it;
  }
  auto now = GetTickCount64();
  if (changed || now - lastMenus >= 500) {
    restoreMenus();
    if (resource)
      EnumWindows(windowMenu, reinterpret_cast<LPARAM>(resource.get()));
    else
      EnumWindows(
          [](HWND w, LPARAM) -> BOOL {
            DWORD pid = 0;
            if (GetWindowThreadProcessId(w, &pid) == GetCurrentThreadId() &&
                pid == GetCurrentProcessId())
              DrawMenuBar(w);
            return TRUE;
          },
          0);
    lastMenus = now;
  }
  if (quiescing.load()) {
    for (auto &v : slots)
      restore(v.first, v.second);
    restoreMenus();
    pending.clear();
    restored.store(true);
    return;
  }
  flushText(buffer);
}
// install 仅安装经过当前镜像证据核对的专用回调
bool install(uint32_t rva, const char *name,
             decltype(HookParam::text_fun) callback) {
  if (!rva)
    return false;
  HookParam hp{};
  hp.address = module + rva;
  hp.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT;
  hp.text_fun = callback;
  return NewHook(hp, name);
}
} // namespace
// attach_function 读取与显示分别登记，不启用全局GDI采集
bool GSX::attach_function() {
  module = (uintptr_t)GetModuleHandleW(nullptr);
  if (!gsxlayout::load(layout) || !layout.fields[gsxlayout::Render] ||
      !layout.fields[gsxlayout::RenderEnd]) {
    Msg::Log("GSX read-only unavailable");
    return false;
  }
  if (!install(layout.fields[gsxlayout::Setter], "GSXAssign", setter) ||
      !install(layout.fields[gsxlayout::Render], "GSXBegin", renderBegin) ||
      !install(layout.fields[gsxlayout::RenderEnd], "GSXDialogue", renderEnd)) {
    Msg::Log("GSX read-only unavailable");
    return false;
  }
  Msg::Log("GSX read-only ready");
  if (layout.display &&
      install(layout.fields[gsxlayout::FontCapture], "GSXFont", captureStyle) &&
      install(layout.fields[gsxlayout::Frame], "GSXSprites", frame)) {
    framed = true;
    stopped.store(false);
    loader = std::thread(loadSnapshots);
    Msg::Log("GSX backfill ready");
  }
  return true;
}
// GSXPrepareDetach 先由游戏线程恢复原文，再等待回调和加载线程退出
bool GSXPrepareDetach() {
  quiescing.store(true);
  auto start = GetTickCount64();
  while (!restored.load() && GetTickCount64() - start < 5000)
    Sleep(10);
  if (!restored.load())
    return false;
  stopping.store(true);
  start = GetTickCount64();
  while ((!stopped.load() || active.load()) && GetTickCount64() - start < 5000)
    Sleep(10);
  if (!stopped.load() || active.load())
    return false;
  if (loader.joinable())
    loader.join();
  std::lock_guard<std::recursive_mutex> lock(mutex);
  slots.clear();
  menus.clear();
  pending.clear();
  budget = 0;
  return true;
}
