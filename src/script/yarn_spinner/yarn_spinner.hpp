#pragma once
#include "reader_bytes.generated.h"
#include <set>

// yarnspinner 只在唯一Mono执行域内读取实际对话视图收到的完整参数
namespace yarnspinner
{
    inline MonoDomain *gameDomain = nullptr;
    inline MonoClass *viewClass = nullptr;
    inline MonoMethod *captureMethod = nullptr;
    inline MonoMethod *messageMethod = nullptr;
    inline MonoMethod *frameMethod = nullptr;
    inline bool installed = false;
    inline void (*domainForeach)(void (*)(MonoDomain *, void *), void *) = nullptr;
    inline void (*assemblyForeach)(MonoDomain *, GFunc, void *) = nullptr;
    inline int (*unloading)(MonoDomain *) = nullptr;
    inline MonoDomain *(*objectDomain)(MonoObject *) = nullptr;
    inline int (*stringLength)(MonoString *) = nullptr;
    inline const wchar_t *(*stringChars)(MonoString *) = nullptr;
    inline MonoClass *(*classGet)(MonoImage *, uint32_t) = nullptr;
    inline MonoMethod *(*jitMethod)(MonoJitInfo *) = nullptr;
    inline void *(*jitStart)(MonoJitInfo *) = nullptr;
    inline uint32_t (*methodFlags)(MonoMethod *, uint32_t *) = nullptr;
    inline thread_local bool reading = false;
    inline std::atomic<unsigned> traceMask{0};
    inline std::atomic<uint64_t> rejectMask{0};
    inline std::atomic<unsigned> captureTraceMask{0};
    // trace 每个安装阶段只记录一次，不包含剧情内容
    void trace(unsigned bit, const char *phase)
    { if (!(traceMask.fetch_or(bit) & bit)) Msg::Log("YarnSpinner %s", phase); }

    // reject 每类安装拒绝只记录一次，瞬时失败仍沿用原重试流程
    bool reject(unsigned reason, const char *phase)
    {
        uint64_t bit = uint64_t(1) << reason;
        if (!(rejectMask.fetch_or(bit) & bit)) Msg::Log("YarnSpinner rejected reason=%u phase=%s", reason, phase);
        return false;
    }

    // captureTrace 有界记录参数读取阶段，不输出原文、姓名及异常消息
    void captureTrace(unsigned bit, const char *phase)
    { if (!(captureTraceMask.fetch_or(bit) & bit)) Msg::Log("YarnSpinner capture phase=%s", phase); }

    template <class T> bool resolve(HMODULE module, T &target, const char *name)
    {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!target && !(rejectMask.fetch_or(uint64_t(1) << 2) & (uint64_t(1) << 2))) Msg::Log("YarnSpinner missing Mono API=%s", name);
        return target != nullptr;
    }

    std::string typeName(MonoType *type)
    {
        auto klass = type ? mono_class_from_mono_type(type) : nullptr;
        const char *name = klass ? mono_class_get_name(klass) : nullptr;
        const char *space = klass ? mono_class_get_namespace(klass) : nullptr;
        if (!name || !space) return {};
        return *space ? std::string(space) + "." + name : std::string(name);
    }

    // viewEntry 拒绝歧义、静态及不兼容的虚方法，只接受标准显示参数
    bool viewEntry(MonoMethod *method, int kind)
    {
        uint32_t implementation = 0, flags = methodFlags(method, &implementation);
        auto signature = mono_method_signature(method);
        if ((implementation & ~0x148u) || (flags & 0x10) || !(flags & 0x40) || !signature ||
            !mono_signature_is_instance(signature) || mono_signature_get_param_count(signature) != 2 ||
            typeName(mono_signature_get_return_type(signature)) != "System.Void") return false;
        void *iterator = nullptr;
        if (typeName(mono_signature_get_params(signature, &iterator)) != (kind == 1 ? "Yarn.Unity.LocalizedLine" : "Yarn.Unity.DialogueOption[]")) return false;
        return typeName(mono_signature_get_params(signature, &iterator)) == (kind == 1 ? "System.Action" : "System.Action`1");
    }

    // capture 在原方法执行前只读参数，异常或执行域不符时保持游戏原调用
    void capture(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        captureTrace(1, "entered");
        if (reading) { captureTrace(2, "reentrant_filtered"); return; }
        if (!gameDomain || unloading(gameDomain) || mono_domain_get() != gameDomain) { captureTrace(4, "domain_filtered"); return; }
        auto view = reinterpret_cast<MonoObject *>(context->rcx);
        auto value = reinterpret_cast<MonoObject *>(context->rdx);
        if (!view || !value || objectDomain(view) != gameDomain || objectDomain(value) != gameDomain) { captureTrace(8, "object_filtered"); return; }
        reading = true;
        struct Guard { ~Guard() { reading = false; } } guard;
        int kind = int(hp->user_value);
        void *args[] = {value, view, &kind}; MonoObject *exception = nullptr;
        auto result = mono_runtime_invoke(captureMethod, nullptr, args, &exception);
        if (exception) { captureTrace(16, "managed_exception"); return; }
        if (!result) { captureTrace(32, "empty_or_filtered"); return; }
        auto text = reinterpret_cast<MonoString *>(result);
        int length = stringLength(text); auto chars = stringChars(text);
        if (!chars || length <= 0 || length > 16000 || size_t(length) * 2 > TEXT_BUFFER_SIZE) { captureTrace(64, "packet_bounds_filtered"); return; }
        buffer->from(std::wstring_view(chars, size_t(length)));
        captureTrace(128, "first_packet_emitted");
    }

    // bindMessage 仅登记已确认正文气泡的原参数，不改写原调用或触发新的剧情事件
    void bindMessage(hook_context *context, HookParam *, TextBuffer *, uintptr_t *)
    {
        if(reading||!messageMethod||!gameDomain||unloading(gameDomain)||mono_domain_get()!=gameDomain)return;
        auto owner=reinterpret_cast<MonoObject *>(context->rcx), text=reinterpret_cast<MonoObject *>(context->rdx);
        if(!owner||!text||objectDomain(owner)!=gameDomain||objectDomain(text)!=gameDomain)return;
        reading=true;struct Guard {~Guard(){reading=false;}}guard;
        void *args[]={owner,text};MonoObject *exception=nullptr;mono_runtime_invoke(messageMethod,nullptr,args,&exception);
    }

    // displayFrame 只在已验证Unity回调中建立独立显示组件，不改写游戏帧逻辑
    void displayFrame(hook_context *context, HookParam *, TextBuffer *, uintptr_t *)
    {
        if(reading||!frameMethod||!gameDomain||unloading(gameDomain)||mono_domain_get()!=gameDomain)return;
        auto owner=reinterpret_cast<MonoObject *>(context->rcx);
        if(!owner||objectDomain(owner)!=gameDomain)return;
        reading=true;struct Guard {~Guard(){reading=false;}}guard;
        void *args[]={owner};MonoObject *exception=nullptr;mono_runtime_invoke(frameMethod,nullptr,args,&exception);
    }

    struct Domain { MonoDomain *domain = nullptr; MonoImage *image = nullptr; unsigned count = 0; };
    void findDomain(MonoDomain *domain, void *state)
    {
        if (!domain || unloading(domain)) return;
        Domain candidate; candidate.domain = domain;
        assemblyForeach(domain, [](void *assembly, void *state) {
            auto image = mono_assembly_get_image(reinterpret_cast<MonoAssembly *>(assembly));
            if (image && mono_class_from_name(image, "Yarn.Unity", "DialogueRunner"))
            { auto &found = *reinterpret_cast<Domain *>(state); found.image = image; ++found.count; }
        }, &candidate);
        if (!candidate.count) return;
        auto &found = *reinterpret_cast<Domain *>(state);
        found.domain = domain; found.image = candidate.image; found.count += candidate.count;
    }

    struct Entry { MonoMethod *method; int kind; uintptr_t address; };
    struct Entries { std::vector<Entry> methods; bool invalid = false; };

    // collectViews 扫描当前执行域中真正的派生视图，不依赖游戏类名或生成方法后缀
    void collectViews(void *assembly, void *state)
    {
        auto &entries = *reinterpret_cast<Entries *>(state);
        auto image = mono_assembly_get_image(reinterpret_cast<MonoAssembly *>(assembly));
        if (!image) return;
        int count = mono_image_get_table_rows(image, 2);
        if (count < 0 || count > 100000) { entries.invalid = true; return; }
        for (int rid = 1; rid <= count; ++rid)
        {
            auto klass = classGet(image, 0x02000000u | uint32_t(rid));
            if (!klass || klass == viewClass) continue;
            auto parent = mono_class_get_parent(klass); int depth = 0;
            while (parent && parent != viewClass && ++depth < 64) parent = mono_class_get_parent(parent);
            if (parent != viewClass) continue;
            void *iterator = nullptr; unsigned names[2]{};
            while (auto method = mono_class_get_methods(klass, &iterator))
            {
                const char *name = mono_method_get_name(method); if (!name) continue;
                int kind = !strcmp(name, "RunLine") ? 1 : !strcmp(name, "RunOptions") ? 2 : 0;
                if (!kind) continue;
                if (++names[kind - 1] != 1 || !viewEntry(method, kind)) { entries.invalid = true; continue; }
                entries.methods.push_back({method, kind, 0});
            }
        }
    }

    // install 所有托管与JIT结构通过后才安装专用只读入口，失败不启用通用扫描
    bool install()
    {
        if (installed) return true;
        trace(256, "diagnostics=20261013-1 install entered");
        HMODULE module = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!module || GetModuleHandleW(L"GameAssembly.dll")) return reject(0, "mono_module_unavailable_or_il2cpp");
        monofunctions::init(module);
        trace(32, "Mono API resolved");
        if (!resolve(module, domainForeach, "mono_domain_foreach") || !resolve(module, assemblyForeach, "mono_domain_assembly_foreach") ||
            !resolve(module, unloading, "mono_domain_is_unloading") || !resolve(module, objectDomain, "mono_object_get_domain") ||
            !resolve(module, stringLength, "mono_string_length") || !resolve(module, stringChars, "mono_string_chars") ||
            !resolve(module, classGet, "mono_class_get") || !resolve(module, jitMethod, "mono_jit_info_get_method") ||
            !resolve(module, mono_method_get_name, "mono_method_get_name") ||
            !resolve(module, jitStart, "mono_jit_info_get_code_start") || !resolve(module, methodFlags, "mono_method_get_flags") ||
            !resolve(module, mono_image_get_table_rows, "mono_image_get_table_rows") ||
            !resolve(module, mono_domain_get, "mono_domain_get") || !resolve(module, mono_domain_set, "mono_domain_set") ||
            !resolve(module, mono_jit_info_table_find, "mono_jit_info_table_find") ||
            !mono_get_root_domain || !mono_thread_attach || !mono_thread_detach || !mono_runtime_invoke || !mono_object_unbox ||
            !mono_image_open_from_data_full || !mono_assembly_load_from_full || !mono_class_get_methods || !mono_class_get_parent ||
            !mono_compile_method || !mono_method_signature || !mono_signature_get_params || !mono_signature_get_return_type || !mono_signature_is_instance || !mono_signature_get_param_count ||
            !mono_assembly_get_image || !mono_class_from_name || !mono_class_get_method_from_name ||
            !mono_class_from_mono_type || !mono_class_get_name || !mono_class_get_namespace) return reject(1, "mono_api_unavailable");
        auto root = mono_get_root_domain(); auto thread = root ? mono_thread_attach(root) : nullptr;
        if (!thread) return reject(3, "root_domain_or_thread_unavailable");
        struct Attachment { MonoThread *thread; ~Attachment() { mono_thread_detach(thread); } } attachment{thread};
        trace(64, "installer thread attached");
        Domain found; domainForeach(findDomain, &found);
        if (found.count != 1 || !found.domain || !found.image || !mono_domain_set(found.domain, 0))
        {
            if (!(rejectMask.fetch_or(uint64_t(1) << 4) & (uint64_t(1) << 4))) Msg::Log("YarnSpinner rejected phase=execution_domain count=%u", found.count);
            return false;
        }
        gameDomain = found.domain;
        trace(1, "execution domain validated");
        viewClass = mono_class_from_name(found.image, "Yarn.Unity", "DialogueViewBase");
        if (!viewClass) return reject(5, "view_base_unavailable");
        int status = 0;
        auto bridgeImage = mono_image_open_from_data_full(readerBytes, sizeof(readerBytes), 1, &status, 0);
        auto bridge = bridgeImage && status == 0 ? mono_assembly_load_from_full(bridgeImage, "Dango.YarnSpinner.Reader", &status, 0) : nullptr;
        auto bridgeClass = bridge && status == 0 ? mono_class_from_name(mono_assembly_get_image(bridge), "Dango.YarnSpinner", "Reader") : nullptr;
        if (!bridgeClass) return reject(6, "reader_load_failed");
        trace(2, "reader loaded");
        auto validate = mono_class_get_method_from_name(bridgeClass, "Validate", 0);
        captureMethod = mono_class_get_method_from_name(bridgeClass, "Capture", 3);
        if (!validate || !captureMethod) return reject(7, "reader_entry_unavailable");
        MonoObject *exception = nullptr; auto valid = mono_runtime_invoke(validate, nullptr, nullptr, &exception);
        if (exception || !valid || !*reinterpret_cast<BYTE *>(mono_object_unbox(valid)))
        {
            if (!(rejectMask.fetch_or(uint64_t(1) << 8) & (uint64_t(1) << 8)))
            {
                Msg::Log("YarnSpinner rejected phase=managed_contract exception=%d", exception != nullptr);
                auto failure = mono_class_get_method_from_name(bridgeClass, "get_Failure", 0); MonoObject *error = nullptr;
                auto detail = failure ? reinterpret_cast<MonoString *>(mono_runtime_invoke(failure, nullptr, nullptr, &error)) : nullptr;
                if (detail && !error && stringLength(detail) > 0 && stringLength(detail) <= 256)
                    Msg::Log("YarnSpinner managed_contract detail=%S", std::wstring(stringChars(detail), stringLength(detail)).c_str());
            }
            return false;
        }
        trace(4, "managed contract validated");
        Entries entries; assemblyForeach(gameDomain, collectViews, &entries);
        if (entries.invalid || entries.methods.empty() || entries.methods.size() > 128) { trace(128, "view layout unavailable"); return reject(9, "view_signatures"); }
        trace(8, "view signatures validated");
        std::set<uintptr_t> addresses; bool kinds[2]{};
        for (auto &entry : entries.methods)
        {
            entry.address = reinterpret_cast<uintptr_t>(mono_compile_method(entry.method));
            auto info = entry.address ? mono_jit_info_table_find(gameDomain, reinterpret_cast<char *>(entry.address)) : nullptr;
            if (!info || jitMethod(info) != entry.method || jitStart(info) != reinterpret_cast<void *>(entry.address) || !addresses.insert(entry.address).second) return reject(10, "jit_identity_or_shared_entry");
            kinds[entry.kind - 1] = true;
        }
        if (!kinds[0] || !kinds[1]) return reject(11, "line_or_options_missing");
        trace(16, "JIT entries validated");
        std::vector<uintptr_t> hooked;
        for (const auto &entry : entries.methods)
        {
            HookParam hook; hook.address = entry.address; hook.user_value = entry.kind;
            hook.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT;
            hook.text_fun = capture; hook.jittype = JITTYPE::UNITY;
            if (!NewHook(hook, "YarnSpinnerDialogue"))
            { for (auto address : hooked) RemoveHook(address, 0); return reject(12, "hook_install_failed"); }
            hooked.push_back(entry.address);
        }
        auto displayClass=mono_class_from_name(mono_assembly_get_image(bridge),"Dango.YarnSpinner","Display");
        auto presentation=mono_class_get_method_from_name(bridgeClass,"get_Presentation",0);
        MonoObject *presentationError=nullptr;
        auto supported=presentation?mono_runtime_invoke(presentation,nullptr,nullptr,&presentationError):nullptr;
        bool display=supported&&!presentationError&&*reinterpret_cast<BYTE *>(mono_object_unbox(supported));
        if(display&&displayClass)
        {
            auto chat=mono_class_get_method_from_name(displayClass,"get_ChatSupported",0);
            auto chatValue=chat?mono_runtime_invoke(chat,nullptr,nullptr,&presentationError):nullptr;
            if(chatValue&&!presentationError&&*reinterpret_cast<BYTE *>(mono_object_unbox(chatValue)))
            {
                struct Target {MonoMethod *method=nullptr;unsigned count=0;}target;
                assemblyForeach(gameDomain,[](void *assembly,void *state){auto &target=*reinterpret_cast<Target *>(state);auto image=mono_assembly_get_image(reinterpret_cast<MonoAssembly *>(assembly));
                    auto klass=image?mono_class_from_name(image,"","MessageBox"):nullptr;if(klass){++target.count;target.method=mono_class_get_method_from_name(klass,"SetText",1);}},&target);
                messageMethod=mono_class_get_method_from_name(displayClass,"BindMessage",2);
                auto address=target.method?reinterpret_cast<uintptr_t>(mono_compile_method(target.method)):0;
                auto info=address?mono_jit_info_table_find(gameDomain,reinterpret_cast<char *>(address)):nullptr;
                HookParam hook;hook.address=address;hook.type=NO_CONTEXT;hook.text_fun=bindMessage;hook.jittype=JITTYPE::UNITY;
                display=target.count==1&&messageMethod&&info&&jitMethod(info)==target.method&&jitStart(info)==reinterpret_cast<void *>(address)&&!addresses.count(address)&&NewHook(hook,"YarnSpinnerMessageBind");
            }
        }
        if(display&&displayClass)
        {
            auto ready=mono_class_get_method_from_name(displayClass,"get_InterfaceSupported",0);
            MonoObject *error=nullptr;auto value=ready?mono_runtime_invoke(ready,nullptr,nullptr,&error):nullptr;
            if(value&&!error&&*reinterpret_cast<BYTE *>(mono_object_unbox(value)))
            {
                struct Target {MonoMethod *method=nullptr;unsigned count=0;}target;
                assemblyForeach(gameDomain,[](void *assembly,void *state){auto &target=*reinterpret_cast<Target *>(state);auto image=mono_assembly_get_image(reinterpret_cast<MonoAssembly *>(assembly));
                    auto klass=image?mono_class_from_name(image,"CupidChatroom.Yarn","ChatroomView"):nullptr;if(klass){++target.count;target.method=mono_class_get_method_from_name(klass,"Update",0);}},&target);
                frameMethod=mono_class_get_method_from_name(displayClass,"Frame",1);
                auto address=target.method?reinterpret_cast<uintptr_t>(mono_compile_method(target.method)):0;
                auto info=address?mono_jit_info_table_find(gameDomain,reinterpret_cast<char *>(address)):nullptr;
                HookParam hook;hook.address=address;hook.type=NO_CONTEXT;hook.text_fun=displayFrame;hook.jittype=JITTYPE::UNITY;
                bool frame=target.count==1&&frameMethod&&info&&jitMethod(info)==target.method&&jitStart(info)==reinterpret_cast<void *>(address)&&!addresses.count(address)&&NewHook(hook,"YarnSpinnerDisplayFrame");
                Msg::Log(frame?"YarnSpinner interface ready":"YarnSpinner interface unavailable");
            }
        }
        installed = true;
        Msg::Log(display?"YarnSpinner presentation ready":"YarnSpinner presentation unavailable");
        Msg::Log("YarnSpinner read-only views ready"); return true;
    }
}
