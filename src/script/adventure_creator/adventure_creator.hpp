#pragma once

namespace adventurecreator
{
    MonoMethod *nameMethod = nullptr;
    MonoMethod *countMethod = nullptr;
    MonoMethod *optionMethod = nullptr;
    MonoMethod *latestSpeechMethod = nullptr;
    MonoClass *kickStarterClass = nullptr;
    MonoClassField *dialogSingletonField = nullptr;
    MonoClassField *originalTextField = nullptr;
    MonoClassField *speakerField = nullptr;
    MonoClassField *speakerNameField = nullptr;
    MonoClassField *aliveField = nullptr;
    std::atomic<bool> initialSnapshotDone{false};
    std::atomic<uintptr_t> conversationOwner{0};
    std::atomic<uint64_t> conversationGeneration{1};
    std::atomic<uint64_t> capturedConversation{0};
    std::atomic<uint64_t> dialogueSequence{0};
    thread_local bool readingChoices = false;

    std::string typeName(MonoType *type)
    {
        auto klass = type && mono_class_from_mono_type ? mono_class_from_mono_type(type) : nullptr;
        if (!klass || !mono_class_get_name || !mono_class_get_namespace)
            return {};
        const char *name = mono_class_get_name(klass);
        const char *nameSpace = mono_class_get_namespace(klass);
        return name && nameSpace ? std::string(nameSpace) + "." + name : std::string();
    }

    std::wstring jsonString(std::wstring_view value)
    {
        std::wstring result = L"\"";
        for (wchar_t character : value)
        {
            if (character == L'"' || character == L'\\')
                result += L'\\';
            if (character == L'\n')
                result += L"\\n";
            else if (character == L'\r')
                result += L"\\r";
            else if (character == L'\t')
                result += L"\\t";
            else if (character < 32)
                return L"\"\"";
            else
                result += character;
        }
        return result + L"\"";
    }

    bool signature(MonoMethod *method, const std::vector<std::string> &parameters, const char *resultType)
    {
        if (!method || !mono_method_signature || !mono_signature_get_param_count || !mono_signature_get_params ||
            !mono_signature_get_return_type || !mono_signature_is_instance || !mono_get_root_domain || !mono_thread_attach || !mono_thread_detach)
            return false;
        auto domain = mono_get_root_domain();
        if (!domain)
            return false;
        auto attached = mono_thread_attach(domain);
        if (!attached)
            return false;
        struct SignatureAttachment
        {
            MonoThread *thread;
            ~SignatureAttachment() { mono_thread_detach(thread); }
        } attachment{attached};
        auto methodSignature = mono_method_signature(method);
        if (!methodSignature || !mono_signature_is_instance(methodSignature) || mono_signature_get_param_count(methodSignature) != parameters.size())
            return false;
        bool valid = typeName(mono_signature_get_return_type(methodSignature)) == resultType;
        void *iterator = nullptr;
        for (const auto &expected : parameters)
        {
            auto parameterType = mono_signature_get_params(methodSignature, &iterator);
            valid = valid && expected == typeName(parameterType);
        }
        return valid;
    }

    MonoMethod *method(const char *klass, const char *name, const std::vector<std::string> &parameters, const char *resultType)
    {
        auto found = reinterpret_cast<MonoMethod *>(tryfindmonoil2cppMethod("AC", "AC", klass, name, static_cast<int>(parameters.size()), true));
        return signature(found, parameters, resultType) ? found : nullptr;
    }

    MonoObject *invoke(MonoMethod *target, MonoObject *instance, void **arguments = nullptr)
    {
        if (!target || !instance || !mono_runtime_invoke)
            return nullptr;
        MonoObject *exception = nullptr;
        auto result = mono_runtime_invoke(target, instance, arguments, &exception);
        return exception ? nullptr : result;
    }

    std::wstring stringValue(MonoObject *value)
    {
        if (auto text = commonsolvemonostring(reinterpret_cast<uintptr_t>(value)))
            return std::wstring(text.value());
        return {};
    }

    void captureDialogue(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        auto body = commonsolvemonostring(context->argof(3, hp));
        if (!body || body->empty())
            return;
        std::wstring bodyCopy(body.value());
        initialSnapshotDone.store(true);
        auto speaker = reinterpret_cast<MonoObject *>(context->argof(2, hp));
        std::wstring label;
        if (speaker)
        {
            int language = 0;
            void *arguments[] = {&language};
            label = stringValue(invoke(nameMethod, speaker, arguments));
        }
        buffer->from(L"{\"page\":" + std::to_wstring(dialogueSequence.fetch_add(1)+1) + L",\"text\":" + jsonString(bodyCopy) + L",\"speaker\":" + jsonString(label) + L"}");
    }

    // captureInitialDialogue 只读同步附加前已显示的当前对白且不重复提交后续入口捕获的页面
    void captureInitialDialogue(hook_context *, HookParam *, TextBuffer *buffer, uintptr_t *)
    {
        if (initialSnapshotDone.load())
            return;
        auto domain = mono_get_root_domain();
        if (!domain)
            return;
        auto attached = mono_thread_attach(domain);
        if (!attached)
            return;
        struct SnapshotAttachment
        {
            MonoThread *thread;
            ~SnapshotAttachment() { mono_thread_detach(thread); }
        } attachment{attached};
        auto vtable = mono_class_vtable(domain, kickStarterClass);
        if (!vtable)
            return;
        MonoObject *dialog = nullptr;
        mono_field_static_get_value(vtable, dialogSingletonField, &dialog);
        if (!dialog)
            return;
        void *arguments[] = {nullptr};
        auto speech = invoke(latestSpeechMethod, dialog, arguments);
        if (!speech)
            return;
        bool alive = false;
        mono_field_get_value(speech, aliveField, &alive);
        if (!alive)
            return;
        MonoObject *bodyObject = nullptr;
        mono_field_get_value(speech, originalTextField, &bodyObject);
        auto body = stringValue(bodyObject);
        if (body.empty())
            return;
        MonoObject *speaker = nullptr;
        mono_field_get_value(speech, speakerField, &speaker);
        std::wstring label;
        if (speaker)
        {
            MonoObject *name = nullptr;
            mono_field_get_value(speech, speakerNameField, &name);
            label = stringValue(name);
        }
        if (!initialSnapshotDone.exchange(true))
            buffer->from(L"{\"page\":" + std::to_wstring(dialogueSequence.fetch_add(1)+1) + L",\"text\":" + jsonString(body) + L",\"speaker\":" + jsonString(label) + L"}");
    }

    // installInitialDialogueSnapshot 核对托管字段后注册一次性只读同步而不调用Unity显示逻辑
    bool installInitialDialogueSnapshot()
    {
        auto runtime = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!runtime)
            return false;
        if (!mono_field_get_value)
            mono_field_get_value = reinterpret_cast<decltype(mono_field_get_value)>(GetProcAddress(runtime, "mono_field_get_value"));
        if (!mono_field_get_type)
            mono_field_get_type = reinterpret_cast<decltype(mono_field_get_type)>(GetProcAddress(runtime, "mono_field_get_type"));
        if (!mono_class_get_field_from_name || !mono_field_get_type || !mono_field_get_flags ||
            !mono_field_get_value || !mono_field_static_get_value || !mono_class_vtable)
            return false;
        kickStarterClass = reinterpret_cast<MonoClass *>(tryfindmonoil2cppClass("AC", "AC", "KickStarter", true));
        auto speechClass = reinterpret_cast<MonoClass *>(tryfindmonoil2cppClass("AC", "AC", "Speech", true));
        latestSpeechMethod = reinterpret_cast<MonoMethod *>(tryfindmonoil2cppMethod("AC", "AC", "Dialog", "GetLatestSpeech", 1, true));
        if (!kickStarterClass || !speechClass || !latestSpeechMethod)
            return false;
        auto domain = mono_get_root_domain();
        auto attached = domain ? mono_thread_attach(domain) : nullptr;
        if (!attached)
            return false;
        struct MetadataAttachment
        {
            MonoThread *thread;
            ~MetadataAttachment() { mono_thread_detach(thread); }
        } attachment{attached};
        dialogSingletonField = mono_class_get_field_from_name(kickStarterClass, "dialogComponent");
        originalTextField = mono_class_get_field_from_name(speechClass, "originalText");
        speakerField = mono_class_get_field_from_name(speechClass, "<speaker>k__BackingField");
        speakerNameField = mono_class_get_field_from_name(speechClass, "realName");
        aliveField = mono_class_get_field_from_name(speechClass, "isAlive");
        if (!dialogSingletonField || !originalTextField || !speakerField || !speakerNameField || !aliveField ||
            !(mono_field_get_flags(dialogSingletonField) & 0x10) ||
            typeName(mono_field_get_type(dialogSingletonField)) != "AC.Dialog" ||
            typeName(mono_field_get_type(originalTextField)) != "System.String" ||
            typeName(mono_field_get_type(speakerField)) != "AC.Char" ||
            typeName(mono_field_get_type(speakerNameField)) != "System.String" ||
            typeName(mono_field_get_type(aliveField)) != "System.Boolean")
            return false;
        auto signatureInfo = latestSpeechMethod ? mono_method_signature(latestSpeechMethod) : nullptr;
        if (!signatureInfo || !mono_signature_is_instance(signatureInfo) || mono_signature_get_param_count(signatureInfo) != 1 ||
            typeName(mono_signature_get_return_type(signatureInfo)) != "AC.Speech")
            return false;
        void *iterator = nullptr;
        if (typeName(mono_signature_get_params(signatureInfo, &iterator)) != "AC.Menu")
            return false;
        HookParam snapshot;
        snapshot.address = reinterpret_cast<uintptr_t>(mono_get_root_domain);
        snapshot.type = DIRECT_READ | USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT;
        snapshot.text_fun = captureInitialDialogue;
        return NewHook(snapshot, "AdventureCreatorDialogue");
    }

    void openConversation(hook_context *context, HookParam *hp, TextBuffer *, uintptr_t *)
    {
        conversationOwner.store(context->argof(1, hp));
        conversationGeneration.fetch_add(1);
    }

    void captureChoices(hook_context *context, HookParam *hp, TextBuffer *buffer, uintptr_t *)
    {
        if (readingChoices)
            return;
        auto generation = conversationGeneration.load();
        if (capturedConversation.load() == generation)
            return;
        auto conversation = reinterpret_cast<MonoObject *>(context->argof(1, hp));
        auto owner = conversationOwner.load();
        if (owner && owner != reinterpret_cast<uintptr_t>(conversation))
            return;
        struct ChoiceReadGuard
        {
            ChoiceReadGuard() { readingChoices = true; }
            ~ChoiceReadGuard() { readingChoices = false; }
        } guard;
        auto boxedCount = invoke(countMethod, conversation);
        if (!boxedCount || !mono_object_unbox)
            return;
        int count = *reinterpret_cast<int *>(mono_object_unbox(boxedCount));
        if (count <= 0 || count > 64)
            return;
        std::wstring labels;
        for (int slot = 0; slot < count; ++slot)
        {
            void *arguments[] = {&slot};
            auto label = stringValue(invoke(optionMethod, conversation, arguments));
            if (!label.empty())
            {
                if (!labels.empty())
                    labels += L'\n';
                labels += label;
            }
        }
        if (!labels.empty())
        {
            capturedConversation.store(generation);
            buffer->from(L"{\"page\":" + std::to_wstring(dialogueSequence.fetch_add(1)+1) + L",\"text\":" + jsonString(labels) + L",\"speaker\":\"\"}");
        }
    }

    bool install()
    {
        HMODULE runtime = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!runtime || GetModuleHandleW(L"GameAssembly.dll"))
            return false;
        monofunctions::init(runtime);
        nameMethod = method("Char", "GetName", {"System.Int32"}, "System.String");
        countMethod = method("Conversation", "GetCount", {}, "System.Int32");
        optionMethod = method("Conversation", "GetOptionName", {"System.Int32"}, "System.String");
        auto optionWithID = method("Conversation", "GetOptionNameWithID", {"System.Int32"}, "System.String");
        auto dialogue = method("Dialog", "StartDialog", {"AC.Char", "System.String", "System.Boolean", "System.Int32", "System.Boolean", "System.Boolean", "UnityEngine.AudioClip", "UnityEngine.TextAsset"}, "AC.Speech");
        auto conversation = method("Conversation", "Interact", {"AC.ActionList", "AC.ActionConversation"}, "System.Void");
        if (!nameMethod || !countMethod || !optionMethod || !optionWithID || !dialogue || !conversation)
            return false;
        auto bodyAddress = tryfindmonoil2cpp("AC", "AC", "Dialog", "StartDialog", 8, true);
        auto choiceAddress = tryfindmonoil2cpp("AC", "AC", "Conversation", "GetOptionName", 1, true);
        auto conversationAddress = tryfindmonoil2cpp("AC", "AC", "Conversation", "Interact", 2, true);
        auto choiceIDAddress = tryfindmonoil2cpp("AC", "AC", "Conversation", "GetOptionNameWithID", 1, true);
        if (!bodyAddress || !choiceAddress || !conversationAddress || !choiceIDAddress)
            return false;
        HookParam body;
        body.address = bodyAddress;
        body.type = USING_STRING | CODEC_UTF16 | FULL_STRING | NO_CONTEXT;
        body.text_fun = captureDialogue;
        body.jittype = JITTYPE::UNITY;
        strcpy_s(body.function, "AC:AC:Dialog:StartDialog:8");
        HookParam choices = body;
        choices.address = choiceAddress;
        choices.text_fun = captureChoices;
        strcpy_s(choices.function, "AC:AC:Conversation:GetOptionName:1");
        HookParam state = body;
        state.address = conversationAddress;
        state.text_fun = openConversation;
        strcpy_s(state.function, "AC:AC:Conversation:Interact:2");
        bool stateInstalled = NewHook(state, "AdventureCreatorConversationState");
        bool choicesInstalled = NewHook(choices, "AdventureCreatorChoices");
        choices.address = choiceIDAddress;
        strcpy_s(choices.function, "AC:AC:Conversation:GetOptionNameWithID:1");
        bool choiceIDsInstalled = NewHook(choices, "AdventureCreatorChoices");
        bool bodyInstalled = stateInstalled && choicesInstalled && choiceIDsInstalled && NewHook(body, "AdventureCreatorDialogue");
        if (bodyInstalled)
            installInitialDialogueSnapshot();
        return stateInstalled && bodyInstalled && choicesInstalled && choiceIDsInstalled;
    }
}
