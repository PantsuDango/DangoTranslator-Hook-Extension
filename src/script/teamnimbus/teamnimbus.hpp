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

    // capture 主线程绑定完整页，在专用消费者处重定向临时显示参数
    void capture(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        if (invoking || !domain || unloading(domain) || mono_domain_get() != domain) return;
        invoking = true;
        struct Guard { ~Guard() { invoking = false; } } guard;
        auto owner = reinterpret_cast<MonoObject *>(context->rcx);
        if (!owner || objectDomain(owner) != domain) return;
        switch (hp->user_value)
        {
        case 1: {
            void *args[] = {owner, reinterpret_cast<void *>(context->rdx), reinterpret_cast<void *>(context->r8)};
            invoke(bind, args); break;
        }
        case 2: {
            void *args[] = {owner, reinterpret_cast<void *>(context->rdx)};
            auto result = invoke(display, args);
            if (result) context->rdx = reinterpret_cast<uintptr_t>(result);
            break;
        }
        case 3: {
            int id = int(context->rdx);
            void *args[] = {owner, &id, reinterpret_cast<void *>(context->r8)};
            invoke(option, args); break;
        }
        case 4: { void *args[] = {owner}; invoke(click, args); break; }
        case 5: {
            void *args[] = {owner, reinterpret_cast<void *>(context->r8)};
            auto result = invoke(bubble, args);
            if (result) context->r8 = reinterpret_cast<uintptr_t>(result);
            break;
        }
        case 6: {
            auto result = reinterpret_cast<MonoString *>(invoke(frame, nullptr));
            auto packet = stringValue(result, (TEXT_BUFFER_SIZE / 2) - 1);
            if (!packet.empty()) buffer->from(packet);
            {
                auto value = reinterpret_cast<MonoString *>(invoke(status, nullptr));
                auto state = stringValue(value, 80);
                if (!state.empty() && state != resourceStatus) {
                    Msg::Log("TeamNimbus resources %S", state.c_str());
                    resourceStatus = state;
                }
                auto detail = stringValue(reinterpret_cast<MonoString *>(invoke(diagnostic, nullptr)), 120);
                if (!detail.empty()) Msg::Log("TeamNimbus display %S", detail.c_str());
            }
            break;
        }
        case 7: { void *args[] = {owner}; invoke(legacyOptions, args); break; }
        case 8: { void *args[] = {owner, reinterpret_cast<void *>(context->rdx)}; invoke(nameplate, args); break; }
        case 9: case 10: { void *args[] = {owner, reinterpret_cast<void *>(context->rdx)}; invoke(recordTemplate, args); break; }
        }
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
            if (image && mono_class_from_name(image, "TeamNimbus.CloudMeadow.Dialogue", "DialogWindow")) {
                images.image = image; images.count++;
            }
        }
        if (images.count != 1) return;
        int code = 0;
        auto image = mono_image_open_from_data_full(teamNimbusReader, sizeof(teamNimbusReader), 1, &code, 0);
        auto assembly = image && !code ? mono_assembly_load_from_full(image, "Dango.TeamNimbus.Reader", &code, 0) : nullptr;
        auto bridge = assembly && !code ? mono_class_from_name(mono_assembly_get_image(assembly), "Dango.TeamNimbus", "Reader") : nullptr;
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
        for (auto candidate : activeDomains) findDomain(candidate, &found);
        if (found.count != 1 || !mono_domain_set(found.domain, 0)) { Msg::Log("TeamNimbus compatible domains %u",found.count); return false; }
        domain = found.domain; reader = found.reader;
        bind = mono_class_get_method_from_name(reader, "Bind", 3);
        display = mono_class_get_method_from_name(reader, "Display", 2);
        option = mono_class_get_method_from_name(reader, "InitializeOption", 3);
        click = mono_class_get_method_from_name(reader, "Click", 1);
        bubble = mono_class_get_method_from_name(reader, "Bubble", 2);
        frame = mono_class_get_method_from_name(reader, "Frame", 0);
        status = mono_class_get_method_from_name(reader, "get_Status", 0);
        diagnostic = mono_class_get_method_from_name(reader, "get_Diagnostic", 0);
        legacyOptions = mono_class_get_method_from_name(reader, "LegacyOptions", 1);
        nameplate = mono_class_get_method_from_name(reader, "Nameplate", 2);
        recordTemplate = mono_class_get_method_from_name(reader, "Template", 2);
        auto configure = mono_class_get_method_from_name(reader, "Configure", 1);
        if (!bind || !display || !option || !click || !bubble || !frame || !status || !configure || !legacyOptions) return false;
        wchar_t executable[32768]; DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) return false;
        std::wstring path(executable, length); path.resize(path.find_last_of(L"\\/")); path += L"\\dango_cache";
        auto directory = mono_string_new_utf16(domain, reinterpret_cast<const gunichar2 *>(path.data()), int(path.size()));
        void *args[] = {directory}; invoke(configure, args);
        auto klass = [&](const char *space, const char *name){ return mono_class_from_name(found.image, space, name); };
        std::vector<std::pair<MonoMethod *, unsigned>> entries{
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindow"), "RevealDialogRoutine", "System.Collections.IEnumerator", {"System.String", "System.String"}), 1},
            {method(klass("Game.UI", "TMPDialogueReveal"), "StartRevealingNewString", "System.Void", {"System.String"}), 2},
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindowOptionClickable"), "Initialize", "System.Void", {"System.Int32", "System.String", "UnityEngine.Color", "TeamNimbus.CloudMeadow.Dialogue.SelectedOptionDelegate"}), 3},
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindowOptionClickable"), "HandleClicked", "System.Void", {}), 4},
            {method(klass("TeamNimbus.CloudMeadow.UI", "DialogueBubbleManager"), "Show", "TeamNimbus.CloudMeadow.UI.DialogueBubbleManager", {"UnityEngine.Transform", "System.String", "System.Single", "System.Action", "System.Single"}), 5},
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindow"), "Legacy_ShowOptions", "System.Void", {"System.Boolean", "System.Collections.Generic.IEnumerable`1", "System.String"}), 7},
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindow"), "SetupNameplateForDialogCharacter", "System.Void", {"TeamNimbus.CloudMeadow.Dialogue.DialogWindowCharacter"}), 8},
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindow"), "FormatDialogString", "System.String", {"System.String"}), 9},
            {method(klass("TeamNimbus.CloudMeadow.Dialogue", "DialogWindow"), "Legacy_ReplacePlayerTokensInLine", "System.String", {"System.String"}), 10},
            {method(klass("TeamNimbus.CloudMeadow.Managers", "GameManager"), "Update", "System.Void", {}), 6}
        };
        std::set<uintptr_t> unique;
        std::vector<std::pair<uintptr_t, unsigned>> addresses;
        if (!entries[2].first || !entries[3].first) { entries[2].first = nullptr; entries[3].first = nullptr; }
        int capabilityMask = 0;
        for (auto entry : entries) {
            if (!entry.first) { if (entry.second == 1 || entry.second == 6) return false; else continue; }
            auto address = reinterpret_cast<uintptr_t>(mono_compile_method(entry.first));
            auto info = address ? mono_jit_info_table_find(domain, reinterpret_cast<char *>(address)) : nullptr;
            if (!info || jitMethod(info) != entry.first || jitStart(info) != reinterpret_cast<void *>(address) || !unique.insert(address).second) {
                if (entry.second == 1 || entry.second == 6) return false;
                continue;
            }
            addresses.emplace_back(address, entry.second);
            capabilityMask |= 1 << entry.second;
        }
        auto capabilities = mono_class_get_method_from_name(reader, "SetCapabilities", 1);
        if (!capabilities) return false;
        void *capabilityArgs[] = {&capabilityMask}; invoke(capabilities, capabilityArgs);
        std::vector<uintptr_t> hooked;
        for (auto entry : addresses) {
            HookParam hp; hp.address = entry.first; hp.user_value = entry.second; hp.jittype = JITTYPE::UNITY;
            hp.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT; hp.text_fun = capture;
            if (!NewHook(hp, entry.second == 6 ? "TeamNimbusDialogue" : "TeamNimbusConsumer")) {
                if (entry.second != 1 && entry.second != 6) {
                    capabilityMask &= ~(1 << entry.second);
                    invoke(capabilities, capabilityArgs);
                    continue;
                }
                for (auto address : hooked) RemoveHook(address, 0);
                return false;
            }
            hooked.push_back(entry.first);
        }
        installed = true;
        Msg::Log("TeamNimbus capabilities %d", capabilityMask);
        Msg::Log("TeamNimbus read-only ready");
        return true;
    }
}
