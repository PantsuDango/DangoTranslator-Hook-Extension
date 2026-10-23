#include <windows.h>

#include <string>
#if defined(DANGO_DXCOMPILED_ONLY)
#include <cstdio>
#endif

// injectionDiagnostic 仅为启用诊断的引擎输出有界阶段信息，不改变注入器退出协议。
static void injectionDiagnostic(const char *stage, DWORD error = ERROR_SUCCESS, DWORD value = 0)
{
#if defined(DANGO_DXCOMPILED_ONLY)
    std::printf("stage=%s win32=%lu value=%lu\n", stage, error, value);
    std::fflush(stdout);
#else
    (void)stage; (void)error; (void)value;
#endif
}

#if defined(DANGO_DXCOMPILED_ONLY)
// injectionTokenDiagnostic 记录提升状态和完整性级别，查询失败不影响实际注入。
static void injectionTokenDiagnostic(HANDLE process, const char *label)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
    {
        injectionDiagnostic(label, GetLastError());
        return;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL valid = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    const DWORD error = valid ? ERROR_SUCCESS : GetLastError();
    alignas(TOKEN_MANDATORY_LABEL) BYTE integrityBuffer[256]{};
    DWORD integrity = 0;
    DWORD integrityError = ERROR_SUCCESS;
    if (GetTokenInformation(token, TokenIntegrityLevel, integrityBuffer, sizeof(integrityBuffer), &size))
    {
        const auto labelInfo = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(integrityBuffer);
        if (IsValidSid(labelInfo->Label.Sid))
        {
            const BYTE count = *GetSidSubAuthorityCount(labelInfo->Label.Sid);
            if (count) integrity = *GetSidSubAuthority(labelInfo->Label.Sid, count - 1);
        }
    }
    else integrityError = GetLastError();
    std::printf("stage=%s win32=%lu elevated=%lu integrity=%lu integrity_win32=%lu\n",
                label, error, elevation.TokenIsElevated, integrity, integrityError);
    std::fflush(stdout);
    CloseHandle(token);
}
#endif

// wmain 将Hook引擎组件注入指定的游戏进程
int wmain(int argc, wchar_t **argv)
{
    if (argc != 4 || std::wstring(argv[1]) != L"dllinject")
    {
        injectionDiagnostic("arguments", ERROR_INVALID_PARAMETER);
        return 0;
    }

    wchar_t *processIdEnd = nullptr;
    const unsigned long processIdValue = wcstoul(argv[2], &processIdEnd, 10);
    if (!processIdEnd || *processIdEnd != L'\0' || processIdValue == 0)
    {
        injectionDiagnostic("process_id", ERROR_INVALID_PARAMETER);
        return 0;
    }

    const std::wstring hookPath = argv[3];
    if (hookPath.empty())
    {
        injectionDiagnostic("hook_path", ERROR_INVALID_PARAMETER);
        return 0;
    }

#if defined(DANGO_DXCOMPILED_ONLY)
    injectionDiagnostic("begin", ERROR_SUCCESS, static_cast<DWORD>(processIdValue));
    injectionTokenDiagnostic(GetCurrentProcess(), "injector_token");
    HANDLE queryProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processIdValue));
    if (queryProcess) { injectionTokenDiagnostic(queryProcess, "target_token"); CloseHandle(queryProcess); }
    else injectionDiagnostic("target_query", GetLastError());
    const DWORD attributes = GetFileAttributesW(hookPath.c_str());
    injectionDiagnostic("hook_file", attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS, attributes);
#endif
    constexpr DWORD injectAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE process = OpenProcess(injectAccess, FALSE, static_cast<DWORD>(processIdValue));
    if (!process)
    {
        injectionDiagnostic("open_process", GetLastError(), injectAccess);
        return 0;
    }
    injectionDiagnostic("open_process");

    const SIZE_T hookPathBytes = (hookPath.size() + 1) * sizeof(wchar_t);
    void *remoteHookPath = VirtualAllocEx(process, nullptr, hookPathBytes,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remoteHookPath)
    {
        injectionDiagnostic("allocate_remote_path", GetLastError());
        CloseHandle(process);
        return 0;
    }

    SIZE_T writtenBytes = 0;
    const BOOL pathWritten = WriteProcessMemory(process, remoteHookPath, hookPath.c_str(),
                                                hookPathBytes, &writtenBytes);
    const DWORD writeError = pathWritten ? ERROR_SUCCESS : GetLastError();
    injectionDiagnostic("write_remote_path", writeError, static_cast<DWORD>(writtenBytes));
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    const DWORD resolveError = loadLibrary ? ERROR_SUCCESS : GetLastError();
    injectionDiagnostic("resolve_load_library", resolveError);
    if (!pathWritten || writtenBytes != hookPathBytes || !loadLibrary)
    {
        VirtualFreeEx(process, remoteHookPath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 0;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteHookPath, 0, nullptr);
    if (!thread)
    {
        injectionDiagnostic("create_remote_thread", GetLastError());
        VirtualFreeEx(process, remoteHookPath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 0;
    }
    injectionDiagnostic("create_remote_thread");

    DWORD remoteModule = 0;
    const DWORD waitResult = WaitForSingleObject(thread, 10000);
    const DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : (waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_SUCCESS);
    injectionDiagnostic("wait_remote_thread", waitError, waitResult);
    const BOOL exitCodeRead = waitResult == WAIT_OBJECT_0 && GetExitCodeThread(thread, &remoteModule);
    if (waitResult == WAIT_OBJECT_0)
    {
        injectionDiagnostic("read_remote_exit", exitCodeRead ? ERROR_SUCCESS : GetLastError(), remoteModule);
        // LoadLibraryW在远端线程返回空值时，本线程GetLastError不代表远端错误，明确标注未知。
#if defined(DANGO_DXCOMPILED_ONLY)
        if (exitCodeRead && !remoteModule) {
            std::printf("stage=load_library outcome=failed remote_win32=unavailable\n");
            std::fflush(stdout);
        }
#endif
    }

    CloseHandle(thread);
#if defined(DANGO_DXCOMPILED_ONLY)
    // 等待失败时远端线程可能仍使用路径，保留这小段内存直至进程退出，避免释放后读取。
    if (waitResult == WAIT_OBJECT_0) VirtualFreeEx(process, remoteHookPath, 0, MEM_RELEASE);
#else
    VirtualFreeEx(process, remoteHookPath, 0, MEM_RELEASE);
#endif
    CloseHandle(process);
    const bool succeeded = exitCodeRead && remoteModule != 0;
    injectionDiagnostic(succeeded ? "complete" : "failed");
    return succeeded ? 1 : 0;
}
