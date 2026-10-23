#include "../ConstructorSSFText.h"
#include "../ConstructorSSFFont.h"
#include <cstdio>
#define CHECK(v) do{if(!(v)){printf("FAIL line %d: %s\n",__LINE__,#v);return 1;}}while(0)

int main(){
 ssftext::Plan p;
 const std::wstring source=L"役名\r\n「最初の行。\r\n　次の行。」\r\n";
 CHECK(ssftext::plan(source,L"角色：“经常这样，虽然内容改变了。”",p));
 CHECK(p.original.size()==3&&p.lines[0]==L"角色"&&!p.preserve[0]);
 CHECK(p.lines[1]+p.lines[2]==L"“经常这样，虽然内容改变了。”");
 CHECK(source==L"役名\r\n「最初の行。\r\n　次の行。」\r\n");
 CHECK(ssftext::plan(source,L"“没有姓名的译文。”",p));
 CHECK(p.preserve[0]&&p.lines[0].empty());
 CHECK(p.lines[1]+p.lines[2]==L"“没有姓名的译文。”");
 CHECK(ssftext::plan(L"『独白。』\r\n",L"“独白。”",p));
 CHECK(p.original.size()==1&&p.lines[0]==L"“独白。”"&&!p.preserve[0]);
 CHECK(ssftext::plan(L"短い地の文、\r\n「引用を含む続き」\r\n",L"开头的叙述，后面还有“引用”的完整句子。",p));
 CHECK(!p.preserve[0]&&p.lines[0]+p.lines[1]==L"开头的叙述，后面还有“引用”的完整句子。");
 CHECK(ssftext::plan(L"一行\r\n\r\n二行\r\n",L"保留中间空行",p));
 CHECK(p.original.size()==3&&p.lines[1].empty()&&p.lines[0]+p.lines[2]==L"保留中间空行");
 CHECK(ssftext::plan(L"\r\n一行\r\n",L"保留前空行",p));
 CHECK(p.original.size()==2&&p.lines[0].empty()&&p.lines[1]==L"保留前空行");
 CHECK(ssftext::plan(L"原文",std::wstring(36,L'译'),p));
 CHECK(!ssftext::plan(L"原文",std::wstring(46,L'译'),p));
 CHECK(!ssftext::plan(source,std::wstring(91,L'译'),p)); // Name has no spare body capacity.
 std::wstring sixteen;for(unsigned n=0;n<16;n++)sixteen+=L"原文\r\n";
 CHECK(ssftext::plan(sixteen,L"正文",p)&&p.original.size()==16);
 CHECK(!ssftext::plan(sixteen+L"额外\r\n",L"正文",p));

 std::map<std::wstring,std::wstring> texts{{L"neutral",L"经虽，。 “ ”　 \u00a0♪"}};
 ssffont::Raster raster;raster.build(texts,L"Microsoft YaHei");
 CHECK(raster.covers(texts.begin()->second));CHECK(!raster.covers(L"\uffff"));
 auto dc=CreateCompatibleDC(nullptr);CHECK(dc);
 auto font=CreateFontW(-22,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");CHECK(font);
 auto old=SelectObject(dc,font);CHECK(old&&old!=HGDI_ERROR);
 MAT2 matrix{};matrix.eM11.value=matrix.eM22.value=1;
 for(auto c:std::wstring(L"经虽")){
  GLYPHMETRICS gm{};auto size=GetGlyphOutlineW(dc,c,GGO_GRAY8_BITMAP,&gm,0,nullptr,&matrix);CHECK(size!=GDI_ERROR&&size>0);
  std::vector<BYTE> bits(size);CHECK(GetGlyphOutlineW(dc,c,GGO_GRAY8_BITMAP,&gm,size,bits.data(),&matrix)==size);
  uint64_t expected=0,actual=0;auto stride=(gm.gmBlackBoxX+3)&~3u;
  for(unsigned y=0;y<gm.gmBlackBoxY;y++)for(unsigned x=0;x<gm.gmBlackBoxX;x++)expected+=(std::min)(64u,unsigned(bits[y*stride+x]))*255/64;
  for(auto pixel:raster.glyphs.at(c).pixels)actual+=pixel>>24;
  CHECK(actual==expected&&actual>0); // Entire ink survives its negative bearing.
 }
 for(auto c:{L' ',L'\u3000',L'\u00a0'})for(auto pixel:raster.glyphs.at(c).pixels)CHECK(pixel==0);
 SelectObject(dc,old);DeleteObject(font);DeleteDC(dc);
 puts("PASS speaker/body, blank slots, capacity, spaces and complete glyph ink");return 0;
}
