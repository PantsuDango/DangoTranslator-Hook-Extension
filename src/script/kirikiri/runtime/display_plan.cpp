#include "display_plan.h"
#include <algorithm>
#include <bcrypt.h>
#include <functional>

namespace dango_krkr {
static constexpr size_t snapshotLimit=32*1024*1024;
static constexpr uint32_t positionLimit=8000000;
static constexpr uint64_t memoryLimit=192*1024*1024;

// snapshotDigest 使用系统SHA256实现校验完整正文，不信任客户端声明的摘要
static bool snapshotDigest(const uint8_t* data,size_t size,const uint8_t* expected){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    uint8_t digest[32]={};
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok)ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    if(ok)ok=BCryptHashData(hash,const_cast<PUCHAR>(data),ULONG(size),0)>=0;
    if(ok)ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash)BCryptDestroyHash(hash);
    if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    return ok && memcmp(digest,expected,sizeof(digest))==0;
}

// SnapshotReader 在长度边界内读取数据，文本只接受合法BMP字符
class SnapshotReader {
    const std::vector<uint8_t>& data;size_t offset;
public:
    SnapshotReader(const std::vector<uint8_t>& value,size_t start):data(value),offset(start){}
    bool number(uint32_t& value){
        if(data.size()-offset<4)return false;
        memcpy(&value,data.data()+offset,4);offset+=4;return true;
    }
    bool width(uint16_t& value){
        if(data.size()-offset<2)return false;
        memcpy(&value,data.data()+offset,2);offset+=2;return true;
    }
    bool text(std::wstring& value){
        uint32_t size=0;if(!number(size) || size>32768 || size>data.size()-offset)return false;
        if(!size){value.clear();return true;}
        auto bytes=reinterpret_cast<const char*>(data.data()+offset);
        int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,int(size),nullptr,0);
        if(count<=0 || count>32768)return false;
        value.resize(count);
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,int(size),value.data(),count)!=count)return false;
        for(wchar_t ch:value)if(!ch || (ch>=0xd800 && ch<=0xdfff))return false;
        offset+=size;return true;
    }
    bool done() const{return offset==data.size();}
};

static bool safeStorage(const std::wstring& value){
    if(value.empty() || value.size()>1024 || value.front()==L'/' || value.back()==L'/')return false;
    size_t start=0;
    for(size_t i=0;i<=value.size();i++){
        if(i<value.size() && value[i]!=L'/'){
            wchar_t ch=value[i];
            if(ch<32 || (ch>=127 && ch<=159) || ch==L'\\' || ch==L':' || ch==L'>')return false;
            continue;
        }
        auto part=value.substr(start,i-start);
        if(part.empty() || part==L"." || part==L"..")return false;
        start=i+1;
    }
    return true;
}
static bool visibleText(const std::wstring& value,bool whitespace){
    for(wchar_t ch:value){
        if(whitespace && (ch==L'\r' || ch==L'\n' || ch==L'\t'))continue;
        if(ch<32 || (ch>=127 && ch<=159))return false;
    }
    return true;
}

// inputTag 只接受无条件、无额外属性的直接变量读取，与客户端标记语义一致
static bool inputTag(const std::wstring& tag){
    size_t pos=0;
    auto space=[&](){while(pos<tag.size() && (tag[pos]==L' ' || tag[pos]==L'\t'))pos++;};
    if(tag.compare(0,4,L"[emb")!=0 || tag.size()<6 || (tag[4]!=L' ' && tag[4]!=L'\t'))return false;
    pos=4;space();
    if(tag.compare(pos,3,L"exp")!=0)return false;
    pos+=3;space();if(pos>=tag.size() || tag[pos++]!=L'=')return false;
    space();if(pos>=tag.size())return false;
    wchar_t quote=(tag[pos]==L'\"' || tag[pos]==L'\'')?tag[pos++]:0;
    size_t start=pos;
    while(pos<tag.size() && (quote?tag[pos]!=quote:tag[pos]!=L' ' && tag[pos]!=L'\t' && tag[pos]!=L']'))pos++;
    auto expression=tag.substr(start,pos-start);
    if(expression.size()>128 || (expression.compare(0,3,L"sf.")!=0 && expression.compare(0,2,L"f.")!=0))return false;
    size_t index=expression[1]==L'f'?3:2;
    auto letter=[](wchar_t ch){return (ch>=L'A' && ch<=L'Z') || (ch>=L'a' && ch<=L'z') || ch==L'_';};
    bool first=true;
    for(;index<expression.size();index++){
        wchar_t ch=expression[index];
        if(ch==L'.'){if(first)return false;first=true;continue;}
        if(!letter(ch) && (first || ch<L'0' || ch>L'9'))return false;
        first=false;
    }
    if(first)return false;
    if(quote){if(pos>=tag.size() || tag[pos++]!=quote)return false;}
    space();return pos+1==tag.size() && tag[pos]==L']';
}

// expandedPositions 仅在整行字面锚点唯一对应时映射游标，动态值与原控制标签不分配译文
static std::vector<uint32_t> expandedPositions(const std::wstring& original,const std::wstring& actual){
    if(actual.size()>32768 || !visibleText(actual,true))return {};
    std::vector<std::pair<size_t,size_t>> holes;
    for(size_t pos=0;pos<original.size();pos++){
        if(original.compare(pos,4,L"[emb")!=0)continue;
        size_t end=original.find(L']',pos);
        if(end==std::wstring::npos || !inputTag(original.substr(pos,end-pos+1)) || holes.size()>=8)return {};
        holes.emplace_back(pos,end+1);pos=end;
    }
    if(holes.empty())return {};
    struct Anchor {size_t source,actual,length;};
    std::vector<Anchor> anchors;
    std::vector<uint32_t> result;
    size_t budget=131072,solutions=0;
    std::function<void(size_t,size_t,size_t)> visit=[&](size_t hole,size_t from,size_t target){
        if(solutions>1 || !budget)return;
        size_t until=hole<holes.size()?holes[hole].first:original.size();
        size_t length=until-from;
        if(length+1>budget){budget=0;return;}
        budget-=length+1;
        if(target>actual.size() || length>actual.size()-target || actual.compare(target,length,original,from,length)!=0)return;
        anchors.push_back({from,target,length});target+=length;
        if(hole==holes.size()){
            if(target==actual.size()){
                solutions++;
                if(solutions==1){
                    result.assign(actual.size()+1,0);
                    for(auto& anchor:anchors)for(size_t i=0;i<anchor.length;i++)result[anchor.actual+i+1]=uint32_t(anchor.source+i+1);
                }
            }
        }else{
            auto marker=holes[hole];size_t tagLength=marker.second-marker.first;
            if(actual.compare(target,tagLength,original,marker.first,tagLength)==0){
                visit(hole+1,marker.second,target+tagLength);
            }else{
                size_t limit=std::min<size_t>(256,actual.size()-target);
                for(size_t width=0;width<=limit && solutions<2 && budget;width++){
                    if(width){wchar_t ch=actual[target+width-1];if(ch<32 || (ch>=127 && ch<=159) || ch==L'[' || ch==L']')break;}
                    visit(hole+1,marker.second,target+width);
                }
            }
        }
        anchors.pop_back();
    };
    visit(0,0,0);
    if(solutions!=1 || !budget)return {};
    return result;
}

// parseDisplayPlan 全部验证通过后才交付不可变快照，失败不能覆盖已验证数据
std::unique_ptr<DisplayPlan> parseDisplayPlan(const std::vector<uint8_t>& data,const std::wstring& nonce,uint64_t previous,uint64_t retained){
    try {
        if(data.size()<96 || data.size()>snapshotLimit || nonce.size()!=32 || retained>memoryLimit)return nullptr;
        uint32_t magic=0,version=0;uint64_t generation=0;
        memcpy(&magic,data.data(),4);memcpy(&version,data.data()+4,4);memcpy(&generation,data.data()+8,8);
        if(magic!=0x3150444b || version!=2 || !generation || generation>uint64_t(1)<<53 || generation<=previous)return nullptr;
        for(size_t i=0;i<32;i++)if(data[16+i]!=nonce[i])return nullptr;
        if(!snapshotDigest(data.data()+80,data.size()-80,data.data()+48))return nullptr;
        SnapshotReader reader(data,80);
        uint32_t strings=0,runs=0,lines=0,ranges=0;
        if(!reader.number(strings)||!reader.number(runs)||!reader.number(lines)||!reader.number(ranges))return nullptr;
        if(!strings || strings>500000 || !runs || runs>200000 || !lines || lines>400000 || !ranges || ranges>500000)return nullptr;
        uint64_t metadata=uint64_t(strings)*96+uint64_t(runs)*16+uint64_t(lines)*128+uint64_t(ranges)*48+256*1024;
        uint64_t memory=metadata+uint64_t(data.size())*4+retained;
        if(memory>memoryLimit)return nullptr;
        if(uint64_t(strings)*4+uint64_t(runs)*8+uint64_t(lines)*12+uint64_t(ranges)*26>data.size()-96)return nullptr;
        auto plan=std::make_unique<DisplayPlan>();plan->generation=generation;
        plan->strings.resize(strings);
        uint64_t textBytes=0;
        for(auto& value:plan->strings){if(!reader.text(value))return nullptr;textBytes+=value.size()*2;}
        plan->runs.resize(runs);
        for(auto& run:plan->runs){
            if(!reader.number(run[0])||!reader.number(run[1])||run[0]>=strings||run[1]>=strings)return nullptr;
            if(plan->strings[run[0]].empty() || plan->strings[run[1]].empty() || !visibleText(plan->strings[run[0]],true) || !visibleText(plan->strings[run[1]],true))return nullptr;
        }
        std::vector<DisplayLine*> lineIDs;lineIDs.reserve(lines);
        for(uint32_t i=0;i<lines;i++){
            uint32_t storage=0,source=0,number=0;
            if(!reader.number(storage)||!reader.number(source)||!reader.number(number)||storage>=strings||source>=strings||number>1000000)return nullptr;
            auto& text=plan->strings[source];
            if(!safeStorage(plan->strings[storage]) || text.find_first_of(L"\r\n")!=std::wstring::npos || !visibleText(text,true))return nullptr;
            auto storageID=plan->storages.emplace(plan->strings[storage],storage).first->second;
            auto inserted=plan->lines.emplace(std::make_pair(storageID,number),DisplayLine{source,{}});
            if(!inserted.second)return nullptr;
            lineIDs.push_back(&inserted.first->second);
        }
        std::vector<uint32_t> references(runs,0),last(runs,0);
        uint32_t positions=0;
        for(uint32_t i=0;i<ranges;i++){
            uint32_t line=0,count=0;DisplayRange segment={};
            if(!reader.number(line)||!reader.number(segment.position)||!reader.number(count)||!reader.number(segment.display)||!reader.number(segment.run)||!reader.number(segment.flags))return nullptr;
            if(line>=lines || !segment.position || segment.position>32768 || !count || count>32768 || segment.display>=strings || segment.run>=runs || segment.flags>1)return nullptr;
            if(count>positionLimit-positions)return nullptr;
            positions+=count;
            if(memory+uint64_t(positions)*2>memoryLimit)return nullptr;
            auto target=lineIDs[line];auto& source=plan->strings[target->source];auto& display=plan->strings[segment.display];
            if(segment.position>source.size() || count>source.size()-(segment.position-1) || !visibleText(display,false))return nullptr;
            for(uint32_t offset=0;offset<count;offset++){
                wchar_t ch=source[segment.position-1+offset];
                if(ch<32 || (ch>=127 && ch<=159))return nullptr;
            }
            segment.offsets.resize(count+1);segment.offsets[0]=0;
            uint32_t end=0;
            for(uint32_t offset=0;offset<count;offset++){
                uint16_t width=0;
                if(!reader.width(width) || uint32_t(width)>display.size()-end)return nullptr;
                end+=width;
                segment.offsets[offset+1]=uint16_t(end);
            }
            if(end!=display.size())return nullptr;
            target->ranges.push_back(std::move(segment));
            references[target->ranges.back().run]+=count;last[target->ranges.back().run]+=target->ranges.back().flags;
        }
        if(!reader.done())return nullptr;
        for(uint32_t i=0;i<runs;i++)if(!references[i] || last[i]!=1)return nullptr;
        for(auto& entry:plan->lines){
            auto& values=entry.second.ranges;
            if(values.empty())return nullptr;
            std::sort(values.begin(),values.end(),[](auto& a,auto& b){return a.position<b.position;});
            for(size_t i=1;i<values.size();i++)if(values[i-1].position+values[i-1].offsets.size()-1>values[i].position)return nullptr;
        }
        plan->slotCount=positions;
        plan->memoryUsage=metadata+textBytes+uint64_t(positions)*2;
        return plan;
    } catch(...){return nullptr;}
}

// lookup 核对整行及原字符后按段内精确边界取得显示副本，不修改原始游标
bool DisplayPlan::lookup(const std::wstring& storage,uint32_t line,uint32_t position,const std::wstring& sourceLine,const std::wstring& source,std::wstring& display) const {
    if(source.size()!=1 || !position || sourceLine.size()>32768 || position>sourceLine.size() || sourceLine[position-1]!=source[0])return false;
    auto storageID=storages.find(storage);
    if(storageID==storages.end())return false;
    auto found=lines.find(std::make_pair(storageID->second,line));
    if(found==lines.end())return false;
    auto& original=strings[found->second.source];
    if(original!=sourceLine){
        std::lock_guard<std::mutex> lock(expandedMutex);
        if(expandedLine!=&found->second || expandedSource!=sourceLine){
            expandedLine=&found->second;expandedSource=sourceLine;
            expandedPositions=dango_krkr::expandedPositions(original,sourceLine);
        }
        if(position>=expandedPositions.size() || !expandedPositions[position])return false;
        position=expandedPositions[position];
    }
    if(position>original.size() || original[position-1]!=source[0])return false;
    auto& ranges=found->second.ranges;
    auto segment=std::upper_bound(ranges.begin(),ranges.end(),position,[](uint32_t pos,auto& value){return pos<value.position;});
    if(segment==ranges.begin())return false;
    --segment;
    size_t offset=position-segment->position;
    if(offset+1>=segment->offsets.size())return false;
    display=strings[segment->display].substr(segment->offsets[offset],segment->offsets[offset+1]-segment->offsets[offset]);
    return true;
}
}
