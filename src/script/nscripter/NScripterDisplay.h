#pragma once
#include "NScripterControl.h"
#include <memory>
#include <vector>
#include <algorithm>
#include <atomic>

namespace nscripterdisplay {
struct Font {
    HFONT handle=nullptr;
    // ~Font 只在原生绘制退出且DC已恢复后释放私有字体
    ~Font(){if(handle)DeleteObject(handle);}
};
struct Frame {
    uintptr_t source=0;std::string raw;std::wstring original,translated;
    nscriptercontrol::Mode mode{};uint64_t request=0;
    std::shared_ptr<Font> font;std::vector<bool> painted;
    bool attempted=false,rejected=false;
    bool backfill=false;uint64_t page=0,part=0;
    bool fitBody=false;int rasterCellWidth=0;LONG textWidth=0;FLOAT horizontalScale=1;
    size_t sourceOffset=SIZE_MAX;uintptr_t sourceStack=0;
};
// safeSource 仅接入连续双字节正文和末尾单个点击或清页控制
inline bool safeSource(const std::wstring &text,std::wstring &original,std::string &raw) {
    original=text;
    if(!original.empty()&&(original.back()==L'@'||original.back()==L'\\'))original.pop_back();
    if(original.empty()||original.size()>2047)return false;
    for(auto c:original)if(c<0x100||c>=0xd800&&c<=0xdfff||c>=0xe000&&c<=0xf8ff)return false;
    BOOL fallback=FALSE;auto n=WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,original.data(),int(original.size()),nullptr,0,nullptr,&fallback);
    if(n!=int(original.size()*2)||fallback)return false;
    raw.resize(n);fallback=FALSE;
    if(WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,original.data(),int(original.size()),raw.data(),n,nullptr,&fallback)!=n||fallback)return false;
    for(size_t i=0;i<raw.size();i+=2) {
        auto lead=BYTE(raw[i]),trail=BYTE(raw[i+1]);
        if(!((lead>=0x81&&lead<=0x9f)||(lead>=0xe0&&lead<=0xfc))||trail<0x40||trail>0xfc||trail==0x7f)return false;
    }
    return true;
}
// safeTranslation 保留宽度适配路径的完整译文，未接入的多行精灵仍按原槽回退
inline bool safeTranslation(Frame &frame) {
    if(frame.translated.empty()||frame.translated.size()>2047||frame.translated.size()>frame.original.size()&&!frame.fitBody)return false;
    for(auto c:frame.translated)if(c<0x20||c>=0x7f&&c<0xa0||c>=0xd800&&c<=0xdfff||c>=0xe000&&c<=0xf8ff||c>=0x202a&&c<=0x202e||c>=0x2066&&c<=0x2069||c==0xfffd)return false;
    frame.painted.resize(frame.fitBody?frame.original.size():frame.translated.size());return true;
}
// fontFailure 为字体故障提供有界子阶段诊断，不输出剧情或译文
inline bool fontFailure(const char *stage,const LOGFONTW &description) {
    static const char *stages[]={"current_font","font_object","font_metrics","font_style","create_font","select_font","font_face","glyph_query","missing_glyph","restore_font"};
    unsigned index=0;for(;index<10;index++)if(!strcmp(stage,stages[index]))break;
    static std::atomic<unsigned> logged{0};const auto bit=1u<<index;
    if(!(logged.fetch_or(bit)&bit))Msg::Log("NScripter font fallback stage=%s height=%d width=%d escapement=%d orientation=%d error=%u",stage,description.lfHeight,description.lfWidth,description.lfEscapement,description.lfOrientation,GetLastError());
    return false;
}
// prepareFont 从实际绘字DC复制样式并核对整段字形，零逻辑高度使用实际字体度量
inline bool prepareFont(Frame &frame,HDC dc) {
    LOGFONTW description{};auto source=GetCurrentObject(dc,OBJ_FONT);
    if(!source)return fontFailure("current_font",description);
    if(GetObjectW(source,sizeof(description),&description)!=sizeof(description))return fontFailure("font_object",description);
    if(description.lfHeight==0){TEXTMETRICW metric{};if(!GetTextMetricsW(dc,&metric)||metric.tmHeight<=0||metric.tmHeight>256)return fontFailure("font_metrics",description);description.lfHeight=-std::max<LONG>(1,metric.tmHeight-metric.tmInternalLeading);}
    if(description.lfEscapement||description.lfOrientation||description.lfHeight < -256||description.lfHeight > 256||description.lfWidth < -256||description.lfWidth > 256)return fontFailure("font_style",description);
    description.lfCharSet=DEFAULT_CHARSET;wcsncpy_s(description.lfFaceName,frame.mode.font,_TRUNCATE);
    auto font=std::make_shared<Font>();font->handle=CreateFontIndirectW(&description);if(!font->handle)return fontFailure("create_font",description);
    auto old=SelectObject(dc,font->handle);if(!old||old==HGDI_ERROR)return fontFailure("select_font",description);
    wchar_t actual[LF_FACESIZE]{};std::vector<WORD> glyphs(frame.translated.size());
    const char *failure=nullptr;
    if(GetTextFaceW(dc,LF_FACESIZE,actual)<=0)failure="font_face";
    else if(GetGlyphIndicesW(dc,frame.translated.data(),int(frame.translated.size()),glyphs.data(),GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR)failure="glyph_query";
    else if(std::find(glyphs.begin(),glyphs.end(),WORD(0xffff))!=glyphs.end())failure="missing_glyph";
    if(!failure&&frame.fitBody) {
        SIZE extent{};
        if(frame.rasterCellWidth<=0||frame.rasterCellWidth>1024||!GetTextExtentPoint32W(dc,frame.translated.data(),int(frame.translated.size()),&extent)||extent.cx<=0||extent.cx>2047*1024)failure="font_metrics";
        else {frame.textWidth=extent.cx;frame.horizontalScale=std::min<FLOAT>(1,FLOAT(frame.original.size()*frame.rasterCellWidth)/extent.cx);}
    }
    if(!SelectObject(dc,old)||GetCurrentObject(dc,OBJ_FONT)!=old)failure="restore_font";
    if(failure)return fontFailure(failure,description);
    frame.font=std::move(font);return true;
}
// paintFittedBody 在原生放大绘字栅格中适配和切片，缩图采样仍由引擎完成
// 原循环负责换行和推进，字体高度、剧情字符串、历史和DC状态均保持
inline bool paintFittedBody(Frame &frame,HDC dc,int x,int y,size_t index) {
    BITMAP bitmap{};POINT viewport{},window{};XFORM previous{};
    const auto mode=GetGraphicsMode(dc);
    if((x!=0&&x!=1)||y!=0||GetMapMode(dc)!=MM_TEXT||GetTextAlign(dc)!=(TA_LEFT|TA_TOP|TA_NOUPDATECP)||GetTextCharacterExtra(dc)!=0||
       !GetViewportOrgEx(dc,&viewport)||viewport.x||viewport.y||!GetWindowOrgEx(dc,&window)||window.x||window.y||
       (mode!=GM_COMPATIBLE&&(mode!=GM_ADVANCED||!GetWorldTransform(dc,&previous)||previous.eM11!=1||previous.eM22!=1||previous.eM12||previous.eM21||previous.eDx||previous.eDy))||
       GetObjectW(GetCurrentObject(dc,OBJ_BITMAP),sizeof(bitmap),&bitmap)!=sizeof(bitmap)||bitmap.bmWidth<frame.rasterCellWidth||bitmap.bmHeight<=0||bitmap.bmHeight>4096)return false;
    auto saved=SaveDC(dc);if(!saved)return false;
    bool ok=IntersectClipRect(dc,0,0,frame.rasterCellWidth,bitmap.bmHeight)!=ERROR&&SetGraphicsMode(dc,GM_ADVANCED)!=0;
    XFORM transform{frame.horizontalScale,0,0,1,FLOAT(x)-FLOAT(index*frame.rasterCellWidth),0};
    if(ok)ok=SetWorldTransform(dc,&transform)&&TextOutW(dc,0,0,frame.translated.data(),int(frame.translated.size()));
    return RestoreDC(dc,saved)&&ok;
}
}
