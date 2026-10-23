#pragma once
#include <windows.h>
#include <string>

namespace teamnimbuslease
{
    // Host 当前Helper创建并持有独立租约，进程身份和撤销由托管主线程核对
    class Host
    {
        HANDLE mapping = nullptr;
        unsigned char *memory = nullptr;
        bool owner = false;
        HANDLE gate = nullptr;
        bool gateOwned = false;
    public:
        bool open(DWORD target, const std::wstring &digest)
        {
            auto name = L"Local\\DangoTeamNimbusLease_" + std::to_wstring(target);
            gate = CreateMutexW(nullptr, TRUE, (name + L".Owner").c_str());
            bool previousGate = GetLastError() == ERROR_ALREADY_EXISTS;
            if (!gate) return false;
            if (previousGate) {
                DWORD state = WaitForSingleObject(gate, 0);
                if (state != WAIT_OBJECT_0 && state != WAIT_ABANDONED) return false;
            }
            gateOwned = true;
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, name.c_str());
            bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
            if (!mapping) return false;
            memory = reinterpret_cast<unsigned char *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 64));
            if (!memory) return false;
            if (existed) {
                DWORD pid = *reinterpret_cast<DWORD *>(memory + 4);
                HANDLE previous = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                bool dead = false;
                if (previous) {
                    FILETIME c{}, e{}, k{}, u{};
                    dead = WaitForSingleObject(previous, 0) == WAIT_OBJECT_0 ||
                        (GetProcessTimes(previous, &c, &e, &k, &u) && CompareFileTime(&c, reinterpret_cast<FILETIME *>(memory + 16)) != 0);
                    CloseHandle(previous);
                } else dead = GetLastError() == ERROR_INVALID_PARAMETER;
                if (!dead) return false;
            }
            FILETIME created{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user)) return false;
            ZeroMemory(memory, 64);
            if (!digest.empty()) {
                if (digest.size() != 64) return false;
                for (size_t i = 0; i < 32; i++) {
                    auto hex = [](wchar_t c)->int { return c >= L'0' && c <= L'9' ? c-L'0' : c >= L'a' && c <= L'f' ? c-L'a'+10 : -1; };
                    int a = hex(digest[2*i]), b = hex(digest[2*i+1]);
                    if (a < 0 || b < 0) return false;
                    memory[32+i] = static_cast<unsigned char>((a << 4) | b);
                }
            }
            *reinterpret_cast<DWORD *>(memory) = 0x544e4c31;
            *reinterpret_cast<DWORD *>(memory + 4) = GetCurrentProcessId();
            *reinterpret_cast<DWORD *>(memory + 8) = target;
            *reinterpret_cast<FILETIME *>(memory + 16) = created;
            InterlockedExchange(reinterpret_cast<LONG *>(memory + 24), 1);
            owner = true;
            return true;
        }
        // revoke 仅撤销当前持有者的显示授权，读取和Helper连接继续保留
        void revoke()
        {
            if (owner && memory && *reinterpret_cast<DWORD *>(memory + 4) == GetCurrentProcessId())
                InterlockedExchange(reinterpret_cast<LONG *>(memory + 24), 0);
        }
        ~Host()
        {
            if (memory) {
                if (owner && *reinterpret_cast<DWORD *>(memory + 4) == GetCurrentProcessId()) InterlockedExchange(reinterpret_cast<LONG *>(memory + 24), 0);
                UnmapViewOfFile(memory);
            }
            if (mapping) CloseHandle(mapping);
            if (gateOwned) ReleaseMutex(gate);
            if (gate) CloseHandle(gate);
        }
    };
}
