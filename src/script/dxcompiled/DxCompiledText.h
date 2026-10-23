#pragma once
#include "DxCompiledSnapshot.h"
#include <algorithm>
namespace dxtext {
struct Cell {size_t offset;int row,column;bool highlight;std::wstring glyph;};
struct Segment {std::wstring original;size_t begin=0,end=0;int page=0;bool unsafe=false;std::vector<Cell> cells;};
inline bool decode(const char *p,size_t n,std::wstring &out){if(n>65535)return false;if(!n){out.clear();return true;}int count=MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,p,int(n),nullptr,0);if(count<=0)return false;out.resize(count);return MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,p,int(n),out.data(),count)==count&&out.find(wchar_t(0xfffd))==std::wstring::npos;}
inline bool parse(const std::string &raw,std::vector<Segment>&out){
 if(raw.empty()||raw.size()>65535)return false;std::vector<Segment> next;Segment s;std::string plain;int row=0,column=0,page=0;bool highlight=false,ended=false;
 auto flush=[&](size_t end){std::wstring text;if(!decode(plain.data(),plain.size(),text))return false;s.original=dxcompiledsnapshot::normalize(text);s.end=end;s.page=page;if(!s.original.empty())next.push_back(s);s=Segment{};s.begin=end+1;plain.clear();return true;};
 for(size_t at=0;at<raw.size();){auto c=raw[at];if(!c)break;if(c==';'){plain+='\n';row++;column=0;at++;}else if(c=='F'){highlight=!highlight;at++;}else if(c=='@'||c=='C'||c=='E'){if(!flush(at))return false;at++;if(c=='C'){row=column=0;page++;}if(c=='E'){ended=true;break;}}else{if(at+1>=raw.size()||!raw[at+1])return false;std::wstring glyph;if(!decode(raw.data()+at,2,glyph))return false;s.unsafe=s.unsafe||glyph.empty()||glyph.size()>2;for(auto g:glyph)s.unsafe=s.unsafe||g<32||g==0x7f;plain.append(raw,at,2);s.cells.push_back({at,row,column,highlight,glyph});column++;if(column==24){column=0;row++;}at+=2;}}
 if(!ended)return false;out=std::move(next);return true;
}
// rowCells 按原生双字节槽恢复整行，英文字母对不改变后续槽位索引
inline bool rowCells(const std::string &raw,std::vector<std::wstring>&cells){
 if(raw.size()>48||raw.size()%2)return false;cells.clear();for(size_t at=0;at<raw.size();at+=2){std::wstring glyph;if(!decode(raw.data()+at,2,glyph)||glyph.empty()||glyph.size()>2)return false;for(auto c:glyph)if(c<32||c==0x7f)return false;cells.push_back(std::move(glyph));}return true;
}
// translate owns only the source's allocated display cells. No buffer, cursor,
// control marker, branch value or input state is ever written.
inline bool fit(const Segment&s,const std::wstring&t,std::wstring &glyphs){
 if(s.unsafe||s.cells.empty()||!dxcompiledsnapshot::safe(s.original,t))return false;glyphs.assign(s.cells.size(),L'　');size_t at=0;int previousRow=s.cells.front().row;bool written=false;
 for(auto c:t){if(c==L'\n'){while(at<s.cells.size()&&s.cells[at].row==previousRow)at++;if(at==s.cells.size())return false;previousRow=s.cells[at].row;}else{if(at==s.cells.size())return false;previousRow=s.cells[at].row;glyphs[at++]=c;written=true;}}
 return written;
}
// displaySpan 扩展到本段已有行内的空位，后续段落即使尚未揭示也预留原位置
inline Segment displaySpan(const Segment&s,const std::vector<Segment>&segments){
 Segment out=s;out.cells.clear();if(s.cells.empty())return out;
 const auto &first=s.cells.front(),&last=s.cells.back();if(first.row<0||last.row>=20||first.row>last.row)return out;
 size_t index=0;for(int row=first.row;row<=last.row;row++){int begin=row==first.row?first.column:0,end=24;
  for(auto &other:segments)if(other.page==s.page&&other.begin>s.begin&&!other.cells.empty()&&other.cells.front().row==row)end=(std::min)(end,other.cells.front().column);
  for(int col=begin;col<end;col++){if(index<s.cells.size()&&s.cells[index].row==row&&s.cells[index].column==col)out.cells.push_back(s.cells[index++]);else out.cells.push_back({SIZE_MAX,row,col,false,L""});}
 }if(index!=s.cells.size())out.cells.clear();return out;
}
inline std::wstring quote(const std::wstring &text){std::wstring out=L"\"";for(auto c:text){if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}else if(c==L'\n')out+=L"\\n";else if(c==L'\r')out+=L"\\r";else if(c==L'\t')out+=L"\\t";else if(c>=32)out+=c;}return out+L'"';}
}
