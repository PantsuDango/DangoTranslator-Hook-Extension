#include "python.h"

namespace
{
volatile LONG renpyCaptureEntryMarker = 0;
volatile LONG renpyEmbedEntryMarker = 0;
}

extern "C" __declspec(dllexport) __declspec(noinline) const wchar_t *renpy_internal_capture_event(const wchar_t *text, int)
{
    (void)renpyCaptureEntryMarker;
    return text;
}

extern "C" __declspec(dllexport) __declspec(noinline) const wchar_t *renpy_internal_embed_text(const wchar_t *text, int)
{
    (void)renpyEmbedEntryMarker;
    return text;
}

extern "C" __declspec(dllexport) const wchar_t *renpy_internal_embed_font()
{
    if (!commonsharedmem)
    {
        return L"";
    }
    return commonsharedmem->fontFamily;
}

void RenpyCommonFilter(TextBuffer *, HookParam *)
{
}

namespace
{
struct PyObject;

typedef enum
{
    PyGILState_LOCKED,
    PyGILState_UNLOCKED
} PyGILState_STATE;

PyGILState_STATE (*PyGILState_Ensure)(void);
void (*PyGILState_Release)(PyGILState_STATE);
int (*PyRun_SimpleString)(const char *command);
PyObject *(*PyImport_ImportModule)(const char *name);

bool loadPyRun(HMODULE module)
{
    PyGILState_Ensure = reinterpret_cast<decltype(PyGILState_Ensure)>(GetProcAddress(module, "PyGILState_Ensure"));
    PyGILState_Release = reinterpret_cast<decltype(PyGILState_Release)>(GetProcAddress(module, "PyGILState_Release"));
    PyRun_SimpleString = reinterpret_cast<decltype(PyRun_SimpleString)>(GetProcAddress(module, "PyRun_SimpleString"));
    PyImport_ImportModule = reinterpret_cast<decltype(PyImport_ImportModule)>(GetProcAddress(module, "PyImport_ImportModule"));
    return PyGILState_Ensure && PyGILState_Release && PyRun_SimpleString && PyImport_ImportModule;
}

struct PyGILState
{
    PyGILState_STATE state;

    PyGILState()
        : state(PyGILState_Ensure())
    {
    }

    ~PyGILState()
    {
        PyGILState_Release(state);
    }
};

bool runPyScript(const char *script)
{
    if (!script || !*script)
    {
        return false;
    }
    PyGILState state;
    return PyRun_SimpleString(script) == 0;
}

bool installRenPyDialogueHook()
{
    const std::string script = LoadResData(L"renpy_hook_text", L"PYSOURCE");
    if (!runPyScript(script.c_str()))
    {
        return false;
    }
    HookParam captureHook;
    captureHook.address = reinterpret_cast<uintptr_t>(renpy_internal_capture_event);
    captureHook.offset = GETARG(1);
    captureHook.split = GETARG(2);
    captureHook.type = USING_SPLIT | USING_STRING | CODEC_UTF16 | NO_CONTEXT | FULL_STRING;

    HookParam embedHook;
    embedHook.address = reinterpret_cast<uintptr_t>(renpy_internal_embed_text);
    embedHook.offset = GETARG(1);
    embedHook.split = GETARG(2);
    embedHook.type = USING_SPLIT | USING_STRING | CODEC_UTF16 | EMBED_ABLE | EMBED_AFTER_NEW | NO_CONTEXT | FULL_STRING;
    return NewHook(captureHook, "RenPyDialogue") && NewHook(embedHook, "RenPyEmbed");
}
} // namespace

bool hookrenpy(HMODULE module)
{
    if (!loadPyRun(module) || !installRenPyDialogueHook())
    {
        return false;
    }
    dont_detach = true;
    return true;
}
