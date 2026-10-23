#pragma once
#include "sdk_api.h"
#include <map>
#include <memory>
#include <mutex>

namespace dango_krkr {
struct DisplayRange {uint32_t position, display, run, flags;std::vector<uint16_t> offsets;};
struct DisplayLine {uint32_t source;std::vector<DisplayRange> ranges;};
struct DisplayPlan {
    uint64_t generation=0;
    std::vector<std::wstring> strings;
    std::vector<std::array<uint32_t,2>> runs;
    std::map<std::wstring,uint32_t> storages;
    std::map<std::pair<uint32_t,uint32_t>,DisplayLine> lines;
    uint32_t slotCount=0;
    uint64_t memoryUsage=0;
    mutable std::mutex expandedMutex;
    mutable const DisplayLine* expandedLine=nullptr;
    mutable std::wstring expandedSource;
    mutable std::vector<uint32_t> expandedPositions;
    bool lookup(const std::wstring&,uint32_t,uint32_t,const std::wstring&,const std::wstring&,std::wstring&) const;
};
std::unique_ptr<DisplayPlan> parseDisplayPlan(const std::vector<uint8_t>&,const std::wstring&,uint64_t,uint64_t retained=0);
}
