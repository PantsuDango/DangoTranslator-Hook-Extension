#include "python.h"

// x86 __cdecl is decorated internally; ctypes uses the same public ABI on both builds.
#ifndef _WIN64
#pragma comment(linker, "/EXPORT:renpy_internal_capture_event=_renpy_internal_capture_event")
#pragma comment(linker, "/EXPORT:renpy_internal_embed_text=_renpy_internal_embed_text")
#pragma comment(linker, "/EXPORT:renpy_internal_embed_font=_renpy_internal_embed_font")
#endif

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
int (*Py_IsInitialized)(void);
PyObject *(*PyImport_AddModule)(const char *name);
PyObject *(*PyModule_GetDict)(PyObject *module);
PyObject *(*PyRun_StringFlags)(const char *code, int start, PyObject *globals, PyObject *locals, void *flags);
int (*PyObject_IsTrue)(PyObject *object);
void (*Py_DecRef)(PyObject *object);
void (*PyErr_Clear)(void);

bool loadPyRun(HMODULE module)
{
    PyGILState_Ensure = reinterpret_cast<decltype(PyGILState_Ensure)>(GetProcAddress(module, "PyGILState_Ensure"));
    PyGILState_Release = reinterpret_cast<decltype(PyGILState_Release)>(GetProcAddress(module, "PyGILState_Release"));
    PyRun_SimpleString = reinterpret_cast<decltype(PyRun_SimpleString)>(GetProcAddress(module, "PyRun_SimpleString"));
    PyImport_ImportModule = reinterpret_cast<decltype(PyImport_ImportModule)>(GetProcAddress(module, "PyImport_ImportModule"));
    Py_IsInitialized = reinterpret_cast<decltype(Py_IsInitialized)>(GetProcAddress(module, "Py_IsInitialized"));
    PyImport_AddModule = reinterpret_cast<decltype(PyImport_AddModule)>(GetProcAddress(module, "PyImport_AddModule"));
    PyModule_GetDict = reinterpret_cast<decltype(PyModule_GetDict)>(GetProcAddress(module, "PyModule_GetDict"));
    PyRun_StringFlags = reinterpret_cast<decltype(PyRun_StringFlags)>(GetProcAddress(module, "PyRun_StringFlags"));
    PyObject_IsTrue = reinterpret_cast<decltype(PyObject_IsTrue)>(GetProcAddress(module, "PyObject_IsTrue"));
    Py_DecRef = reinterpret_cast<decltype(Py_DecRef)>(GetProcAddress(module, "Py_DecRef"));
    PyErr_Clear = reinterpret_cast<decltype(PyErr_Clear)>(GetProcAddress(module, "PyErr_Clear"));
    return PyGILState_Ensure && PyGILState_Release && PyRun_SimpleString && PyImport_ImportModule &&
           Py_IsInitialized && PyImport_AddModule && PyModule_GetDict && PyRun_StringFlags && PyObject_IsTrue && Py_DecRef && PyErr_Clear;
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

bool renpyRuntimeReady()
{
    if (!Py_IsInitialized())
        return false;
    PyGILState state;
    PyObject *mainModule = PyImport_AddModule("__main__"); // borrowed
    PyObject *globals = mainModule ? PyModule_GetDict(mainModule) : nullptr; // borrowed
    // Inspect existing modules only: importing Ren'Py here can race its startup
    // and observe a partially initialized module. No game code is executed.
    const char *check =
        "(lambda r: r is not None and "
        "hasattr(r, 'character') and hasattr(r.character, 'ADVCharacter') and "
        "hasattr(r, 'store') and hasattr(r.store, 'NVLCharacter') and "
        "hasattr(r.store, 'menu') and hasattr(r.store, 'nvl_menu') and hasattr(r.store, '_preferences') and "
        "hasattr(r, 'substitutions') and hasattr(r.substitutions, 'formatter') and "
        "hasattr(r, 'config') and hasattr(r.config, 'interact_callbacks') and hasattr(r.config, 'periodic_callbacks') and "
        "hasattr(r, 'game') and bool(r.game.contexts) and not getattr(r.game.contexts[-1], 'init_phase', True))"
        "(__import__('sys').modules.get('renpy'))";
    PyObject *result = globals ? PyRun_StringFlags(check, 258 /* Py_eval_input */, globals, globals, nullptr) : nullptr;
    bool ready = result && PyObject_IsTrue(result) == 1;
    if (result)
        Py_DecRef(result);
    PyErr_Clear();
    return ready;
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
    if (!loadPyRun(module))
        return false;
    // Readiness polling is separate from the shared first-text handshake.
    // Do not execute the install script once and abandon an early game import.
    const ULONGLONG started = GetTickCount64();
    while (!renpyRuntimeReady())
    {
        if (GetTickCount64() - started > 120000)
        {
            Msg::Log("RenPy runtime initialization timed out");
            return false;
        }
        Sleep(50);
    }
    if (!installRenPyDialogueHook())
    {
        Msg::Log("RenPy dialogue installation failed");
        return false;
    }
    dont_detach = true;
    return true;
}
