#include <windows.h>
#if defined(DANGO_TEAMNIMBUS_ONLY)
#include "../teamnimbus/Lease.h"
#endif
#if defined(DANGO_NEMK_NOVEL_ONLY)
#include "../nemk_novel/NemkControl.h"
#endif
#if defined(DANGO_NSCRIPTER_ONLY)
#include "../nscripter/NScripterControl.h"
#endif
#if defined(DANGO_CSYSTEM_ONLY)
#include "../csystem/CSystemControl.h"
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
#include "../yarn_spinner/YarnControl.h"
#endif
#if defined(DANGO_LCSE_ONLY)
#include "../lcse/LCSEControl.h"
#endif
#if defined(DANGO_PJADV_ONLY) || defined(DANGO_YARN_SPINNER_ONLY)
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
#if defined(DANGO_NEMK_NOVEL_ONLY)
nemkcontrol::Host g_nemkControl;
bool g_nemkChannel=false;
#endif
constexpr int kProtocolVersion = 1;
constexpr DWORD kInjectorTimeoutMs = 60000;
constexpr DWORD kInjectorSuccessExitCode = 1;
constexpr int kKiriKiriTextFlushDelayMs = 0;
constexpr int kCatSystem2TextFlushDelayMs = 0;
constexpr int kAZSystemTextFlushDelayMs = 8;
#if defined(DANGO_PJADV_ONLY)
constexpr std::uint64_t kPJADVFastSkipFollowupMs = 250;
constexpr std::uint64_t kPJADVFastSkipResumeMs = 700;
#endif
#if defined(DANGO_TASKFORCE2_ONLY)
constexpr std::uint64_t kTaskforce2FastSkipFollowupMs = 180;
constexpr std::uint64_t kTaskforce2FastSkipResumeMs = 500;
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
constexpr std::uint64_t kBGIFastSkipFollowupMs = 220;
constexpr std::uint64_t kBGIFastSkipResumeMs = 700;
#endif
#if defined(DANGO_QLIE_ONLY)
constexpr std::uint64_t kQLIEFastSkipFollowupMs = 220;
constexpr std::uint64_t kQLIEFastSkipResumeMs = 700;
constexpr unsigned kQLIEFastSkipRequiredFollowups = 2;
#endif
#if defined(DANGO_WILLPLUS_ONLY)
// WillPlus每页只产生一个完整正文请求；译文等待解除后紧接着出现不同正文，
// 即表示游戏正在连续推进。进入跳过后保持一小段静默窗口，避免高速页重新等待译文。
constexpr std::uint64_t kWillPlusFastSkipFollowupMs = 250;
constexpr std::uint64_t kWillPlusFastSkipResumeMs = 700;
#endif
#if defined(DANGO_VALKYRIA_ONLY)
constexpr std::uint64_t kValkyriaFastSkipFollowupMs = 250;
constexpr std::uint64_t kValkyriaFastSkipResumeMs = 700;
#endif
#if defined(DANGO_RENPY_ONLY) || defined(DANGO_UNITY_EVENTSCRIPT_ONLY) || defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY) || defined(DANGO_QLIE_ONLY)
constexpr UINT32 kEmbedWaitTimeoutMs = 15000;
#elif defined(DANGO_WOLF_ONLY)
// WOLF在游戏主线程同步等待译文，限制最长阻塞时间以免单句异常时长时间无响应
constexpr UINT32 kEmbedWaitTimeoutMs = 12000;
#elif defined(DANGO_WILLPLUS_ONLY) || defined(DANGO_VALKYRIA_ONLY)
// 完整页面入口同步等待译文；Host异常退出时必须尽快恢复游戏原文。
constexpr UINT32 kEmbedWaitTimeoutMs = 12000;
#else
constexpr UINT32 kEmbedWaitTimeoutMs = 45000;
#endif

struct ThreadParam
{
    DWORD processId;
    std::uint64_t addr;
    std::uint64_t ctx;
    std::uint64_t ctx2;
};

using ProcessEvent = void(__cdecl *)(DWORD);
using ThreadEventMaybeEmbed = void(__cdecl *)(const wchar_t *, const char *, ThreadParam, bool);
using ThreadEvent = void(__cdecl *)(const wchar_t *, const char *, ThreadParam);
using OutputCallback = void(__cdecl *)(const wchar_t *, const char *, ThreadParam, const wchar_t *);
using HostInfoHandler = void(__cdecl *)(int, const wchar_t *);
using HookInsertHandler = void(__cdecl *)(DWORD, std::uint64_t, const wchar_t *);
using EmbedCallback = void(__cdecl *)(const wchar_t *, ThreadParam);
using I18NQueryCallback = wchar_t *(__cdecl *)(const wchar_t *);
using EmuGameInfoCallback = void(__cdecl *)(const wchar_t *, const wchar_t *, const wchar_t *);

using HookStart = void(__cdecl *)(ProcessEvent, ProcessEvent, ThreadEventMaybeEmbed, ThreadEvent,
                                  OutputCallback, HostInfoHandler, HookInsertHandler, EmbedCallback,
                                  I18NQueryCallback, EmuGameInfoCallback);
using HookSettings = void(__cdecl *)(int, bool, int, int, int, bool);
using HookConnectProcess = void(__cdecl *)(DWORD);
using HookCheckIfNeedInject = bool(__cdecl *)(DWORD);
using HookDetachProcess = void(__cdecl *)(DWORD);
using HookSettingsEx = void(__cdecl *)(DWORD, UINT32, UINT8, bool, wchar_t *, UINT32, bool, bool, bool, float, bool);
using HookUseEmbed = void(__cdecl *)(ThreadParam, bool);
using HookEmbedResult = void(__cdecl *)(ThreadParam, LPCWSTR, LPCWSTR);

struct ThreadParamLess
{
    bool operator()(const ThreadParam &left, const ThreadParam &right) const
    {
        return std::tie(left.processId, left.addr, left.ctx, left.ctx2) <
               std::tie(right.processId, right.addr, right.ctx, right.ctx2);
    }
};

struct EmbedThread
{
    ThreadParam thread;
    std::string hook;
};

struct EmbedRequest
{
    ThreadParam thread;
    std::string hook;
    std::wstring original;
};

std::mutex g_outputMutex;
std::atomic_bool g_supportedHookFound{false};
std::atomic_bool g_processExitReported{false};
std::atomic_uint64_t g_sequence{0};
std::atomic_int g_exitCode{0};
HANDLE g_shutdownEvent = nullptr;
DWORD g_targetProcessId = 0;
#if defined(DANGO_NSCRIPTER_ONLY)
nscriptercontrol::Host g_nscripterControl;
bool g_nscripterChannel=false;
std::atomic_bool g_nscripterBackfillReady{false},g_nscripterBackfillEnabled{false};
#endif
#if defined(DANGO_CSYSTEM_ONLY)
csystemcontrol::Host g_csystemControl;
bool g_csystemChannel = false;
std::atomic_bool g_csystemBackfillEnabled{false};
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
yarncontrol::Host g_yarnControl;
bool g_yarnChannel = false;
std::atomic_bool g_yarnConnected{false}, g_yarnAnyThread{false}, g_yarnSupportedThread{false};
std::atomic_bool g_yarnFirstOutput{false};
#endif
#if defined(DANGO_PJADV_ONLY)
std::uint64_t g_pjadvTargetBase = 0;
std::uint64_t g_pjadvMessageRVA = 0;
std::uint64_t g_pjadvChoiceRVA = 0;
std::set<std::uint64_t> g_pjadvSpeakerContexts;
#endif
HookDetachProcess g_hookDetachProcess = nullptr;
HookSettingsEx g_hookSettingsEx = nullptr;
HookUseEmbed g_hookUseEmbed = nullptr;
HookEmbedResult g_hookEmbedResult = nullptr;
#if defined(DANGO_LCSE_ONLY)
using HookLCSEControl = bool(__cdecl *)(DWORD, const LCSEControl::Request *);
HookLCSEControl g_hookLCSEControl = nullptr;
#endif
std::atomic_bool g_embedEnabled{false};
std::atomic_bool g_embedReadyReported{false};
std::atomic_uint64_t g_embedRequestSequence{0};
std::mutex g_embedMutex;
std::map<ThreadParam, EmbedThread, ThreadParamLess> g_embedThreads;
std::map<std::uint64_t, EmbedRequest> g_embedRequests;
std::mutex g_embedFontMutex;
std::wstring g_embedFontFamily;
#if defined(DANGO_PJADV_ONLY)
struct PJADVEmbedFinishedState
{
    std::uint64_t finishedAt = 0;
    std::wstring original;
};
std::mutex g_pjadvFastSkipMutex;
std::map<ThreadParam, PJADVEmbedFinishedState, ThreadParamLess> g_pjadvEmbedFinishedByThread;
std::atomic_uint64_t g_pjadvLastFastSkipAt{0};
std::atomic_uint64_t g_pjadvFastSkipGeneration{0};
std::atomic_bool g_pjadvFastSkipping{false};
#endif
#if defined(DANGO_KIRIKIRI_ONLY)
std::mutex g_kiriKiriHookAttemptMutex;
std::map<std::uint64_t, std::string> g_kiriKiriHookAttempts;
#endif
#if defined(DANGO_TASKFORCE2_ONLY)
std::atomic_uint64_t g_taskforce2LastEmbedFinishedAt{0};
std::atomic_uint64_t g_taskforce2LastFastSkipAt{0};
std::atomic_uint64_t g_taskforce2FastSkipGeneration{0};
std::atomic_bool g_taskforce2FastSkipping{false};
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
struct BGIEmbedFinishedState
{
    std::uint64_t finishedAt;
    std::wstring original;
};

std::mutex g_bgiFastSkipMutex;
std::map<ThreadParam, BGIEmbedFinishedState, ThreadParamLess> g_bgiEmbedFinishedByThread;
std::atomic_uint64_t g_bgiLastFastSkipAt{0};
std::atomic_uint64_t g_bgiFastSkipGeneration{0};
std::atomic_bool g_bgiFastSkipping{false};
#if defined(DANGO_SIGLUS_ONLY)
std::map<ThreadParam, unsigned, ThreadParamLess> g_siglusConsecutivePages;
#endif
#endif
#if defined(DANGO_QLIE_ONLY)
struct QLIEEmbedFinishedState
{
    std::uint64_t finishedAt = 0;
    std::wstring original;
    unsigned rapidFollowups = 0;
};

std::mutex g_qlieFastSkipMutex;
std::map<ThreadParam, QLIEEmbedFinishedState, ThreadParamLess> g_qlieEmbedFinishedByThread;
std::atomic_uint64_t g_qlieLastFastSkipAt{0};
std::atomic_uint64_t g_qlieFastSkipGeneration{0};
std::atomic_bool g_qlieFastSkipping{false};
#endif
#if defined(DANGO_WILLPLUS_ONLY)
struct WillPlusEmbedFinishedState
{
    std::uint64_t finishedAt = 0;
    std::wstring original;
};

std::mutex g_willPlusFastSkipMutex;
std::map<ThreadParam, WillPlusEmbedFinishedState, ThreadParamLess> g_willPlusEmbedFinishedByThread;
std::atomic_uint64_t g_willPlusLastFastSkipAt{0};
std::atomic_uint64_t g_willPlusFastSkipGeneration{0};
std::atomic_bool g_willPlusFastSkipping{false};
std::atomic_bool g_willPlusFullTextBackfillEnabled{false};
#endif
#if defined(DANGO_VALKYRIA_ONLY)
struct ValkyriaEmbedFinishedState
{
    std::uint64_t finishedAt = 0;
    std::wstring original;
};

std::mutex g_valkyriaFastSkipMutex;
std::map<ThreadParam, ValkyriaEmbedFinishedState, ThreadParamLess> g_valkyriaEmbedFinishedByThread;
std::atomic_uint64_t g_valkyriaLastFastSkipAt{0};
std::atomic_uint64_t g_valkyriaFastSkipGeneration{0};
std::atomic_bool g_valkyriaFastSkipping{false};
std::atomic_bool g_valkyriaFullTextBackfillEnabled{false};
#endif

std::int64_t unixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string wideToUtf8(const wchar_t *value)
{
    if (!value || !*value)
    {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

std::wstring utf8ToWide(const std::string &value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), required) <= 0)
    {
        return {};
    }
    return result;
}

std::string decodeBase64(const std::string &value)
{
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned int bits = 0;
    int bitCount = 0;
    for (const unsigned char character : value)
    {
        if (character == '=')
        {
            break;
        }
        const auto position = alphabet.find(static_cast<char>(character));
        if (position == std::string::npos)
        {
            continue;
        }
        bits = (bits << 6) | static_cast<unsigned int>(position);
        bitCount += 6;
        if (bitCount >= 8)
        {
            bitCount -= 8;
            result.push_back(static_cast<char>((bits >> bitCount) & 0xff));
            bits = bitCount == 0 ? 0 : bits & ((1u << bitCount) - 1u);
        }
    }
    return result;
}

std::string jsonEscape(const std::string &value)
{
    std::ostringstream output;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20)
            {
                constexpr char hex[] = "0123456789abcdef";
                output << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

void emitEvent(const std::string &type,
               const std::vector<std::pair<std::string, std::string>> &stringFields = {},
               const std::vector<std::pair<std::string, std::uint64_t>> &numberFields = {})
{
    std::ostringstream output;
    output << "{\"version\":" << kProtocolVersion
           << ",\"type\":\"" << jsonEscape(type) << "\""
           << ",\"pid\":" << g_targetProcessId
           << ",\"timestamp\":" << unixMilliseconds();
    for (const auto &[key, value] : stringFields)
    {
        output << ",\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"";
    }
    for (const auto &[key, value] : numberFields)
    {
        output << ",\"" << jsonEscape(key) << "\":" << value;
    }
    output << "}";

    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::cout << output.str() << std::endl;
}

#if defined(DANGO_YARN_SPINNER_ONLY)
// yarnDiagnostic 只输出技术阶段及进程身份，不记录原文、译文或回调参数
void yarnDiagnostic(const std::string &message)
{
    emitEvent("host_info", {{"message", "YarnSpinner diagnostic v=20261013-1 helper_pid=" + std::to_string(GetCurrentProcessId()) + " " + message}});
}

// yarnProcessPath 只读查询候选进程路径，查询失败时保留系统错误码
std::string yarnProcessPath(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "query_error=" + std::to_string(GetLastError());
    wchar_t path[32768]; DWORD length = _countof(path);
    bool valid = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    DWORD error = valid ? 0 : GetLastError(); CloseHandle(process);
    return valid ? wideToUtf8(std::wstring(path, length).c_str()) : "query_error=" + std::to_string(error);
}

// yarnEnvironmentSnapshot 有界记录相关模块和候选Helper，候选列表不冒充互斥对象持有者
void yarnEnvironmentSnapshot(const char *stage)
{
    std::string prefix = std::string("stage=") + stage;
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_targetProcessId);
    if (modules == INVALID_HANDLE_VALUE) yarnDiagnostic(prefix + " module_snapshot_error=" + std::to_string(GetLastError()));
    else
    {
        MODULEENTRY32W module{}; module.dwSize = sizeof(module); unsigned scanned = 0, matched = 0;
        BOOL more = Module32FirstW(modules, &module);
        if (!more) yarnDiagnostic(prefix + " module_first_error=" + std::to_string(GetLastError()));
        for (; more && scanned++ < 256 && matched < 24; more = Module32NextW(modules, &module))
        {
            std::wstring name = module.szModule;
            for (auto &c : name) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            if (name.find(L"hook") == std::wstring::npos && name.find(L"bridge") == std::wstring::npos &&
                name.find(L"mono") == std::wstring::npos && name != L"unityplayer.dll" && name != L"gameassembly.dll") continue;
            ++matched;
            yarnDiagnostic(prefix + " module=" + wideToUtf8(module.szModule) + " path=" + wideToUtf8(module.szExePath) + " bytes=" + std::to_string(module.modBaseSize));
        }
        yarnDiagnostic(prefix + " modules_scanned=" + std::to_string(scanned) + " modules_matched=" + std::to_string(matched));
        CloseHandle(modules);
    }
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) { yarnDiagnostic(prefix + " process_snapshot_error=" + std::to_string(GetLastError())); return; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); unsigned scanned = 0, matched = 0;
    BOOL more = Process32FirstW(processes, &entry);
    if (!more) yarnDiagnostic(prefix + " process_first_error=" + std::to_string(GetLastError()));
    for (; more && scanned++ < 8192 && matched < 16; more = Process32NextW(processes, &entry))
    {
        std::wstring name = entry.szExeFile;
        for (auto &c : name) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (entry.th32ProcessID != GetCurrentProcessId() && name.find(L"host64.exe") == std::wstring::npos &&
            name.find(L"host32.exe") == std::wstring::npos && name.find(L"dango") == std::wstring::npos &&
            name.find(L"luna") == std::wstring::npos && name.find(L"textractor") == std::wstring::npos) continue;
        ++matched;
        yarnDiagnostic(prefix + " candidate_pid=" + std::to_string(entry.th32ProcessID) + " parent_pid=" + std::to_string(entry.th32ParentProcessID) + " name=" + wideToUtf8(entry.szExeFile) + " path=" + yarnProcessPath(entry.th32ProcessID));
        if (entry.th32ProcessID == GetCurrentProcessId()) yarnDiagnostic(prefix + " client_pid=" + std::to_string(entry.th32ParentProcessID) + " client_path=" + yarnProcessPath(entry.th32ParentProcessID));
    }
    CloseHandle(processes);
}
#endif

// canonicalSupportedHookName 将引擎回调的Hook名称或Hook Code统一为标准名称
std::string canonicalSupportedHookName(const char *hookName)
{
    if (!hookName)
    {
        return {};
    }
    const std::string name(hookName);
#if defined(DANGO_PJADV_ONLY)
    if (name == "EmbedPensil" || name == "EmbedPensilChoice") return name;
#elif defined(DANGO_KIRIKIRI_ONLY)
    if (name == "KiriKiri2" || name == "KiriKiri1" ||
        name == "tTVPNativeBaseBitmap::DrawText" || name == "KiriKiriZ" ||
        name == "KiriKiriZ Hook" || name == "TextRender" ||
        name == "EmbedKrkr2" || name == "EmbedKrkrZ" ||
        name == "Krkr2wcs" || name == "KiriKiri4" || name == "krkrz2")
    {
        return name;
    }
#elif defined(DANGO_RENPY_ONLY)
    if (name == "RenPyDialogue" || name == "RenPyEmbed")
    {
        return name;
    }
#elif defined(DANGO_SIGLUS_ONLY)
    if (name == "EmbedSiglus" || (!name.empty() && name.front() == 'E')) return "EmbedSiglus";
#elif defined(DANGO_BGI_ONLY)
    if (name == "BGI")
    {
        return name;
    }
#elif defined(DANGO_CATSYSTEM2_ONLY)
    if (!name.empty() && name.front() == 'E')
    {
        return "EmbedCS2";
    }
    if (name == "CatSystem2" || name == "CatSystem2new" ||
        name == "EmbedCS2" || name == "catsystem")
    {
        return name;
    }
#elif defined(DANGO_AZSYSTEM_ONLY)
    if (name == "EmbedAZSystem" || (!name.empty() && name.front() == 'E'))
    {
        return "EmbedAZSystem";
    }
    if (name == "AZSystem" || name == "AZSystem2007A" || name == "AZSystem2007B" ||
        name == "AZSystem2007C")
    {
        return name;
    }
#elif defined(DANGO_NEMK_NOVEL_ONLY)
    if (name == "NemkNovelDialogue") return name;
#elif defined(DANGO_YARN_SPINNER_ONLY)
    if (name == "YarnSpinnerDialogue") return name;
#elif defined(DANGO_UNITY_SCENARIO_ONLY)
    if (name == "UnityScenarioDialogue" || name.find(":Scenario:getCurrentText:") != std::string::npos)
        return "UnityScenarioDialogue";
#elif defined(DANGO_TEAMNIMBUS_ONLY)
    if (name == "TeamNimbusDialogue") return "TeamNimbusDialogue";
#elif defined(DANGO_ADVENTURE_CREATOR_ONLY)
    if (name == "AdventureCreatorDialogue" || name.find(":Dialog:StartDialog:") != std::string::npos)
        return "AdventureCreatorDialogue";
    if (name == "AdventureCreatorChoices" || name.find(":Conversation:GetOptionName:") != std::string::npos ||
        name.find(":Conversation:GetOptionNameWithID:") != std::string::npos)
        return "AdventureCreatorChoices";
#elif defined(DANGO_RPGMAKER_UNITE_ONLY)
    if (name == "RPGMakerUniteDialogue" || name.find(":ShowMessage:") != std::string::npos ||
        name.find(":ShowCurrentMessage:") != std::string::npos)
        return "RPGMakerUniteDialogue";
    if (name == "RPGMakerUniteName" || name.find(":ShowName:") != std::string::npos)
        return "RPGMakerUniteName";
    if (name == "RPGMakerUniteChoices" || name.find(":ActiveSelectFrame:") != std::string::npos ||
        name.find(":ShowChoiceButtons:") != std::string::npos)
        return "RPGMakerUniteChoices";
#elif defined(DANGO_UTAGE_ONLY)
    if (name == "NovelTextComponentWrapper:SetNovelTextData" ||
        name.find(":NovelTextComponentWrapper:SetNovelTextData:") != std::string::npos)
    {
        return "NovelTextComponentWrapper:SetNovelTextData";
    }
    if (name == "NovelTextComponentWrapper:SetText" ||
        name.find(":NovelTextComponentWrapper:SetText:") != std::string::npos)
    {
        return "NovelTextComponentWrapper:SetText";
    }
    if (name == "NovelTextComponentWrapper:SetTextDirect" ||
        name.find(":NovelTextComponentWrapper:SetTextDirect:") != std::string::npos)
    {
        return "NovelTextComponentWrapper:SetTextDirect";
    }
    if (name == "AdvUguiMessageWindow:OnTextChanged" ||
        name.find(":AdvUguiMessageWindow:OnTextChanged:") != std::string::npos)
    {
        return "AdvUguiMessageWindow:OnTextChanged";
    }
#elif defined(DANGO_UNITY_EVENTSCRIPT_ONLY)
    if (name == "SaiminTextWindow:AddText" ||
        name.find(":SaiminTextWindow:AddText:") != std::string::npos)
    {
        return "SaiminTextWindow:AddText";
    }
    if (name == "BackLogSaveData:Add" ||
        name.find(":BackLogSaveData:Add:") != std::string::npos)
    {
        return "BackLogSaveData:Add";
    }
    if (name == "SelectWindow:Show" ||
        name.find(":SelectWindow:Show:") != std::string::npos)
    {
        return "SelectWindow:Show";
    }
#elif defined(DANGO_YURIS_ONLY)
    if (name == "EmbedYuris" || (!name.empty() && name.front() == 'E'))
    {
        return "EmbedYuris";
    }
    if (name == "YU-RIS" || name == "YU-RIS2" || name == "YU-RIS4" ||
        name == "YU-RIS5" || name == "YU-RIS6" || name == "yuris7" ||
        name == "yuris8" || name == "yuris9" || name == "YurisDialogue" || name == "YurisChoices")
    {
        return name;
    }
#elif defined(DANGO_BOLERO_ONLY)
    if (name == "EmbedBolero" || (!name.empty() && name.front() == 'E'))
    {
        return "EmbedBolero";
    }
    if (name == "ScenarioPlayerA" || name == "ScenarioPlayerW" ||
        name == "sutajioryokutyaW" || name == "Iyashikei")
    {
        return name;
    }
#elif defined(DANGO_WOLF_ONLY)
    if (name == "WolfRPG" || name == "WolfRPG_String_Copy" ||
        name == "WolfRPG4" || name == "Wolf5_1" || name == "Wolf5" ||
        name == "Wolf6" || name == "EmbedWolf" || name == "Wolf7" ||
        name == "GuruGuruSMF4_2" || name == "wolf8" || name == "wolf9" ||
        name == "wolf10" || name == "GuruGuruSMF4")
    {
        return name;
    }
#elif defined(DANGO_MIDAREAGEHA_ONLY)
    if (name == "EmbedMIDAREAGEHA" || (!name.empty() && name.front() == 'E'))
    {
        return "EmbedMIDAREAGEHA";
    }
    if (name == "MIDAREAGEHA")
    {
        return name;
    }
#elif defined(DANGO_TASKFORCE2_ONLY)
    if (name == "EmbedTaskforce" || name == "Taskforce2" || name == "strcpy")
    {
        return name;
    }
#elif defined(DANGO_QLIE_ONLY)
    if (name == "QLiE" || name == "QLiE2" || name == "QLiE3" ||
        name == "QLIE4" || name == "QLIE5" || name == "EmbedQLIE")
    {
        return name;
    }
#elif defined(DANGO_WILLPLUS_ONLY)
    if (name == "EmbedWillplusA" || name == "EmbedWillplus_other")
    {
        return name;
    }
#elif defined(DANGO_LCSE_ONLY)
    if (name == "LCSEDialogue" || name == "LCSEChoices")
    {
        return name;
    }
#elif defined(DANGO_CSYSTEM_ONLY)
    if (name == "CSystemDialogue" || name == "CSystemChoices" || name == "CSystemState") return name;
#elif defined(DANGO_NSCRIPTER_ONLY)
    if (name == "NScripterDialogue" || name == "NScripterChoices" || name == "NScripterRenderReturn" || name == "NScripterSprites") return name;
#elif defined(DANGO_DDWORKS_ONLY)
    if (name == "DDWorksDialogue")
    {
        return name;
    }
#elif defined(DANGO_VALKYRIA_ONLY)
    if (name == "ValkyriaMessage" || name == "ValkyriaChoice")
    {
        return name;
    }
#else
    if (name == "Live2" || name == "Live")
    {
        return name;
    }
#endif
    return {};
}

bool isSupportedHook(const char *hookName)
{
    return !canonicalSupportedHookName(hookName).empty();
}

std::string engineDisplayName()
{
#if defined(DANGO_PJADV_ONLY)
    return "2RM / PJADV";
#elif defined(DANGO_KIRIKIRI_ONLY)
    return "KiriKiri";
#elif defined(DANGO_RENPY_ONLY)
    return "RenPy";
#elif defined(DANGO_SIGLUS_ONLY)
    return "SiglusEngine";
#elif defined(DANGO_BGI_ONLY)
    return "BGI";
#elif defined(DANGO_CATSYSTEM2_ONLY)
    return "CatSystem2";
#elif defined(DANGO_AZSYSTEM_ONLY)
    return "AZSystem";
#elif defined(DANGO_UTAGE_ONLY)
    return "Utage";
#elif defined(DANGO_NEMK_NOVEL_ONLY)
    return "Unity Nemk Novel";
#elif defined(DANGO_UNITY_SCENARIO_ONLY)
    return "Unity Scenario";
#elif defined(DANGO_TEAMNIMBUS_ONLY)
    #if defined(DANGO_UNITY_MONO_ONLY)
    return "Unity Mono";
#else
    return "Unity TeamNimbus Dialogue";
#endif
#elif defined(DANGO_YARN_SPINNER_ONLY)
    return "Yarn Spinner";
#elif defined(DANGO_ADVENTURE_CREATOR_ONLY)
    return "Adventure Creator";
#elif defined(DANGO_RPGMAKER_UNITE_ONLY)
    return "RPG Maker Unite";
#elif defined(DANGO_UNITY_EVENTSCRIPT_ONLY)
    return "Unity EventScript";
#elif defined(DANGO_YURIS_ONLY)
    return "YU-RIS";
#elif defined(DANGO_BOLERO_ONLY)
    return "Bolero ADV System";
#elif defined(DANGO_WOLF_ONLY)
    return "WOLF RPG Editor";
#elif defined(DANGO_MIDAREAGEHA_ONLY)
    return "MIDAREAGEHA";
#elif defined(DANGO_TASKFORCE2_ONLY)
    return "Taskforce2";
#elif defined(DANGO_QLIE_ONLY)
    return "QLIE";
#elif defined(DANGO_WILLPLUS_ONLY)
    return "WillPlus";
#elif defined(DANGO_LCSE_ONLY)
    return "LC-ScriptEngine";
#elif defined(DANGO_CSYSTEM_ONLY)
    return "C,system";
#elif defined(DANGO_NSCRIPTER_ONLY)
    return "NScripter";
#elif defined(DANGO_DDWORKS_ONLY)
    return "DDWorks Game System";
#elif defined(DANGO_VALKYRIA_ONLY)
    return "VALKYRIA";
#else
    return "LiveMaker";
#endif
}

std::wstring engineFilePrefix()
{
#if defined(DANGO_PJADV_ONLY)
    return L"PJADV";
#elif defined(DANGO_KIRIKIRI_ONLY)
    return L"KiriKiri";
#elif defined(DANGO_RENPY_ONLY)
    return L"RenPy";
#elif defined(DANGO_SIGLUS_ONLY)
    return L"Siglus";
#elif defined(DANGO_BGI_ONLY)
    return L"BGI";
#elif defined(DANGO_CATSYSTEM2_ONLY)
    return L"CatSystem2";
#elif defined(DANGO_AZSYSTEM_ONLY)
    return L"AZSystem";
#elif defined(DANGO_UTAGE_ONLY)
    return L"Utage";
#elif defined(DANGO_NEMK_NOVEL_ONLY)
    return L"NemkNovel";
#elif defined(DANGO_UNITY_SCENARIO_ONLY)
    return L"UnityScenario";
#elif defined(DANGO_TEAMNIMBUS_ONLY)
    #if defined(DANGO_UNITY_MONO_ONLY)
    return L"UnityMono";
#else
    return L"TeamNimbus";
#endif
#elif defined(DANGO_YARN_SPINNER_ONLY)
    return L"YarnSpinner";
#elif defined(DANGO_ADVENTURE_CREATOR_ONLY)
    return L"AdventureCreator";
#elif defined(DANGO_RPGMAKER_UNITE_ONLY)
    return L"RPGMakerUnite";
#elif defined(DANGO_UNITY_EVENTSCRIPT_ONLY)
    return L"UnityEventScript";
#elif defined(DANGO_YURIS_ONLY)
    return L"Yuris";
#elif defined(DANGO_BOLERO_ONLY)
    return L"Bolero";
#elif defined(DANGO_WOLF_ONLY)
    return L"Wolf";
#elif defined(DANGO_MIDAREAGEHA_ONLY)
    return L"Midareageha";
#elif defined(DANGO_TASKFORCE2_ONLY)
    return L"Taskforce2";
#elif defined(DANGO_QLIE_ONLY)
    return L"QLIE";
#elif defined(DANGO_WILLPLUS_ONLY)
    return L"WillPlus";
#elif defined(DANGO_LCSE_ONLY)
    return L"LCSE";
#elif defined(DANGO_CSYSTEM_ONLY)
    return L"CSystem";
#elif defined(DANGO_NSCRIPTER_ONLY)
    return L"NScripter";
#elif defined(DANGO_DDWORKS_ONLY)
    return L"DDWorks";
#elif defined(DANGO_VALKYRIA_ONLY)
    return L"Valkyria";
#else
    return L"LiveMaker";
#endif
}

bool nativeEmbedSupported()
{
#if defined(DANGO_PJADV_ONLY) || defined(DANGO_BOLERO_ONLY) || defined(DANGO_YURIS_ONLY) || defined(DANGO_QLIE_ONLY) || defined(DANGO_WILLPLUS_ONLY) || defined(DANGO_VALKYRIA_ONLY)
    return true;
#elif defined(DANGO_RENPY_ONLY) || defined(DANGO_UTAGE_ONLY) || defined(DANGO_UNITY_EVENTSCRIPT_ONLY) || defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY) || \
    defined(DANGO_WOLF_ONLY) || defined(DANGO_TASKFORCE2_ONLY) || defined(DANGO_CATSYSTEM2_ONLY) || defined(DANGO_AZSYSTEM_ONLY) || defined(DANGO_MIDAREAGEHA_ONLY)
    return true;
#else
    return false;
#endif
}

bool nativeEmbedHookSupported(const std::string &hook)
{
#if defined(DANGO_PJADV_ONLY)
    return hook == "EmbedPensil" || hook == "EmbedPensilChoice";
#elif defined(DANGO_RENPY_ONLY)
    return hook == "RenPyEmbed";
#elif defined(DANGO_SIGLUS_ONLY)
    return hook == "EmbedSiglus";
#elif defined(DANGO_BGI_ONLY)
    return hook == "BGI";
#elif defined(DANGO_UTAGE_ONLY)
    return hook == "NovelTextComponentWrapper:SetNovelTextData" ||
           hook == "NovelTextComponentWrapper:SetText" ||
           hook == "NovelTextComponentWrapper:SetTextDirect";
#elif defined(DANGO_AZSYSTEM_ONLY)
    return hook == "EmbedAZSystem";
#elif defined(DANGO_YURIS_ONLY)
    return hook == "EmbedYuris";
#elif defined(DANGO_WOLF_ONLY)
    return hook == "EmbedWolf";
#elif defined(DANGO_TASKFORCE2_ONLY)
    return hook == "EmbedTaskforce";
#elif defined(DANGO_CATSYSTEM2_ONLY)
    return hook == "EmbedCS2";
#elif defined(DANGO_UNITY_EVENTSCRIPT_ONLY)
    return hook == "SaiminTextWindow:AddText";
#elif defined(DANGO_MIDAREAGEHA_ONLY)
    return hook == "EmbedMIDAREAGEHA";
#elif defined(DANGO_BOLERO_ONLY)
    return hook == "EmbedBolero";
#elif defined(DANGO_QLIE_ONLY)
    return hook == "EmbedQLIE";
#elif defined(DANGO_WILLPLUS_ONLY)
    return hook == "EmbedWillplusA" || hook == "EmbedWillplus_other";
#elif defined(DANGO_VALKYRIA_ONLY)
    return hook == "ValkyriaMessage" || hook == "ValkyriaChoice";
#else
    static_cast<void>(hook);
    return false;
#endif
}

bool nativeEmbedUsesFixedBuffer(const std::string &hook)
{
#if defined(DANGO_WOLF_ONLY)
    return hook == "EmbedWolf";
#elif defined(DANGO_WILLPLUS_ONLY)
    return hook == "EmbedWillplusA";
#elif defined(DANGO_VALKYRIA_ONLY)
    return hook == "ValkyriaMessage" || hook == "ValkyriaChoice";
#else
    static_cast<void>(hook);
    return false;
#endif
}

#if defined(DANGO_WOLF_ONLY)
constexpr std::size_t kWolfEmbedDefaultLineWidth = 76;
constexpr std::size_t kWolfEmbedMinimumLineWidth = 48;
constexpr std::size_t kWolfEmbedMaximumLineWidth = 96;

// wolfDisplayCharacterWidth 计算WOLF消息框中的近似显示宽度
std::size_t wolfDisplayCharacterWidth(const std::wstring &value, std::size_t position, std::size_t *characterLength)
{
    const wchar_t character = value[position];
    if (character >= 0xd800 && character <= 0xdbff && position + 1 < value.size() &&
        value[position + 1] >= 0xdc00 && value[position + 1] <= 0xdfff)
    {
        *characterLength = 2;
        return 2;
    }
    *characterLength = 1;
    if ((character >= 0x0300 && character <= 0x036f) ||
        (character >= 0x1ab0 && character <= 0x1aff) ||
        (character >= 0x1dc0 && character <= 0x1dff) ||
        (character >= 0x20d0 && character <= 0x20ff) ||
        (character >= 0xfe20 && character <= 0xfe2f))
    {
        return 0;
    }
    if (character <= 0x7f || (character >= 0xff61 && character <= 0xff9f))
    {
        return 1;
    }
    return 2;
}

// wolfControlSequenceLength 识别不应拆开的WOLF控制码和显示标签
std::size_t wolfControlSequenceLength(const std::wstring &value, std::size_t position)
{
    if (value[position] == L'\\')
    {
        std::size_t end = position + 1;
        while (end < value.size() &&
               ((value[end] >= L'a' && value[end] <= L'z') ||
                (value[end] >= L'A' && value[end] <= L'Z') || value[end] == L'_'))
        {
            ++end;
        }
        if (end < value.size() && value[end] == L'[')
        {
            int depth = 0;
            do
            {
                if (value[end] == L'[')
                {
                    ++depth;
                }
                else if (value[end] == L']')
                {
                    --depth;
                }
                ++end;
            } while (end < value.size() && depth > 0);
        }
        return end > position + 1 ? end - position : 1;
    }
    if (value[position] == L'<')
    {
        const auto end = value.find(L'>', position + 1);
        if (end != std::wstring::npos && end - position <= 64)
        {
            return end - position + 1;
        }
    }
    return 0;
}

// wolfLineStartsWithPunctuation 判断字符是否适合跟随在上一行末尾
bool wolfLineStartsWithPunctuation(wchar_t character)
{
    static constexpr wchar_t punctuation[] = L"、。，．！？!?：:；;）」』】》〉〕］｝”’…,.";
    return std::wcschr(punctuation, character) != nullptr;
}

// wolfLineEndsWithOpeningPunctuation 判断字符是否不适合停留在行尾
bool wolfLineEndsWithOpeningPunctuation(wchar_t character)
{
    static constexpr wchar_t punctuation[] = L"（「『【《〈〔［｛“‘";
    return std::wcschr(punctuation, character) != nullptr;
}

// measureWolfDisplayLine 计算单行文本的近似显示宽度
std::size_t measureWolfDisplayLine(const std::wstring &value, std::size_t begin, std::size_t end)
{
    std::size_t width = 0;
    for (std::size_t position = begin; position < end;)
    {
        const auto controlLength = wolfControlSequenceLength(value, position);
        if (controlLength > 0)
        {
            position += controlLength;
            continue;
        }
        std::size_t characterLength = 1;
        width += wolfDisplayCharacterWidth(value, position, &characterLength);
        position += characterLength;
    }
    return width;
}

// wolfEmbedLineWidth 根据原文已有分行估算当前消息框宽度
std::size_t wolfEmbedLineWidth(const std::wstring &original)
{
    std::size_t lineCount = 1;
    std::size_t maximumWidth = 0;
    std::size_t lineBegin = 0;
    for (std::size_t position = 0; position <= original.size(); ++position)
    {
        if (position < original.size() && original[position] != L'\r' && original[position] != L'\n')
        {
            continue;
        }
        const auto lineWidth = measureWolfDisplayLine(original, lineBegin, position);
        if (lineWidth > maximumWidth)
        {
            maximumWidth = lineWidth;
        }
        if (position < original.size())
        {
            if (original[position] == L'\r' && position + 1 < original.size() && original[position + 1] == L'\n')
            {
                ++position;
            }
            lineBegin = position + 1;
            ++lineCount;
        }
    }
    if (lineCount <= 1)
    {
        return kWolfEmbedDefaultLineWidth;
    }
    if (maximumWidth < kWolfEmbedMinimumLineWidth)
    {
        return kWolfEmbedMinimumLineWidth;
    }
    if (maximumWidth > kWolfEmbedMaximumLineWidth)
    {
        return kWolfEmbedMaximumLineWidth;
    }
    return maximumWidth;
}

// wrapWolfEmbedTranslation 在WOLF写回前补充分行，避免长译文越出消息框
std::wstring wrapWolfEmbedTranslation(const std::wstring &original, const std::wstring &translation)
{
    if (translation.empty())
    {
        return translation;
    }
    const auto maximumWidth = wolfEmbedLineWidth(original);
    std::wstring result;
    result.reserve(translation.size() + translation.size() / 16);
    std::size_t lineWidth = 0;
    for (std::size_t position = 0; position < translation.size();)
    {
        if (translation[position] == L'\r' || translation[position] == L'\n')
        {
            if (translation[position] == L'\r' && position + 1 < translation.size() && translation[position + 1] == L'\n')
            {
                ++position;
            }
            if (result.empty() || result.back() != L'\n')
            {
                result.push_back(L'\n');
            }
            lineWidth = 0;
            ++position;
            continue;
        }

        const auto controlLength = wolfControlSequenceLength(translation, position);
        if (controlLength > 0)
        {
            result.append(translation, position, controlLength);
            position += controlLength;
            continue;
        }

        std::size_t characterLength = 1;
        const auto characterWidth = wolfDisplayCharacterWidth(translation, position, &characterLength);
        const auto character = translation[position];
        const bool whitespace = character == L' ' || character == L'\t' || character == 0x3000;
        const bool exceedsLine = lineWidth > 0 && lineWidth + characterWidth > maximumWidth;
        const bool openingAtLineEnd = lineWidth > 0 && wolfLineEndsWithOpeningPunctuation(character) &&
                                      lineWidth + characterWidth >= maximumWidth;
        if ((exceedsLine && !wolfLineStartsWithPunctuation(character)) || openingAtLineEnd)
        {
            result.push_back(L'\n');
            lineWidth = 0;
            if (whitespace)
            {
                position += characterLength;
                continue;
            }
        }
        result.append(translation, position, characterLength);
        lineWidth += characterWidth;
        position += characterLength;
    }
    return result;
}
#endif

#if defined(DANGO_TASKFORCE2_ONLY)
// resetTaskforce2FastSkipState 清除Taskforce2快速跳过检测状态
void resetTaskforce2FastSkipState()
{
    g_taskforce2FastSkipGeneration.fetch_add(1);
    g_taskforce2LastEmbedFinishedAt.store(0);
    g_taskforce2LastFastSkipAt.store(0);
    g_taskforce2FastSkipping.store(false);
}

// startTaskforce2FastSkipMonitor 在快速文本停止后恢复正常注入状态
void startTaskforce2FastSkipMonitor(std::uint64_t generation)
{
    std::thread([generation]()
                {
                    while (g_embedEnabled.load() && g_taskforce2FastSkipping.load() &&
                           g_taskforce2FastSkipGeneration.load() == generation)
                    {
                        Sleep(100);
                        const auto lastSkipAt = g_taskforce2LastFastSkipAt.load();
                        const auto now = static_cast<std::uint64_t>(GetTickCount64());
                        if (lastSkipAt == 0 || now - lastSkipAt < kTaskforce2FastSkipResumeMs)
                        {
                            continue;
                        }
                        bool expected = true;
                        if (g_taskforce2FastSkipGeneration.load() == generation &&
                            g_taskforce2FastSkipping.compare_exchange_strong(expected, false))
                        {
                            emitEvent("embed_fast_skip_end");
                        }
                        return;
                    } })
        .detach();
}

// markTaskforce2EmbedFinished 记录最近一次解除游戏译文等待的时间
void markTaskforce2EmbedFinished()
{
    g_taskforce2LastEmbedFinishedAt.store(static_cast<std::uint64_t>(GetTickCount64()));
}

// shouldSkipTaskforce2EmbedRequest 判断当前请求是否属于连续快速翻页
bool shouldSkipTaskforce2EmbedRequest()
{
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (g_taskforce2FastSkipping.load())
    {
        const auto previous = g_taskforce2LastFastSkipAt.exchange(now);
        if (previous != 0 && now - previous <= kTaskforce2FastSkipResumeMs)
        {
            return true;
        }
        g_taskforce2FastSkipping.store(false);
        emitEvent("embed_fast_skip_end");
    }

    const auto lastFinishedAt = g_taskforce2LastEmbedFinishedAt.load();
    if (lastFinishedAt == 0 || now - lastFinishedAt > kTaskforce2FastSkipFollowupMs)
    {
        return false;
    }
    g_taskforce2FastSkipping.store(true);
    g_taskforce2LastFastSkipAt.store(now);
    const auto generation = g_taskforce2FastSkipGeneration.fetch_add(1) + 1;
    emitEvent("embed_fast_skip");
    startTaskforce2FastSkipMonitor(generation);
    return true;
}
#endif

#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
// resetBGIFastSkipState 清除BGI快速跳过检测状态
void resetBGIFastSkipState()
{
    g_bgiFastSkipGeneration.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_bgiFastSkipMutex);
        g_bgiEmbedFinishedByThread.clear();
#if defined(DANGO_SIGLUS_ONLY)
        g_siglusConsecutivePages.clear();
#endif
    }
    g_bgiLastFastSkipAt.store(0);
    g_bgiFastSkipping.store(false);
}

// startBGIFastSkipMonitor 在快速文本停止后恢复正常注入状态
void startBGIFastSkipMonitor(std::uint64_t generation)
{
    std::thread([generation]()
                {
                    while (g_embedEnabled.load() && g_bgiFastSkipping.load() &&
                           g_bgiFastSkipGeneration.load() == generation)
                    {
                        Sleep(100);
                        const auto lastSkipAt = g_bgiLastFastSkipAt.load();
                        const auto now = static_cast<std::uint64_t>(GetTickCount64());
                        if (lastSkipAt == 0 || now - lastSkipAt < kBGIFastSkipResumeMs)
                        {
                            continue;
                        }
                        bool expected = true;
                        if (g_bgiFastSkipGeneration.load() == generation &&
                            g_bgiFastSkipping.compare_exchange_strong(expected, false))
                        {
                            emitEvent("embed_fast_skip_end");
                        }
                        return;
                    } })
        .detach();
}

// markBGIEmbedFinished 记录最近一次解除游戏译文等待的时间
void markBGIEmbedFinished(ThreadParam thread, const std::wstring &original)
{
    std::lock_guard<std::mutex> lock(g_bgiFastSkipMutex);
    g_bgiEmbedFinishedByThread[thread] = BGIEmbedFinishedState{
        static_cast<std::uint64_t>(GetTickCount64()),
        original};
}

// shouldSkipBGIEmbedRequest 判断当前请求是否属于连续快速翻页
bool shouldSkipBGIEmbedRequest(ThreadParam thread, const std::wstring &original)
{
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (g_bgiFastSkipping.load())
    {
        const auto previous = g_bgiLastFastSkipAt.exchange(now);
        if (previous != 0 && now - previous <= kBGIFastSkipResumeMs)
        {
            return true;
        }
        g_bgiFastSkipping.store(false);
        emitEvent("embed_fast_skip_end");
    }

    BGIEmbedFinishedState lastFinished{};
    {
        std::lock_guard<std::mutex> lock(g_bgiFastSkipMutex);
        const auto found = g_bgiEmbedFinishedByThread.find(thread);
        if (found == g_bgiEmbedFinishedByThread.end())
        {
            return false;
        }
        lastFinished = found->second;
    }
    if (lastFinished.finishedAt == 0 || lastFinished.original == original ||
        now - lastFinished.finishedAt > kBGIFastSkipFollowupMs)
    {
#if defined(DANGO_SIGLUS_ONLY)
        std::lock_guard<std::mutex> lock(g_bgiFastSkipMutex);
        g_siglusConsecutivePages[thread] = 0;
#endif
        return false;
    }
#if defined(DANGO_SIGLUS_ONLY)
    // 同页逐段增长不算翻页；至少连续三个独立页面才能进入快速跳过
    if (original.compare(0, lastFinished.original.size(), lastFinished.original) == 0 ||
        lastFinished.original.compare(0, original.size(), original) == 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_bgiFastSkipMutex);
        if (++g_siglusConsecutivePages[thread] < 3) return false;
    }
#endif
    g_bgiFastSkipping.store(true);
    g_bgiLastFastSkipAt.store(now);
    const auto generation = g_bgiFastSkipGeneration.fetch_add(1) + 1;
    emitEvent("embed_fast_skip");
    startBGIFastSkipMonitor(generation);
    return true;
}
#endif

#if defined(DANGO_QLIE_ONLY)
// resetQLIEFastSkipState 清除QLiE快速跳过检测状态
void resetQLIEFastSkipState()
{
    g_qlieFastSkipGeneration.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_qlieFastSkipMutex);
        g_qlieEmbedFinishedByThread.clear();
    }
    g_qlieLastFastSkipAt.store(0);
    g_qlieFastSkipping.store(false);
}

// startQLIEFastSkipMonitor 在快速文本停止后恢复正常注入状态
void startQLIEFastSkipMonitor(std::uint64_t generation)
{
    std::thread([generation]()
                {
                    while (g_embedEnabled.load() && g_qlieFastSkipping.load() &&
                           g_qlieFastSkipGeneration.load() == generation)
                    {
                        Sleep(100);
                        const auto lastSkipAt = g_qlieLastFastSkipAt.load();
                        const auto now = static_cast<std::uint64_t>(GetTickCount64());
                        if (lastSkipAt == 0 || now - lastSkipAt < kQLIEFastSkipResumeMs)
                        {
                            continue;
                        }
                        bool expected = true;
                        if (g_qlieFastSkipGeneration.load() == generation &&
                            g_qlieFastSkipping.compare_exchange_strong(expected, false))
                        {
                            emitEvent("embed_fast_skip_end");
                        }
                        return;
                    } })
        .detach();
}

// markQLIEEmbedFinished 记录最近一次解除游戏译文等待的时间和正文
void markQLIEEmbedFinished(ThreadParam thread, const std::wstring &original)
{
    std::lock_guard<std::mutex> lock(g_qlieFastSkipMutex);
    auto &state = g_qlieEmbedFinishedByThread[thread];
    state.finishedAt = static_cast<std::uint64_t>(GetTickCount64());
    state.original = original;
}

// shouldSkipQLIEEmbedRequest 以连续不同页面的请求密度识别快速跳过
bool shouldSkipQLIEEmbedRequest(ThreadParam thread, const std::wstring &original)
{
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (g_qlieFastSkipping.load())
    {
        const auto previous = g_qlieLastFastSkipAt.exchange(now);
        if (previous != 0 && now - previous <= kQLIEFastSkipResumeMs)
        {
            return true;
        }
        g_qlieFastSkipping.store(false);
        emitEvent("embed_fast_skip_end");
    }

    unsigned rapidFollowups = 0;
    {
        std::lock_guard<std::mutex> lock(g_qlieFastSkipMutex);
        const auto found = g_qlieEmbedFinishedByThread.find(thread);
        if (found == g_qlieEmbedFinishedByThread.end())
        {
            return false;
        }
        auto &state = found->second;
        if (state.finishedAt == 0 || state.original == original ||
            now - state.finishedAt > kQLIEFastSkipFollowupMs)
        {
            state.rapidFollowups = 0;
            return false;
        }
        rapidFollowups = ++state.rapidFollowups;
    }
    // QLiE的一页可能连续请求角色名和正文；下一页仍高速到来时才判定为跳过。
    if (rapidFollowups < kQLIEFastSkipRequiredFollowups)
    {
        return false;
    }
    g_qlieFastSkipping.store(true);
    g_qlieLastFastSkipAt.store(now);
    const auto generation = g_qlieFastSkipGeneration.fetch_add(1) + 1;
    emitEvent("embed_fast_skip");
    startQLIEFastSkipMonitor(generation);
    return true;
}
#endif

#if defined(DANGO_WILLPLUS_ONLY)
// resetWillPlusFastSkipState 清除WillPlus快速跳过检测状态
void resetWillPlusFastSkipState()
{
    g_willPlusFastSkipGeneration.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_willPlusFastSkipMutex);
        g_willPlusEmbedFinishedByThread.clear();
    }
    g_willPlusLastFastSkipAt.store(0);
    g_willPlusFastSkipping.store(false);
}

// clearWillPlusFastSkipThread 清除已经销毁的文本线程，避免上下文复用造成误判
void clearWillPlusFastSkipThread(ThreadParam thread)
{
    std::lock_guard<std::mutex> lock(g_willPlusFastSkipMutex);
    g_willPlusEmbedFinishedByThread.erase(thread);
}

// startWillPlusFastSkipMonitor 在高速正文静默后恢复正常注入
void startWillPlusFastSkipMonitor(std::uint64_t generation)
{
    std::thread([generation]()
                {
                    while (g_embedEnabled.load() && g_willPlusFastSkipping.load() &&
                           g_willPlusFastSkipGeneration.load() == generation)
                    {
                        Sleep(100);
                        const auto lastSkipAt = g_willPlusLastFastSkipAt.load();
                        const auto now = static_cast<std::uint64_t>(GetTickCount64());
                        if (lastSkipAt == 0 || now - lastSkipAt < kWillPlusFastSkipResumeMs)
                        {
                            continue;
                        }
                        bool expected = true;
                        if (g_willPlusFastSkipGeneration.load() == generation &&
                            g_willPlusFastSkipping.compare_exchange_strong(expected, false))
                        {
                            emitEvent("embed_fast_skip_end");
                        }
                        return;
                    } })
        .detach();
}

// markWillPlusEmbedFinished 必须在解除游戏线程等待前记录，保证紧随其后的页面能够命中快跳检测
void markWillPlusEmbedFinished(ThreadParam thread, const std::wstring &original)
{
    std::lock_guard<std::mutex> lock(g_willPlusFastSkipMutex);
    auto &state = g_willPlusEmbedFinishedByThread[thread];
    state.finishedAt = static_cast<std::uint64_t>(GetTickCount64());
    state.original = original;
}

// shouldSkipWillPlusEmbedRequest 根据同一正文线程连续出现的不同完整页面识别快速跳过
bool shouldSkipWillPlusEmbedRequest(ThreadParam thread, const std::wstring &original)
{
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (g_willPlusFastSkipping.load())
    {
        const auto previous = g_willPlusLastFastSkipAt.exchange(now);
        if (previous != 0 && now - previous <= kWillPlusFastSkipResumeMs)
        {
            return true;
        }
        g_willPlusFastSkipping.store(false);
        emitEvent("embed_fast_skip_end");
    }

    WillPlusEmbedFinishedState lastFinished{};
    {
        std::lock_guard<std::mutex> lock(g_willPlusFastSkipMutex);
        const auto found = g_willPlusEmbedFinishedByThread.find(thread);
        if (found == g_willPlusEmbedFinishedByThread.end())
        {
            return false;
        }
        lastFinished = found->second;
    }
    if (lastFinished.finishedAt == 0 || lastFinished.original == original ||
        now - lastFinished.finishedAt > kWillPlusFastSkipFollowupMs)
    {
        return false;
    }

    g_willPlusFastSkipping.store(true);
    g_willPlusLastFastSkipAt.store(now);
    const auto generation = g_willPlusFastSkipGeneration.fetch_add(1) + 1;
    emitEvent("embed_fast_skip");
    startWillPlusFastSkipMonitor(generation);
    return true;
}
#endif

#if defined(DANGO_VALKYRIA_ONLY)
void resetValkyriaFastSkipState()
{
    g_valkyriaFastSkipGeneration.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_valkyriaFastSkipMutex);
        g_valkyriaEmbedFinishedByThread.clear();
    }
    g_valkyriaLastFastSkipAt.store(0);
    g_valkyriaFastSkipping.store(false);
}

void clearValkyriaFastSkipThread(ThreadParam thread)
{
    std::lock_guard<std::mutex> lock(g_valkyriaFastSkipMutex);
    g_valkyriaEmbedFinishedByThread.erase(thread);
}

void startValkyriaFastSkipMonitor(std::uint64_t generation)
{
    std::thread([generation]()
                {
                    while (g_embedEnabled.load() && g_valkyriaFastSkipping.load() &&
                           g_valkyriaFastSkipGeneration.load() == generation)
                    {
                        Sleep(100);
                        const auto lastSkipAt = g_valkyriaLastFastSkipAt.load();
                        const auto now = static_cast<std::uint64_t>(GetTickCount64());
                        if (lastSkipAt == 0 || now - lastSkipAt < kValkyriaFastSkipResumeMs)
                        {
                            continue;
                        }
                        bool expected = true;
                        if (g_valkyriaFastSkipGeneration.load() == generation &&
                            g_valkyriaFastSkipping.compare_exchange_strong(expected, false))
                        {
                            emitEvent("embed_fast_skip_end");
                        }
                        return;
                    } })
        .detach();
}

void markValkyriaEmbedFinished(ThreadParam thread, const std::wstring &original)
{
    std::lock_guard<std::mutex> lock(g_valkyriaFastSkipMutex);
    auto &state = g_valkyriaEmbedFinishedByThread[thread];
    state.finishedAt = static_cast<std::uint64_t>(GetTickCount64());
    state.original = original;
}

bool shouldSkipValkyriaEmbedRequest(ThreadParam thread, const std::wstring &original)
{
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    // The page-rate heuristic cannot see the first page after a scene pause.
    // Holding Ctrl is an explicit skip request, including when a choice is
    // shown; never wait for or apply a translated display string during it.
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        g_valkyriaLastFastSkipAt.store(now);
        if (!g_valkyriaFastSkipping.exchange(true))
        {
            const auto generation = g_valkyriaFastSkipGeneration.fetch_add(1) + 1;
            emitEvent("embed_fast_skip");
            startValkyriaFastSkipMonitor(generation);
        }
        return true;
    }
    if (g_valkyriaFastSkipping.load())
    {
        const auto previous = g_valkyriaLastFastSkipAt.exchange(now);
        if (previous != 0 && now - previous <= kValkyriaFastSkipResumeMs)
        {
            return true;
        }
        g_valkyriaFastSkipping.store(false);
        emitEvent("embed_fast_skip_end");
    }
    ValkyriaEmbedFinishedState lastFinished{};
    {
        std::lock_guard<std::mutex> lock(g_valkyriaFastSkipMutex);
        const auto found = g_valkyriaEmbedFinishedByThread.find(thread);
        if (found == g_valkyriaEmbedFinishedByThread.end())
        {
            return false;
        }
        lastFinished = found->second;
    }
    if (lastFinished.finishedAt == 0 || lastFinished.original == original ||
        now - lastFinished.finishedAt > kValkyriaFastSkipFollowupMs)
    {
        return false;
    }
    g_valkyriaFastSkipping.store(true);
    g_valkyriaLastFastSkipAt.store(now);
    const auto generation = g_valkyriaFastSkipGeneration.fetch_add(1) + 1;
    emitEvent("embed_fast_skip");
    startValkyriaFastSkipMonitor(generation);
    return true;
}
#endif

#if defined(DANGO_PJADV_ONLY)
void finishEmbedRequest(std::uint64_t requestId, const std::wstring &translation, bool reportFailure);

// resetPJADVFastSkipState 清除注入会话中的快进和正文线程状态
void resetPJADVFastSkipState()
{
    g_pjadvFastSkipGeneration.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_pjadvFastSkipMutex);
    g_pjadvEmbedFinishedByThread.clear();
    g_pjadvLastFastSkipAt.store(0);
    g_pjadvFastSkipping.store(false);
}

// enterPJADVFastSkip 刷新快进时间并只在进入时通知实时翻译侧
void enterPJADVFastSkip(std::uint64_t now)
{
    std::lock_guard<std::mutex> lock(g_pjadvFastSkipMutex);
    g_pjadvLastFastSkipAt.store(now);
    if (!g_pjadvFastSkipping.exchange(true)) emitEvent("embed_fast_skip");
}

// markPJADVEmbedFinished 在解除等待前记录正文完成时间
void markPJADVEmbedFinished(ThreadParam thread, const std::wstring &original)
{
    std::lock_guard<std::mutex> lock(g_pjadvFastSkipMutex);
    g_pjadvEmbedFinishedByThread[thread] = {static_cast<std::uint64_t>(GetTickCount64()), original};
}

// shouldSkipPJADVEmbedRequest 根据Ctrl和同一正文线程的连续翻页识别快进
bool shouldSkipPJADVEmbedRequest(ThreadParam thread, const std::wstring &original, const std::string &hook)
{
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || g_pjadvFastSkipping.load())
    {
        enterPJADVFastSkip(now);
        return true;
    }
    if (hook != "EmbedPensil" || g_pjadvSpeakerContexts.count(thread.ctx)) return false;
    PJADVEmbedFinishedState lastFinished{};
    {
        std::lock_guard<std::mutex> lock(g_pjadvFastSkipMutex);
        const auto found = g_pjadvEmbedFinishedByThread.find(thread);
        if (found == g_pjadvEmbedFinishedByThread.end()) return false;
        lastFinished = found->second;
    }
    if (!lastFinished.finishedAt || lastFinished.original == original ||
        now - lastFinished.finishedAt > kPJADVFastSkipFollowupMs) return false;
    enterPJADVFastSkip(now);
    return true;
}

// startPJADVFastSkipMonitor 即使当前正文正在等待译文也能响应Ctrl并解除阻塞
void startPJADVFastSkipMonitor(std::uint64_t generation)
{
    std::thread([generation]()
                {
                    while (g_embedEnabled.load() && g_pjadvFastSkipGeneration.load() == generation)
                    {
                        Sleep(20);
                        if (!g_embedEnabled.load() || g_pjadvFastSkipGeneration.load() != generation) return;
                        const auto now = static_cast<std::uint64_t>(GetTickCount64());
                        const bool controlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (controlPressed) enterPJADVFastSkip(now);
                        if (!g_pjadvFastSkipping.load()) continue;
                        std::vector<std::pair<std::uint64_t, std::string>> requests;
                        {
                            std::lock_guard<std::mutex> lock(g_embedMutex);
                            for (const auto &[requestId, request] : g_embedRequests)
                                requests.emplace_back(requestId, request.hook);
                        }
                        for (const auto &[requestId, hook] : requests)
                        {
                            emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
                            finishEmbedRequest(requestId, L"", false);
                        }
                        if (!controlPressed)
                        {
                            std::lock_guard<std::mutex> lock(g_pjadvFastSkipMutex);
                            const auto resumeNow = static_cast<std::uint64_t>(GetTickCount64());
                            const auto lastSkipAt = g_pjadvLastFastSkipAt.load();
                            if (resumeNow >= lastSkipAt && resumeNow - lastSkipAt >= kPJADVFastSkipResumeMs &&
                                g_pjadvFastSkipGeneration.load() == generation)
                            {
                                bool expected = true;
                                if (g_pjadvFastSkipping.compare_exchange_strong(expected, false)) emitEvent("embed_fast_skip_end");
                            }
                        }
                    } })
        .detach();
}
#endif

void setNativeEmbedEnabled(bool enabled)
{
#if defined(DANGO_NSCRIPTER_ONLY)
    g_nscripterBackfillEnabled.store(false);
    enabled=enabled&&g_nscripterChannel&&g_embedReadyReported.load();
    if(g_nscripterChannel)g_nscripterControl.enable(enabled);
#endif
#if defined(DANGO_CSYSTEM_ONLY)
    g_csystemBackfillEnabled.store(false);
    enabled = enabled && g_csystemChannel && g_embedReadyReported.load();
#endif
    g_embedEnabled.store(enabled);
#if defined(DANGO_CSYSTEM_ONLY)
    if (g_csystemChannel) g_csystemControl.enable(enabled && g_embedReadyReported.load());
#endif
#if defined(DANGO_PJADV_ONLY)
    resetPJADVFastSkipState();
    if (enabled) startPJADVFastSkipMonitor(g_pjadvFastSkipGeneration.load());
#endif
#if defined(DANGO_TASKFORCE2_ONLY)
    resetTaskforce2FastSkipState();
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
    resetBGIFastSkipState();
#endif
#if defined(DANGO_QLIE_ONLY)
    resetQLIEFastSkipState();
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    if (!enabled)
    {
        g_willPlusFullTextBackfillEnabled.store(false);
    }
    resetWillPlusFastSkipState();
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    if (!enabled)
    {
        g_valkyriaFullTextBackfillEnabled.store(false);
    }
    resetValkyriaFastSkipState();
#endif
    std::vector<EmbedThread> threads;
    {
        std::lock_guard<std::mutex> lock(g_embedMutex);
        for (const auto &[thread, state] : g_embedThreads)
        {
            static_cast<void>(thread);
            threads.push_back(state);
        }
    }
    if (g_hookUseEmbed)
    {
        // 上游Hook_UseEmbed(true)不会判断同一ThreadParam是否已经登记，再次调用会
        // 重复占用只有32项的共享槽。切换模式时先撤销本组件登记的全部线程，再按
        // 当前策略重新选择，避免多次启停后静默耗尽写入槽。
        for (const auto &state : threads)
        {
            g_hookUseEmbed(state.thread, false);
        }
        for (const auto &state : threads)
        {
            bool threadEnabled = enabled;
#if defined(DANGO_WILLPLUS_ONLY)
            threadEnabled = enabled && (state.hook == "EmbedWillplusA" ||
                                        (g_willPlusFullTextBackfillEnabled.load() && state.hook == "EmbedWillplus_other"));
#endif
#if defined(DANGO_VALKYRIA_ONLY)
            threadEnabled = enabled && (state.hook == "ValkyriaMessage" ||
                                        (g_valkyriaFullTextBackfillEnabled.load() && state.hook == "ValkyriaChoice"));
#endif
            if (threadEnabled)
            {
                g_hookUseEmbed(state.thread, true);
            }
        }
    }
}

void finishEmbedRequest(std::uint64_t requestId, const std::wstring &translation, bool reportFailure)
{
    EmbedRequest request{};
    {
        std::lock_guard<std::mutex> lock(g_embedMutex);
        const auto found = g_embedRequests.find(requestId);
        if (found == g_embedRequests.end())
        {
            return;
        }
        request = found->second;
        g_embedRequests.erase(found);
    }
#if defined(DANGO_CSYSTEM_ONLY)
    // Acknowledge actual consumption only at the renderer's return hook.
    if (!g_csystemChannel || !g_csystemControl.reply(requestId, translation))
        emitEvent("embed_skipped", {{"message", "当前文本已失效，保留游戏原文"}}, {{"requestId", requestId}});
    return;
#endif
#if defined(DANGO_NSCRIPTER_ONLY)
    if(!g_nscripterChannel||!g_nscripterControl.reply(requestId,translation))
        emitEvent("embed_skipped",{{"message","当前文本已失效，保留游戏原文"}},{{"requestId",requestId}});
    return;
#endif
    if (!g_hookEmbedResult)
    {
        if (reportFailure)
        {
            emitEvent("embed_failed", {{"message", "Hook桥接组件不支持译文回写"}}, {{"requestId", requestId}});
        }
        return;
    }
#if defined(DANGO_VALKYRIA_ONLY)
    // A reply can arrive after Ctrl was pressed. Do not let an in-flight
    // translation write into a page that the engine has already skipped.
    if ((request.hook == "ValkyriaMessage" || request.hook == "ValkyriaChoice") &&
        ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || g_valkyriaFastSkipping.load()))
    {
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        if (reportFailure)
            emitEvent("embed_skipped", {{"message", "快速跳过期间保留游戏原文"}}, {{"requestId", requestId}});
        return;
    }
#endif
    std::wstring appliedTranslation = translation;
#if defined(DANGO_PJADV_ONLY)
    std::wstring formatted;
    std::size_t requiredBytes = 0;
    bool safeText = true;
    for (const wchar_t character : appliedTranslation)
    {
        if (!character || character == L'\\' || (character < 0x20 && character != L'\n' && character != L'\r') ||
            (character >= 0xd800 && character <= 0xdfff) || (character >= 0xe000 && character <= 0xf8ff))
        {
            safeText = false;
            break;
        }
        if (character == L'\r') continue;
        if (character == L'\n') formatted += L"\\n";
        else formatted.push_back(character);
        requiredBytes += character <= 0x7f && character != L'\n' ? 1 : 2;
        if (requiredBytes > 200)
        {
            safeText = false;
            break;
        }
    }
    if (!safeText)
    {
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        if (reportFailure) emitEvent("embed_skipped", {{"message", "当前对话无法安全注入，已保留游戏原文"}}, {{"requestId", requestId}});
        return;
    }
    appliedTranslation = std::move(formatted);
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    // 该版本正文写回目标是相邻的1024字节ANSI缓冲区。保留一部分空间给
    // 引擎原有的首尾控制码，并拒绝动态Shift-JIS无法安全表达的控制字符。
    constexpr std::size_t kWillPlusSafeTextBytes = 896;
    std::size_t requiredBytes = 1;
    bool safeText = true;
    for (std::size_t index = 0; index < appliedTranslation.size(); ++index)
    {
        const wchar_t character = appliedTranslation[index];
        if (!character || (character < 0x20 && character != L'\n' && character != L'\r' && character != L'\t') ||
            (character >= 0xd800 && character <= 0xdfff))
        {
            safeText = false;
            break;
        }
        if (character == L'\n' || character == L'\r')
        {
            requiredBytes += 2;
        }
        else
        {
            requiredBytes += character <= 0x7f ? 1 : 2;
        }
        if (requiredBytes > kWillPlusSafeTextBytes)
        {
            break;
        }
    }
    if (!safeText)
    {
        if (request.hook == "EmbedWillplusA")
        {
            markWillPlusEmbedFinished(request.thread, request.original);
        }
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        if (reportFailure)
        {
            emitEvent("embed_skipped", {{"message", "当前对话无法安全注入，已保留游戏原文"}}, {{"requestId", requestId}});
        }
        return;
    }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    // The pre-layout temporary text can be only 256 bytes. Reserve the
    // optional /s command and reject engine control syntax.
    std::size_t requiredBytes = 3;
    bool safeText = true;
    for (const wchar_t character : appliedTranslation)
    {
        if (!character || (character < 0x20 && character != L'\n') ||
            (character >= 0xd800 && character <= 0xdfff) || character == L'/')
        {
            safeText = false;
            break;
        }
        requiredBytes += character == L'\n' ? 2 : (character <= 0x7f ? 1 : 2);
        if (requiredBytes > 0xff)
        {
            break;
        }
    }
    if (!safeText)
    {
        if (request.hook == "ValkyriaMessage")
            markValkyriaEmbedFinished(request.thread, request.original);
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        if (reportFailure)
        {
            emitEvent("embed_skipped", {{"message", "当前对话无法安全注入，已保留游戏原文"}}, {{"requestId", requestId}});
        }
        return;
    }
#endif
#if defined(DANGO_SIGLUS_ONLY)
    bool safeSiglusText = appliedTranslation.size() <= 8192 && request.original.find(L'#') == std::wstring::npos &&
                         appliedTranslation.find(L'#') == std::wstring::npos;
    for (wchar_t character : appliedTranslation)
        if (!character || (character < 0x20 && character != L'\n' && character != L'\t')) safeSiglusText = false;
    if (!safeSiglusText)
    {
        markBGIEmbedFinished(request.thread, request.original);
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        if (reportFailure) emitEvent("embed_skipped", {{"message", "当前文本暂不支持注入，已保留原文"}}, {{"requestId", requestId}});
        return;
    }
#endif
#if defined(DANGO_WOLF_ONLY)
    appliedTranslation = wrapWolfEmbedTranslation(request.original, appliedTranslation);
#endif
    bool fixedBufferTooSmall = false;
    if (!appliedTranslation.empty() && nativeEmbedUsesFixedBuffer(request.hook))
    {
#if defined(DANGO_WOLF_ONLY)
        // WOLF场景文本在线程split中携带真实字节容量，动态Shift-JIS中非ASCII字符最多占两个字节
        std::size_t requiredBytes = 1;
        for (const wchar_t character : appliedTranslation)
        {
            requiredBytes += character <= 0x7f ? 1 : 2;
        }
        const std::size_t bufferCapacity = static_cast<std::size_t>(request.thread.ctx2);
        fixedBufferTooSmall = bufferCapacity == 0 || requiredBytes > bufferCapacity;
#elif defined(DANGO_WILLPLUS_ONLY)
        fixedBufferTooSmall = requiredBytes > kWillPlusSafeTextBytes;
#elif defined(DANGO_VALKYRIA_ONLY)
        fixedBufferTooSmall = requiredBytes > 0xff;
#else
        fixedBufferTooSmall = translation.size() > request.original.size();
#endif
    }
    if (fixedBufferTooSmall)
    {
#if defined(DANGO_TASKFORCE2_ONLY)
        markTaskforce2EmbedFinished();
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
        markBGIEmbedFinished(request.thread, request.original);
#endif
#if defined(DANGO_WILLPLUS_ONLY)
        if (request.hook == "EmbedWillplusA")
        {
            markWillPlusEmbedFinished(request.thread, request.original);
        }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
        if (request.hook == "ValkyriaMessage")
            markValkyriaEmbedFinished(request.thread, request.original);
#endif
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        if (reportFailure)
        {
            emitEvent("embed_skipped", {{"message", "译文超过当前游戏文本缓冲区可安全覆盖的长度，已保留游戏原文"}}, {{"requestId", requestId}});
        }
        return;
    }
#if defined(DANGO_TASKFORCE2_ONLY)
    markTaskforce2EmbedFinished();
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
    markBGIEmbedFinished(request.thread, request.original);
#endif
#if defined(DANGO_QLIE_ONLY)
    markQLIEEmbedFinished(request.thread, request.original);
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    if (request.hook == "EmbedWillplusA")
    {
        markWillPlusEmbedFinished(request.thread, request.original);
    }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    if (request.hook == "ValkyriaMessage")
        markValkyriaEmbedFinished(request.thread, request.original);
#endif
#if defined(DANGO_PJADV_ONLY)
    markPJADVEmbedFinished(request.thread, request.original);
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || g_pjadvFastSkipping.load())
    {
        enterPJADVFastSkip(static_cast<std::uint64_t>(GetTickCount64()));
        g_hookEmbedResult(request.thread, request.original.c_str(), L"");
        emitEvent("embed_fast_skip_request", {{"hook", request.hook}}, {{"requestId", requestId}});
        return;
    }
#endif
    g_hookEmbedResult(request.thread, request.original.c_str(), appliedTranslation.c_str());
    if (appliedTranslation.empty())
    {
        if (reportFailure)
        {
            emitEvent("embed_failed", {{"message", "当前对话未取得有效译文"}}, {{"requestId", requestId}});
        }
        return;
    }
    emitEvent("embed_applied", {}, {{"requestId", requestId}});
}

void cancelEmbedRequests(bool reportFailure)
{
    std::vector<std::uint64_t> requestIds;
    {
        std::lock_guard<std::mutex> lock(g_embedMutex);
        for (const auto &[requestId, request] : g_embedRequests)
        {
            static_cast<void>(request);
            requestIds.push_back(requestId);
        }
    }
    for (const auto requestId : requestIds)
    {
        finishEmbedRequest(requestId, L"", reportFailure);
    }
}

void reportProcessExit()
{
    if (!g_processExitReported.exchange(true))
    {
        emitEvent("process_exit");
    }
}

void configureNativeEmbed(DWORD processId)
{
#if defined(DANGO_NSCRIPTER_ONLY)
    // 专用绘字通道不使用上游动态编码、全局字体Hook或通用注入入口
    return;
#endif
    if (!nativeEmbedSupported() || !g_hookSettingsEx || !g_hookUseEmbed || !g_hookEmbedResult)
    {
        return;
    }
    std::wstring fontFamily;
    {
        std::lock_guard<std::mutex> lock(g_embedFontMutex);
        fontFamily = g_embedFontFamily;
    }
#if defined(DANGO_RENPY_ONLY) || defined(DANGO_UTAGE_ONLY) || defined(DANGO_UNITY_EVENTSCRIPT_ONLY) || defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
    g_hookSettingsEx(processId, kEmbedWaitTimeoutMs, DEFAULT_CHARSET, false, fontFamily.data(), 0, false, false, false, 1.0f, false);
#else
    // 原生引擎仍按Shift-JIS处理普通字符，动态汉字由GDI Hook转换为Unicode绘制
    g_hookSettingsEx(processId, kEmbedWaitTimeoutMs, SHIFTJIS_CHARSET, true, fontFamily.data(), 0, false, false, false, 1.0f, false);
#endif
}

void updateNativeEmbedFont(const std::wstring &fontFamily)
{
#if defined(DANGO_NSCRIPTER_ONLY)
    if(g_nscripterChannel)g_nscripterControl.font(fontFamily);
#endif
    {
        std::lock_guard<std::mutex> lock(g_embedFontMutex);
        g_embedFontFamily = fontFamily.substr(0, 99);
    }
    configureNativeEmbed(g_targetProcessId);
}

#if defined(DANGO_YURIS_ONLY)
std::mutex g_yurisHookMutex;
std::set<std::uint64_t> g_yurisEmbedAddresses;
#endif

void reportNativeEmbedReady(const std::string &hook)
{
#if defined(DANGO_YURIS_ONLY)
    // 任一已确认且真实安装成功的显示路径即可就绪，其他路径保留原文
    std::lock_guard<std::mutex> lock(g_yurisHookMutex);
    if (g_yurisEmbedAddresses.empty())
    {
        return;
    }
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    // 辅助绘制入口只能补充角色名和选项，不能冒充完整正文入口已经就绪。
    if (hook != "EmbedWillplusA")
    {
        return;
    }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    if (hook != "ValkyriaMessage")
    {
        return;
    }
#endif
    if (!nativeEmbedHookSupported(hook) || g_embedReadyReported.exchange(true))
    {
        return;
    }
    emitEvent("embed_ready", {{"hook", hook}});
}

void __cdecl onProcessConnect(DWORD processId)
{
    if (processId == g_targetProcessId)
    {
#if defined(DANGO_YARN_SPINNER_ONLY)
        g_yarnConnected.store(true);
        yarnDiagnostic("stage=pipe_connected");
#endif
        configureNativeEmbed(processId);
        emitEvent("status", {{"status", "connected"}});
    }
}

void __cdecl onProcessDisconnect(DWORD processId)
{
    if (processId != g_targetProcessId)
    {
        return;
    }
#if defined(DANGO_YARN_SPINNER_ONLY)
    g_yarnConnected.store(false);
    yarnDiagnostic("stage=pipe_disconnected");
#endif
    reportProcessExit();
    if (g_shutdownEvent)
    {
        SetEvent(g_shutdownEvent);
    }
}

void __cdecl onThreadCreate(const wchar_t *, const char *hookName, ThreadParam thread, bool isEmbedable)
{
    const std::string canonicalHookName = canonicalSupportedHookName(hookName);
#if defined(DANGO_YARN_SPINNER_ONLY)
    if (thread.processId == g_targetProcessId && !g_yarnAnyThread.exchange(true))
        yarnDiagnostic("stage=first_thread hook=" + std::string(hookName ? hookName : "") + " accepted=" + (canonicalHookName.empty() ? "false" : "true"));
    if (thread.processId == g_targetProcessId && !canonicalHookName.empty() && !g_yarnSupportedThread.exchange(true))
        yarnDiagnostic("stage=first_supported_thread hook=" + canonicalHookName);
#endif
    if (thread.processId != g_targetProcessId || canonicalHookName.empty())
    {
        return;
    }
    if (isEmbedable && nativeEmbedHookSupported(canonicalHookName))
    {
        {
            std::lock_guard<std::mutex> lock(g_embedMutex);
            g_embedThreads[thread] = EmbedThread{thread, canonicalHookName};
        }
        if (g_embedEnabled.load() && g_hookUseEmbed)
        {
            bool threadEnabled = true;
#if defined(DANGO_WILLPLUS_ONLY)
            threadEnabled = canonicalHookName == "EmbedWillplusA" ||
                            (g_willPlusFullTextBackfillEnabled.load() && canonicalHookName == "EmbedWillplus_other");
#endif
#if defined(DANGO_VALKYRIA_ONLY)
            threadEnabled = canonicalHookName == "ValkyriaMessage" ||
                            (g_valkyriaFullTextBackfillEnabled.load() && canonicalHookName == "ValkyriaChoice");
#endif
            // 先移除可能存在的同线程旧登记，避免上游UseEmbed(true)重复占槽。
            g_hookUseEmbed(thread, false);
            if (threadEnabled)
            {
                g_hookUseEmbed(thread, true);
            }
        }
        reportNativeEmbedReady(canonicalHookName);
#if defined(DANGO_WILLPLUS_ONLY)
        if (canonicalHookName == "EmbedWillplus_other")
        {
            return;
        }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
        if (canonicalHookName == "ValkyriaChoice")
        {
            return;
        }
#endif
#if defined(DANGO_RENPY_ONLY)
        if (canonicalHookName == "RenPyEmbed")
        {
            return;
        }
#endif
    }
    if (!g_supportedHookFound.exchange(true))
    {
        emitEvent("hook_found", {{"hook", canonicalHookName}});
        emitEvent("ready", {{"hook", canonicalHookName}});
    }
}

void __cdecl onThreadDestroy(const wchar_t *, const char *, ThreadParam thread)
{
#if defined(DANGO_PJADV_ONLY)
    {
        std::lock_guard<std::mutex> lock(g_pjadvFastSkipMutex);
        g_pjadvEmbedFinishedByThread.erase(thread);
    }
#endif
    if (g_hookUseEmbed)
    {
        g_hookUseEmbed(thread, false);
    }
    {
        std::lock_guard<std::mutex> lock(g_embedMutex);
        g_embedThreads.erase(thread);
    }
#if defined(DANGO_WILLPLUS_ONLY)
    clearWillPlusFastSkipThread(thread);
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    clearValkyriaFastSkipThread(thread);
#endif
}

void __cdecl onEmbedRequest(const wchar_t *text, ThreadParam thread)
{
    if (!text || !*text)
    {
        return;
    }
    if (!g_embedEnabled.load())
    {
        if (g_hookEmbedResult)
        {
            g_hookEmbedResult(thread, text, L"");
        }
        return;
    }
    std::string hook;
    bool threadFound = false;
    {
        std::lock_guard<std::mutex> lock(g_embedMutex);
        const auto found = g_embedThreads.find(thread);
        if (found != g_embedThreads.end())
        {
            hook = found->second.hook;
            threadFound = true;
        }
    }
    if (!threadFound)
    {
        if (g_hookEmbedResult)
        {
            g_hookEmbedResult(thread, text, L"");
        }
        return;
    }
    if (!nativeEmbedHookSupported(hook))
    {
        if (g_hookEmbedResult)
        {
            g_hookEmbedResult(thread, text, L"");
        }
        return;
    }
#if defined(DANGO_WILLPLUS_ONLY)
    if (hook == "EmbedWillplus_other" && !g_willPlusFullTextBackfillEnabled.load())
    {
        if (g_hookEmbedResult)
        {
            g_hookEmbedResult(thread, text, L"");
        }
        return;
    }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    if (hook == "ValkyriaChoice" && !g_valkyriaFullTextBackfillEnabled.load())
    {
        if (g_hookEmbedResult)
            g_hookEmbedResult(thread, text, L"");
        return;
    }
#endif
#if defined(DANGO_TASKFORCE2_ONLY)
    const bool fastSkipRequest = shouldSkipTaskforce2EmbedRequest();
#endif
#if defined(DANGO_PJADV_ONLY)
    const bool fastSkipRequest = shouldSkipPJADVEmbedRequest(thread, text, hook);
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
    const bool fastSkipRequest = shouldSkipBGIEmbedRequest(thread, text);
#endif
#if defined(DANGO_QLIE_ONLY)
    const bool fastSkipRequest = shouldSkipQLIEEmbedRequest(thread, text);
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    const bool fastSkipRequest = hook == "EmbedWillplusA" && shouldSkipWillPlusEmbedRequest(thread, text);
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    const bool fastSkipRequest = shouldSkipValkyriaEmbedRequest(thread, text);
#endif
    const std::uint64_t requestId = ++g_embedRequestSequence;
    {
        std::lock_guard<std::mutex> lock(g_embedMutex);
        g_embedRequests[requestId] = EmbedRequest{thread, hook, text};
    }
#if defined(DANGO_PJADV_ONLY)
    if (fastSkipRequest)
    {
        emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
        return;
    }
#endif
#if defined(DANGO_TASKFORCE2_ONLY)
    if (fastSkipRequest)
    {
        // Taskforce2不能在注入回调内重入回写接口，交由Helper控制线程异步解除等待
        emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
        return;
    }
#endif
#if defined(DANGO_BGI_ONLY) || defined(DANGO_SIGLUS_ONLY)
    if (fastSkipRequest)
    {
        // BGI不能在注入回调内重入回写接口，交由Helper控制线程异步解除等待
        emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
        return;
    }
#endif
#if defined(DANGO_QLIE_ONLY)
    if (fastSkipRequest)
    {
        // QLiE不能在注入回调内重入回写接口，交由Helper控制线程异步解除等待
        emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
        return;
    }
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    if (fastSkipRequest)
    {
        // WillPlus的回写接口不能在注入回调内重入，交给Helper控制线程立即保留原文并解除等待
        emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
        return;
    }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    if (fastSkipRequest)
    {
        emitEvent("embed_fast_skip_request", {{"hook", hook}}, {{"requestId", requestId}});
        return;
    }
#endif
    emitEvent("embed_request", {{"hook", hook}, {"text", wideToUtf8(text)}},
              {{"requestId", requestId}, {"address", thread.addr}, {"context", thread.ctx}, {"subContext", thread.ctx2}});
}

void __cdecl onHookInsert(DWORD processId, std::uint64_t address, const wchar_t *hookName)
{
    if (processId != g_targetProcessId)
    {
        return;
    }
    const std::string utf8HookName = wideToUtf8(hookName);
#if defined(DANGO_YURIS_ONLY)
    if (!utf8HookName.empty() && utf8HookName.front() == 'E')
    {
        std::lock_guard<std::mutex> lock(g_yurisHookMutex);
        g_yurisEmbedAddresses.insert(address);
    }
#else
    static_cast<void>(address);
#endif
    if (!utf8HookName.empty())
    {
        emitEvent("hook_available", {{"hook", utf8HookName}});
    }
#if defined(DANGO_CSYSTEM_ONLY) || defined(DANGO_NSCRIPTER_ONLY)
    // 调用点的安装通知不是完整读取组就绪，等原生专用确认
    return;
#endif
#if defined(DANGO_MIDAREAGEHA_ONLY)
    // 只有带写入标志的显示入口可以报告全量回填就绪
    const std::string canonicalHookName = !utf8HookName.empty() && utf8HookName.front() == 'E' ? "EmbedMIDAREAGEHA" : "MIDAREAGEHA";
#elif defined(DANGO_CATSYSTEM2_ONLY)
    // 只有通过字符处理检查的原生写入入口才能报告全量回填就绪
    const std::string canonicalHookName = !utf8HookName.empty() && utf8HookName.front() == 'E' ? "EmbedCS2" : "CatSystem2";
#elif defined(DANGO_AZSYSTEM_ONLY)
    // 仅带嵌入标志的参数入口可以提前报告写入就绪，普通监听入口不能冒充注入能力
    const std::string canonicalHookName = !utf8HookName.empty() && utf8HookName.front() == 'E' ? "EmbedAZSystem" : "AZSystem";
#elif defined(DANGO_ADVENTURE_CREATOR_ONLY) || defined(DANGO_RPGMAKER_UNITE_ONLY) || defined(DANGO_UTAGE_ONLY) || defined(DANGO_UNITY_EVENTSCRIPT_ONLY) || defined(DANGO_YURIS_ONLY) || defined(DANGO_BOLERO_ONLY) || defined(DANGO_WOLF_ONLY) || defined(DANGO_TASKFORCE2_ONLY)
    const std::string canonicalHookName = canonicalSupportedHookName(utf8HookName.c_str());
    if (canonicalHookName.empty())
    {
        return;
    }
#elif defined(DANGO_PJADV_ONLY)
    const std::string canonicalHookName = address == g_pjadvTargetBase + g_pjadvMessageRVA ? "EmbedPensil" : g_pjadvChoiceRVA && address == g_pjadvTargetBase + g_pjadvChoiceRVA ? "EmbedPensilChoice" : "";
    if (canonicalHookName.empty()) return;
#elif defined(DANGO_KIRIKIRI_ONLY)
    std::string canonicalHookName;
    {
        // 上游安装通知只携带Hook Code，不携带NewHook使用的稳定名称。
        // TextHook在真正安装前会记录「名称+地址」，成功后再发送同一地址的安装通知；
        // 用地址关联两者，既不把一次安装尝试误报为成功，也不绑定游戏RVA。
        std::lock_guard<std::mutex> lock(g_kiriKiriHookAttemptMutex);
        const auto found = g_kiriKiriHookAttempts.find(address);
        if (found != g_kiriKiriHookAttempts.end())
        {
            canonicalHookName = found->second;
            g_kiriKiriHookAttempts.erase(found);
        }
    }
    if (canonicalHookName.empty())
    {
        canonicalHookName = canonicalSupportedHookName(utf8HookName.c_str());
    }
    if (canonicalHookName.empty())
    {
        return;
    }
#else
    const std::string canonicalHookName = canonicalSupportedHookName(utf8HookName.c_str());
    if (canonicalHookName.empty())
    {
        return;
    }
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    if (canonicalHookName == "EmbedWillplus_other")
    {
        return;
    }
#endif
#if defined(DANGO_RENPY_ONLY)
    if (canonicalHookName == "RenPyEmbed")
    {
        return;
    }
#endif
    reportNativeEmbedReady(canonicalHookName);
    if (!g_supportedHookFound.exchange(true))
    {
#if defined(DANGO_KIRIKIRI_ONLY)
        emitEvent("hook_found", {{"hook", "KiriKiri"}});
        emitEvent("ready", {{"hook", "KiriKiri"}});
#elif defined(DANGO_YURIS_ONLY)
        emitEvent("hook_found", {{"hook", "YU-RIS"}});
        emitEvent("ready", {{"hook", "YU-RIS"}});
#else
        emitEvent("hook_found", {{"hook", canonicalHookName}});
        emitEvent("ready", {{"hook", canonicalHookName}});
#endif
    }
}

void __cdecl onOutput(const wchar_t *, const char *hookName, ThreadParam thread, const wchar_t *text)
{
    const std::string canonicalHookName = canonicalSupportedHookName(hookName);
    if (thread.processId != g_targetProcessId || canonicalHookName.empty() || !text || !*text)
    {
        return;
    }
#if defined(DANGO_CSYSTEM_ONLY)
    if (g_csystemBackfillEnabled.load() || (canonicalHookName == "CSystemDialogue" && g_embedEnabled.load())) return;
#endif
#if defined(DANGO_NSCRIPTER_ONLY)
    if(canonicalHookName=="NScripterSprites") {
        const bool backfill=wcsstr(text,L"\"backfillGeneration\":0,")==nullptr;
        if(g_nscripterBackfillEnabled.load()!=backfill)return;
    }else if(g_nscripterBackfillEnabled.load()!=(canonicalHookName=="NScripterRenderReturn"))return;
#endif
#if defined(DANGO_YURIS_ONLY)
    // 回填专用入口不参与普通实时监听，激活后同时屏蔽全部备用入口
    if (canonicalHookName == "EmbedYuris" || g_embedEnabled.load())
    {
        return;
    }
#endif
#if defined(DANGO_BOLERO_ONLY)
    // 完整显示参数仅用于全量回填，激活时同时屏蔽逐字绘制产生的重复快照
    if (canonicalHookName == "EmbedBolero" || g_embedEnabled.load())
    {
        return;
    }
#endif
#if defined(DANGO_AZSYSTEM_ONLY)
    // 角色名和章节标题仍参与全量回填请求，但不单独覆盖实时译文框的正文
    if (canonicalHookName == "EmbedAZSystem" && (thread.ctx2 == 1 || thread.ctx2 == 4))
    {
        return;
    }
#endif
#if defined(DANGO_MIDAREAGEHA_ONLY)
    // 名字和选项只参与全量回填，不单独覆盖实时译文框的正文
    if (canonicalHookName == "EmbedMIDAREAGEHA" && thread.ctx2 != 1)
    {
        return;
    }
#endif
#if defined(DANGO_RENPY_ONLY)
    if (canonicalHookName == "RenPyEmbed")
    {
        return;
    }
#endif
#if defined(DANGO_WILLPLUS_ONLY)
    // 辅助入口只负责全量缓存精确回填，不生成实时监听快照。
    if (canonicalHookName == "EmbedWillplus_other")
    {
        return;
    }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
    if (g_valkyriaFastSkipping.load())
    {
        return;
    }
#endif
    const std::string utf8 = wideToUtf8(text);
    if (utf8.empty())
    {
        return;
    }
#if defined(DANGO_YARN_SPINNER_ONLY)
    if (!g_yarnFirstOutput.exchange(true)) yarnDiagnostic("stage=first_output hook=" + canonicalHookName + " bytes=" + std::to_string(utf8.size()));
#endif
    emitEvent("text_snapshot", {{"hook", canonicalHookName}, {"text", utf8}},
              {{"sequence", ++g_sequence}, {"context", thread.ctx}, {"subContext", thread.ctx2}});
}

void __cdecl onHostInfo(int level, const wchar_t *message)
{
    const std::string utf8 = wideToUtf8(message);
#if defined(DANGO_NEMK_NOVEL_ONLY)
    if(utf8=="NemkNovel live ready"||utf8=="NemkNovel live unavailable")emitEvent("nemk_live_contract",{{"status",utf8=="NemkNovel live ready"?"ready":"unavailable"}});
    if((utf8=="NemkNovel presentation ready"||utf8=="NemkNovel presentation unavailable")&&g_nemkChannel)emitEvent("nemk_presentation",{{"status",utf8=="NemkNovel presentation ready"?"ready":"unavailable"}});
    if(utf8=="NemkNovel read-only ready"&&!g_supportedHookFound.exchange(true)){emitEvent("hook_found",{{"hook","NemkNovelDialogue"}});emitEvent("ready",{{"hook","NemkNovelDialogue"}});}
#endif
#if defined(DANGO_NSCRIPTER_ONLY)
    if(utf8=="NScripter display ready"&&g_nscripterChannel&&!g_embedReadyReported.exchange(true))
        emitEvent("embed_ready",{{"hook","NScripterDialogue"}});
    if (utf8 == "NScripter read-only unavailable") {
        emitEvent("error", {{"code", "hook_not_found"}, {"message", "NScripter引擎暂不支持该版本的监听功能"}});
        g_exitCode.store(9);
        SetEvent(g_shutdownEvent);
    }
    if (utf8 == "NScripter read-only ready" && !g_supportedHookFound.exchange(true)) {
        emitEvent("hook_found", {{"hook", "NScripterDialogue"}});
        emitEvent("ready", {{"hook", "NScripterDialogue"}});
    }
    if(utf8=="NScripter backfill ready"&&g_nscripterChannel&&!g_nscripterBackfillReady.exchange(true))emitEvent("nscripter_backfill_ready",{{"hook","NScripterDialogue"}});
#endif
#if defined(DANGO_CSYSTEM_ONLY)
    if (utf8 == "CSystem display ready" && g_csystemChannel && !g_embedReadyReported.exchange(true))
        emitEvent("embed_ready", {{"hook", "CSystemDialogue"}});
    if (utf8 == "CSystem read-only ready" && !g_supportedHookFound.exchange(true)) {
        emitEvent("hook_found", {{"hook", "CSystemDialogue"}});
        emitEvent("ready", {{"hook", "CSystemDialogue"}});
    }
#endif
#if defined(DANGO_LCSE_ONLY)
    // 原生安装组完成后可在无对话线程的标题页启动准备，普通读取不冒充呈现能力
    {
        std::istringstream installed(utf8);
        std::string engine, role, extra;
        unsigned version = 0, reading = 0, presentation = 0;
        if ((installed >> engine >> role >> version >> reading >> presentation) && !(installed >> extra) &&
            engine == "LCSE" && role == "installed" && version == 1 && reading == 1 && presentation <= 1)
        {
            if (!g_supportedHookFound.exchange(true))
            {
                emitEvent("hook_found", {{"hook", "LCSEDialogue"}});
                emitEvent("ready", {{"hook", "LCSEDialogue"}});
            }
            emitEvent("lcse_presentation_ready", {{"status", presentation ? "ready" : "unavailable"}});
        }
    }
    // 确认只携带本次会话身份及资源结果，普通ready不表示资源已经加载
    std::istringstream control(utf8);
    std::string engine, role, token, state, extra;
    unsigned version = 0;
    std::uint64_t generation = 0;
    std::array<BYTE, 16> decoded{};
    if ((control >> engine >> role >> version >> generation >> token >> state) && !(control >> extra) &&
        engine == "LCSE" && role == "control" && version == 1 && generation && LCSEControl::decodeHex(token, decoded) &&
        (state == "loaded" || state == "captured" || state == "off" || state == "failed" || state == "invalid" || state == "stale"))
        emitEvent("lcse_resources_result", {{"token", token}, {"status", state}}, {{"generation", generation}});
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
    // 所有入口完成安装后即可监听，标题页无需先产生对白来创建文本线程
    if(utf8=="YarnSpinner presentation ready"||utf8=="YarnSpinner presentation unavailable")
        emitEvent("yarn_presentation",{{"status",utf8=="YarnSpinner presentation ready"&&g_yarnChannel?"ready":"unavailable"}});
    if (utf8 == "YarnSpinner read-only views ready" && !g_supportedHookFound.exchange(true))
    {
        emitEvent("hook_found", {{"hook", "YarnSpinnerDialogue"}});
        emitEvent("ready", {{"hook", "YarnSpinnerDialogue"}});
    }
#endif
#if defined(DANGO_UNITY_SCENARIO_ONLY)
    const std::string scenarioReady = "UnityScenario resources ready ";
    if (const auto pos = utf8.find(scenarioReady); pos != std::string::npos) {
        auto digest = utf8.substr(pos + scenarioReady.size());
        const auto end = digest.find_last_not_of(" \t\r\n");
        digest.resize(end == std::string::npos ? 0 : end + 1);
        if (digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos)
            emitEvent("unity_scenario_resources_ready", {{"code", digest}});
    }
    if (utf8.find("UnityScenario resources failed") != std::string::npos) emitEvent("unity_scenario_resources_failed");
#endif
#if defined(DANGO_TEAMNIMBUS_ONLY)
    if (utf8 == "TeamNimbus read-only ready" && !g_supportedHookFound.exchange(true)) {
        emitEvent("hook_found", {{"hook", "TeamNimbusDialogue"}});
        emitEvent("ready", {{"hook", "TeamNimbusDialogue"}});
    }
    const std::string teamReady = "TeamNimbus resources ready ";
    if (const auto pos = utf8.find(teamReady); pos != std::string::npos) {
        auto digest = utf8.substr(pos + teamReady.size());
        const auto end = digest.find_last_not_of(" \t\r\n");
        digest.resize(end == std::string::npos ? 0 : end + 1);
        if (digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos)
            emitEvent("teamnimbus_resources_ready", {{"code", digest}});
    }
    if (utf8.find("TeamNimbus resources failed") != std::string::npos) emitEvent("teamnimbus_resources_failed");
#endif
#if defined(DANGO_DDWORKS_ONLY)
    if (utf8.find("DDWorks resource snapshot ready") != std::string::npos)
    {
        emitEvent("ddworks_resources_ready");
        // 全量模式不创建文本线程，成功加载事件同时确认已经安装的VM复制入口
        if (!g_supportedHookFound.exchange(true))
        {
            emitEvent("hook_found", {{"hook", "DDWorksDialogue"}});
            emitEvent("ready", {{"hook", "DDWorksDialogue"}});
        }
    }
    if (utf8.find("DDWorks resource snapshot failed") != std::string::npos)
        emitEvent("ddworks_resources_failed");
#endif
#if defined(DANGO_KIRIKIRI_ONLY)
    if (!utf8.empty())
    {
        // 只记录能够由KiriKiri组件静态确认的Hook名称。日志先于实际安装，
        // onHookInsert随后以相同地址确认成功后才会使用这条记录。
        static const std::vector<std::string> hookNames = {
            "tTVPNativeBaseBitmap::DrawText", "EmbedKrkr2", "Krkr2wcs",
            "KiriKiri2X", "KiriKiri2", "KiriKiri1", "KiriKiri4",
            "KiriKiriZ Hook", "KiriKiriZ", "TextRender", "EmbedKrkrZ", "krkrz2"};
        for (const auto &name : hookNames)
        {
            const std::string marker = name + " ";
            const auto markerPosition = utf8.rfind(marker);
            if (markerPosition == std::string::npos)
            {
                continue;
            }
            const auto addressPosition = markerPosition + marker.size();
            try
            {
                std::size_t consumed = 0;
                const auto address = std::stoull(utf8.substr(addressPosition), &consumed, 16);
                if (address != 0 && consumed != 0)
                {
                    std::lock_guard<std::mutex> lock(g_kiriKiriHookAttemptMutex);
                    g_kiriKiriHookAttempts[address] = name;
                }
            }
            catch (const std::exception &)
            {
            }
            break;
        }
    }
#endif
    if (!utf8.empty())
    {
        emitEvent("host_info", {{"message", utf8}}, {{"level", static_cast<std::uint64_t>(level)}});
    }
}

std::filesystem::path executableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size())
    {
        return std::filesystem::current_path();
    }
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring quoteArgument(const std::wstring &value)
{
    std::wstring result = L"\"";
    unsigned int backslashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

DWORD runInjector(const std::filesystem::path &injectorPath,
                  const std::filesystem::path &hookPath,
                  const std::filesystem::path &workingDirectory,
                  DWORD processId)
{
    std::wstring commandLine = quoteArgument(injectorPath.wstring()) + L" dllinject " +
                               std::to_wstring(processId) + L" " + quoteArgument(hookPath.wstring());

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#if defined(DANGO_DXCOMPILED_ONLY)
    // 仅DxCompiled启用诊断管道，其他引擎保持既有标准句柄和退出码协议。
    HANDLE diagnosticRead = nullptr, diagnosticWrite = nullptr;
    if (nullHandle != INVALID_HANDLE_VALUE)
    {
        if (!CreatePipe(&diagnosticRead, &diagnosticWrite, &attributes, 16384))
        {
            emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 stage=diagnostic_pipe win32=" + std::to_string(GetLastError())}});
        }
        else if (!SetHandleInformation(diagnosticRead, HANDLE_FLAG_INHERIT, 0))
        {
            const DWORD pipeError = GetLastError();
            CloseHandle(diagnosticRead); CloseHandle(diagnosticWrite);
            diagnosticRead = diagnosticWrite = nullptr;
            emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 stage=diagnostic_handle win32=" + std::to_string(pipeError)}});
        }
    }
    emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 stage=launch target_pid=" + std::to_string(processId) + " helper_pid=" + std::to_string(GetCurrentProcessId()) + " hook=" + wideToUtf8(hookPath.c_str())}});
#endif

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (nullHandle != INVALID_HANDLE_VALUE)
    {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = nullHandle;
        startup.hStdOutput = nullHandle;
        startup.hStdError = nullHandle;
#if defined(DANGO_DXCOMPILED_ONLY)
        if (diagnosticWrite) startup.hStdOutput = diagnosticWrite;
#endif
    }

    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        injectorPath.c_str(), commandLine.data(), nullptr, nullptr,
        nullHandle != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
        workingDirectory.c_str(), &startup, &process);
    const DWORD startError = started ? ERROR_SUCCESS : GetLastError();
#if defined(DANGO_DXCOMPILED_ONLY)
    if (diagnosticWrite) CloseHandle(diagnosticWrite);
#endif
    if (nullHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nullHandle);
    }
    if (!started)
    {
#if defined(DANGO_DXCOMPILED_ONLY)
        if (diagnosticRead) CloseHandle(diagnosticRead);
        emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 stage=create_injector win32=" + std::to_string(startError)}});
#endif
        return startError;
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, kInjectorTimeoutMs);
    DWORD exitCode = ERROR_TIMEOUT;
    if (waitResult == WAIT_OBJECT_0)
    {
        if (!GetExitCodeProcess(process.hProcess, &exitCode))
        {
            exitCode = GetLastError();
        }
    }
    else
    {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    }
#if defined(DANGO_DXCOMPILED_ONLY)
    // 子进程输出总量固定且小于管道容量；退出后有界读取，不等待可能遗留的写端。
    if (diagnosticRead)
    {
        std::string diagnostic;
        DWORD available = 0;
        while (diagnostic.size() < 8192 && PeekNamedPipe(diagnosticRead, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buffer[1024]; DWORD bytes = 0;
            const DWORD count = static_cast<DWORD>((std::min)(sizeof(buffer), static_cast<size_t>(available)));
            if (!ReadFile(diagnosticRead, buffer, count, &bytes, nullptr) || !bytes) break;
            diagnostic.append(buffer, bytes);
        }
        CloseHandle(diagnosticRead);
        std::istringstream lines(diagnostic);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 " + line}});
        }
        if (diagnostic.empty()) emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 stage=injector_output outcome=empty"}});
    }
    emitEvent("host_info", {{"message", "DxCompiled injection diagnostic v=20261017-1 stage=injector_exit wait=" + std::to_string(waitResult) + " exit=" + std::to_string(exitCode)}});
#endif
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode;
}

template <typename Function>
Function loadFunction(HMODULE module, const char *name)
{
    FARPROC address = GetProcAddress(module, name);
#if defined(_M_IX86)
    if (!address)
    {
        const std::string decoratedName = std::string("_") + name;
        address = GetProcAddress(module, decoratedName.c_str());
    }
#endif
    return reinterpret_cast<Function>(address);
}

#if defined(DANGO_KIRIKIRI_ONLY)
bool hasConflictingKiriKiriWrapper(const std::wstring &executablePath)
{
    if (executablePath.empty())
    {
        return false;
    }
    const std::filesystem::path gameDirectory = std::filesystem::path(executablePath).parent_path();
    return std::filesystem::is_regular_file(gameDirectory / L"TrsData.bin") &&
           std::filesystem::is_regular_file(gameDirectory / L"injectPath") &&
           std::filesystem::is_regular_file(gameDirectory / L"winmm.dll");
}
#endif

bool parseArguments(int argc, wchar_t **argv, DWORD &processId, std::wstring &executablePath)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (argument == L"--pid" && index + 1 < argc)
        {
            try
            {
                processId = static_cast<DWORD>(std::stoul(argv[++index]));
            }
            catch (...)
            {
                return false;
            }
        }
        else if (argument == L"--exe" && index + 1 < argc)
        {
            executablePath = argv[++index];
        }
#if defined(DANGO_PJADV_ONLY)
        else if ((argument == L"--pjadv-message" || argument == L"--pjadv-choice") && index + 1 < argc)
        {
            try
            {
                const auto value = std::stoull(argv[++index]);
                if (value > 0xffffffff) return false;
                if (argument == L"--pjadv-message") g_pjadvMessageRVA = value;
                else g_pjadvChoiceRVA = value;
            }
            catch (...) { return false; }
        }
        else if (argument == L"--pjadv-speakers" && index + 1 < argc)
        {
            std::wistringstream input(argv[++index]);
            std::wstring token;
            while (std::getline(input, token, L','))
            {
                try { g_pjadvSpeakerContexts.insert(std::stoull(token)); }
                catch (...) { return false; }
            }
        }
#endif
    }
#if defined(DANGO_PJADV_ONLY)
    return processId != 0 && g_pjadvMessageRVA != 0;
#else
    return processId != 0;
#endif
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    std::wstring targetExecutablePath;
    if (!parseArguments(argc, argv, g_targetProcessId, targetExecutablePath))
    {
        emitEvent("error", {{"code", "invalid_arguments"}, {"message", "缺少有效的目标进程ID"}});
        return 2;
    }
#if defined(DANGO_TEAMNIMBUS_ONLY)
    teamnimbuslease::Host teamNimbusLease;
    std::wstring teamNimbusDigest;
    for (int i = 1; i + 1 < argc; i++) if (std::wstring(argv[i]) == L"--teamnimbus-backfill") teamNimbusDigest = argv[i+1];
    if (!teamNimbusLease.open(g_targetProcessId, teamNimbusDigest)) {
        emitEvent("error", {{"code", "teamnimbus_lease"}, {"message", "无法建立当前游戏会话"}});
        return 3;
    }
#endif

#if defined(DANGO_PJADV_ONLY)
    const HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_targetProcessId);
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    if (modules != INVALID_HANDLE_VALUE)
    {
        if (Module32FirstW(modules, &module)) g_pjadvTargetBase = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
        CloseHandle(modules);
    }
    if (!g_pjadvTargetBase)
    {
        emitEvent("error", {{"code", "process_modules"}, {"message", "无法读取游戏进程模块"}});
        return 3;
    }
#endif

#if defined(DANGO_KIRIKIRI_ONLY)
    if (hasConflictingKiriKiriWrapper(targetExecutablePath))
    {
        emitEvent("error", {{"code", "conflicting_wrapper"},
                            {"message", "检测到已有第三方文本注入层，请使用未修改的游戏程序"}});
        return 12;
    }
#endif

    const std::filesystem::path componentDirectory = executableDirectory();
    const std::wstring filePrefix = engineFilePrefix();
    const std::wstring bitSuffix = sizeof(void *) == 8 ? L"64" : L"32";
    const std::filesystem::path hostPath = componentDirectory / (filePrefix + L"Bridge" + bitSuffix + L".dll");
    const std::filesystem::path hookPath = componentDirectory / (filePrefix + L"Hook" + bitSuffix + L".dll");
    const std::filesystem::path injectorPath = componentDirectory / (filePrefix + L"Injector" + bitSuffix + L".exe");
    if (!std::filesystem::is_regular_file(hostPath) ||
        !std::filesystem::is_regular_file(hookPath) ||
        !std::filesystem::is_regular_file(injectorPath))
    {
        emitEvent("error", {{"code", "component_missing"}, {"message", engineDisplayName() + " Hook组件文件不完整"}});
        return 3;
    }

    HANDLE targetProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_targetProcessId);
    if (!targetProcess)
    {
        emitEvent("error", {{"code", "process_open_failed"}, {"message", "无法打开目标" + engineDisplayName() + "进程"}},
                  {{"win32Error", GetLastError()}});
        return 4;
    }

    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_shutdownEvent)
    {
        emitEvent("error", {{"code", "event_create_failed"}, {"message", "无法创建Helper退出事件"}},
                  {{"win32Error", GetLastError()}});
        CloseHandle(targetProcess);
        return 5;
    }

#if defined(DANGO_NEMK_NOVEL_ONLY)
    g_nemkChannel=g_nemkControl.open(g_targetProcessId,targetProcess);
#endif
#if defined(DANGO_CSYSTEM_ONLY)
    g_csystemChannel = g_csystemControl.open(g_targetProcessId, targetProcess);
#endif
#if defined(DANGO_NSCRIPTER_ONLY)
    g_nscripterChannel=g_nscripterControl.open(g_targetProcessId,targetProcess);
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
    g_yarnChannel = g_yarnControl.create(g_targetProcessId, targetProcess);
    DWORD channelError = GetLastError();
    FILETIME created{}, ended{}, kernel{}, user{};
    BOOL timeValid = GetProcessTimes(targetProcess, &created, &ended, &kernel, &user);
    yarnDiagnostic("stage=start target_path=" + yarnProcessPath(g_targetProcessId) + " requested_path=" + wideToUtf8(targetExecutablePath.c_str()) + " component=" + wideToUtf8(componentDirectory.c_str()) + " creation_time=" + std::to_string(timeValid ? (uint64_t(created.dwHighDateTime) << 32 | created.dwLowDateTime) : 0) + " channel=" + (g_yarnChannel ? "ready" : "unavailable") + " channel_last_win32_error=" + std::to_string(g_yarnChannel ? 0 : channelError));
    yarnEnvironmentSnapshot("before_connect");
#endif
    HMODULE hostModule = LoadLibraryW(hostPath.c_str());
    if (!hostModule)
    {
        emitEvent("error", {{"code", "host_load_failed"}, {"message", "无法加载Hook桥接组件"}},
                  {{"win32Error", GetLastError()}});
        CloseHandle(g_shutdownEvent);
        CloseHandle(targetProcess);
        return 6;
    }

    const auto hookStart = loadFunction<HookStart>(hostModule, "Hook_Start");
    const auto hookSettings = loadFunction<HookSettings>(hostModule, "Hook_Settings");
    const auto hookConnectProcess = loadFunction<HookConnectProcess>(hostModule, "Hook_ConnectProcess");
    const auto hookCheckIfNeedInject = loadFunction<HookCheckIfNeedInject>(hostModule, "Hook_CheckIfNeedInject");
    g_hookDetachProcess = loadFunction<HookDetachProcess>(hostModule, "Hook_DetachProcess");
    g_hookSettingsEx = loadFunction<HookSettingsEx>(hostModule, "Hook_SettingsEx");
    g_hookUseEmbed = loadFunction<HookUseEmbed>(hostModule, "Hook_UseEmbed");
    g_hookEmbedResult = loadFunction<HookEmbedResult>(hostModule, "Hook_EmbedCallback");
#if defined(DANGO_LCSE_ONLY)
    g_hookLCSEControl = loadFunction<HookLCSEControl>(hostModule, "Hook_LCSEControl");
    if (!g_hookLCSEControl)
    {
        emitEvent("error", {{"code", "lcse_control_missing"}, {"message", "Hook桥接组件接口版本不兼容"}});
        FreeLibrary(hostModule); CloseHandle(g_shutdownEvent); CloseHandle(targetProcess); return 7;
    }
#endif
    if (!hookStart || !hookSettings || !hookConnectProcess || !hookCheckIfNeedInject || !g_hookDetachProcess)
    {
        emitEvent("error", {{"code", "host_api_missing"}, {"message", "Hook桥接组件接口版本不兼容"}});
        FreeLibrary(hostModule);
        CloseHandle(g_shutdownEvent);
        CloseHandle(targetProcess);
        return 7;
    }

    hookStart(onProcessConnect, onProcessDisconnect, onThreadCreate, onThreadDestroy,
              onOutput, onHostInfo, onHookInsert, nativeEmbedSupported() ? onEmbedRequest : nullptr, nullptr, nullptr);
#if defined(DANGO_KIRIKIRI_ONLY)
    // KiriKiri字符流必须即时交给主程序聚合，避免快速跳过时在Host内跨页堆叠
    hookSettings(kKiriKiriTextFlushDelayMs, false, 932, 3000, 1000000, false);
#elif defined(DANGO_CATSYSTEM2_ONLY)
    // CatSystem2完整场景Hook必须逐句立即提交，避免持续快进时在Host内堆叠多页文本
    hookSettings(kCatSystem2TextFlushDelayMs, false, 932, 3000, 1000000, false);
#elif defined(DANGO_MIDAREAGEHA_ONLY)
    // MIDAREAGEHA直接读取完整对话，无需等待逐字文本合并
    hookSettings(0, false, 932, 3000, 1000000, false);
#elif defined(DANGO_LCSE_ONLY)
    // LCSE用完整JSON页包保留姓名和页序号，控制字符转义后仍须保持包边界
    hookSettings(0, false, 932, 8192, 1000000, false);
#elif defined(DANGO_TEAMNIMBUS_ONLY) || defined(DANGO_NEMK_NOVEL_ONLY) || defined(DANGO_UNITY_SCENARIO_ONLY) || defined(DANGO_YARN_SPINNER_ONLY) || defined(DANGO_CSYSTEM_ONLY) || defined(DANGO_NSCRIPTER_ONLY)
    // 完整 JSON 页包立即交付，容量覆盖正文、姓名和转义后的协议边界
    hookSettings(0, false, 932, 65536, 1000000, false);
#elif defined(DANGO_AZSYSTEM_ONLY)
    // AZSystem逐字符调用在同一帧内完成，使用短等待切开快速跳过时的相邻页面
    hookSettings(kAZSystemTextFlushDelayMs, false, 932, 3000, 1000000, false);
#elif defined(DANGO_WOLF_ONLY)
    hookSettings(0, true, 932, 3000, 1000000, false);
#else
    hookSettings(0, false, 932, 3000, 1000000, false);
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
    // 查询句柄必须在连接与注入判断前关闭，避免诊断本身制造已存在判断
    HANDLE existingMutex = OpenMutexW(SYNCHRONIZE, FALSE, (L"TEXT_VNR_HOOKMAN_" + std::to_wstring(g_targetProcessId)).c_str());
    DWORD mutexError = existingMutex ? 0 : GetLastError();
    if (existingMutex) CloseHandle(existingMutex);
    yarnDiagnostic("stage=before_connect mutex_open=" + std::string(existingMutex ? "true" : "false") + " mutex_query_error=" + std::to_string(mutexError));
#endif
    hookConnectProcess(g_targetProcessId);

    emitEvent("status", {{"status", "injecting"}});
    const bool needsInject = hookCheckIfNeedInject(g_targetProcessId);
#if defined(DANGO_YARN_SPINNER_ONLY)
    yarnDiagnostic(std::string("stage=inject_decision needs_inject=") + (needsInject ? "true" : "false") + " pipe_connected=" + (g_yarnConnected.load() ? "true" : "false") + " mutex=TEXT_VNR_HOOKMAN_" + std::to_string(g_targetProcessId));
    if (!needsInject) yarnEnvironmentSnapshot("existing_session");
#endif
    if (needsInject)
    {
        const DWORD injectorExitCode = runInjector(injectorPath, hookPath, componentDirectory, g_targetProcessId);
#if defined(DANGO_YARN_SPINNER_ONLY)
        yarnDiagnostic("stage=injector_exit code=" + std::to_string(injectorExitCode));
#endif
        // 专用注入器以1表示注入流程执行成功
        if (injectorExitCode != kInjectorSuccessExitCode)
        {
            emitEvent("error", {{"code", "inject_failed"}, {"message", "向" + engineDisplayName() + "游戏注入Hook失败"}},
                      {{"injectorExitCode", injectorExitCode}});
            g_exitCode.store(8);
            SetEvent(g_shutdownEvent);
        }
    }
    else
    {
        emitEvent("status", {{"status", "already_injected"}});
    }
    if (WaitForSingleObject(g_shutdownEvent, 0) == WAIT_TIMEOUT && !g_supportedHookFound.load())
    {
        emitEvent("status", {{"status", "waiting_text"}});
    }

#if defined(DANGO_NSCRIPTER_ONLY)
    std::thread nscripterResults([](){
        while(WaitForSingleObject(g_shutdownEvent,10)==WAIT_TIMEOUT) {
            nscriptercontrol::Result result{};
            nscriptercontrol::Acknowledgement ack{};
            if(g_nscripterChannel&&g_nscripterControl.acknowledgement(ack))emitEvent("nscripter_backfill_ack",{{"status",ack.status==1?"loaded":"failed"}},{{"generation",ack.generation}});
            while(g_nscripterChannel&&g_nscripterControl.result(result)) {
                {std::lock_guard<std::mutex> lock(g_embedMutex);g_embedRequests.erase(result.id);}
                emitEvent(result.status==1?"embed_applied":"embed_skipped",{{"message",result.status==1?"":"当前文本未注入，保留游戏原文"}},{{"requestId",result.id}});
            }
            nscriptercontrol::Request request{};
            if(g_nscripterChannel&&g_nscripterControl.request(request)) {
                std::wstring text(request.text,request.length);
                {std::lock_guard<std::mutex> lock(g_embedMutex);g_embedRequests[request.id]={{g_targetProcessId,0,request.page,request.part},"NScripterDialogue",text};}
                if(!g_embedEnabled.load()){finishEmbedRequest(request.id,L"",false);continue;}
                emitEvent("embed_request",{{"hook","NScripterDialogue"},{"text",wideToUtf8(text.c_str())}},{{"requestId",request.id},{"context",request.page},{"subContext",request.part}});
            }
        }
    });
#endif
#if defined(DANGO_CSYSTEM_ONLY)
    std::thread csystemResults([]() {
        while (WaitForSingleObject(g_shutdownEvent, 10) == WAIT_TIMEOUT) {
            csystemcontrol::Acknowledgement acknowledgement{};
            if(g_csystemChannel&&g_csystemControl.acknowledgement(acknowledgement))
                emitEvent("csystem_backfill_ack",{{"status",acknowledgement.status==1?"loaded":"failed"}},{{"generation",acknowledgement.generation}});
            csystemcontrol::Display display{};
            if(g_csystemChannel&&g_csystemControl.display(display))
                emitEvent("csystem_backfill_display",{{"status",display.status==3?"reset":display.status==1?"applied":"original"},{"text",wideToUtf8(display.text)},{"translation",wideToUtf8(display.translation)}},
                          {{"generation",display.generation},{"requestId",display.id},{"context",display.page},{"subContext",display.part}});
            csystemcontrol::Result result{};
            while (g_csystemChannel && g_csystemControl.result(result)) {
                { std::lock_guard<std::mutex> lock(g_embedMutex); g_embedRequests.erase(result.id); }
                emitEvent(result.status == 1 ? "embed_applied" : "embed_skipped",
                          {{"message", result.status == 1 ? "" : "当前文本未注入，保留游戏原文"}}, {{"requestId", result.id}});
            }
            csystemcontrol::Request request{};
            if (g_csystemChannel && g_csystemControl.request(request)) {
                const std::wstring text(request.text, request.length);
                { std::lock_guard<std::mutex> lock(g_embedMutex);
                  g_embedRequests[request.id] = {{g_targetProcessId, 0, request.page, request.part}, "CSystemDialogue", text}; }
                if (!g_embedEnabled.load()) { finishEmbedRequest(request.id, L"", false); continue; }
                emitEvent("embed_request", {{"hook", "CSystemDialogue"}, {"text", wideToUtf8(text.c_str())}},
                          {{"requestId", request.id}, {"context", request.page}, {"subContext", request.part}});
            }
        }
    });
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
    std::thread yarnResults([]() {
        const ULONGLONG diagnosticStart = GetTickCount64(); bool loggedWaiting = false;
        while(WaitForSingleObject(g_shutdownEvent,20)==WAIT_TIMEOUT) {
            if (!loggedWaiting && GetTickCount64() - diagnosticStart >= 25000 && !g_supportedHookFound.load()) {
                loggedWaiting = true;
                yarnDiagnostic(std::string("stage=ready_wait_25s pipe_connected=") + (g_yarnConnected.load() ? "true" : "false") + " listener_ready=false any_thread=" + (g_yarnAnyThread.load() ? "true" : "false") + " channel=" + (g_yarnChannel ? "ready" : "unavailable"));
                yarnEnvironmentSnapshot("ready_wait_25s");
            }
            uint64_t revision=0,page=0;uint32_t state=0;
            if(g_yarnChannel&&g_yarnControl.result(revision,page,state)) {
                const char *states[]={"pending","on","off","applied","failed","skipped"};
                emitEvent("yarn_display",{{"status",states[state]}},{{"generation",revision},{"requestId",page}});
            }
            std::wstring text;uint64_t request=0,mode=0;uint32_t source=0;
            if(g_yarnChannel&&g_yarnControl.query(request,mode,text,&source))
                emitEvent("yarn_cache_query",{{"text",wideToUtf8(text.c_str())},{"code",source==1?"posted_choice":"display"}},{{"generation",mode},{"requestId",request}});
        }
    });
#endif
#if defined(DANGO_NEMK_NOVEL_ONLY)
    std::thread nemkResults([](){while(WaitForSingleObject(g_shutdownEvent,20)==WAIT_TIMEOUT){nemkcontrol::Result r;if(g_nemkChannel&&g_nemkControl.result(r)){const char*states[]={"pending","loaded","off","failed"};emitEvent("nemk_resources",{{"status",states[r.status]},{"code",std::string(r.digest,64)}},{{"generation",r.generation}});}nemkcontrol::LiveResult live;if(g_nemkChannel&&g_nemkControl.liveResult(live)){const char*states[]={"pending","ready","off","failed","applied","skipped"};emitEvent("nemk_live",{{"status",states[live.status]}},{{"generation",live.epoch},{"sequence",live.sequence}});}}});
#endif
    std::thread([
#if defined(DANGO_TEAMNIMBUS_ONLY)
                    &teamNimbusLease
#endif
                ]()
                {
                    std::string command;
                    while (std::getline(std::cin, command))
                    {
#if defined(DANGO_TEAMNIMBUS_ONLY)
                        if (command == "teamnimbus_off") { teamNimbusLease.revoke(); continue; }
#endif
                        if (command == "shutdown")
                        {
                            setNativeEmbedEnabled(false);
                            cancelEmbedRequests(false);
                            SetEvent(g_shutdownEvent);
                            return;
                        }
                        if (command == "embed_on")
                        {
#if defined(DANGO_WILLPLUS_ONLY)
                            g_willPlusFullTextBackfillEnabled.store(false);
#endif
#if defined(DANGO_VALKYRIA_ONLY)
                            g_valkyriaFullTextBackfillEnabled.store(false);
#endif
                            setNativeEmbedEnabled(true);
                            continue;
                        }
#if defined(DANGO_WILLPLUS_ONLY)
                        if (command == "embed_backfill_on")
                        {
                            g_willPlusFullTextBackfillEnabled.store(true);
                            setNativeEmbedEnabled(true);
                            continue;
                        }
#endif
#if defined(DANGO_VALKYRIA_ONLY)
                        if (command == "embed_backfill_on")
                        {
                            g_valkyriaFullTextBackfillEnabled.store(true);
                            setNativeEmbedEnabled(true);
                            continue;
                        }
#endif
                        if (command == "embed_off")
                        {
#if defined(DANGO_WILLPLUS_ONLY)
                            g_willPlusFullTextBackfillEnabled.store(false);
#endif
#if defined(DANGO_VALKYRIA_ONLY)
                            g_valkyriaFullTextBackfillEnabled.store(false);
#endif
                            setNativeEmbedEnabled(false);
                            cancelEmbedRequests(false);
                            continue;
                        }
                        std::istringstream input(command);
                        std::string action;
                        input >> action;
#if defined(DANGO_NSCRIPTER_ONLY)
                        if(action=="nscripter_backfill") {
                            uint64_t generation=0;std::string path,hash,extra;std::array<BYTE,32> digest{};
                            bool valid=bool(input>>generation>>path>>hash)&&generation&&hash.size()==64&&!(input>>extra);
                            for(size_t i=0;valid&&i<32;i++){auto hi=std::string("0123456789abcdef").find(hash[i*2]),lo=std::string("0123456789abcdef").find(hash[i*2+1]);if(hi==std::string::npos||lo==std::string::npos)valid=false;else digest[i]=BYTE((hi<<4)|lo);}
                            setNativeEmbedEnabled(false);cancelEmbedRequests(false);g_nscripterBackfillEnabled.store(true);
                            const auto filename=utf8ToWide(decodeBase64(path));
                            valid=valid&&g_nscripterChannel&&g_nscripterBackfillReady.load()&&!filename.empty()&&filename.find(L'\0')==std::wstring::npos;
                            try{valid=valid&&g_nscripterControl.backfill(generation,filename,digest);}catch(...){valid=false;}
                            if(valid)g_embedEnabled.store(true);else emitEvent("nscripter_backfill_ack",{{"status","failed"}},{{"generation",generation}});
                            continue;
                        }
#endif
#if defined(DANGO_CSYSTEM_ONLY)
                        if(action=="csystem_backfill") {
                            uint64_t generation=0;std::string path,hash,extra;std::array<BYTE,32> digest{};
                            bool valid=bool(input>>generation>>path>>hash)&&generation&&hash.size()==64&&!(input>>extra);
                            for(size_t i=0;valid&&i<32;i++) {
                                auto hi=std::string("0123456789abcdef").find(hash[i*2]),lo=std::string("0123456789abcdef").find(hash[i*2+1]);
                                if(hi==std::string::npos||lo==std::string::npos)valid=false;else digest[i]=BYTE((hi<<4)|lo);
                            }
                            setNativeEmbedEnabled(false);cancelEmbedRequests(false);g_csystemBackfillEnabled.store(true);
                            const auto filename=utf8ToWide(decodeBase64(path));
                            valid=valid&&g_csystemChannel&&g_embedReadyReported.load()&&!filename.empty()&&filename.find(L'\0')==std::wstring::npos;
                            try {valid=valid&&g_csystemControl.backfill(generation,filename,digest);}catch(...){valid=false;}
                            if(valid)g_embedEnabled.store(true);
                            else emitEvent("csystem_backfill_ack",{{"status","failed"}},{{"generation",generation}});
                            continue;
                        }
#endif
#if defined(DANGO_NEMK_NOVEL_ONLY)
                        if(action=="nemk_live_on"||action=="nemk_live_off") {
                            uint64_t epoch=0;std::string font,extra;bool enabled=action=="nemk_live_on";bool valid=bool(input>>epoch)&&epoch;
                            if(enabled)valid=valid&&bool(input>>font);
                            if(input>>extra)valid=false;
                            valid=valid&&g_nemkChannel&&g_nemkControl.live(epoch,enabled,utf8ToWide(decodeBase64(font)));
                            if(!valid)emitEvent("nemk_live",{{"status","failed"}},{{"generation",epoch}});
                            continue;
                        }
                        if(action=="nemk_live_result") {
                            uint64_t epoch=0,generation=0,sequence=0,owner=0;std::string encoded,extra;
                            bool valid=bool(input>>epoch>>generation>>sequence>>owner>>encoded)&&!(input>>extra)&&g_nemkChannel;
                            valid=valid&&g_nemkControl.reply(epoch,generation,sequence,owner,encoded=="-"?L"":utf8ToWide(decodeBase64(encoded)));
                            if(!valid)emitEvent("nemk_live",{{"status","skipped"}},{{"generation",epoch},{"sequence",sequence}});
                            continue;
                        }
                        if(action=="nemk_on"||action=="nemk_off"||action=="nemk_index") {
                            uint64_t revision=0;std::string path,digest,fingerprint,font,extra;bool valid=bool(input>>revision)&&revision;
                            if(action!="nemk_off")valid=valid&&bool(input>>path>>digest>>fingerprint>>font);
                            if(input>>extra)valid=false;
                            if(valid&&g_nemkChannel)valid=g_nemkControl.write(revision,action=="nemk_index"?2:action=="nemk_on"?1:0,utf8ToWide(decodeBase64(path)),digest,fingerprint,utf8ToWide(decodeBase64(font)));
                            else valid=false;
                            if(!valid)emitEvent("nemk_resources",{{"status","failed"}},{{"generation",revision}});
                            continue;
                        }
#endif
#if defined(DANGO_YARN_SPINNER_ONLY)
                        if(action=="yarn_cache_result") {
                            uint64_t mode=0,id=0;std::string encoded,extra;
                            if((input>>mode>>id>>encoded)&&mode&&id&&!(input>>extra)&&g_yarnChannel)
                                g_yarnControl.cacheResult(id,mode,encoded=="-"?L"":utf8ToWide(decodeBase64(encoded)));
                            continue;
                        }
                        if(action=="yarn_on"||action=="yarn_off"||action=="yarn_result") {
                            std::string encoded,extra;uint64_t page=0;bool valid=false;unsigned chinese=0;
                            if(action=="yarn_on")valid=bool(input>>encoded>>chinese)&&!encoded.empty()&&chinese<=1;
                            if(action=="yarn_off")valid=true;
                            if(action=="yarn_result")valid=bool(input>>page>>encoded)&&page&&!encoded.empty();
                            if(input>>extra)valid=false;
                            if(valid&&g_yarnChannel) {
                                if(action=="yarn_on")valid=g_yarnControl.write(true,0,utf8ToWide(decodeBase64(encoded)),L"",true,chinese==1);
                                if(action=="yarn_off")valid=g_yarnControl.write(false,0,L"",L"",true);
                                if(action=="yarn_result")valid=g_yarnControl.write(true,page,L"",utf8ToWide(decodeBase64(encoded)));
                            } else valid=false;
                            if(!valid)emitEvent("yarn_display",{{"status","failed"}},{{"requestId",page}});
                            continue;
                        }
#endif
#if defined(DANGO_LCSE_ONLY)
                        if (action == "lcse_resource")
                        {
                            LCSEControl::Request request;
                            std::string arguments;
                            std::getline(input, arguments);
                            HANDLE target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_targetProcessId);
                            const bool valid = LCSEControl::parseTextRequest(arguments, target, g_targetProcessId, request);
                            if (target) CloseHandle(target);
                            if (!valid || !g_hookLCSEControl || !g_hookLCSEControl(g_targetProcessId, &request))
                                emitEvent("lcse_resources_result", {{"token", LCSEControl::hexadecimal(request.token)}, {"status", "send_failed"}}, {{"generation", request.generation}});
                            continue;
                        }
#endif
                        if (action == "embed_font")
                        {
                            std::string encoded;
                            input >> encoded;
                            updateNativeEmbedFont(utf8ToWide(decodeBase64(encoded)));
                            continue;
                        }
                        std::uint64_t requestId = 0;
                        input >> requestId;
                        if (action == "embed_cancel" && requestId != 0)
                        {
                            finishEmbedRequest(requestId, L"", false);
                            continue;
                        }
                        if (action == "embed_skip" && requestId != 0)
                        {
                            finishEmbedRequest(requestId, L"", false);
                            emitEvent("embed_skipped", {{"message", "当前对话未取得有效译文，已保留游戏原文"}}, {{"requestId", requestId}});
                            continue;
                        }
                        if (action == "embed_fail" && requestId != 0)
                        {
                            finishEmbedRequest(requestId, L"", false);
                            emitEvent("embed_skipped", {{"message", "当前对话未取得有效译文，已保留游戏原文"}}, {{"requestId", requestId}});
                            continue;
                        }
                        if (action == "embed_result" && requestId != 0)
                        {
                            std::string encoded;
                            input >> encoded;
                            const std::wstring translation = utf8ToWide(decodeBase64(encoded));
                            finishEmbedRequest(requestId, translation, true);
                        }
                    }
                    setNativeEmbedEnabled(false);
                    cancelEmbedRequests(false);
                    SetEvent(g_shutdownEvent); })
        .detach();

    HANDLE waitHandles[] = {targetProcess, g_shutdownEvent};
    const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0)
    {
        reportProcessExit();
    }

#if defined(DANGO_CSYSTEM_ONLY)
    setNativeEmbedEnabled(false);
    SetEvent(g_shutdownEvent);
    if (csystemResults.joinable()) csystemResults.join();
    cancelEmbedRequests(false);
#endif
#if defined(DANGO_NSCRIPTER_ONLY)
    setNativeEmbedEnabled(false);
    SetEvent(g_shutdownEvent);
    if(nscripterResults.joinable())nscripterResults.join();
    cancelEmbedRequests(false);
#endif
#if defined(DANGO_NEMK_NOVEL_ONLY)
    SetEvent(g_shutdownEvent);if(nemkResults.joinable())nemkResults.join();
    if(g_nemkChannel)g_nemkControl.live(g_nemkControl.shared->live.epoch+1,false);
    if(g_nemkChannel){auto revision=g_nemkControl.shared->command.generation+1;g_nemkControl.write(revision,false);auto end=GetTickCount64()+2500;while(GetTickCount64()<end){nemkcontrol::Result r;if(g_nemkControl.result(r)&&r.generation==revision&&r.status==2)break;Sleep(10);}}
#endif
    if (g_hookDetachProcess)
    {
#if defined(DANGO_YARN_SPINNER_ONLY)
        if(g_yarnChannel)g_yarnControl.write(false,0,L"",L"",true);
        SetEvent(g_shutdownEvent);
        if(yarnResults.joinable())yarnResults.join();
        // 显示组件没有指向Hook DLL的回调，暂停帧的恢复在后续Unity帧继续执行
#endif
        setNativeEmbedEnabled(false);
        cancelEmbedRequests(false);
        g_hookDetachProcess(g_targetProcessId);
        Sleep(100);
    }
    CloseHandle(g_shutdownEvent);
    CloseHandle(targetProcess);
    return g_exitCode.load();
}
