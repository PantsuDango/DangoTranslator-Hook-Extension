#pragma once
#include "ConstructorSSFSnapshot.h"
#include <algorithm>

namespace ssftext {
constexpr size_t NormalCells=30,MaximumCells=45;
struct Plan {
 std::vector<std::wstring> original,lines;
 std::vector<bool> preserve;
};
inline bool opening(wchar_t c){return c==L'「'||c==L'『'||c==L'“'||c==L'"';}
inline std::wstring trim(const std::wstring &s){return constructorssfsnapshot::normalize(s);}
// Reflow only visible body slots. A separate speaker slot is a structural
// property of a short label followed by quoted dialogue, never a name list.
inline bool plan(const std::wstring &source,const std::wstring &translated,Plan &out){
 Plan next;auto full=constructorssfsnapshot::newlines(source);size_t begin=0;
 while(true){auto end=full.find(L'\n',begin);next.original.push_back(full.substr(begin,end==std::wstring::npos?end:end-begin));if(end==std::wstring::npos)break;begin=end+1;}
 while(!next.original.empty()&&next.original.back().empty())next.original.pop_back();
 if(next.original.empty()||next.original.size()>16)return false;
 next.lines.resize(next.original.size());next.preserve.resize(next.original.size());
 size_t first=0;while(first<next.original.size()&&next.original[first].empty())first++;
 if(first==next.original.size())return false;
 size_t body=first;auto flat=constructorssfsnapshot::newlines(translated);
 flat.erase(std::remove(flat.begin(),flat.end(),L'\n'),flat.end());
 auto label=trim(next.original[first]);size_t second=first+1;while(second<next.original.size()&&next.original[second].empty())second++;
 if(!label.empty()&&label.size()<=16&&!opening(label.front())&&label.find_first_of(L"、。…「『」』“”\"：:")==std::wstring::npos&&second<next.original.size()){
  auto following=trim(next.original[second]);
  if(!following.empty()&&opening(following.front())){
   body=second;next.preserve[first]=true;
   auto quote=flat.find_first_of(L"「『“\"");
   // Background translation flattens newlines. Recover an explicit translated
   // label before the opening quote; an omitted label keeps the source label.
   if(quote!=std::wstring::npos&&quote>0){auto name=trim(flat.substr(0,quote));while(!name.empty()&&(name.back()==L':'||name.back()==L'：'))name.pop_back();name=trim(name);
    if(!name.empty()&&name.size()<=16){next.lines[first]=name;next.preserve[first]=false;flat.erase(0,quote);}
   }
  }
 }
 size_t weight=0,remaining=0;for(size_t n=0;n<next.original.size();n++){auto &s=next.original[n];if(s.size()>63)return false;if(n>=body&&!s.empty()){weight+=s.size();remaining++;}}
 if(!remaining||flat.empty()||flat.size()>MaximumCells*remaining)return false;
 size_t at=0;for(size_t n=body;n<next.original.size();n++){auto &s=next.original[n];if(s.empty())continue;remaining--;auto left=flat.size()-at;auto take=remaining?(left*s.size()+weight-1)/weight:left;take=(std::min)(MaximumCells,take);if(left>take+remaining*MaximumCells)take=left-remaining*MaximumCells;if(take>MaximumCells)return false;next.lines[n]=flat.substr(at,take);at+=take;weight-=s.size();}
 if(at!=flat.size())return false;out=std::move(next);return true;
}
}
