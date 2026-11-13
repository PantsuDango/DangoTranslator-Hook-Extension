#include "sdk_api.h"
#include "display_plan.h"
#define TJS_INTF_METHOD __cdecl
#if defined(_WIN64)
#define DANGO_KRKR_SDK_ABI L"x64"
#else
#define DANGO_KRKR_SDK_ABI L"cdecl"
#endif
namespace dango_krkr { namespace current {
#include "sdk_impl.inc"
}
const SdkApi* currentSdk(){return &current::api;}
}
