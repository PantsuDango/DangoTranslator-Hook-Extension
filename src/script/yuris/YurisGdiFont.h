#pragma once

// isFontCustomized 判断当前是否启用了字体或字符集设置
bool isFontCustomized()
{
  return commonsharedmem->fontCharSetEnabled || wcslen(maybe_disabled_fontFamily());
}

// isFontSizeCustomized 判断当前是否启用了字体尺寸缩放
bool isFontSizeCustomized()
{
  return commonsharedmem->FontSizeRelative != 1;
}

// DCFontSwitcher 在单次绘制期间借用有容量上限的字体，结束后恢复游戏原字体
class DCFontSwitcher
{
  struct CachedFont
  {
    LOGFONTW description;
    HFONT font;
    size_t users;
  };
  static constexpr size_t maximumFonts = 64;
  inline static std::mutex cacheMutex;
  inline static std::list<CachedFont> cache;
  HDC dc = nullptr;
  HFONT previousFont = nullptr;
  HFONT borrowedFont = nullptr;

  // acquireFont 按完整字体参数复用字体，只淘汰未被绘制使用的对象
  static HFONT acquireFont(const LOGFONTW &description)
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    for (auto &entry : cache)
    {
      if (memcmp(&entry.description, &description, sizeof(description)) == 0)
      {
        ++entry.users;
        return entry.font;
      }
    }
    if (cache.size() >= maximumFonts)
    {
      for (auto entry = cache.begin(); entry != cache.end(); ++entry)
      {
        if (!entry->users && DeleteObject(entry->font))
        {
          cache.erase(entry);
          break;
        }
      }
      if (cache.size() >= maximumFonts)
        return nullptr;
    }
    const auto font = Hijack::oldCreateFontIndirectW(&description);
    if (font)
      cache.push_back({description, font, 1});
    return font;
  }

  // releaseFont 归还已经从设备上下文移除的字体，供后续绘制复用
  static void releaseFont(HFONT font)
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    for (auto &entry : cache)
    {
      if (entry.font == font)
      {
        if (entry.users)
          --entry.users;
        return;
      }
    }
  }

public:
  // DCFontSwitcher 读取游戏原字体的逻辑参数，避免从绘制度量反推导致字号累积变化
  explicit DCFontSwitcher(HDC deviceContext) : dc(deviceContext)
  {
    if (!dc || !commonsharedmem)
      return;
    const auto original = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
    LOGFONTW description{};
    if (!original || GetObjectW(original, sizeof(description), &description) != sizeof(description))
      return;
    customizeLogFontA(reinterpret_cast<LOGFONTA *>(&description));
    ScaleFont(&description);
    const auto family = maybe_disabled_fontFamily();
    if (family[0])
      wcsncpy_s(description.lfFaceName, LF_FACESIZE, family, _TRUNCATE);
    borrowedFont = acquireFont(description);
    if (!borrowedFont)
      return;
    const auto previous = SelectObject(dc, borrowedFont);
    if (!previous || previous == HGDI_ERROR)
    {
      releaseFont(borrowedFont);
      borrowedFont = nullptr;
      return;
    }
    previousFont = static_cast<HFONT>(previous);
  }

  // ~DCFontSwitcher 恢复游戏字体后归还缓存，恢复失败时保留引用以防删除仍被选中的字体
  ~DCFontSwitcher()
  {
    if (previousFont && borrowedFont)
    {
      const auto restored = SelectObject(dc, previousFont);
      if (restored && restored != HGDI_ERROR)
        releaseFont(borrowedFont);
    }
  }

  DCFontSwitcher(const DCFontSwitcher &) = delete;
  DCFontSwitcher &operator=(const DCFontSwitcher &) = delete;
};

// createYurisFontIndirect 按Unicode创建且只返回一个字体对象，避免中文字体名分支泄漏句柄
HFONT createYurisFontIndirect(const LOGFONTA *source)
{
  if (!source || !commonsharedmem)
    return Hijack::oldCreateFontIndirectA(source);
  LOGFONTW description{};
  memcpy(&description, source, offsetof(LOGFONTA, lfFaceName));
  const auto nameLength = strnlen_s(source->lfFaceName, LF_FACESIZE);
  MultiByteToWideChar(CP_ACP, 0, source->lfFaceName, static_cast<int>(nameLength),
      description.lfFaceName, LF_FACESIZE - 1);
  customizeLogFontA(reinterpret_cast<LOGFONTA *>(&description));
  ScaleFont(&description);
  const auto family = maybe_disabled_fontFamily();
  if (family[0])
    wcsncpy_s(description.lfFaceName, LF_FACESIZE, family, _TRUNCATE);
  return Hijack::oldCreateFontIndirectW(&description);
}
