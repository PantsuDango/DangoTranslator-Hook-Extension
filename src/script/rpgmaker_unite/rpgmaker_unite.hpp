#pragma once

namespace rpgmakerunite
{
    std::atomic<uint64_t> sequence{0};
    MonoClassField *messagesField = nullptr;
    MonoClassField *indexField = nullptr;
    using ArrayLength = uintptr_t (*)(MonoArray *);
    using ArrayAddress = char *(*)(MonoArray *, int, uintptr_t);
    ArrayLength arrayLength = nullptr;
    ArrayAddress arrayAddress = nullptr;
    std::mutex choiceMutex;
    uintptr_t choiceOwner = 0;
    std::wstring choiceLabels;

    std::string typeName(MonoType *type)
    {
        auto klass = type && mono_class_from_mono_type ? mono_class_from_mono_type(type) : nullptr;
        if (!klass || !mono_class_get_name || !mono_class_get_namespace)
            return {};
        const char *name = mono_class_get_name(klass);
        const char *space = mono_class_get_namespace(klass);
        return name && space ? (space[0] ? std::string(space) + "." + name : std::string(name)) : std::string();
    }

    bool signature(MonoMethod *method, const std::vector<std::string> &parameters, const char *result)
    {
        if (!method || !mono_method_signature || !mono_signature_get_param_count || !mono_signature_get_params ||
            !mono_signature_get_return_type || !mono_signature_is_instance || !mono_get_root_domain || !mono_thread_attach || !mono_thread_detach)
            return false;
        auto domain = mono_get_root_domain();
        auto attached = domain ? mono_thread_attach(domain) : nullptr;
        if (!attached)
            return false;
        struct Attachment { MonoThread *thread; ~Attachment() { mono_thread_detach(thread); } } guard{attached};
        auto sig = mono_method_signature(method);
        if (!sig || !mono_signature_is_instance(sig) || mono_signature_get_param_count(sig) != parameters.size())
            return false;
        bool valid = typeName(mono_signature_get_return_type(sig)) == result;
        void *iterator = nullptr;
        for (const auto &expected : parameters)
            valid = valid && typeName(mono_signature_get_params(sig, &iterator)) == expected;
        return valid;
    }

    MonoMethod *method(const char *assembly, const char *space, const char *klass, const char *name,
                       const std::vector<std::string> &parameters, const char *result)
    {
        auto found = reinterpret_cast<MonoMethod *>(tryfindmonoil2cppMethod(assembly, space, klass, name,
                                                                             static_cast<int>(parameters.size()), true));
        return signature(found, parameters, result) ? found : nullptr;
    }

    std::wstring jsonString(std::wstring_view value)
    {
        std::wstring result = L"\"";
        for (wchar_t character : value)
        {
            if (character == L'"' || character == L'\\') result += L'\\';
            if (character == L'\n') result += L"\\n";
            else if (character == L'\r') result += L"\\r";
            else if (character == L'\t') result += L"\\t";
            else if (character < 32) return L"\"\"";
            else result += character;
        }
        return result + L"\"";
    }

    void emit(TextBuffer *buffer, std::wstring_view text, std::wstring_view speaker = L"")
    {
        if (text.empty() || text.size() > 32768) return;
        buffer->from(L"{\"page\":" + std::to_wstring(sequence.fetch_add(1)+1) +
                     L",\"text\":" + jsonString(text) + L",\"speaker\":" + jsonString(speaker) + L"}");
    }

    void captureString(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        auto text = commonsolvemonostring(context->argof(2, hp));
        if (text) emit(buffer, text.value());
    }

    void captureCustomPage(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        if (!messagesField || !indexField || !mono_field_get_value || !arrayLength || !arrayAddress)
            return;
        auto instance = reinterpret_cast<MonoObject *>(context->argof(1, hp));
        if (!instance) return;
        MonoArray *messages = nullptr;
        int index = -1;
        mono_field_get_value(instance, messagesField, &messages);
        mono_field_get_value(instance, indexField, &index);
        if (!messages || index < 0 || index >= 1024 || static_cast<uintptr_t>(index) >= arrayLength(messages))
            return;
        auto address = arrayAddress(messages, sizeof(void *), static_cast<uintptr_t>(index));
        if (!address) return;
        auto text = commonsolvemonostring(reinterpret_cast<uintptr_t>(*reinterpret_cast<void **>(address)));
        if (text) emit(buffer, text.value());
    }

    void recordCustomChoices(hook_context *context, HookParam *hp, TextBuffer *, uintptr_t *)
    {
        auto first = commonsolvemonostring(context->argof(3, hp));
        auto second = commonsolvemonostring(context->argof(4, hp));
        if (!first || !second || first->empty() || second->empty()) return;
        std::lock_guard<std::mutex> guard(choiceMutex);
        choiceOwner = context->argof(1, hp);
        choiceLabels = std::wstring(first.value()) + L"\n" + std::wstring(second.value());
    }

    void captureCustomChoices(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        std::wstring labels;
        {
            std::lock_guard<std::mutex> guard(choiceMutex);
            if (choiceOwner != context->argof(1, hp)) return;
            labels = std::move(choiceLabels);
            choiceOwner = 0;
        }
        emit(buffer, labels);
    }

    bool addHook(uintptr_t address, void (*callback)(hook_context *, HookParam *, TextBuffer *, uintptr_t *),
                 const char *function, const char *name)
    {
        if (!address) return false;
        HookParam parameter;
        parameter.address = address;
        // Mono JIT methods can be too far from MinHook's x64 trampoline pool.
        // Use the upstream VEH path for these few entry points explicitly.
        parameter.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT | BREAK_POINT;
        parameter.text_fun = callback;
        parameter.jittype = JITTYPE::UNITY;
        strcpy_s(parameter.function, function);
        return NewHook(parameter, name);
    }

    bool install()
    {
        HMODULE runtime = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!runtime || GetModuleHandleW(L"GameAssembly.dll")) return false;
        monofunctions::init(runtime);
        constexpr auto core = "RPGMaker.CodeBase.CoreSystem";
        constexpr auto messageSpace = "RPGMaker.Codebase.Runtime.Common.Component.Hud.Message";
        constexpr auto hudSpace = "RPGMaker.Codebase.Runtime.Common.Component";
        auto body = method(core, messageSpace, "MessageWindow", "ShowMessage", {"System.String"}, "System.Void");
        auto name = method(core, messageSpace, "MessageWindow", "ShowName", {"System.String"}, "System.Void");
        auto choice = method(core, hudSpace, "HudHandler", "ActiveSelectFrame", {"System.String"}, "System.Void");
        if (!body || !name || !choice) return false;
        auto bodyAddress = tryfindmonoil2cpp(core, messageSpace, "MessageWindow", "ShowMessage", 1, true);
        auto nameAddress = tryfindmonoil2cpp(core, messageSpace, "MessageWindow", "ShowName", 1, true);
        auto choiceAddress = tryfindmonoil2cpp(core, hudSpace, "HudHandler", "ActiveSelectFrame", 1, true);
        if (!bodyAddress || !nameAddress || !choiceAddress) return false;
        if (!addHook(bodyAddress, captureString, "RPGMaker.CodeBase.CoreSystem:MessageWindow:ShowMessage:1", "RPGMakerUniteDialogue") ||
            !addHook(nameAddress, captureString, "RPGMaker.CodeBase.CoreSystem:MessageWindow:ShowName:1", "RPGMakerUniteName") ||
            !addHook(choiceAddress, captureString, "RPGMaker.CodeBase.CoreSystem:HudHandler:ActiveSelectFrame:1", "RPGMakerUniteChoices"))
            return false;

        // TouchScene is an optional layout extension. Standard Unite hooks stay
        // active when this game's custom class is absent or incompatible.
        auto page = method("Assembly-CSharp", "", "MessageWindow", "ShowCurrentMessage", {}, "System.Void");
        auto labels = method("Assembly-CSharp", "", "MessageWindow", "ShowWithChoice",
                             {"System.String[]", "System.String", "System.String", "System.String[]", "System.String[]", "System.Action"}, "System.Void");
        auto show = method("Assembly-CSharp", "", "MessageWindow", "ShowChoiceButtons", {}, "System.Void");
        if (page && labels && show)
        {
            auto klass = reinterpret_cast<MonoClass *>(tryfindmonoil2cppClass("Assembly-CSharp", "", "MessageWindow", true));
            if (!mono_field_get_value)
                mono_field_get_value = reinterpret_cast<decltype(mono_field_get_value)>(GetProcAddress(runtime, "mono_field_get_value"));
            arrayLength = reinterpret_cast<ArrayLength>(GetProcAddress(runtime, "mono_array_length"));
            arrayAddress = reinterpret_cast<ArrayAddress>(GetProcAddress(runtime, "mono_array_addr_with_size"));
            messagesField = klass && mono_class_get_field_from_name ? mono_class_get_field_from_name(klass, "currentMessages") : nullptr;
            indexField = klass && mono_class_get_field_from_name ? mono_class_get_field_from_name(klass, "currentMessageIndex") : nullptr;
            if (messagesField && indexField && mono_field_get_type &&
                typeName(mono_field_get_type(messagesField)) == "System.String[]" &&
                typeName(mono_field_get_type(indexField)) == "System.Int32" && mono_field_get_value && arrayLength && arrayAddress)
            {
                addHook(tryfindmonoil2cpp("Assembly-CSharp", "", "MessageWindow", "ShowCurrentMessage", 0, true),
                        captureCustomPage, "Assembly-CSharp:MessageWindow:ShowCurrentMessage:0", "RPGMakerUniteDialogue");
                addHook(tryfindmonoil2cpp("Assembly-CSharp", "", "MessageWindow", "ShowWithChoice", 6, true),
                        recordCustomChoices, "Assembly-CSharp:MessageWindow:ShowWithChoice:6", "RPGMakerUniteChoiceState");
                addHook(tryfindmonoil2cpp("Assembly-CSharp", "", "MessageWindow", "ShowChoiceButtons", 0, true),
                        captureCustomChoices, "Assembly-CSharp:MessageWindow:ShowChoiceButtons:0", "RPGMakerUniteChoices");
            }
        }
        return true;
    }
}
