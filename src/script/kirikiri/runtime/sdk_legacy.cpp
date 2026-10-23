#include "sdk_api.h"
#include "display_plan.h"
#define TJS_INTF_METHOD __stdcall
#define DANGO_KRKR_SDK_ABI L"stdcall"
namespace dango_krkr { namespace legacy {
#include "sdk_impl.inc"
}
const SdkApi* legacySdk(){return &legacy::api;}
}
