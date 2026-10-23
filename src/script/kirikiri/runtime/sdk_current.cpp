#include "sdk_api.h"
#include "display_plan.h"
#define TJS_INTF_METHOD __cdecl
#define DANGO_KRKR_SDK_ABI L"cdecl"
namespace dango_krkr { namespace current {
#include "sdk_impl.inc"
}
const SdkApi* currentSdk(){return &current::api;}
}
