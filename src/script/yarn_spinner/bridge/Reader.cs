using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace Dango.YarnSpinner
{
    // Reader 只读取引擎已经本地化和解析的显示参数，不调用游戏推进回调
    public static partial class Reader
    {
        static readonly object Gate = new object();
        sealed class PageIdentity { internal long Sequence; }
        static readonly ConditionalWeakTable<object, PageIdentity> Seen = new ConditionalWeakTable<object, PageIdentity>();
        static long sequence;
        static Type lineType, optionType, viewType;
        static PropertyInfo textProperty, speakerProperty, attributeName;
        static FieldInfo optionLine, available, markupText, markupAttributes;
        static FieldInfo restoring;
        static MethodInfo displayObserve, displayReset;
        static bool wasRestoring;
        public static string Failure { get; private set; }
        public static bool Presentation { get; private set; }
        public static string PresentationFailure { get; private set; }
        internal static Type LineType { get { return lineType; } }
        internal static bool Restoring { get { return restoring != null && (bool)restoring.GetValue(null); } }
        internal static string DisplayText(object value) { return LineText(value); }
        internal static string DisplaySpeaker(object value) { return (string)speakerProperty.GetValue(value, null) ?? ""; }

        // Validate 在唯一游戏执行域内核对完整方法契约和显示字段
        public static bool Validate()
        {
            try
            {
                Failure = "assembly";
                Assembly yarn = null;
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if (assembly.GetType("Yarn.Unity.DialogueRunner", false) != null)
                    { if (yarn != null) return false; yarn = assembly; }
                if (yarn == null) return false;
                foreach (Contract contract in Contracts) { Failure = "contract:" + contract.Class + "." + contract.Name; if (!Check(yarn, contract)) return false; }
                Failure = "fields";
                lineType = yarn.GetType("Yarn.Unity.LocalizedLine", true);
                optionType = yarn.GetType("Yarn.Unity.DialogueOption", true);
                viewType = yarn.GetType("Yarn.Unity.DialogueViewBase", true);
                foreach (string field in new string[] { "TextID", "RawText" })
                    if (!Field(lineType, field, typeof(string))) return false;
                if (!Field(lineType, "Substitutions", typeof(string[])) || !Field(lineType, "Metadata", typeof(string[]))) return false;
                if (!Field(optionType, "Line", lineType) || !Field(optionType, "IsAvailable", typeof(bool)) ||
                    !Field(optionType, "DialogueOptionID", typeof(int)) || !Field(optionType, "TextID", typeof(string))) return false;
                MethodInfo runLine = viewType.GetMethod("RunLine", BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                MethodInfo runOptions = viewType.GetMethod("RunOptions", BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                if (!Entry(runLine, lineType, typeof(Action)) || !Entry(runOptions, optionType.MakeArrayType(), typeof(Action<int>))) return false;
                textProperty = lineType.GetProperty("TextWithoutCharacterName");
                speakerProperty = lineType.GetProperty("CharacterName");
                Type markup = textProperty.PropertyType;
                if (markup.FullName != "Yarn.Markup.MarkupParseResult") return false;
                markupText = markup.GetField("Text"); markupAttributes = markup.GetField("Attributes");
                if (markupText == null || markupText.FieldType != typeof(string) || markupAttributes == null) return false;
                Type attribute = markup.Assembly.GetType("Yarn.Markup.MarkupAttribute", true);
                attributeName = attribute.GetProperty("Name");
                if (attributeName.PropertyType != typeof(string)) return false;
                optionLine = optionType.GetField("Line"); available = optionType.GetField("IsAvailable");
                // 保存回放开关是可选游戏扩展，只接受派生对话视图上的自动静态布尔属性
                restoring = null;
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (assembly == typeof(Reader).Assembly) continue;
                    foreach (Type type in assembly.GetTypes())
                    {
                        if (!viewType.IsAssignableFrom(type)) continue;
                        foreach (MethodInfo entry in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
                        {
                            if (entry.Name == "RunLine" && !Entry(entry, lineType, typeof(Action))) return false;
                            if (entry.Name == "RunOptions" && !Entry(entry, optionType.MakeArrayType(), typeof(Action<int>))) return false;
                        }
                        PropertyInfo property = type.GetProperty("IsRestoring", BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly);
                        if (property == null) continue;
                        FieldInfo field = type.GetField("<IsRestoring>k__BackingField", BindingFlags.NonPublic | BindingFlags.Static);
                        if (restoring != null || property.PropertyType != typeof(bool) || field == null || field.FieldType != typeof(bool)) return false;
                        byte[] code = property.GetGetMethod().GetMethodBody().GetILAsByteArray();
                        if (code.Length != 6 || code[0] != 0x7e || code[5] != 0x2a || BitConverter.ToInt32(code, 1) != field.MetadataToken) return false;
                        restoring = field;
                    }
                }
                Assembly game = null;
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if (assembly.GetType("MessageBox", false) != null) { if (game != null) { game = null; break; } game = assembly; }
                Presentation = false;
                try
                {
                    Type display = typeof(Reader).Assembly.GetType("Dango.YarnSpinner.Display", true);
                    MethodInfo validate = display.GetMethod("Validate", BindingFlags.Static | BindingFlags.NonPublic);
                    Presentation = (bool)validate.Invoke(null, new object[] { yarn, game });
                    if (Presentation)
                    {
                        displayObserve = display.GetMethod("Observe", BindingFlags.Static | BindingFlags.NonPublic);
                        displayReset = display.GetMethod("Invalidate", BindingFlags.Static | BindingFlags.NonPublic);
                    }
                }
                catch { Presentation = false; displayObserve = displayReset = null; }
                Failure = ""; return true;
            }
            catch (Exception exception) { Failure += ":" + exception.GetType().Name; return false; }
        }

        static bool Field(Type type, string name, Type kind)
        { FieldInfo field = type.GetField(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly); return field != null && field.FieldType == kind; }
        static bool Entry(MethodInfo method, Type first, Type second)
        { if (method == null || !method.IsVirtual || method.IsStatic || method.ReturnType != typeof(void)) return false; ParameterInfo[] p = method.GetParameters(); return p.Length == 2 && p[0].ParameterType == first && p[1].ParameterType == second; }

        // Capture 按本地化参数对象去除多视图重复，真实再次执行的新对象仍产生新页
        public static string Capture(object value, object view, int kind)
        {
            lock (Gate)
            {
                try
                {
                    if (lineType == null || value == null || view == null || !viewType.IsInstanceOfType(view)) return null;
                    bool replay = restoring != null && (bool)restoring.GetValue(null);
                    if (replay)
                    { ResetDisplay(); if (wasRestoring) return null; wasRestoring = true; return Packet("reset", "", ""); }
                    wasRestoring = false;
                    PageIdentity found;
                    if (Seen.TryGetValue(value, out found)) { ObserveDisplay(value, view, found.Sequence, kind == 1); return null; }
                    if (kind == 1)
                    {
                        if (!lineType.IsInstanceOfType(value)) return null;
                        string text = LineText(value); if (String.IsNullOrWhiteSpace(text)) { ResetDisplay(); return null; }
                        long id = ++sequence; Seen.Add(value, new PageIdentity { Sequence = id });
                        ObserveDisplay(value, view, id, true);
                        return Packet("line", text, (string)speakerProperty.GetValue(value, null) ?? "", id);
                    }
                    if (kind != 2 || value.GetType() != optionType.MakeArrayType()) return null;
                    Array options = (Array)value; if (options.Length == 0 || options.Length > 128) return null;
                    StringBuilder result = new StringBuilder();
                    foreach (object option in options)
                    {
                        if (option == null || !(bool)available.GetValue(option)) continue;
                        string text = LineText(optionLine.GetValue(option));
                        if (String.IsNullOrWhiteSpace(text)) continue;
                        if (result.Length != 0) result.Append('\n'); result.Append(text);
                    }
                    if (result.Length == 0) { ResetDisplay(); return null; }
                    long menu = ++sequence; Seen.Add(value, new PageIdentity { Sequence = menu }); ObserveDisplay(value, view, menu, false);
                    return Packet("options", result.ToString(), "", menu);
                }
                catch { return null; }
            }
        }

        // ObserveDisplay 可选显示依赖加载失败时仍保留原有只读采集
        static void ObserveDisplay(object value, object view, long page, bool body)
        { try { if (displayObserve != null) displayObserve.Invoke(null, new object[] { value, view, page, body }); } catch { } }
        static void ResetDisplay()
        { try { if (displayReset != null) displayReset.Invoke(null, null); } catch { } }

        static string LineText(object line)
        {
            if (line == null || !lineType.IsInstanceOfType(line)) return null;
            object markup = textProperty.GetValue(line, null);
            // 图像显示标记的内容是资源键，不能当作对白
            System.Collections.IEnumerable attrs = markupAttributes.GetValue(markup) as System.Collections.IEnumerable;
            if (attrs == null) return null;
            foreach (object attr in attrs)
            { string name = (string)attributeName.GetValue(attr, null); if (name == "emote" || name == "selfie") return null; }
            return (string)markupText.GetValue(markup);
        }

        static string Packet(string kind, string text, string speaker, long id = 0)
        {
            if (text.Length > 12000 || speaker.Length > 512 || text.IndexOf('\0') >= 0 || speaker.IndexOf('\0') >= 0) return null;
            string packet = "{\"sequence\":" + (id == 0 ? ++sequence : id).ToString(System.Globalization.CultureInfo.InvariantCulture) + ",\"kind\":" + Quote(kind) + ",\"text\":" + Quote(text) + ",\"speaker\":" + Quote(speaker) + "}";
            return packet.Length <= 16000 ? packet : null;
        }

        static string Quote(string value)
        {
            StringBuilder b = new StringBuilder("\"");
            foreach (char c in value)
            {
                if (c == '"' || c == '\\') { b.Append('\\'); b.Append(c); }
                else if (c < 32) b.Append("\\u" + ((int)c).ToString("x4"));
                else b.Append(c);
            }
            return b.Append('"').ToString();
        }

        internal sealed class Contract
        {
            internal string Class, Name, Code, Signature, Locals, Handlers;
            internal int[] Offsets; internal string[] Tokens; internal bool Static;
            internal Contract(string c, string n, string s, string code, string locals, int[] offsets, string[] tokens, bool isStatic = false, string handlers = "")
            { Class = c; Name = n; Signature = s; Code = code; Locals = locals; Offsets = offsets; Tokens = tokens; Static = isStatic; Handlers = handlers; }
        }

        internal static bool CheckDisplay(Assembly assembly, Contract[] contracts)
        { try { foreach (Contract c in contracts) { PresentationFailure = assembly.GetName().Name + ":" + c.Class + "." + c.Name; if (!Check(assembly, c)) return false; } PresentationFailure = ""; return contracts.Length != 0; } catch { return false; } }

        static bool Check(Assembly assembly, Contract c)
        {
            Type type = assembly.GetType(c.Class, false); MethodInfo method = null;
            if (type == null && c.Class.StartsWith("<>", StringComparison.Ordinal))
                foreach (Type candidate in assembly.GetTypes()) if (candidate.IsNested && candidate.Name == c.Class)
                { if (type != null) return false; type = candidate; }
            if (type == null) return false;
            foreach (MethodInfo candidate in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly))
                if (candidate.Name == c.Name) { if (method != null) return false; method = candidate; }
            if (method == null || method.IsStatic != c.Static || method.GetMethodImplementationFlags() != MethodImplAttributes.IL) return false;
            Module module = method.Module; MethodBody body = method.GetMethodBody();
            if (body == null || MethodSignature(method) != c.Signature) return false;
            List<string> handlers = new List<string>();
            foreach (ExceptionHandlingClause handler in body.ExceptionHandlingClauses)
            {
                if (handler.Flags != ExceptionHandlingClauseOptions.Finally) return false;
                handlers.Add("2|" + handler.TryOffset + "|" + handler.TryLength + "|" + handler.HandlerOffset + "|" + handler.HandlerLength);
            }
            if (String.Join(";", handlers.ToArray()) != c.Handlers) return false;
            string locals = body.LocalVariables.Count == 0 ? "" : "07";
            foreach (LocalVariableInfo local in body.LocalVariables) { if (local.IsPinned) return false; locals += "|" + SignatureType(local.LocalType); }
            if (locals != c.Locals) return false;
            byte[] code = body.GetILAsByteArray();
            for (int i = 0; i < c.Offsets.Length; i++)
            {
                int o = c.Offsets[i]; if (o < 0 || o + 4 > code.Length || Token(module, BitConverter.ToInt32(code, o)) != c.Tokens[i]) return false;
                Array.Clear(code, o, 4);
            }
            return Hex(code) == c.Code;
        }

        static string Hex(byte[] data) { StringBuilder b = new StringBuilder(); foreach (byte v in data) b.Append(v.ToString("x2")); return b.ToString(); }
        static string ReflectedType(Type t)
        {
            if (!t.IsGenericType) return t.IsNested ? t.Name : t.FullName;
            List<string> args = new List<string>();
            foreach (Type a in t.GetGenericArguments())
                args.Add(SignatureType(a));
            Type definition = t.GetGenericTypeDefinition();
            return (t.IsValueType ? "11:" : "12:") + (definition.IsNested ? definition.Name : definition.FullName) + "<" + String.Join(",", args.ToArray()) + ">";
        }
        static string SignatureType(Type type)
        {
            Type[] primitives = { typeof(void), typeof(bool), typeof(char), typeof(sbyte), typeof(byte), typeof(short), typeof(ushort), typeof(int), typeof(uint), typeof(long), typeof(ulong), typeof(float), typeof(double), typeof(string), typeof(IntPtr), typeof(UIntPtr), typeof(object) };
            string[] names = { "void", "bool", "char", "sbyte", "byte", "short", "ushort", "int", "uint", "long", "ulong", "float", "double", "string", "nativeint", "nativeuint", "object" };
            for (int i = 0; i < primitives.Length; i++) if (type == primitives[i]) return names[i];
            if (type.IsByRef) return "10:" + SignatureType(type.GetElementType());
            if (type.IsPointer) return "0f:" + SignatureType(type.GetElementType());
            if (type.IsArray) { if (type.GetArrayRank() != 1) throw new InvalidOperationException(); return "1d:" + SignatureType(type.GetElementType()); }
            if (type.IsGenericParameter) return (type.DeclaringMethod == null ? "13:" : "1e:") + type.GenericParameterPosition;
            if (!type.IsGenericType) return (type.IsValueType ? "11:" : "12:") + (type.IsNested ? type.Name : type.FullName);
            List<string> args = new List<string>(); foreach (Type argument in type.GetGenericArguments()) args.Add(SignatureType(argument));
            Type definition = type.GetGenericTypeDefinition();
            return (type.IsValueType ? "11:" : "12:") + (definition.IsNested ? definition.Name : definition.FullName) + "<" + String.Join(",", args.ToArray()) + ">";
        }
        static string MethodSignature(MethodBase method)
        {
            string signature = method.IsGenericMethod ? (method.IsStatic ? "10" : "30") + "g" + method.GetGenericArguments().Length : (method.IsStatic ? "00" : "20");
            MethodInfo info = method as MethodInfo; signature += "|" + (info == null ? "void" : SignatureType(info.ReturnType));
            foreach (ParameterInfo parameter in method.GetParameters()) signature += "|" + SignatureType(parameter.ParameterType);
            return signature;
        }
        static string Token(Module m, int token)
        {
            int table = (int)((uint)token >> 24);
            if (table == 0x70)
            {
                string value = m.ResolveString(token); bool special = false;
                foreach (char c in value) special |= c > 255 || c >= 1 && c <= 8 || c >= 14 && c <= 31 || c == 39 || c == 45 || c == 127;
                return "str:" + Hex(Encoding.Unicode.GetBytes(value)) + (special ? "01" : "00");
            }
            if (table == 1 || table == 2 || table == 27) return "type:" + ReflectedType(m.ResolveType(token));
            if (table == 43)
            {
                MethodInfo generic = (MethodInfo)m.ResolveMethod(token); MethodInfo definition = generic.GetGenericMethodDefinition();
                string result = "generic:" + (definition.Module == m ? "method:" : "member:") + ReflectedType(definition.DeclaringType) + ":" + definition.Name + ":" + MethodSignature(definition) + ":0a";
                foreach (Type argument in generic.GetGenericArguments()) result += "|" + SignatureType(argument);
                return result;
            }
            MemberInfo member = m.ResolveMember(token);
            string prefix = table == 4 ? "field:" : table == 6 ? "method:" : "member:";
            string owner = ReflectedType(member.DeclaringType); FieldInfo field = member as FieldInfo; string signature;
            if (field != null) signature = "06:" + SignatureType(field.FieldType);
            else
            {
                MethodBase method = member as MethodBase;
                if (member.DeclaringType.IsGenericType)
                {
                    method = null;
                    foreach (MethodInfo candidate in member.DeclaringType.GetGenericTypeDefinition().GetMethods(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic))
                        if (candidate.MetadataToken == member.MetadataToken) { method = candidate; break; }
                    if (method == null)
                        foreach (ConstructorInfo candidate in member.DeclaringType.GetGenericTypeDefinition().GetConstructors(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic))
                            if (candidate.MetadataToken == member.MetadataToken) { method = candidate; break; }
                    if (method == null) throw new InvalidOperationException();
                }
                signature = MethodSignature(method);
            }
            return prefix + owner + ":" + member.Name + ":" + signature;
        }
    }
}
