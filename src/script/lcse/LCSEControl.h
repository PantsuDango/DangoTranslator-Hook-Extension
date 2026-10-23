#pragma once

#include <windows.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <limits>
#include <string>
#include <sstream>

namespace LCSEControl
{
constexpr DWORD command = 0x4c435345;
#pragma pack(push, 1)
// Request 在既有进程管道传递固定资源身份，禁止以普通监听ready代替资源加载确认
struct Request
{
    DWORD commandType = command, version = 1, size = sizeof(Request), action = 0, process = 0;
    uint64_t generation = 0, creation = 0;
    std::array<BYTE, 16> token{};
    std::array<BYTE, 32> fingerprint{}, digest{};
    wchar_t path[1024]{};
};
#pragma pack(pop)
static_assert(sizeof(Request) == 2164, "LCSE control wire layout");

// hexadecimal 按固定小写十六进制传递会话及摘要，不依赖本机代码页
template<size_t N> inline std::string hexadecimal(const std::array<BYTE, N> &value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

// decodeHex 拒绝长度、大小写或非十六进制字符不符合协议的身份值
template<size_t N> inline bool decodeHex(const std::string &text, std::array<BYTE, N> &value)
{
    if (text.size() != N * 2) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1); };
    std::array<BYTE, N> parsed{};
    for (size_t index = 0; index < N; ++index)
    {
        const int first = digit(text[index * 2]), second = digit(text[index * 2 + 1]);
        if (first < 0 || second < 0) return false;
        parsed[index] = BYTE(first * 16 + second);
    }
    value = parsed;
    return true;
}

// decodeNumber 拒绝符号、溢出和非十进制代次，避免负数被流解析为无符号最大值
inline bool decodeNumber(const std::string &text, uint64_t &value)
{
    if (text.empty() || text.size() > 20) return false;
    uint64_t parsed = 0;
    for (char digit : text)
    {
        if (digit < '0' || digit > '9' || parsed > ((std::numeric_limits<uint64_t>::max)() - (digit - '0')) / 10) return false;
        parsed = parsed * 10 + (digit - '0');
    }
    value = parsed;
    return true;
}

// processIdentity 绑定PID及真实进程创建时间，拒绝PID复用和迟到控制
inline bool processIdentity(HANDLE process, DWORD expected, uint64_t &creation)
{
    FILETIME started{}, exited{}, kernel{}, user{};
    if (!process || GetProcessId(process) != expected || !GetProcessTimes(process, &started, &exited, &kernel, &user)) return false;
    creation = uint64_t(started.dwLowDateTime) | (uint64_t(started.dwHighDateTime) << 32);
    return creation != 0;
}

// validRequest 完整验证固定请求，关闭必须不携带旧文件身份或路径
inline bool validRequest(const Request &request)
{
    if (request.commandType != command || request.version != 1 || request.size != sizeof(Request) ||
        request.action > 2 || !request.process || !request.generation || !request.creation ||
        request.token == std::array<BYTE, 16>{}) return false;
    size_t length = 0;
    while (length < 1024 && request.path[length]) ++length;
    if (length == 1024) return false;
    for (size_t index = length; index < 1024; ++index) if (request.path[index]) return false;
    if (!request.action)
        return !length && request.fingerprint == std::array<BYTE, 32>{} && request.digest == std::array<BYTE, 32>{};
    if (length < 4 || !((request.path[0] >= L'A' && request.path[0] <= L'Z') || (request.path[0] >= L'a' && request.path[0] <= L'z')) ||
        request.path[1] != L':' || (request.path[2] != L'\\' && request.path[2] != L'/') ||
        request.fingerprint == std::array<BYTE, 32>{} || request.digest == std::array<BYTE, 32>{}) return false;
    for (size_t index = 0; index < length; ++index)
        if (request.path[index] < 0x20 || request.path[index] == L'"' ||
            (request.path[index] >= 0xd800 && request.path[index] <= 0xdfff)) return false;
    return true;
}

// decodePath 严格解析标准Base64和UTF8路径，拒绝忽略杂字节或不完整尾组的宽松解码
inline bool decodePath(const std::string &encoded, std::wstring &path)
{
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (encoded.empty() || encoded.size() > 4096 || encoded.size() % 4) return false;
    std::string bytes;
    for (size_t offset = 0; offset < encoded.size(); offset += 4)
    {
        uint32_t group = 0;
        size_t padding = 0;
        for (size_t index = 0; index < 4; ++index)
        {
            const char value = encoded[offset + index];
            if (value == '=')
            {
                if (index < 2 || offset + 4 != encoded.size()) return false;
                ++padding; group <<= 6;
            }
            else
            {
                const auto digit = alphabet.find(value);
                if (padding || digit == std::string::npos) return false;
                group = (group << 6) | uint32_t(digit);
            }
        }
        if ((padding == 1 && (group & 0xff)) || (padding == 2 && (group & 0xffff))) return false;
        bytes += char(group >> 16);
        if (padding < 2) bytes += char(group >> 8);
        if (!padding) bytes += char(group);
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    if (length <= 0 || length >= 1024) return false;
    std::wstring parsed(size_t(length), 0);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), parsed.data(), length) != length) return false;
    if (parsed.find(wchar_t(0)) != std::wstring::npos) return false;
    path = std::move(parsed);
    return true;
}

// parseTextRequest 将Helper命令关联到真实目标进程，只接受单个完整请求
inline bool parseTextRequest(const std::string &text, HANDLE target, DWORD process, Request &request)
{
    if (text.size() > 5000) return false;
    std::istringstream input(text);
    std::string generation, token, action, fingerprint, digest, encoded, extra;
    uint64_t mode = 0;
    Request parsed;
    parsed.process = process;
    if (!(input >> generation >> token >> action >> fingerprint >> digest >> encoded) || (input >> extra) ||
        !decodeNumber(generation, parsed.generation) || !decodeNumber(action, mode) || mode > 2 ||
        !decodeHex(token, parsed.token) || !decodeHex(fingerprint, parsed.fingerprint) || !decodeHex(digest, parsed.digest) ||
        !processIdentity(target, process, parsed.creation)) return false;
    parsed.action = DWORD(mode);
    if (!mode) { if (encoded != "-") return false; }
    else
    {
        std::wstring path;
        if (!decodePath(encoded, path)) return false;
        std::copy(path.begin(), path.end(), parsed.path);
    }
    if (!validRequest(parsed)) return false;
    request = parsed;
    return true;
}

// State 串行消费会话代次，加载失败撤销旧资源，迟到或外部会话不得覆盖当前配置
class State
{
    std::mutex lock;
    std::array<BYTE, 16> token{};
    uint64_t generation = 0;
public:
    // apply 在进程控制线程加载资源，绘制回调不读取文件或等待此互斥锁
    std::string apply(const Request &request, const std::function<bool(const Request &)> &load,
        const std::function<void()> &disable)
    {
        std::lock_guard<std::mutex> guard(lock);
        uint64_t creation = 0;
        if (!validRequest(request) || !processIdentity(GetCurrentProcess(), request.process, creation) || creation != request.creation) return "invalid";
        if ((generation && token != request.token) || request.generation <= generation) return "stale";
        token = request.token; generation = request.generation;
        disable();
        if (!request.action) return "off";
        try { if (load(request)) return request.action == 1 ? "loaded" : "captured"; }
        catch (...) {}
        disable();
        return "failed";
    }

    // reset 进程管道断开时立即撤销资源及旧会话，不让重连恢复上次注入状态
    void reset(const std::function<void()> &disable)
    {
        std::lock_guard<std::mutex> guard(lock);
        disable(); token = {}; generation = 0;
    }
};
}
