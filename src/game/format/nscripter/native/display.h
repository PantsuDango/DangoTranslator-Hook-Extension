#pragma once
#include <stdint.h>
#include <stddef.h>
// NSDOperand 保存从机器指令恢复的有效地址，不固定寄存器或栈位置
typedef struct { int base,index,scale; int32_t displacement; } NSDOperand;
typedef struct { uint32_t rva; int reg; uint32_t stackArguments; } NSDCall;
typedef struct {
 uint32_t source,vertical,textOut,buffer,cellWidth,cellScale; NSDOperand operand;
 uint32_t callCount; NSDCall calls[8];
} NSDLayout;
#ifdef __cplusplus
#include <vector>
#include <cstring>
#include <algorithm>
namespace nsd {
#include "decoder.inc"
struct Section { uint32_t rva,size,flags; };
struct Image {
 const uint8_t *data; size_t size; uint32_t base; std::vector<Section> sections;
 // at 仅允许读取完整的PE节范围，防止跨节及整数溢出
 const uint8_t *at(uint32_t rva,size_t n,bool executable=false) const {
  if(rva>size||n>size-rva)return nullptr;
  for(const auto &s:sections)if(rva>=s.rva&&uint64_t(rva-s.rva)+n<=s.size&&(!executable||s.flags&0x20000000))return data+rva;
  return nullptr;
 }
 // scalar 通过复制读取未对齐的小端字段
 uint32_t scalar(uint32_t rva) const {uint32_t v=0;auto p=at(rva,4);if(p)memcpy(&v,p,4);return v;}
 // init 核对映像和节表，解析器同用于客户端和实际进程
 bool init() {
  if(size<0x100||size>32*1024*1024||data[0]!='M'||data[1]!='Z')return false;
  uint32_t nt=0;memcpy(&nt,data+0x3c,4);
  if(nt>size||size-nt<248||memcmp(data+nt,"PE\0\0",4))return false;
  uint16_t machine=0,count=0,optional=0,magic=0;
  memcpy(&machine,data+nt+4,2);memcpy(&count,data+nt+6,2);memcpy(&optional,data+nt+20,2);memcpy(&magic,data+nt+24,2);
  if(machine!=0x14c||magic!=0x10b||optional<224||count==0||count>96)return false;
  uint64_t table=uint64_t(nt)+24+optional;if(table+uint64_t(count)*40>size)return false;
  for(unsigned i=0;i<count;i++){Section s{};memcpy(&s.size,data+table+i*40+8,4);memcpy(&s.rva,data+table+i*40+12,4);memcpy(&s.flags,data+table+i*40+36,4);if(s.rva>size||s.size>size-s.rva)return false;sections.push_back(s);}
  return true;
 }
 // textOutSlot 按PE导入语义核对唯一TextOutA入口，不接受任意GDI调用
 uint32_t textOutSlot() const {
  uint32_t nt=0;memcpy(&nt,data+0x3c,4);uint32_t imports=0;memcpy(&imports,data+nt+24+104,4);
  uint32_t found=0;
  for(unsigned d=0;d<256;d++) {
   uint64_t pos=uint64_t(imports)+d*20;if(pos>UINT32_MAX||!at(uint32_t(pos),20))return 0;
   auto desc=uint32_t(pos);auto lookup=scalar(desc),iat=scalar(desc+16);if(!lookup&&!iat)break;if(!lookup||!iat)return 0;
   for(unsigned i=0;i<4096;i++) {
    uint64_t a=uint64_t(lookup)+i*4,b=uint64_t(iat)+i*4;if(a>UINT32_MAX||b>UINT32_MAX||!at(uint32_t(a),4)||!at(uint32_t(b),4))return 0;
    auto name=scalar(uint32_t(a));if(!name)break;if(name&0x80000000)continue;
    if(name>UINT32_MAX-11)continue;auto p=at(name+2,9);if(p&&!memcmp(p,"TextOutA\0",9)){if(found)return 0;found=uint32_t(b);}
   }
  }
  return found;
 }
};
struct Instruction {uint32_t rva;nsd_hde32s h;};
// memory 解码ModRM和SIB有效地址，保留符号位移
inline NSDOperand memory(const nsd_hde32s &h) {
 NSDOperand m{-1,-1,1,0};
 if(!(h.flags&nsd_F_MODRM)||h.modrm_mod==3||h.p_67)return m;
 if(h.modrm_rm==4){m.base=h.sib_base;m.index=h.sib_index==4?-1:h.sib_index;m.scale=1<<h.sib_scale;if(h.modrm_mod==0&&h.sib_base==5)m.base=-1;}
 else if(h.modrm_mod!=0||h.modrm_rm!=5)m.base=h.modrm_rm;
 if(h.flags&nsd_F_DISP8)m.displacement=int8_t(h.disp.disp8);else if(h.flags&nsd_F_DISP32)m.displacement=int32_t(h.disp.disp32);
 return m;
}
// byteRead 只识别无段覆盖的普通字节读取，不借用未知指令的参数
inline int byteRead(const nsd_hde32s &h) {
 if(h.p_seg||h.p_67||h.p_66||h.modrm_mod==3)return -1;
 if(h.opcode==0x8a&&h.modrm_reg<4)return h.modrm_reg;
 if(h.opcode==0x0f&&h.opcode2==0xb6&&h.modrm_reg<4)return h.modrm_reg;
 return -1;
}
// locate 由原文双字节复制和已导入绘字调用恢复正文布局
inline bool locate(const uint8_t *data,size_t size,uint32_t base,uint32_t render,NSDLayout &result) {
 Image image{data,size,base,{}};if(!image.init())return false;auto slot=image.textOutSlot();if(!slot)return false;
 std::vector<Instruction> code;uint32_t cursor=render;
 for(unsigned n=0;n<24576&&uint64_t(cursor)-render<0x6000;n++) {
  auto p=image.at(cursor,15,true);if(!p)return false;Instruction i{cursor,{}};nsd_hde32_disasm(p,&i.h);
  if(!i.h.len||i.h.flags&nsd_F_ERROR)return false;code.push_back(i);cursor+=i.h.len;
  if(i.h.opcode==0xc2||i.h.opcode==0xc3)break;
 }
 if(code.empty()||(code.back().h.opcode!=0xc2&&code.back().h.opcode!=0xc3))return false;
 unsigned matches=0;
 for(size_t start=0;start<code.size();start++) {
  auto reg=byteRead(code[start].h);if(reg<0)continue;auto source=memory(code[start].h);
  if(source.base<0||source.index<0||source.base==4||source.index==4||source.scale!=1)continue;
  int provenance[4]={-1,-1,-1,-1};int stores[2]={-1,-1};unsigned reads=0;
  for(size_t k=start;k<(std::min)(start+10,code.size());k++) {
   const auto &h=code[k].h;if(h.opcode==0x90)continue;
   auto r=byteRead(h);auto m=memory(h);
   if(r>=0&&m.base==source.base&&m.index==source.index&&m.scale==1&&m.displacement-source.displacement>=0&&m.displacement-source.displacement<=1){provenance[r]=m.displacement-source.displacement;reads|=1u<<provenance[r];continue;}
   if(h.opcode==0x88&&h.modrm_mod!=3&&h.modrm_reg<4&&m.base==4&&m.index==-1&&provenance[h.modrm_reg]>=0){stores[provenance[h.modrm_reg]]=m.displacement;if(reads==3&&stores[0]>=0&&stores[1]==stores[0]+1)break;continue;}
   break;
  }
  if(reads!=3||stores[0]<0||stores[1]!=stores[0]+1)continue;
  NSDLayout found{};found.source=code[start].rva;found.operand=source;found.buffer=uint32_t(stores[0]);found.textOut=slot;uint32_t drawEnd=0;
  for(size_t k=start+1;k<code.size()&&code[k].rva-code[start].rva<1024;k++) {
   const auto &h=code[k].h;if(h.opcode!=0xff||h.modrm_mod!=3||h.modrm_reg!=2)continue;int target=h.modrm_rm;
   bool loaded=false,buffer=false,two=false;uint32_t vertical=0;
   for(size_t j=k;j-->start;) {
    const auto &v=code[j].h;auto m=memory(v);auto delta=code[k].rva-code[j].rva;
    if(delta>512)break;
    if(v.opcode==0x8b&&v.modrm_reg==target&&v.modrm_mod==0&&v.modrm_rm==5&&uint32_t(m.displacement)==base+slot)loaded=true;
    if(delta>128)continue;
    if(v.opcode==0x8d&&m.base==4&&m.index==-1&&m.displacement==stores[0]+4)buffer=true;
    if(v.opcode==0x6a&&v.imm.imm8==2)two=true;
    if(!vertical&&v.opcode==0x83&&v.modrm_reg==7&&v.modrm_mod==0&&v.modrm_rm==5&&v.imm.imm8==0) {
     auto va=uint32_t(m.displacement);if(va>=base){auto rva=va-base;for(const auto &s:image.sections)if((s.flags&0x80000000)&&!(s.flags&0x20000000)&&rva>=s.rva&&uint64_t(rva-s.rva)+4<=s.size)vertical=rva;}
    }
   }
   if(!loaded||!buffer||!two||!vertical||k==0)continue;
   // 挂在最后一个参数压栈前，避免两字节call后紧接短跳转无法搬迁
   const auto &argument=code[k-1];
   if(argument.h.opcode<0x50||argument.h.opcode>0x57||argument.h.opcode-0x50==target||argument.rva+argument.h.len!=code[k].rva)continue;
   // 至少五字节完整参数构造位于call之前，避免覆盖无阴影分支的call后落点
   uint32_t hook=0,arguments=4;
   // 旧排版的横排阴影跳到共同HDC参数；在横排完整压栈分支挂钩
   for(size_t j=k-1;j>start+1&&code[k].rva-code[j].rva<=64;j--) {
    const auto &jump=code[j];const auto &x=code[j-1];const auto &y=code[j-2];
    if(jump.h.opcode==0xeb&&int64_t(jump.rva)+jump.h.len+int8_t(jump.h.imm.imm8)==argument.rva&&
       y.h.opcode==0x6a&&y.h.imm.imm8==0&&((x.h.opcode>=0x50&&x.h.opcode<=0x57)||(x.h.opcode==0x6a&&x.h.imm.imm8==1))&&
       y.rva+y.h.len==x.rva&&x.rva+x.h.len==jump.rva&&jump.rva+jump.h.len-y.rva>=5){hook=y.rva;arguments=2;break;}
   }
   if(!hook&&k>=2) {
    const auto &load=code[k-2];
    if(load.rva+load.h.len==argument.rva&&argument.h.opcode==0x50&&load.h.opcode==0xa1&&load.h.len>=5)hook=load.rva;
    else if(load.rva+load.h.len==argument.rva&&load.h.opcode==0x8b&&load.h.modrm_mod==0&&load.h.modrm_rm==5&&load.h.modrm_reg==argument.h.opcode-0x50&&load.h.len>=5)hook=load.rva;
    else if(k>=3&&load.h.opcode>=0x50&&load.h.opcode<=0x57) {
     const auto &coordinate=code[k-3];
     if(coordinate.rva+coordinate.h.len==load.rva&&load.rva+load.h.len==argument.rva&&coordinate.h.opcode==0x8d&&coordinate.h.modrm_reg==load.h.opcode-0x50&&code[k].rva-coordinate.rva>=5){hook=coordinate.rva;arguments=3;}
    }
    else if(k>=3&&load.h.opcode==0x6a&&load.h.imm.imm8==0) {
     const auto &coordinate=code[k-3];
     if(coordinate.rva+coordinate.h.len==load.rva&&load.rva+load.h.len==argument.rva&&coordinate.h.opcode==0x6a&&coordinate.h.imm.imm8==0&&code[k].rva-coordinate.rva>=5){hook=coordinate.rva;arguments=2;}
    }
   }
   if(!hook)continue;
   if(found.vertical&&found.vertical!=vertical)return false;found.vertical=vertical;
   if(found.callCount>=8)return false;found.calls[found.callCount++]={hook,target,arguments};drawEnd=code[k].rva+h.len;
  }
  if(found.callCount<2)continue;
  // 竖排起点恢复一格绘字栅格的宽度倍率，不把屏幕逻辑字宽当成DC像素
  // 缺失或歧义只关闭长译文，已确认的短句绘字及读取能力保持
  uint32_t width=0,scale=0;bool ambiguous=false;
  for(size_t k=start+1;k<code.size()&&code[k].rva<drawEnd;k++) {
   const auto &h=code[k].h;auto m=memory(h);
   if(h.opcode!=0x8d||m.base<0||m.base!=m.index||m.scale!=2||(m.displacement!=0&&m.displacement!=1))continue;
   for(size_t j=k;j-->start&&code[k].rva-code[j].rva<=32;) {
    const auto &v=code[j].h;auto load=memory(v);uint32_t address=0;
    if(v.opcode==0xa1&&m.base==0)address=v.imm.imm32;
    else if(v.opcode==0x8b&&v.modrm_reg==m.base&&v.modrm_mod==0&&v.modrm_rm==5)address=uint32_t(load.displacement);
    if(!address)continue;
    if(address>=base)for(const auto &s:image.sections)if((s.flags&0x80000000)&&!(s.flags&0x20000000)&&address-base>=s.rva&&uint64_t(address-base-s.rva)+4<=s.size) {
     auto rasterScale=uint32_t(1+m.scale);
     if(width&&(width!=address-base||scale!=rasterScale))ambiguous=true;else {width=address-base;scale=rasterScale;}
    }
    break;
   }
  }
  found.cellWidth=ambiguous?0:width;found.cellScale=ambiguous?0:scale;
  result=found;matches++;
 }
 return matches==1;
}
}
extern "C" int nsd_locate(const uint8_t *,size_t,uint32_t,uint32_t,NSDLayout *);
#else
int nsd_locate(const uint8_t *,size_t,uint32_t,uint32_t,NSDLayout *);
#endif
