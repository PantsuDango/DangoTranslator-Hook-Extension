#pragma once
#include "teamnimbus_reader.generated.h"
#include <set>

// teamnimbus 复用Mono与公共Hook协议，按托管签名发现完整对话入口
namespace teamnimbus
{
    inline MonoDomain *domain = nullptr;
    inline MonoClass *reader = nullptr;
    inline MonoMethod *bind = nullptr, *display = nullptr, *option = nullptr, *click = nullptr, *bubble = nullptr, *frame = nullptr, *status = nullptr, *diagnostic = nullptr, *legacyOptions = nullptr, *nameplate = nullptr, *recordTemplate = nullptr;
    inline void (*domains)(void (*)(MonoDomain *, void *), void *) = nullptr;
    inline void (*assemblies)(GFunc, void *) = nullptr;
    inline int (*unloading)(MonoDomain *) = nullptr;
    inline MonoDomain *(*objectDomain)(MonoObject *) = nullptr;
    inline wchar_t *(*stringUTF16)(MonoString *) = nullptr;
    inline void (*freeString)(void *) = nullptr;
    inline MonoMethod *(*jitMethod)(MonoJitInfo *) = nullptr;
    inline void *(*jitStart)(MonoJitInfo *) = nullptr;
    inline uint32_t (*methodFlags)(MonoMethod *, uint32_t *) = nullptr;
    inline bool installed = false;
    inline std::wstring resourceStatus;
    inline thread_local bool invoking = false;

    // stringValue 使用旧Mono公开转换接口，不依赖未导出的字符串宏或对象偏移
    std::wstring stringValue(MonoString *value, size_t limit)
    {
        if (!value) return {};
        auto text = stringUTF16(value);
        if (!text) return {};
        size_t size = wcsnlen_s(text, limit + 1);
        std::wstring result = size <= limit ? std::wstring(text, size) : std::wstring();
        freeString(text);
        return result;
    }

    // resolve 严格读取旧Mono实际导出，不调用新运行库专有API
    template<class T> bool resolve(HMODULE module, T &target, const char *name)
    {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!target) Msg::Log("TeamNimbus missing Mono API %s", name);
        return target != nullptr;
    }

    // typeName 取得签名的稳定类型语义，不依赖元数据编号
    std::string typeName(MonoType *type)
    {
        auto klass = type ? mono_class_from_mono_type(type) : nullptr;
        auto name = klass ? mono_class_get_name(klass) : nullptr;
        auto space = klass ? mono_class_get_namespace(klass) : nullptr;
        if (!name || !space) return {};
        return *space ? std::string(space) + "." + name : std::string(name);
    }

    // method 验证唯一实例签名及IL方法，不猜选重载或内部运行库实现
    MonoMethod *method(MonoClass *klass, const char *name, const char *result, std::initializer_list<const char *> parameters)
    {
        if (!klass) return nullptr;
        void *iterator = nullptr;
        MonoMethod *found = nullptr;
        while (auto m = mono_class_get_methods(klass, &iterator))
        {
            if (strcmp(mono_method_get_name(m), name)) continue;
            uint32_t implementation = 0;
            auto flags = methodFlags(m, &implementation);
            auto signature = mono_method_signature(m);
            if ((flags & 0x10) || (implementation & 3) || !signature || !mono_signature_is_instance(signature) || mono_signature_get_param_count(signature) != parameters.size() || typeName(mono_signature_get_return_type(signature)) != result) continue;
            void *param = nullptr; bool valid = true;
            for (auto expected : parameters) if (typeName(mono_signature_get_params(signature, &param)) != expected) valid = false;
            if (!valid) continue;
            if (found) return nullptr;
            found = m;
        }
        return found;
    }

    // invoke 捕获托管异常，任何失败都保留原游戏调用
    MonoObject *invoke(MonoMethod *method, void **args)
    {
        if (!method) return nullptr;
        MonoObject *error = nullptr;
        auto result = mono_runtime_invoke(method, nullptr, args, &error);
        return error ? nullptr : result;
    }

    // Shared frame callback runs on Unity's canvas render thread, never the injector worker.
    void capture(hook_context *, HookParam *, TextBuffer *buffer, uintptr_t *)
    {
        if (invoking || !domain || unloading(domain) || mono_domain_get() != domain) return;
        invoking = true;
        struct Guard { ~Guard() { invoking = false; } } guard;
        auto packet = stringValue(reinterpret_cast<MonoString *>(invoke(frame, nullptr)), (TEXT_BUFFER_SIZE / 2) - 1);
        if (!packet.empty()) buffer->from(packet);
        auto state = stringValue(reinterpret_cast<MonoString *>(invoke(status, nullptr)), 80);
        if (!state.empty() && state != resourceStatus) { Msg::Log("TeamNimbus resources %S", state.c_str()); resourceStatus = state; }
        auto detail = stringValue(reinterpret_cast<MonoString *>(invoke(diagnostic, nullptr)), 160);
        if (!detail.empty()) Msg::Log("UnityMono display %S", detail.c_str());
    }

    struct Candidate { MonoDomain *domain = nullptr; MonoImage *image = nullptr; MonoClass *reader = nullptr; unsigned count = 0; };
    // findDomain 通过当前执行域反射验证程序集归属，兼容没有domain_assembly_foreach的旧Mono
    void findDomain(MonoDomain *candidate, void *state)
    {
        if (!candidate || unloading(candidate) || !mono_domain_set(candidate, 0)) return;
        struct Images { MonoImage *image = nullptr; unsigned count = 0; } images;
        std::vector<MonoAssembly *> inventory;
        assemblies([](void *assembly, void *state) {
            reinterpret_cast<std::vector<MonoAssembly *> *>(state)->push_back(reinterpret_cast<MonoAssembly *>(assembly));
        }, &inventory);
        // 旧Mono枚举回调持有运行库锁，类型解析和程序集加载必须在回调返回后执行
        for (auto assembly : inventory) {
            auto image = mono_assembly_get_image(assembly);
            if (image && mono_class_from_name(image, "UnityEngine", "Canvas")) {
                images.image = image; images.count++;
            }
        }
        if (images.count != 1) return;
        int code = 0;
        auto image = mono_image_open_from_data_full(teamNimbusReader, sizeof(teamNimbusReader), 1, &code, 0);
        auto assembly = image && !code ? mono_assembly_load_from_full(image, "Dango.UnityMono.Reader", &code, 0) : nullptr;
        auto bridge = assembly && !code ? mono_class_from_name(mono_assembly_get_image(assembly), "Dango.UnityMono", "Reader") : nullptr;
        auto validate = bridge ? mono_class_get_method_from_name(bridge, "Validate", 0) : nullptr;
        auto valid = invoke(validate, nullptr);
        if (!valid || !*reinterpret_cast<BYTE *>(mono_object_unbox(valid))) return;
        auto found = reinterpret_cast<Candidate *>(state);
        found->domain = candidate; found->image = images.image; found->reader = bridge; found->count++;
    }

    // install 先验证方法和JIT身份，再成组安装，失败撤销已安装入口
    bool install()
    {
        if (installed) return true;
        HMODULE module = GetModuleHandleW(L"mono.dll");
        if (!module) module = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!module || GetModuleHandleW(L"GameAssembly.dll")) return false;
        monofunctions::init(module);
        if (!resolve(module, domains, "mono_domain_foreach") || !resolve(module, assemblies, "mono_assembly_foreach") ||
            !resolve(module, unloading, "mono_domain_is_unloading") || !resolve(module, objectDomain, "mono_object_get_domain") ||
            !resolve(module, stringUTF16, "mono_string_to_utf16") || !resolve(module, freeString, "g_free") ||
            !resolve(module, jitMethod, "mono_jit_info_get_method") || !resolve(module, jitStart, "mono_jit_info_get_code_start") || !resolve(module, methodFlags, "mono_method_get_flags") ||
            !resolve(module, mono_jit_info_table_find, "mono_jit_info_table_find") || !resolve(module, mono_method_get_name, "mono_method_get_name") ||
            !mono_get_root_domain || !mono_thread_attach || !mono_thread_detach || !mono_domain_set || !mono_domain_get || !mono_runtime_invoke || !mono_object_unbox || !mono_jit_info_table_find ||
            !mono_image_open_from_data_full || !mono_assembly_load_from_full || !mono_class_from_name || !mono_assembly_get_image || !mono_class_get_methods || !mono_method_get_name || !mono_method_signature || !mono_signature_is_instance || !mono_signature_get_params || !mono_signature_get_param_count || !mono_signature_get_return_type || !mono_class_from_mono_type || !mono_class_get_name || !mono_class_get_namespace || !mono_compile_method || !mono_class_get_method_from_name || !mono_string_new_utf16) return false;
        auto thread = mono_thread_attach(mono_get_root_domain());
        if (!thread) return false;
        struct Attachment { MonoThread *thread; ~Attachment(){ mono_thread_detach(thread); } } attachment{thread};
        std::vector<MonoDomain *> activeDomains;
        domains([](MonoDomain *value, void *state) { reinterpret_cast<std::vector<MonoDomain *> *>(state)->push_back(value); }, &activeDomains);
        Candidate found;
        // Unity's old Mono assembly enumeration is global. Its child domain owns
        // the live scene; probing/loading the bridge in root first creates a false
        // second candidate. Root is considered only for a single-domain player.
        for (auto candidate : activeDomains) {
            if (activeDomains.size() > 1 && candidate == mono_get_root_domain()) continue;
            findDomain(candidate, &found);
        }
        if (found.count != 1 || !mono_domain_set(found.domain, 0)) { Msg::Log("TeamNimbus compatible domains %u",found.count); return false; }
        domain = found.domain; reader = found.reader;
        frame = mono_class_get_method_from_name(reader, "Frame", 0);
        status = mono_class_get_method_from_name(reader, "get_Status", 0);
        diagnostic = mono_class_get_method_from_name(reader, "get_Diagnostic", 0);
        auto configure = mono_class_get_method_from_name(reader, "Configure", 1);
        if (!frame || !status || !configure || !diagnostic) return false;
        wchar_t executable[32768]; DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) return false;
        std::wstring path(executable, length); path.resize(path.find_last_of(L"\\/")); path += L"\\dango_cache";
        auto directory = mono_string_new_utf16(domain, reinterpret_cast<const gunichar2 *>(path.data()), int(path.size()));
        void *args[] = {directory}; invoke(configure, args);
        auto canvas = mono_class_from_name(found.image, "UnityEngine", "Canvas");
        auto entry = canvas ? mono_class_get_method_from_name(canvas, "SendWillRenderCanvases", 0) : nullptr;
        uint32_t implementation = 0;
        if (!entry || !(methodFlags(entry, &implementation) & 0x10) || (implementation & 3)) return false;
        auto address = reinterpret_cast<uintptr_t>(mono_compile_method(entry));
        auto info = address ? mono_jit_info_table_find(domain, reinterpret_cast<char *>(address)) : nullptr;
        if (!info || jitMethod(info) != entry || jitStart(info) != reinterpret_cast<void *>(address)) return false;
        HookParam hp; hp.address = address; hp.jittype = JITTYPE::UNITY;
        hp.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT; hp.text_fun = capture;
        if (!NewHook(hp, "TeamNimbusDialogue")) return false;
        installed = true;

        Msg::Log("TeamNimbus read-only ready");
        return true;
    }
}
