#pragma once
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <array>

namespace dango_krkr {
enum class Action : uint32_t { Inspect=1, ReadResource=2, Detach=3, StageDisplay=4, PrepareDisplay=5, DisplayStatus=6, InspectLayout=7, ActivateDisplay=8, DeactivateDisplay=9, PollDisplay=10 };
struct Reply {
    uint32_t status=0;
    std::vector<uint8_t> data;
};
struct SdkApi {
    bool (*initialize)(void*, const std::wstring&);
    HWND (*applicationWindow)();
    Reply (*perform)(Action, const std::wstring&, const std::vector<uint8_t>&);
    const wchar_t* abiName;
};
const SdkApi* legacySdk();
const SdkApi* currentSdk();
// negotiateSdk 按公开接口实际清栈行为核对调用约定
const SdkApi* negotiateSdk(void* exporter);
}
