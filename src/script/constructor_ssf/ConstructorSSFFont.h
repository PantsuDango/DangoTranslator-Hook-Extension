#pragma once
#include <windows.h>
#include <array>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <cstdint>

// Minimal documented Direct3D8 COM ABI. No D3DX or redistributable DLL dependency.
// Method slots are the IDirect3DDevice8/IDirect3DTexture8 public interfaces.
namespace ssffont {
template<unsigned N,class R=HRESULT,class... A> R com(void *p,A... args){using F=R(STDMETHODCALLTYPE*)(void*,A...);return ((F)(*(void***)p)[N])(p,args...);}
struct LockedRect {int pitch;void *bits;};
struct Viewport {DWORD x,y,width,height;float minZ,maxZ;};
struct Vertex {float x,y,z,rhw;DWORD color;float u,v;};
struct Glyph {std::array<DWORD,1024> pixels{};bool valid=false;};
inline Glyph rasterGlyph(HDC dc,wchar_t c){
 Glyph glyph;WORD index=0xffff;if(GetGlyphIndicesW(dc,&c,1,&index,GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR||index==0xffff)return glyph;
 MAT2 matrix{};matrix.eM11.value=matrix.eM22.value=1;GLYPHMETRICS gm{};auto n=GetGlyphOutlineW(dc,c,GGO_GRAY8_BITMAP,&gm,0,nullptr,&matrix);
 bool space=c==32||c==0xa0||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x202f||c==0x205f||c==0x3000;
 if(n==0&&space){glyph.valid=true;return glyph;}
 if(n==GDI_ERROR||n>4096||gm.gmBlackBoxX>24||gm.gmBlackBoxY>24)return glyph;
 std::vector<BYTE>b(n);if(n&&GetGlyphOutlineW(dc,c,GGO_GRAY8_BITMAP,&gm,n,b.data(),&matrix)!=n)return glyph;
 auto left=gm.gmptGlyphOrigin.x,top=20-gm.gmptGlyphOrigin.y;auto stride=(gm.gmBlackBoxX+3)&~3u;
 // Fit the complete ink box, including legitimate negative side bearings.
 if(left < -24||left>48||top < -24||top>48||uint64_t(stride)*gm.gmBlackBoxY>b.size())return glyph;
 left=(std::max)(LONG(0),(std::min)(left,LONG(24-gm.gmBlackBoxX)));top=(std::max)(LONG(0),(std::min)(top,LONG(24-gm.gmBlackBoxY)));
 for(unsigned y=0;y<gm.gmBlackBoxY;y++)for(unsigned x=0;x<gm.gmBlackBoxX;x++){auto alpha=(std::min)(64u,unsigned(b[y*stride+x]))*255/64;glyph.pixels[(top+y)*32+left+x]=(alpha<<24)|0xffffff;}
 glyph.valid=true;return glyph;
}
struct Raster {
 std::map<wchar_t,Glyph> glyphs;
 // Build the whole resource's CPU glyph set before publication. Missing glyphs
 // stay invalid and cause complete-message fallback; they don't reject loading.
 void build(const std::map<std::wstring,std::wstring> &texts,const wchar_t *face){
  std::set<wchar_t> chars;for(auto &p:texts)for(auto c:p.second)if(c!=L'\n')chars.insert(c);
  // Bound memory without disabling unrelated messages when the budget is full.
  while(chars.size()>8192)chars.erase(std::prev(chars.end()));
  glyphs.clear();for(auto c:chars)glyphs.emplace(c,Glyph{});
  auto dc=CreateCompatibleDC(nullptr);if(!dc)return;
  // Explicit fallback faces let GetGlyphIndices prove symbol availability;
  // relying on an outline from implicit GDI font linking can draw .notdef.
  for(auto family:{face,L"Segoe UI Symbol",L"MS Gothic",L"Arial"}){
   auto font=CreateFontW(-22,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,family);if(!font)continue;
   auto old=SelectObject(dc,font);if(old&&old!=HGDI_ERROR){for(auto &g:glyphs)if(!g.second.valid)g.second=rasterGlyph(dc,g.first);SelectObject(dc,old);}DeleteObject(font);
  }
  DeleteDC(dc);
 }
 bool covers(const std::wstring &s)const{for(auto c:s){if(c==L'\n')continue;auto f=glyphs.find(c);if(f==glyphs.end()||!f->second.valid)return false;}return true;}
};
class Renderer {
 void *device=nullptr;std::map<wchar_t,void*>textures;std::shared_ptr<const Raster> raster;
 void clear(){for(auto &p:textures)com<2,ULONG>(p.second);textures.clear();if(device)com<2,ULONG>(device);device=nullptr;raster.reset();}
 bool upload(wchar_t c){if(textures.count(c))return true;auto f=raster->glyphs.find(c);if(f==raster->glyphs.end()||!f->second.valid)return false;void *texture=nullptr;
  // A8R8G8B8 = 21, D3DPOOL_MANAGED = 1. Managed textures survive Reset.
  if(FAILED(com<20>(device,32u,32u,1u,DWORD(0),21,1,&texture))||!texture)return false;LockedRect rect{};bool locked=SUCCEEDED(com<16>(texture,0u,&rect,(const RECT*)nullptr,DWORD(0)));bool ok=locked&&rect.bits&&rect.pitch>=128&&rect.pitch<=65536;
  if(ok){for(unsigned y=0;y<32;y++)memcpy((BYTE*)rect.bits+y*rect.pitch,f->second.pixels.data()+y*32,128);}if(locked)ok=SUCCEEDED(com<17>(texture,0u))&&ok;if(!ok){com<2,ULONG>(texture);return false;}textures.emplace(c,texture);return true;
 }
 bool state(DWORD type,DWORD value){return SUCCEEDED(com<50>(device,type,value));}
 bool stage(DWORD n,DWORD type,DWORD value){return SUCCEEDED(com<63>(device,n,type,value));}
 bool quad(wchar_t c,int x,int y,int size,DWORD color){float a=float(x)-.5f,b=float(y)-.5f,z=float(size);Vertex v[4]={{a,b,0,1,color,0,0},{a+z,b,0,1,color,.75f,0},{a,b+z,0,1,color,0,.75f},{a+z,b+z,0,1,color,.75f,.75f}};return SUCCEEDED(com<61>(device,DWORD(0),textures[c]))&&SUCCEEDED(com<72>(device,5,2u,v,UINT(sizeof(Vertex))));}
public:
 ~Renderer(){clear();}
 void release(){clear();}
 // Allocate every required glyph before suppressing the original line. All
 // rendering state and stream zero are restored, including failed operations.
 bool draw(void *d,const std::shared_ptr<const Raster>&r,const std::wstring &full,const std::wstring &visible,int x,int y,int size,DWORD color,bool shadow){
  if(!d||!r||size<8||size>96||visible.size()>128||!r->covers(full)||!r->covers(visible)||FAILED(com<3>(d)))return false;
  if(device!=d||raster!=r){clear();device=d;com<1,ULONG>(device);raster=r;}
  for(auto c:full)if(c!=L'\n'&&!upload(c))return false;
  for(auto c:visible)if(c!=L'\n'&&!upload(c))return false;
  DWORD token=0;if(FAILED(com<57>(device,1,&token)))return false;
  void *stream=nullptr;UINT stride=0;bool streamSaved=SUCCEEDED(com<84>(device,0u,&stream,&stride));
  bool ok=streamSaved&&state(7,0)&&state(14,0)&&state(15,0)&&state(27,1)&&state(19,5)&&state(20,6)&&state(22,1)&&state(28,0)&&state(137,0)&&state(136,1)&&SUCCEEDED(com<76>(device,DWORD(0x144)))&&SUCCEEDED(com<88>(device,DWORD(0)))&&stage(0,1,4)&&stage(0,2,2)&&stage(0,3,0)&&stage(0,4,4)&&stage(0,5,2)&&stage(0,6,0)&&stage(0,11,0)&&stage(0,13,3)&&stage(0,14,3)&&stage(0,16,1)&&stage(0,17,1)&&stage(0,24,0)&&stage(1,1,1)&&stage(1,4,1);
  if(ok)for(size_t n=0;n<visible.size();n++){if(shadow)ok=quad(visible[n],x+int(n)*size+2,y+2,size,0xff000000)&&ok;ok=quad(visible[n],x+int(n)*size,y,size,color)&&ok;}
  bool restored=SUCCEEDED(com<54>(device,token));if(streamSaved)restored=SUCCEEDED(com<83>(device,0u,stream,stride))&&restored;if(stream)com<2,ULONG>(stream);com<56>(device,token);return ok&&restored;
 }
};
}
