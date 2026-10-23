#pragma once
#include <dwrite.h>
#include <memory>
#include <string>
#include <vector>

namespace csystemfont {
// Bundle 只持有私有显示格式，不修改游戏原字体对象
struct Bundle {
    std::vector<std::pair<IDWriteTextFormat *,IDWriteTextFormat *>> formats;
    std::wstring text;
    ~Bundle(){for(auto &format:formats){format.second->Release();format.first->Release();}}
};
struct Resources {
    HMODULE module=nullptr;
    IDWriteFactory *factory=nullptr;
    IDWriteFontCollection *collection=nullptr;
    IDWriteFontFamily *family=nullptr;
    // open 核对中文字体确实存在，禁止依赖系统自动补字
    bool open() {
        module=LoadLibraryW(L"dwrite.dll");if(!module)return false;
        using Create=HRESULT(WINAPI *)(DWRITE_FACTORY_TYPE,REFIID,IUnknown **);
        auto create=reinterpret_cast<Create>(GetProcAddress(module,"DWriteCreateFactory"));
        if(!create||FAILED(create(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown **>(&factory)))||
           FAILED(factory->GetSystemFontCollection(&collection)))return false;
        UINT32 index=0;BOOL exists=FALSE;
        return SUCCEEDED(collection->FindFamilyName(L"Microsoft YaHei",&index,&exists))&&exists&&SUCCEEDED(collection->GetFontFamily(index,&family));
    }
    // close 在全部活动排版退出后释放私有字体服务
    void close(){if(family)family->Release();if(collection)collection->Release();if(factory)factory->Release();if(module)FreeLibrary(module);family=nullptr;collection=nullptr;factory=nullptr;module=nullptr;}
    ~Resources(){close();}
    // copy 按原字号和样式创建格式，逐字检查中文字形覆盖
    IDWriteTextFormat *copy(IDWriteTextFormat *source,const std::wstring &text) {
        if(!family||!source)return nullptr;
        auto weight=source->GetFontWeight();auto style=source->GetFontStyle();auto stretch=source->GetFontStretch();
        IDWriteFont *font=nullptr;
        if(FAILED(family->GetFirstMatchingFont(weight,stretch,style,&font)))return nullptr;
        bool covered=true;
        for(auto c:text){if(c==0xfffc||c==L'\n'||c==L'\r'||c==L'\t')continue;BOOL has=FALSE;if(FAILED(font->HasCharacter(c,&has))||!has){covered=false;break;}}
        font->Release();if(!covered)return nullptr;
        auto size=source->GetFontSize();if(!(size>0&&size<=512))return nullptr;
        wchar_t locale[85]{};auto length=source->GetLocaleNameLength();
        if(length>=85||FAILED(source->GetLocaleName(locale,85)))return nullptr;
        IDWriteTextFormat *format=nullptr;
        if(FAILED(factory->CreateTextFormat(L"Microsoft YaHei",collection,weight,style,stretch,size,locale,&format)))return nullptr;
        DWRITE_LINE_SPACING_METHOD method{};FLOAT spacing=0,baseline=0;
        DWRITE_TRIMMING trimming{};IDWriteInlineObject *sign=nullptr;
        bool ok=SUCCEEDED(source->GetLineSpacing(&method,&spacing,&baseline))&&SUCCEEDED(source->GetTrimming(&trimming,&sign));
        ok=ok&&SUCCEEDED(format->SetTextAlignment(source->GetTextAlignment()))&&SUCCEEDED(format->SetParagraphAlignment(source->GetParagraphAlignment()))&&
            SUCCEEDED(format->SetWordWrapping(source->GetWordWrapping()))&&SUCCEEDED(format->SetReadingDirection(source->GetReadingDirection()))&&
            SUCCEEDED(format->SetFlowDirection(source->GetFlowDirection()))&&SUCCEEDED(format->SetIncrementalTabStop(source->GetIncrementalTabStop()))&&
            SUCCEEDED(format->SetLineSpacing(method,spacing,baseline))&&SUCCEEDED(format->SetTrimming(&trimming,sign));
        if(sign)sign->Release();if(!ok){format->Release();return nullptr;}return format;
    }
    // prepare 为当前T保留两份原格式引用并建立有界私有格式
    std::shared_ptr<Bundle> prepare(IDWriteTextFormat *first,IDWriteTextFormat *second,const std::wstring &text) {
        auto bundle=std::make_shared<Bundle>();bundle->text=text;
        for(auto source:{first,second})if(!resolve(*bundle,source))return {};
        return bundle;
    }
    // sameSettings 比较不可变私有格式的样式，避免逐字重建产生无限引用
    bool sameSettings(IDWriteTextFormat *a,IDWriteTextFormat *b) {
        if(!b||a->GetFontSize()!=b->GetFontSize()||a->GetFontWeight()!=b->GetFontWeight()||a->GetFontStyle()!=b->GetFontStyle()||a->GetFontStretch()!=b->GetFontStretch()||
            a->GetTextAlignment()!=b->GetTextAlignment()||a->GetParagraphAlignment()!=b->GetParagraphAlignment()||a->GetWordWrapping()!=b->GetWordWrapping()||
            a->GetReadingDirection()!=b->GetReadingDirection()||a->GetFlowDirection()!=b->GetFlowDirection()||a->GetIncrementalTabStop()!=b->GetIncrementalTabStop())return false;
        wchar_t al[85]{},bl[85]{};DWRITE_LINE_SPACING_METHOD am{},bm{};FLOAT as=0,bs=0,ab=0,bb=0;
        if(FAILED(a->GetLocaleName(al,85))||FAILED(b->GetLocaleName(bl,85))||wcscmp(al,bl)||FAILED(a->GetLineSpacing(&am,&as,&ab))||FAILED(b->GetLineSpacing(&bm,&bs,&bb))||am!=bm||as!=bs||ab!=bb)return false;
        DWRITE_TRIMMING at{},bt{};IDWriteInlineObject *ai=nullptr,*bi=nullptr;
        bool ok=SUCCEEDED(a->GetTrimming(&at,&ai))&&SUCCEEDED(b->GetTrimming(&bt,&bi))&&at.granularity==bt.granularity&&at.delimiter==bt.delimiter&&at.delimiterCount==bt.delimiterCount&&ai==bi;
        if(ai)ai->Release();if(bi)bi->Release();return ok;
    }
    // resolve 覆盖原排版中合法的样式切换，最多保留32份格式
    IDWriteTextFormat *resolve(Bundle &bundle,IDWriteTextFormat *source) {
        for(auto &format:bundle.formats)if(sameSettings(format.second,source))return format.second;
        if(bundle.formats.size()>=32)return nullptr;
        auto format=copy(source,bundle.text);if(!format)return nullptr;
        source->AddRef();bundle.formats.emplace_back(source,format);return format;
    }
};
}
