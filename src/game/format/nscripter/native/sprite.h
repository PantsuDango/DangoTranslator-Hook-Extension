#pragma once
#include "display.h"
typedef struct { uint32_t entry,source,end,buffer,glyph,shadow; } NSSite;
#ifdef __cplusplus
extern "C" int nss_locate(const uint8_t *,size_t,uint32_t,uint32_t,NSSite *,uint32_t *);
#else
int nss_locate(const uint8_t *,size_t,uint32_t,uint32_t,NSSite *,uint32_t *);
#endif
#ifdef __cplusplus
#include <set>
namespace nss {
struct Scene {uint32_t names=0,stride=0,count=0,x=0,y=0,visible=0,printReturn=0,wait=0,clear=0;};
struct Render {uint32_t entry=0,source=0,end=0,buffer=0;NSDOperand operand{};NSDOperand index{};std::vector<NSDCall> calls;};
struct Texture {uint32_t entry=0,start=0,step=0,end=0,source=0,cursor=0,nextCursor=0,buffer=0,width=0,scale=0,kind=0;std::vector<NSDCall> calls;};
// decode 从已确认入口读取完整函数或命令块，遇到非法指令不继续猜测。
inline bool decode(const nsd::Image &im,uint32_t entry,uint32_t limit,std::vector<nsd::Instruction> &out,bool function=false) {
 uint32_t at=entry;for(unsigned n=0;n<32768&&uint64_t(at)-entry<limit;n++) {
  auto p=im.at(at,15,true);if(!p)return false;nsd::Instruction i{at,{}};nsd::nsd_hde32_disasm(p,&i.h);
  if(!i.h.len||i.h.flags&nsd_F_ERROR)return false;out.push_back(i);at+=i.h.len;
  if(i.h.opcode==0xc2||i.h.opcode==0xc3)return true;
 }return !function&&!out.empty();
}
// branch 恢复短跳转与近跳转的实际目标，不固定命令地址。
inline uint32_t branch(const nsd::Instruction &i) {
 int64_t delta=0;
 if(i.h.opcode==0xe8||i.h.opcode==0xe9||(i.h.opcode==0x0f&&i.h.opcode2>=0x80&&i.h.opcode2<=0x8f))delta=int32_t(i.h.imm.imm32);
 else if(i.h.opcode==0xeb||(i.h.opcode>=0x70&&i.h.opcode<=0x7f))delta=int8_t(i.h.imm.imm8);else return 0;
 auto v=int64_t(i.rva)+i.h.len+delta;return v>0&&v<=UINT32_MAX?uint32_t(v):0;
}
// command 由标准命令字符串、逐字节比较及相等分支恢复唯一成功块。
inline bool command(const nsd::Image &im,const char *name,uint32_t &start,uint32_t &limit) {
 std::vector<uint32_t> literals;auto length=strlen(name)+1;
 for(const auto &s:im.sections)if(!(s.flags&0x20000000))for(uint32_t n=0;n+length<=s.size;n++) {
  auto p=im.at(s.rva+n,length);if(p&&!memcmp(p,name,length)&&(n==0||im.data[s.rva+n-1]==0))literals.push_back(im.base+s.rva+n);
 }
 unsigned matches=0;
 for(const auto &s:im.sections)if(s.flags&0x20000000)for(uint32_t n=0;n+80<=s.size;n++) {
  auto p=im.at(s.rva+n,5,true);if(!p||p[0]<0xb8||p[0]>0xbf)continue;uint32_t value=0;memcpy(&value,p+1,4);
  if(std::find(literals.begin(),literals.end(),value)==literals.end())continue;
  std::vector<nsd::Instruction> code;if(!decode(im,s.rva+n,80,code))continue;
  unsigned reads=0,tests=0,compares=0;bool equal=false,unequal=false;
  for(size_t k=1;k+1<code.size();k++) {
   const auto &h=code[k].h;if(h.opcode==0x8a&&h.modrm_mod!=3)reads++;
   if(h.opcode==0x84&&h.modrm_mod==3)tests++;
   if(h.opcode==0x3a&&h.modrm_mod!=3)compares++;
   if(h.opcode==0x33&&h.modrm_mod==3&&h.modrm_reg==h.modrm_rm)equal=true;
   if(h.opcode==0x1b&&h.modrm_mod==3)unequal=true;
   if(reads<2||tests<2||compares<2||!equal||!unequal||h.opcode!=0x3b||h.modrm_mod!=3)continue;
   const auto &j=code[k+1];auto target=branch(j);
   if(j.h.opcode==0x75||(j.h.opcode==0x0f&&j.h.opcode2==0x85)){start=j.rva+j.h.len;limit=target;}
   else if(j.h.opcode==0x74||(j.h.opcode==0x0f&&j.h.opcode2==0x84)){start=target;limit=start+256;}
   else break;
   if(!im.at(start,15,true)||limit<=start||limit-start>8192)break;matches++;break;
  }
 }return matches==1;
}
// data 核对精灵数组属于主模块可写非代码节。
inline bool data(const nsd::Image &im,uint32_t va,uint32_t size) {
 if(va<im.base)return false;for(const auto &s:im.sections)if((s.flags&0x80000000)&&!(s.flags&0x20000000)&&va-im.base>=s.rva&&uint64_t(va-im.base-s.rva)+size<=s.size)return true;return false;
}
// scene 由lsp/csp数组读写关系及print/btnwait提交点恢复标准精灵场景。
inline bool scene(const nsd::Image &im,Scene &out,std::vector<uint32_t> &roots) {
 uint32_t begin=0,end=0;if(!command(im,"lsp",begin,end))return false;
 std::vector<nsd::Instruction> code;if(!decode(im,begin,end-begin,code))return false;Scene found{};
 for(size_t k=0;k<code.size();k++) {
  const auto &h=code[k].h;auto m=nsd::memory(h);
  if(h.opcode==0xe8)roots.push_back(branch(code[k]));
  if(h.opcode==0xc1&&h.modrm_mod==3&&h.modrm_reg==4&&h.imm.imm8>=5&&h.imm.imm8<=10) {
   auto reg=h.modrm_rm;
   for(size_t j=k+1;j<code.size()&&j<=k+3;j++){const auto &v=code[j].h;auto a=nsd::memory(v);if(v.opcode==0x8d&&a.base==reg&&a.index==-1&&data(im,uint32_t(a.displacement),1)){found.names=uint32_t(a.displacement);found.stride=1u<<h.imm.imm8;break;}}
  }
  if(h.opcode==0x89&&m.base==-1&&m.index>=0&&m.scale==4&&data(im,uint32_t(m.displacement),4)) {
   auto va=uint32_t(m.displacement);if(!found.x)found.x=va;else if(!found.y&&va!=found.x)found.y=va;else if(va!=found.x&&va!=found.y)found.visible=va;
  }
 }
 if(!found.names||!found.x||!found.y||!found.visible||!command(im,"csp",begin,end))return false;
 found.clear=begin;code.clear();if(!decode(im,begin,end-begin,code))return false;
 bool clears=false,coordinates=false;uint32_t namesEnd=0;
 for(const auto &i:code){auto m=nsd::memory(i.h);if(i.h.opcode==0xc6&&i.h.imm.imm8==0)clears=true;if(i.h.opcode==0x89&&uint32_t(m.displacement)==found.x)coordinates=true;if(i.h.opcode==0x81&&i.h.modrm_mod==3&&i.h.modrm_reg==7&&i.h.imm.imm32>found.names&&i.h.imm.imm32-found.names<=4096*found.stride)namesEnd=i.h.imm.imm32;}
 if(!clears||!coordinates||!namesEnd||(namesEnd-found.names)%found.stride)return false;found.count=(namesEnd-found.names)/found.stride;
 if(!found.count||found.count>4096||!data(im,found.names,found.stride*found.count)||!data(im,found.x,4*found.count)||!data(im,found.y,4*found.count)||!data(im,found.visible,4*found.count))return false;
 if(!command(im,"print",begin,end))return false;code.clear();if(!decode(im,begin,32,code))return false;
 if(code.size()<2||code[0].h.opcode!=0xe8||code[1].h.opcode!=0xe9)return false;found.printReturn=code[1].rva;
 if(!command(im,"btnwait",begin,end))return false;found.wait=begin;
 if(command(im,"strsp",begin,end)){code.clear();if(decode(im,begin,end-begin,code))for(const auto &i:code)if(i.h.opcode==0xe8)roots.push_back(branch(i));}
 out=found;return true;
}
// render 按首字节循环、双字节临时缓冲、共享GDI与函数退出恢复独立绘字布局。
inline bool render(const nsd::Image &im,uint32_t entry,const NSDLayout &body,Render &out) {
 std::vector<nsd::Instruction> code;if(!decode(im,entry,0x8000,code,true)||code.size()<30)return false;
 Render result{};result.entry=entry;unsigned sources=0;
 for(size_t k=0;k+1<code.size();k++) {
  const auto &h=code[k].h;auto reg=nsd::byteRead(h);auto m=nsd::memory(h);
  if(reg<0||m.base<0||m.index<0||m.scale!=1)continue;
  bool dispatch=false,backedge=false;for(size_t j=k+1;j<code.size()&&j<k+10;j++){auto &v=code[j].h;if(v.opcode==0x83&&v.modrm_mod==3&&v.modrm_reg==7&&v.imm.imm8==0x5c)dispatch=true;}
  if(!dispatch)continue;
  for(const auto &i:code)if(i.rva>code[k].rva&&branch(i)>=code[k].rva-128&&branch(i)<=code[k].rva)backedge=true;
  if(!backedge)continue;
  NSDOperand index{-1,-1,1,0};
  for(size_t j=k;j>0&&k-j<4;){const auto &v=code[--j].h;auto a=nsd::memory(v);if(v.opcode==0x8b&&v.modrm_reg==m.index&&a.base==4&&a.index==-1)index=a;}
  if(index.base!=4)for(size_t j=k+1;j<code.size();j++)if(branch(code[j])>=code[k].rva-128&&branch(code[j])<=code[k].rva) {
   for(size_t z=j;z>0&&j-z<8;){const auto &v=code[--z].h;auto a=nsd::memory(v);if(v.opcode==0x8b&&v.modrm_reg==m.index&&a.base==4&&a.index==-1){if(index.base==4&&index.displacement!=a.displacement)return false;index=a;break;}}
  }
  if(index.base!=4)continue;
  result.source=code[k].rva;result.operand=m;result.index=index;sources++;
 }
 if(sources!=1)return false;
 uint32_t buffer=0;
 for(size_t k=0;k<code.size();k++) {
  const auto &h=code[k].h;if(h.opcode!=0xff||h.modrm_mod!=3||h.modrm_reg!=2)continue;int target=h.modrm_rm;bool loaded=false,two=false,vertical=false;int temporary=-1;
  for(size_t j=k;j-->0&&code[k].rva-code[j].rva<=192;) {
   const auto &v=code[j].h;auto m=nsd::memory(v);
   if(v.opcode==0x8b&&v.modrm_reg==target&&v.modrm_mod==0&&v.modrm_rm==5&&uint32_t(m.displacement)==im.base+body.textOut)loaded=true;
   if(v.opcode==0x6a&&v.imm.imm8==2)two=true;
   if(v.opcode==0x8d&&m.base==4&&m.index==-1&&m.displacement>=4&&code[k].rva-code[j].rva<40)temporary=m.displacement-4;
   if((v.opcode==0x83||v.opcode==0x39)&&v.modrm_mod==0&&v.modrm_rm==5&&uint32_t(m.displacement)==im.base+body.vertical)vertical=true;
  }
  if(!loaded||!two||!vertical||temporary<0)continue;
  bool first=false,second=false;for(size_t j=0;j<k;j++) {auto &v=code[j].h;auto m=nsd::memory(v);if(v.opcode==0x88&&m.base==4&&m.index==-1){first=first||m.displacement==temporary;second=second||m.displacement==temporary+1;}}
  if(!first||!second)return false;if(buffer&&buffer!=uint32_t(temporary))return false;buffer=uint32_t(temporary);
  // 挂在push 2前，所有横竖排路径均尚未压栈，不覆盖call后的短跳转落点。
  uint32_t hook=0;for(size_t j=k;j-->0&&code[k].rva-code[j].rva<=40;)if(code[j].h.opcode==0x6a&&code[j].h.imm.imm8==2){hook=code[j].rva;break;}
  if(!hook)return false;result.calls.push_back({hook,target,0});
 }
 if(result.calls.size()<2||result.calls.size()>8)return false;result.buffer=buffer;
 // 函数统一退出的cookie/SEH恢复起点必须在任何pop和ESP修改之前。
 auto ret=code.back().rva;for(size_t k=0;k<code.size();k++) {
  const auto &h=code[k].h;auto m=nsd::memory(h);if(ret-code[k].rva>64||h.opcode!=0x8b||h.modrm_reg!=1||m.base!=4||m.index!=-1||h.len<5)continue;
  bool cookie=false,restores=false;for(size_t j=k+1;j<code.size();j++){auto &v=code[j].h;if(v.opcode==0x33&&v.modrm_mod==3&&v.modrm_reg==1&&v.modrm_rm==4)cookie=true;if(v.opcode>=0x58&&v.opcode<=0x5f)restores=true;}
  if(cookie&&restores){result.end=code[k].rva;break;}
 }
 if(!result.end)return false;out=std::move(result);return true;
}
// calls 遍历函数直接控制流的全部返回分支，不能因首个ret漏掉后续资源类型。
inline std::vector<uint32_t> calls(const nsd::Image &im,uint32_t entry) {
 std::vector<uint32_t> work{entry},out;std::set<uint32_t> seen;
 while(!work.empty()&&seen.size()<32768) {
  auto at=work.back();work.pop_back();
  for(unsigned n=0;n<32768&&seen.size()<32768;n++) {
   if(!seen.insert(at).second)break;auto p=im.at(at,15,true);if(!p)break;nsd::Instruction i{at,{}};nsd::nsd_hde32_disasm(p,&i.h);
   if(!i.h.len||i.h.flags&nsd_F_ERROR)break;
   if(i.h.opcode==0xe8)out.push_back(branch(i));
   else if(i.h.opcode==0xe9||i.h.opcode==0xeb){auto dest=branch(i);if(dest)work.push_back(dest);break;}
   else if((i.h.opcode>=0x70&&i.h.opcode<=0x7f)||(i.h.opcode==0x0f&&i.h.opcode2>=0x80&&i.h.opcode2<=0x8f)){auto dest=branch(i);if(dest)work.push_back(dest);}
   if(i.h.opcode==0xff&&i.h.modrm_reg==4) {
    auto m=nsd::memory(i.h);auto table=uint32_t(m.displacement);
    if(m.base==-1&&m.index>=0&&m.scale==4&&table>=im.base)for(unsigned j=0;j<128;j++) {
     auto address=uint64_t(table-im.base)+j*4;if(address>UINT32_MAX||!im.at(uint32_t(address),4))break;auto target=im.scalar(uint32_t(address));
     if(target<im.base||!im.at(target-im.base,15,true)||target-im.base<entry||target-im.base-entry>0x8000)break;work.push_back(target-im.base);
    }break;
   }
   if(i.h.opcode==0xc2||i.h.opcode==0xc3||i.h.opcode==0xcc)break;at+=i.h.len;
  }
 }return out;
}
// texture 恢复标准文字资源的临时原文、双字节游标、三倍字宽和实际绘字调用
inline bool texture(const nsd::Image &im,uint32_t entry,const NSDLayout &body,Texture &out) {
 std::vector<nsd::Instruction> code;if(!decode(im,entry,0x8000,code,true)||code.size()<100||code.back().h.opcode!=0xc2||code.back().h.imm.imm16!=44)return false;
 Texture found{};found.entry=entry;unsigned sources=0;
 for(size_t k=0;k+6<code.size();k++) {
  auto &h=code[k].h;auto m=nsd::memory(h);
  if(h.opcode!=0x8b||m.base!=4||m.index!=-1||m.displacement<0)continue;
  auto &a=code[k+1].h;auto ma=nsd::memory(a);auto &b=code[k+2].h;auto mb=nsd::memory(b);auto &c=code[k+3].h;auto mc=nsd::memory(c);
  auto &d=code[k+4].h;auto md=nsd::memory(d);auto &e=code[k+5].h;auto me=nsd::memory(e);
  if(nsd::byteRead(a)<0||ma.base!=h.modrm_reg||ma.index!=-1||ma.displacement||b.opcode!=0x8b||mb.base!=4||mb.index!=-1||mb.displacement==m.displacement||mb.displacement<0||nsd::byteRead(c)<0||mc.base!=b.modrm_reg||mc.index!=-1||mc.displacement)continue;
  if(d.opcode!=0x88||e.opcode!=0x88||md.base!=4||me.base!=4||md.index!=-1||me.index!=-1||d.modrm_reg!=nsd::byteRead(a)||e.modrm_reg!=nsd::byteRead(c)||me.displacement!=md.displacement+1)continue;
  found.step=code[k].rva;found.cursor=m.displacement;found.nextCursor=mb.displacement;found.buffer=md.displacement;sources++;
 }
 if(sources!=1)return false;
 // 原文起点必须由栈内副本初始化两个相邻游标，避免接入正文或未知像素函数
 for(size_t k=0;k+2<code.size()&&code[k].rva<found.step;k++) {
  auto &h=code[k].h;auto m=nsd::memory(h);
  if(nsd::byteRead(h)<0||m.base!=4||m.index!=-1||m.displacement<=int(found.buffer))continue;
  unsigned cursors=0;for(size_t j=k+1;j<code.size()&&j<k+16;j++) {
   auto &v=code[j].h;auto a=nsd::memory(v);if(v.opcode==0x8d&&a.base==4&&a.index==-1&&(a.displacement==m.displacement||a.displacement==m.displacement+1)) {
    for(size_t z=j+1;z<code.size()&&z<j+5;z++){auto &w=code[z].h;auto b=nsd::memory(w);if(w.opcode==0x89&&w.modrm_reg==v.modrm_reg&&b.base==4&&b.index==-1&&b.displacement==int(a.displacement==m.displacement?found.cursor:found.nextCursor))cursors|=1u<<(a.displacement-m.displacement);}
   }
  }
  if(cursors==3){if(found.start)return false;found.start=code[k].rva;found.source=m.displacement;}
 }
 if(!found.start)return false;
 // 从字号参数到三倍栅格的LEA恢复宽度，不能借用正文的全局字宽
 for(size_t k=0;k+4<code.size()&&code[k].rva<found.start;k++) {
  auto &h=code[k].h;auto m=nsd::memory(h);if(h.opcode!=0x8b||m.base!=4||m.index!=-1||m.displacement<=int(found.source))continue;
  for(size_t j=k+1;j<code.size()&&j<k+5;j++){auto &v=code[j].h;auto a=nsd::memory(v);if(v.opcode==0x8d&&v.modrm_reg==h.modrm_reg&&a.base==h.modrm_reg&&a.index==h.modrm_reg&&a.scale==2&&!a.displacement){if(found.width)return false;found.width=m.displacement;found.scale=3;}}
 }
 if(!found.width)return false;
 // 独立直接DC和外部字体回调不使用离屏三倍采样，恢复实际样式参数供局部回退
 for(const auto &i:code){auto m=nsd::memory(i.h);if(i.rva<found.start&&i.h.opcode==0x83&&i.h.modrm_reg==7&&i.h.imm.imm8==3&&m.base==4&&m.index==-1&&m.displacement>int(found.width)){if(found.kind&&found.kind!=uint32_t(m.displacement))return false;found.kind=m.displacement;}}
 if(!found.kind)return false;
 int target=-1;for(const auto &i:code){auto m=nsd::memory(i.h);if(i.h.opcode==0x8b&&m.base==-1&&m.index==-1&&uint32_t(m.displacement)==im.base+body.textOut){if(target!=-1&&target!=i.h.modrm_reg)return false;target=i.h.modrm_reg;}}
 if(target<0)return false;
 for(size_t k=0;k<code.size();k++) {
  auto &h=code[k].h;if(h.opcode!=0xff||h.modrm_mod!=3||h.modrm_reg!=2||h.modrm_rm!=target)continue;
  uint32_t hook=0;bool input=false;for(size_t j=k;j-->0&&code[k].rva-code[j].rva<=24;){auto &v=code[j].h;auto m=nsd::memory(v);if(v.opcode==0x8d&&m.base==4&&m.index==-1&&m.displacement==int(found.buffer)+4)input=true;if(v.opcode==0x6a&&v.imm.imm8==2)hook=code[j].rva;}
  // 不接入独立直接绘制样式，标准资源路径使用三倍离屏纹理
  if(hook&&input)found.calls.push_back({hook,target,0});
 }
 if(found.calls.size()<2||found.calls.size()>8)return false;
 for(size_t k=0;k+1<code.size();k++){auto &h=code[k].h;auto m=nsd::memory(h);auto &next=code[k+1].h;if(code.back().rva-code[k].rva<96&&h.opcode==0x8b&&m.base==4&&m.index==-1&&next.p_seg==0x64&&next.opcode==0x89){found.end=code[k].rva;break;}}
 if(!found.end)return false;out=std::move(found);return true;
}
// locate 从标准精灵命令调用图发现绘字消费者，不登记游戏或版本白名单。
inline bool locate(const uint8_t *bytes,size_t size,uint32_t base,const NSDLayout &body,Scene &out,std::vector<Render> &renders,std::vector<Texture> *textures=nullptr) {
 nsd::Image im{bytes,size,base,{}};if(!im.init())return false;std::vector<uint32_t> roots;if(!scene(im,out,roots))return false;
 std::set<uint32_t> visited;std::vector<uint32_t> layer=roots;
 for(unsigned depth=0;depth<7&&!layer.empty();depth++) {
  std::vector<uint32_t> next;
  for(auto entry:layer) {
   if(visited.size()>2048||!entry||!visited.insert(entry).second||!im.at(entry,15,true))continue;
   if(textures){Texture t{};if(texture(im,entry,body,t))textures->push_back(std::move(t));}
   Render r{};if(render(im,entry,body,r)){renders.push_back(std::move(r));continue;}
   auto children=calls(im,entry);next.insert(next.end(),children.begin(),children.end());
  }layer=std::move(next);
 }
 // 图像对象可能经间接分派绘字；补查主模块直接调用目标，仍须完整显示语义吻合。
 // 不用函数地址、序号或版本签名辨认消费者，歧义消费者整体保持原文。
 std::set<uint32_t> candidates;
 for(const auto &s:im.sections)if(s.flags&0x20000000)for(uint32_t n=0;n+5<=s.size;n++) {
  auto p=im.at(s.rva+n,5,true);if(!p||p[0]!=0xe8)continue;int32_t delta=0;memcpy(&delta,p+1,4);auto target=int64_t(s.rva)+n+5+delta;
  if(target>0&&target<=UINT32_MAX&&im.at(uint32_t(target),15,true))candidates.insert(uint32_t(target));
 }
 if(candidates.size()<=8192)for(auto entry:candidates) {
  if(visited.count(entry))continue;if(textures){Texture t{};if(texture(im,entry,body,t))textures->push_back(std::move(t));}Render r{};if(render(im,entry,body,r))renders.push_back(std::move(r));
 }
 if(renders.size()>2)renders.clear();
 if(textures&&textures->size()>1)textures->clear();
 std::sort(renders.begin(),renders.end(),[](const Render &a,const Render &b){return a.entry<b.entry;});
 return true;
}
}
#endif
