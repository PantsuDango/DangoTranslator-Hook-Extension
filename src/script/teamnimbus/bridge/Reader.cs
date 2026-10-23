using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using TMPro;
using UnityEngine;
using UnityEngine.UI;

namespace Dango.TeamNimbus
{
    // Reader 只在已确认的完整消息和独立显示消费者之间传递显示副本
    public static class Reader
    {
        const BindingFlags Fields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        static Type windowType, revealType, menuType, optionType, bubbleType;
        static Type introType, settingsType;
        static DateTime startupSilentUntil;
        static string uiKey;
        static readonly List<string> uiFormats = new List<string>();
        static readonly Dictionary<Text, LegacyBinding> uiLabels = new Dictionary<Text, LegacyBinding>();
        static FieldInfo revealField, nameField, optionTextField, optionIDField, optionsField, headerField;
        static readonly Queue<string> packets = new Queue<string>();
        static readonly Dictionary<object, Page> pages = new Dictionary<object, Page>();
        static readonly Dictionary<object, Option> options = new Dictionary<object, Option>();
        static readonly Dictionary<string, string> translations = new Dictionary<string, string>(StringComparer.Ordinal);
        static readonly Regex tags = new Regex(@"</?(?:b|i|u|s|color|size|alpha|rainbow|shake|speed|wiggle)(?:[ =][^<>]*)?>", RegexOptions.IgnoreCase);
        static long sequence;
        static int frames;
        static string cache, digest, failure;
        static readonly Queue<string> diagnostics = new Queue<string>();
        static readonly HashSet<string> diagnosticKeys = new HashSet<string>();
        static string nextCache;
        static volatile bool pendingConfigure;
        static bool configured, loaded, enabled, attempted;
        static int capabilities;
        static IntPtr lease, leaseMemory, host;
        static long hostCreated;
        static Pump pump;
        static TMP_FontAsset chinese;
        static Font bubbleFont;
        static readonly List<FontBinding> fonts = new List<FontBinding>();
        static readonly Dictionary<object, string> menuKeys = new Dictionary<object, string>();
        static readonly Dictionary<TMP_Text, TextBinding> labels = new Dictionary<TMP_Text, TextBinding>();
        static readonly Dictionary<object, object> speakers = new Dictionary<object, object>();
        static readonly Dictionary<object, BubbleBinding> bubbles = new Dictionary<object, BubbleBinding>();
        static readonly Dictionary<object, string> templates = new Dictionary<object, string>();
        static readonly Regex tokens = new Regex(@"\{[^{}\r\n]{1,256}\}|<(?:PLAYERNAME|PARTYSIZE|[A-Z_][A-Z_0-9]*)>");
        static readonly List<UnityEngine.Object> owned = new List<UnityEngine.Object>();
        static bool presentationContract, menuContract, menuReadable;
        public static string Failure { get { return failure; } }
        public static string Status { get { return loaded ? "ready " + digest : attempted ? "failed" : ""; } }
        public static string Diagnostic { get { return diagnostics.Count == 0 ? "" : diagnostics.Dequeue(); } }

        // Diagnose 每类原因仅记录一次，不记录原文、译文或玩家信息
        static void Diagnose(string reason)
        {
            if (diagnosticKeys.Count < 32 && diagnosticKeys.Add(reason)) diagnostics.Enqueue(reason);
        }

        sealed class Page
        {
            internal object Window, Reveal;
            internal string Speaker, Original, Applied, Template;
            internal long ID;
        }
        sealed class Option
        {
            internal Component Owner;
            internal TMP_Text Text;
            internal int ID;
            internal string Original, Applied;
            internal bool Clicked;
            internal long Revision;
        }
        sealed class FontBinding
        {
            internal TMP_Text Text;
            internal TMP_FontAsset Original, Private;
            internal Material OriginalMaterial;
        }
        sealed class TextBinding { internal string Original, Applied; }
        sealed class LegacyBinding {
            internal string Original, Applied;
            internal Font Font;
            internal Material Material;
            internal Vector3 Position, AppliedPosition;
            internal bool Moved;
        }
        sealed class BubbleBinding { internal Text Text; internal Font Font; internal string Original, Applied; }

        // Template 只记录游戏本次格式化的输入，绝不重放会消耗队列的格式化函数
        public static void Template(object window, string text)
        {
            if (!configured || window == null || text == null || text.Length > 16000) return;
            EnsurePump();
            templates[window] = text;
            if (templates.Count > 32) { templates.Clear(); templates[window] = text; }
        }

        // TemplateTranslation 仅用本次输入模板和实际输出还原替换值，占位符不一致整句回退
        static string TemplateTranslation(string template, string actual)
        {
            if (template == null || actual == null) return null;
            template = Normalize(template); actual = Normalize(actual);
            string translation;
            if (!translations.TryGetValue(template, out translation)) return null;
            MatchCollection found = tokens.Matches(template);
            if (found.Count == 0 || found.Count > 32) return null;
            List<string> before = new List<string>(), after = new List<string>();
            foreach (Match m in found) before.Add(m.Value);
            foreach (Match m in tokens.Matches(translation)) after.Add(m.Value);
            before.Sort(StringComparer.Ordinal); after.Sort(StringComparer.Ordinal);
            if (String.Join("\n", before.ToArray()) != String.Join("\n", after.ToArray())) return null;
            StringBuilder pattern = new StringBuilder("\\A"); int offset = 0;
            foreach (Match m in found) { pattern.Append(Regex.Escape(template.Substring(offset, m.Index-offset))).Append("([^\\r\\n]{0,1024}?)"); offset=m.Index+m.Length; }
            pattern.Append(Regex.Escape(template.Substring(offset))).Append("\\z");
            // 仅允许有足够原始文字锚点且占位符之间有分隔，防止多种分解导致值错位
            if (tokens.Replace(template, "").Trim().Length < 3) return null;
            for (int i=1;i<found.Count;i++) {
                int start=found[i-1].Index+found[i-1].Length;
                string separator=template.Substring(start,found[i].Index-start);
                if (separator.Length==0 || actual.IndexOf(separator,StringComparison.Ordinal)!=actual.LastIndexOf(separator,StringComparison.Ordinal)) return null;
            }
            Match result = Regex.Match(actual, pattern.ToString());
            if (!result.Success) return null;
            Dictionary<string,string> values = new Dictionary<string,string>(StringComparer.Ordinal);
            for (int i=0;i<found.Count;i++) {
                string previous, value=result.Groups[i+1].Value;
                if (value.IndexOfAny(new char[]{'<','>','{','}','\0'})>=0 || values.TryGetValue(found[i].Value,out previous) && previous!=value) return null;
                values[found[i].Value]=value;
            }
            string expanded=tokens.Replace(translation, delegate(Match m) { return values[m.Value]; });
            return Safe(expanded)?expanded:null;
        }

        // Nameplate 只记录说话者身份，玩家和自定义队员姓名不参与静态替换
        public static void Nameplate(object window, object character)
        {
            if (!configured) return;
            EnsurePump();
            if (window != null && character != null) speakers[window] = character;
        }

        // ApplyLabel 保存独立显示原文，外部更新文字后自动建立新绑定
        static string ApplyLabel(TMP_Text target, bool allow)
        {
            if (target == null) return "";
            TextBinding item;
            if (!labels.TryGetValue(target, out item) || target.text != item.Applied)
                labels[target] = item = new TextBinding { Original = target.text };
            string translated = enabled && allow ? Lookup(item.Original) : null;
            if (translated != null && AttachFont(target, translated)) { item.Applied = translated; target.text = translated; }
            return item.Original;
        }

        // StaticSpeaker 按角色结构排除所有玩家及自定义队员，不按姓名猜身份
        static bool StaticSpeaker(object window)
        {
            object character;
            if ((capabilities & 256) == 0 || !speakers.TryGetValue(window, out character)) return false;
            Type type = character.GetType();
            FieldInfo party = type.GetField("_assignedPartyCharacter", Fields), id = type.GetField("_characterID", Fields);
            if (party == null || id == null || !id.FieldType.IsEnum || party.GetValue(character) != null) return false;
            object limit;
            try { limit = Enum.Parse(id.FieldType, "Protagonist"); } catch { return false; }
            return Convert.ToInt64(id.GetValue(character)) > Convert.ToInt64(limit);
        }

        // Normalize 与Go及离线工具保持相同的已知标签语义
        public static string Normalize(string text)
        {
            return text == null ? "" : tags.Replace(text.Replace("\r\n", "\n").Replace('\r', '\n'), "").Trim('\ufeff', '\0', ' ', '\t', '\n');
        }

        // Method 只接受唯一实例方法和精确参数类型，不使用元数据token或机器码地址
        static MethodInfo Method(Type type, string name, Type result, params Type[] parameters)
        {
            MethodInfo found = null;
            foreach (MethodInfo m in type.GetMethods(Fields | BindingFlags.DeclaredOnly))
            {
                if (m.Name != name || m.IsStatic || m.ReturnType != result) continue;
                ParameterInfo[] p = m.GetParameters();
                if (p.Length != parameters.Length) continue;
                bool match = true;
                for (int i = 0; i < p.Length; i++) if (p[i].ParameterType != parameters[i]) match = false;
                if (!match) continue;
                if (found != null || m.GetMethodBody() == null) return null;
                found = m;
            }
            return found;
        }

        // Validate 独立核对运行时类型、显示字段和方法关系
        public static bool Validate()
        {
            try
            {
                Assembly found = null;
                foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
                    if (a.GetType("TeamNimbus.CloudMeadow.Dialogue.DialogWindow") != null)
                    { if (found != null) return false; found = a; }
                if (found == null) return false;
                windowType = found.GetType("TeamNimbus.CloudMeadow.Dialogue.DialogWindow", true);
                revealType = found.GetType("Game.UI.TMPDialogueReveal");
                menuType = found.GetType("TeamNimbus.CloudMeadow.Dialogue.DialogWindowOptionsManager");
                optionType = found.GetType("TeamNimbus.CloudMeadow.Dialogue.DialogWindowOptionClickable");
                bubbleType = found.GetType("TeamNimbus.CloudMeadow.UI.DialogueBubbleManager");
                introType = found.GetType("TeamNimbus.CloudMeadow.UI.IntroManager");
                settingsType = found.GetType("TeamNimbus.CloudMeadow.UI.GameplaySettingsManager");
                uiFormats.Clear();
                if (settingsType != null) {
                    List<string> literals = new List<string>();
                    List<MemberInfo> members = Members(Method(settingsType, "UpdateButtonLabels", typeof(void)), literals);
                    bool format = false, setter = false;
                    if (members != null) foreach (MemberInfo member in members) {
                        if (member.DeclaringType == typeof(StringBuilder) && member.Name == "AppendFormat") format = true;
                        if (member.DeclaringType == typeof(Text) && member.Name == "set_text") setter = true;
                    }
                    if (format && setter) foreach (string value in literals)
                        if (Regex.Matches(value, @"\{0\}").Count == 1 && Safe(value.Replace("{0}", "x"))) uiFormats.Add(value);
                }
                revealField = windowType.GetField("_tmpDialogueRevealText", Fields);
                nameField = windowType.GetField("_nameplateText", Fields);
                optionTextField = optionType == null ? null : optionType.GetField("_optionText", Fields);
                optionIDField = optionType == null ? null : optionType.GetField("_assignedID", Fields);
                optionsField = menuType == null ? null : menuType.GetField("_activeOptions", Fields);
                headerField = menuType == null ? null : menuType.GetField("_contextHeaderText", Fields);
                presentationContract = ValidatePresentation(found);
                menuContract = ValidateMenu();
                return Method(windowType, "RevealDialogRoutine", typeof(IEnumerator), typeof(string), typeof(string)) != null;
            }
            catch (Exception e) { failure = e.GetType().Name; return false; }
        }

        // Configure 安装器仅设置本会话路径，Unity对象必须等主线程建立
        public static void Configure(string directory)
        {
            startupSilentUntil = System.Diagnostics.Process.GetCurrentProcess().StartTime.ToUniversalTime().AddSeconds(3);
            nextCache = Path.GetFullPath(directory);
            pendingConfigure = true;
            configured = true;
        }

        // SetCapabilities 读取、显示和菜单分开判定，辅助入口失败不关闭正文读取
        public static void SetCapabilities(int value)
        {
            capabilities = value;
            menuReadable = (value & 8)!=0 && optionTextField!=null && typeof(TMP_Text).IsAssignableFrom(optionTextField.FieldType) && optionIDField!=null && optionIDField.FieldType==typeof(int) && optionsField!=null && headerField!=null;
            if ((value & 24) != 24) capabilities &= ~8;
            if (revealField == null || revealField.FieldType != revealType || nameField == null || !typeof(TMP_Text).IsAssignableFrom(nameField.FieldType)) capabilities &= ~4;
            if (!presentationContract || (value & 128) == 0) capabilities &= ~4;
            if (!menuContract || optionTextField == null || !typeof(TMP_Text).IsAssignableFrom(optionTextField.FieldType) || optionIDField == null || optionIDField.FieldType != typeof(int) || optionsField == null || headerField == null) capabilities &= ~8;
        }

        // Bind 在协程工厂读取完整原文，保留原协程和历史的字符串参数
        public static void Bind(object owner, string speaker, string text)
        {
            if (!configured || owner == null || !windowType.IsInstanceOfType(owner) || String.IsNullOrEmpty(text) || text.Length > 16000) return;
            EnsurePump();
            object reveal = revealField == null ? null : revealField.GetValue(owner);
            Page p = new Page { Window = owner, Reveal = reveal, Speaker = speaker ?? "", Original = text };
            string template;
            if (templates.TryGetValue(owner,out template)) { p.Template=template; templates.Remove(owner); }
            if (!enabled) { p.ID=++sequence; Emit(p.ID, owner, "body", text, p.Speaker, ""); }
            if (reveal == null) return;
            if (!pages.ContainsKey(reveal) && pages.Count>=32) {
                if (enabled) Emit(++sequence,owner,"body",text,p.Speaker,"");
                return;
            }
            pages[reveal] = p;
        }

        // InitializeOption 登记原始选项文字，之后只对显示对象生成副本
        public static void InitializeOption(object owner, int id, string text)
        {
            Component component = owner as Component;
            if (component == null || owner.GetType() != optionType || text == null || text.Length > 16000) return;
            EnsurePump();
            if (!options.ContainsKey(owner) && options.Count>=512) { Prune(); if (options.Count>=512) return; }
            options[owner] = new Option { Owner = component, Text = optionTextField.GetValue(owner) as TMP_Text, ID = id, Original = text, Revision = ++sequence };
        }

        // Click 在原处理器读取文字前恢复原文和原ID，不替换分支参数
        public static void Click(object owner)
        {
            Option item;
            if (owner != null && options.TryGetValue(owner, out item))
            {
                if (item.Text != null && item.Applied != null && item.Text.text == item.Applied) item.Text.text = item.Original;
                item.Clicked = true;
            }
        }

        // LegacyOptions 在旧菜单读取正文显示值之前恢复原文，随后由原函数清页
        public static void LegacyOptions(object owner)
        {
            if (!enabled || owner == null || revealField == null) return;
            object reveal = revealField.GetValue(owner);
            Page page;
            if (reveal == null || !pages.TryGetValue(reveal, out page) || page.Applied == null) return;
            TMP_Text target = revealType.GetField("_text", Fields).GetValue(reveal) as TMP_Text;
            if (target != null && Normalize(target.text) == Normalize(page.Applied)) target.text = page.Original;
        }

        // Members 解码完整IL操作数，仅解析元数据引用而不执行游戏代码
        static List<MemberInfo> Members(MethodBase method, List<string> literals = null)
        {
            if (method == null || method.GetMethodBody() == null) return null;
            Dictionary<short, OpCode> ops = new Dictionary<short, OpCode>();
            foreach (FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
            { if (f.FieldType == typeof(OpCode)) { OpCode op = (OpCode)f.GetValue(null); ops[op.Value] = op; } }
            byte[] code = method.GetMethodBody().GetILAsByteArray();
            List<MemberInfo> members = new List<MemberInfo>();
            try {
                int p = 0;
                while (p < code.Length) {
                    short value = code[p++];
                    if (value == 254) value = (short)(0xfe00 | code[p++]);
                    OpCode op;
                    if (!ops.TryGetValue(value, out op)) return null;
                    int size;
                    switch (op.OperandType) {
                    case OperandType.InlineNone: size=0; break;
                    case OperandType.ShortInlineI: case OperandType.ShortInlineVar: case OperandType.ShortInlineBrTarget: size=1; break;
                    case OperandType.InlineVar: size=2; break;
                    case OperandType.InlineI8: case OperandType.InlineR: size=8; break;
                    case OperandType.InlineSwitch: size=checked(4+4*BitConverter.ToInt32(code,p)); break;
                    default: size=4; break;
                    }
                    if (size < 0 || p+size > code.Length) return null;
                    if (op.OperandType == OperandType.InlineMethod || op.OperandType == OperandType.InlineField)
                        members.Add(method.Module.ResolveMember(BitConverter.ToInt32(code,p), method.DeclaringType.GetGenericArguments(), method.IsGenericMethod ? method.GetGenericArguments() : null));
                    if (literals != null && op.OperandType == OperandType.InlineString) literals.Add(method.Module.ResolveString(BitConverter.ToInt32(code,p)));
                    p += size;
                }
                return members;
            } catch { return null; }
        }

        // ValidatePresentation 按调用关系确认排版和原文读取用途，未知新增读取只关闭显示
        static bool ValidatePresentation(Assembly assembly)
        {
            if (revealType == null) return false;
            MethodInfo start = Method(revealType,"StartRevealingNewString",typeof(void),typeof(string));
            List<MemberInfo> refs = Members(start);
            if (refs == null) return false;
            bool clear=false, coroutine=false, reveal=false;
            foreach (MemberInfo m in refs) {
                if (m.Name=="ClearAndReset" && m.DeclaringType==revealType) clear=true;
                if (m.Name=="RevealCharacters" && m.DeclaringType==revealType) reveal=true;
                if (m.Name=="StartCoroutine" && typeof(MonoBehaviour).IsAssignableFrom(m.DeclaringType)) coroutine=true;
            }
            if (!clear || !coroutine || !reveal) return false;
            PropertyInfo property=revealType.GetProperty("Text",Fields);
            MethodInfo getter=property==null?null:property.GetGetMethod(true);
            if (getter==null) return false;
            int readers=0;
            foreach (Type type in assembly.GetTypes()) {
                foreach (MethodInfo method in type.GetMethods(Fields|BindingFlags.Static|BindingFlags.DeclaredOnly)) {
                    List<MemberInfo> references=Members(method);
                    if (references==null) { if (method.GetMethodBody()!=null) return false; continue; }
                    foreach (MemberInfo member in references) {
                        if (member.Module != getter.Module || member.MetadataToken != getter.MetadataToken) continue;
                        if (method.DeclaringType != windowType || method.Name != "Legacy_ShowOptions") return false;
                        readers++;
                    }
                }
            }
            return readers==1;
        }

        // ValidateMenu 核对点击只把原ID和文字传给原委托，未知文本消费者关闭菜单替换
        static bool ValidateMenu()
        {
            if (optionType==null || optionTextField==null || optionIDField==null) return false;
            MethodInfo click=Method(optionType,"HandleClicked",typeof(void));
            List<MemberInfo> refs=Members(click);
            if (refs==null || refs.Count!=5) return false;
            bool id=false, text=false, getter=false, callback=false, invoke=false;
            foreach (MemberInfo m in refs) {
                if (m.Module==optionIDField.Module && m.MetadataToken==optionIDField.MetadataToken) id=true;
                else if (m.Module==optionTextField.Module && m.MetadataToken==optionTextField.MetadataToken) text=true;
                else if (m.DeclaringType==typeof(TMP_Text) && m.Name=="get_text") getter=true;
                else if (m.DeclaringType==optionType && m.Name=="_clickCallback") callback=true;
                else if (m.Name=="Invoke" && typeof(Delegate).IsAssignableFrom(m.DeclaringType)) invoke=true;
                else return false;
            }
            if (!id || !text || !getter || !callback || !invoke) return false;
            foreach (MethodInfo m in optionType.GetMethods(Fields|BindingFlags.DeclaredOnly)) {
                List<MemberInfo> members=Members(m);
                if (members==null) { if (m.GetMethodBody()!=null) return false; continue; }
                foreach (MemberInfo member in members)
                    if (member.Module==optionTextField.Module && member.MetadataToken==optionTextField.MetadataToken && m.Name!="Initialize" && m.Name!="OnDisable" && m.Name!="HandleClicked") return false;
            }
            return true;
        }

        // Display 仅替换正文排版消费者参数，原状态机和历史继续持有原文
        public static string Display(object owner, string text)
        {
            Page page;
            if (!enabled || (capabilities & 4) == 0 || text == null || !pages.TryGetValue(owner, out page) || page.Original != text) return text;
            string translated = Lookup(text) ?? TemplateTranslation(page.Template, text);
            if (translated == null) Diagnose("body:no-safe-mapping");
            page.Applied = null;
            try {
            TMP_Text target = revealType.GetField("_text", Fields).GetValue(owner) as TMP_Text;
            if (translated != null && AttachFont(target, translated)) page.Applied = translated;
            } catch (Exception e) { Diagnose("body:" + e.GetType().Name); page.Applied=null; }
            try {
            TMP_Text name = nameField.GetValue(page.Window) as TMP_Text;
            ApplyLabel(name, StaticSpeaker(page.Window));
            } catch { }
            if (page.ID==0) page.ID=++sequence;
            Emit(page.ID, page.Window, "body", page.Original, page.Speaker, page.Applied ?? "");
            return page.Applied ?? text;
        }

        // Bubble 在逐字和WordWrap之前获取完整消息，气泡不与主窗口页面合并
        public static string Bubble(object owner, string text)
        {
            if (!configured || owner == null || owner.GetType() != bubbleType || String.IsNullOrEmpty(text) || text.Length > 16000) return text;
            EnsurePump();
            string translated = enabled ? Lookup(text) : null;
            FieldInfo messageText = bubbleType.GetField("messageText", Fields);
            Text target = messageText==null?null:messageText.GetValue(owner) as Text;
            BubbleBinding prior;
            if (bubbles.TryGetValue(owner, out prior)) {
                if (prior.Text != null && prior.Text.font == bubbleFont) prior.Text.font = prior.Font;
                bubbles.Remove(owner);
            }
            try {
            if (translated != null && target != null)
            {
                if (bubbleFont == null) bubbleFont = CreateBubbleFont();
                bool safe = bubbleFont != null;
                if (safe) foreach (char c in translated) if (!Char.IsWhiteSpace(c) && !bubbleFont.HasCharacter(c)) safe = false;
                if (safe) {
                    BubbleBinding previous;
                    Font originalFont = bubbles.TryGetValue(owner, out previous) && target.font == bubbleFont ? previous.Font : target.font;
                    bubbles[owner] = new BubbleBinding { Text = target, Font = originalFont, Original = text, Applied = translated };
                    target.font = bubbleFont;
                } else translated = null;
            }
            else translated = null;
            } catch { translated=null; }
            Emit(++sequence, owner, "bubble", text, "", translated ?? "");
            return translated ?? text;
        }

        // CreateBubbleFont 按中英文名称匹配实际安装字体，缺失微软雅黑时使用宋体等中文字体
        static Font CreateBubbleFont()
        {
            string[] installed = Font.GetOSInstalledFontNames();
            List<string> names = new List<string>();
            foreach (string alias in new string[] { "Microsoft YaHei", "微软雅黑", "SimSun", "宋体", "NSimSun", "新宋体", "SimHei", "黑体", "DengXian", "等线" })
                foreach (string name in installed)
                    if (String.Equals(name.Trim().TrimStart('@'), alias, StringComparison.OrdinalIgnoreCase) && !names.Contains(name)) names.Add(name);
            if (names.Count == 0) return null;
            Font font = Font.CreateDynamicFontFromOSFont(names.ToArray(), 20);
            if (font != null) owned.Add(font);
            return font;
        }

        // Safe 拒绝有状态模板、未知标签和超限译文，局部原文回退
        static bool Safe(string value)
        {
            if (String.IsNullOrEmpty(value) || value.Length > 16000 || value.IndexOfAny(new char[] {'<', '>', '{', '}', '\0'}) >= 0) return false;
            foreach (char c in value) if (Char.IsSurrogate(c) || (c < 32 && c != '\n' && c != '\r' && c != '\t')) return false;
            return true;
        }
        // Lookup 不保存未命中，不猜测动态参数与剧情条件
        static string Lookup(string original)
        {
            string translated;
            string key = Normalize(original);
            return Safe(key) && translations.TryGetValue(key, out translated) && Safe(translated) ? translated : null;
        }
        // Quote 转义完整JSON页包，保留原始Unicode文本
        static string Quote(string value)
        {
            StringBuilder s = new StringBuilder("\"");
            foreach (char c in value ?? "")
            {
                if (c == '"' || c == '\\') s.Append('\\').Append(c);
                else if (c < 32) s.Append("\\u").Append(((int)c).ToString("x4"));
                else s.Append(c);
            }
            return s.Append('"').ToString();
        }
        // Emit 记录真实完整页及实际应用译文，不回抓显示控件中的中文
        static void Emit(long id, object owner, string kind, string text, string speaker, string applied)
        {
            Component c = owner as Component;
            if (c == null) return;
            while (packets.Count >= 8) packets.Dequeue();
            packets.Enqueue("{\"sequence\":" + id + ",\"owner\":" + Math.Abs((long)c.GetInstanceID()) + ",\"kind\":" + Quote(kind) + ",\"text\":" + Quote(text) + ",\"speaker\":" + Quote(speaker) + ",\"translation\":" + Quote(applied) + ",\"backfill\":" + (enabled ? "true" : "false") + "}");
        }

        // EnsurePump 主线程创建独立生命周期组件，解绑后仍可恢复字体和显示
        static void EnsurePump()
        {
            if (pendingConfigure) {
                Stop();
                diagnostics.Clear(); diagnosticKeys.Clear();
                cache = nextCache;
                attempted = false; failure = null; digest = null;
                pendingConfigure = false;
            }
            if (pump == null) {
                GameObject go = new GameObject("DangoTeamNimbusDisplay");
                UnityEngine.Object.DontDestroyOnLoad(go);
                pump = go.AddComponent<Pump>();
            }
            if (leaseMemory == IntPtr.Zero && !attempted) OpenLease();
            if (!attempted) LoadSnapshot();
        }
        // Frame 经已确认Unity回调泵出完整页，普通读取不会触发资源写入
        public static string Frame()
        {
            if (!configured) return null;
            EnsurePump();
            if (leaseMemory!=IntPtr.Zero && !LeaseValid()) Stop();
            if (++frames%120==0) Prune();
            // 辅助菜单失败不能堵住已采集正文和气泡的只读队列
            try { Tick(); } catch { }
            if (frames % 15 == 1) try { TickStartupUI(); } catch (Exception e) { Diagnose("ui:" + e.GetType().Name); }
            return packets.Count == 0 ? null : packets.Dequeue();
        }

        // StartupTranslation 只展开已确认设置显示模板，实际状态值由游戏生成
        static string StartupTranslation(string original)
        {
            if (Regex.IsMatch(original, @"^[\d\s.,%+\-]+$")) return null;
            string exact = Lookup(original);
            if (exact != null) return exact;
            string result = null;
            foreach (string template in uiFormats) {
                int at = template.IndexOf("{0}", StringComparison.Ordinal);
                string prefix = template.Substring(0, at), suffix = template.Substring(at + 3);
                if (!original.StartsWith(prefix, StringComparison.Ordinal) || !original.EndsWith(suffix, StringComparison.Ordinal) || original.Length < prefix.Length + suffix.Length) continue;
                string argument = original.Substring(prefix.Length, original.Length - prefix.Length - suffix.Length);
                string translation;
                if (!Safe(argument) || !translations.TryGetValue(Normalize(template), out translation) || Regex.Matches(translation, @"\{0\}").Count != 1 || !Safe(translation.Replace("{0}", "x"))) continue;
                string expanded = translation.Replace("{0}", Lookup(argument) ?? argument);
                if (!Safe(expanded) || result != null && result != expanded) return null;
                result = expanded;
            }
            return result;
        }

        // AddStartupRoot 仅沿管理器明确持有的开场说明及设置引用收集控件
        static void AddStartupRoot(object value, List<GameObject> roots)
        {
            GameObject root = value as GameObject;
            Component component = value as Component;
            if (component != null) root = component.gameObject;
            if (root != null && root.activeInHierarchy && !roots.Contains(root)) roots.Add(root);
        }

        // ViewportIntersects 按镜头投影排除屏幕外和裁剪范围外的场景文字
        static bool ViewportIntersects(Vector3[] corners, float near, float far)
        {
            if (corners == null || corners.Length == 0) return false;
            float left = Single.PositiveInfinity, right = Single.NegativeInfinity;
            float bottom = Single.PositiveInfinity, top = Single.NegativeInfinity;
            foreach (Vector3 p in corners) {
                if (Single.IsNaN(p.x) || Single.IsNaN(p.y) || Single.IsNaN(p.z) ||
                    Single.IsInfinity(p.x) || Single.IsInfinity(p.y) || Single.IsInfinity(p.z) || p.z < near || p.z > far) return false;
                left = Math.Min(left, p.x); right = Math.Max(right, p.x);
                bottom = Math.Min(bottom, p.y); top = Math.Max(top, p.y);
            }
            return right >= 0 && left <= 1 && top >= 0 && bottom <= 1;
        }

        // WorldGraphicVisible 检查实际画布、镜头层及文字几何，零尺寸溢出文字使用已生成字形范围
        static bool WorldGraphicVisible(Graphic graphic)
        {
            Canvas canvas = graphic.canvas;
            if (canvas == null || canvas.renderMode != RenderMode.WorldSpace || !canvas.isActiveAndEnabled) return false;
            Camera camera = canvas.worldCamera != null ? canvas.worldCamera : Camera.main;
            if (camera == null || !camera.isActiveAndEnabled || (camera.cullingMask & (1 << graphic.gameObject.layer)) == 0 || graphic.canvasRenderer.cull) return false;
            Vector3[] corners = new Vector3[4];
            Text legacy = graphic as Text;
            TMP_Text tmp = graphic as TMP_Text;
            if (legacy != null) {
                IList<UIVertex> vertices = legacy.cachedTextGenerator.verts;
                if (vertices.Count < 8 || legacy.pixelsPerUnit <= 0) return false;
                Vector3 min = vertices[0].position, max = min;
                for (int i = 1; i < vertices.Count - 4; ++i) {
                    min = Vector3.Min(min, vertices[i].position); max = Vector3.Max(max, vertices[i].position);
                }
                min /= legacy.pixelsPerUnit; max /= legacy.pixelsPerUnit;
                corners[0] = new Vector3(min.x, min.y, 0); corners[1] = new Vector3(min.x, max.y, 0);
                corners[2] = new Vector3(max.x, max.y, 0); corners[3] = new Vector3(max.x, min.y, 0);
                for (int i = 0; i < 4; ++i) corners[i] = graphic.transform.TransformPoint(corners[i]);
            } else if (tmp != null) {
                Bounds bounds = tmp.textBounds;
                corners[0] = new Vector3(bounds.min.x, bounds.min.y, 0); corners[1] = new Vector3(bounds.min.x, bounds.max.y, 0);
                corners[2] = new Vector3(bounds.max.x, bounds.max.y, 0); corners[3] = new Vector3(bounds.max.x, bounds.min.y, 0);
                for (int i = 0; i < 4; ++i) corners[i] = graphic.transform.TransformPoint(corners[i]);
            } else return false;
            for (int i = 0; i < 4; ++i) corners[i] = camera.WorldToViewportPoint(corners[i]);
            return ViewportIntersects(corners, camera.nearClipPlane, camera.farClipPlane);
        }

        // KeycapDisplay 保留唯一黑色键名，缺失或歧义只回退对应说明句
        static string KeycapDisplay(string original, string translation, out string key)
        {
            key = null;
            MatchCollection matches = Regex.Matches(original, @"(?<open><color=(?:black|#000000(?:ff)?)>)(?<key>[A-Za-z][A-Za-z0-9 +\-]{0,31})(?<close></color>)", RegexOptions.IgnoreCase);
            if (matches.Count == 0) return translation;
            if (matches.Count != 1 || original.IndexOf('\n') >= 0 || translation.IndexOf('\n') >= 0) return null;
            Match match = matches[0];
            key = match.Groups["key"].Value;
            int at = translation.IndexOf(key, StringComparison.Ordinal);
            if (at < 0 || translation.IndexOf(key, at + key.Length, StringComparison.Ordinal) >= 0 || Normalize(original).IndexOf(key, StringComparison.Ordinal) != Normalize(original).LastIndexOf(key, StringComparison.Ordinal)) return null;
            return translation.Substring(0, at) + match.Groups["open"].Value + key + match.Groups["close"].Value + translation.Substring(at + key.Length);
        }

        // KeyCenter 用实际字体布局度量键名中心，不依赖语言长度或固定像素偏移
        static bool KeyCenter(Text text, string value, string key, Font font, out float center)
        {
            center = 0;
            int index = value.IndexOf(key, StringComparison.Ordinal);
            if (font == null || index < 0) return false;
            float units = font.dynamic ? (text.canvas == null ? 1 : text.canvas.scaleFactor) : (float)font.fontSize / text.fontSize;
            if (units <= 0) return false;
            TextGenerationSettings settings = text.GetGenerationSettings(text.rectTransform.rect.size);
            settings.font = font; settings.richText = false; settings.scaleFactor = units;
            settings.fontSize = font.dynamic ? text.fontSize : 0;
            settings.resizeTextMinSize = font.dynamic ? text.resizeTextMinSize : 0;
            settings.resizeTextMaxSize = font.dynamic ? text.resizeTextMaxSize : 0;
            using (TextGenerator generator = new TextGenerator()) {
                if (!generator.Populate(value, settings) || generator.lineCount != 1 || generator.characters.Count < index + key.Length) return false;
                IList<UICharInfo> chars = generator.characters;
                center = (chars[index].cursorPos.x + chars[index + key.Length - 1].cursorPos.x + chars[index + key.Length - 1].charWidth) / (2 * units);
            }
            // 现有字距效果仍由游戏执行，两端保留相同标签，仅补偿可见字符变化
            foreach (MonoBehaviour effect in text.GetComponents<MonoBehaviour>()) {
                if (effect == null || !effect.isActiveAndEnabled || effect.GetType().FullName != "UnityEngine.UI.LetterSpacing") continue;
                PropertyInfo property = effect.GetType().GetProperty("spacing");
                if (property == null || property.PropertyType != typeof(float)) return false;
                float spacing = (float)property.GetValue(effect, null) * text.fontSize / 100;
                int alignment = (int)text.alignment % 3;
                center += spacing * (index + (key.Length - 1) * 0.5f - (value.Length - 1) * alignment * 0.5f);
            }
            return !Single.IsNaN(center) && !Single.IsInfinity(center);
        }

        // WrapWorldText 按原说明的实际宽高插入显示换行，零尺寸控件仍保持原排版属性
        static string WrapWorldText(Text text, string translated, Font font)
        {
            float width = text.preferredWidth, height = text.preferredHeight;
            float units = font.dynamic ? (text.canvas == null ? 1 : text.canvas.scaleFactor) : (float)font.fontSize / text.fontSize;
            if (width <= 0 || height <= 0 || units <= 0 || Single.IsInfinity(width) || Single.IsInfinity(height)) return null;
            TextGenerationSettings settings = text.GetGenerationSettings(new Vector2(width, 0));
            settings.font = font; settings.richText = false; settings.scaleFactor = units;
            settings.fontSize = font.dynamic ? text.fontSize : 0;
            settings.resizeTextMinSize = font.dynamic ? text.resizeTextMinSize : 0;
            settings.resizeTextMaxSize = font.dynamic ? text.resizeTextMaxSize : 0;
            settings.horizontalOverflow = HorizontalWrapMode.Wrap; settings.verticalOverflow = VerticalWrapMode.Overflow;
            using (TextGenerator generator = new TextGenerator()) {
                float measured = generator.GetPreferredHeight(translated, settings) / units;
                if (Single.IsNaN(measured) || Single.IsInfinity(measured) || measured > height * 1.25f + 1 / units || !generator.Populate(translated, settings)) return null;
                List<int> breaks = new List<int>();
                for (int i = 1; i < generator.lines.Count; ++i) breaks.Add(generator.lines[i].startCharIdx);
                return InsertDisplayBreaks(translated, breaks);
            }
        }

        // InsertDisplayBreaks 仅添加排版换行，保留译文字符及已有换行
        static string InsertDisplayBreaks(string value, List<int> breaks)
        {
            int previous = 0;
            foreach (int index in breaks) {
                if (index <= previous || index >= value.Length) return null;
                previous = index;
            }
            StringBuilder result = new StringBuilder(value);
            for (int i = breaks.Count - 1; i >= 0; --i) {
                int index = breaks[i];
                if (value[index - 1] != '\n' && value[index] != '\n') result.Insert(index, '\n');
            }
            return result.ToString();
        }

        // RestoreLegacyUI 仅恢复本组件仍持有的文字、字体及键名对齐位置
        static void RestoreLegacyUI(Text text, LegacyBinding item)
        {
            if (item.Applied != null && text.text == item.Applied) text.text = item.Original;
            if (text.font == bubbleFont && bubbleFont != null) { text.font = item.Font; text.material = item.Material; }
            if (item.Moved && text.transform.localPosition == item.AppliedPosition) text.transform.localPosition = item.Position;
            item.Applied = null; item.Moved = false;
        }

        // ApplyLegacyUI 仅改变显示副本，场景键名与原键帽中心对齐
        static void ApplyLegacyUI(Text text, LegacyBinding item, string translated, bool world)
        {
            if (bubbleFont == null) bubbleFont = CreateBubbleFont();
            if (bubbleFont == null) { Diagnose("ui:missing-system-glyph"); return; }
            foreach (char c in translated) if (!Char.IsWhiteSpace(c) && !bubbleFont.HasCharacter(c)) { Diagnose("ui:missing-system-glyph"); return; }
            if (world) {
                translated = WrapWorldText(text, translated, bubbleFont);
                if (translated == null) { Diagnose("ui:world-layout"); return; }
            }
            string key = null;
            string display = world ? KeycapDisplay(item.Original, translated, out key) : translated;
            if (display == null) { Diagnose("ui:keycap-format"); return; }
            Vector3 position = text.transform.localPosition;
            if (key != null) {
                float before, after;
                if (!text.supportRichText || !KeyCenter(text, Normalize(item.Original), key, item.Font, out before) || !KeyCenter(text, translated, key, bubbleFont, out after)) { Diagnose("ui:keycap-layout"); return; }
                Vector3 delta = text.transform.localRotation * Vector3.Scale(new Vector3(before - after, 0, 0), text.transform.localScale);
                if (delta.sqrMagnitude > 10000) { Diagnose("ui:keycap-layout"); return; }
                position += delta;
            }
            item.Position = text.transform.localPosition; item.AppliedPosition = position; item.Moved = key != null;
            text.font = bubbleFont; text.material = bubbleFont.material;
            text.transform.localPosition = position; text.text = item.Applied = display;
        }

        // TickStartupUI 采集开场引用树及镜头内世界空间画布，不触碰设置值和输入事件
        static void TickStartupUI()
        {
            List<GameObject> roots = new List<GameObject>();
            if (introType != null) foreach (UnityEngine.Object obj in UnityEngine.Object.FindObjectsOfType(introType)) {
                foreach (string field in new string[] { "betaWarningObject", "closeWarningButton", "continueButton", "gameplaySettingsManager" }) {
                    FieldInfo info = introType.GetField(field, Fields);
                    if (info != null) AddStartupRoot(info.GetValue(obj), roots);
                }
            }
            if (settingsType != null) foreach (UnityEngine.Object obj in UnityEngine.Object.FindObjectsOfType(settingsType)) AddStartupRoot(obj, roots);
            foreach (Canvas canvas in UnityEngine.Object.FindObjectsOfType(typeof(Canvas)))
                if (canvas.isActiveAndEnabled && canvas.renderMode == RenderMode.WorldSpace) AddStartupRoot(canvas, roots);
            HashSet<Component> seen = new HashSet<Component>();
            StringBuilder raw = new StringBuilder(), shown = new StringBuilder(), identity = new StringBuilder();
            Component owner = null;
            bool silent = DateTime.UtcNow < startupSilentUntil;
            foreach (GameObject root in roots) {
                List<Graphic> graphics = new List<Graphic>();
                graphics.AddRange(root.GetComponentsInChildren<Text>());
                graphics.AddRange(root.GetComponentsInChildren<TMP_Text>());
                foreach (Graphic graphic in graphics) {
                    if (!graphic.isActiveAndEnabled || graphic.color.a <= 0 || graphic.GetComponentInParent<InputField>() != null || graphic.GetComponentInParent<TMP_InputField>() != null) continue;
                    bool visible = true;
                    foreach (CanvasGroup group in graphic.GetComponentsInParent<CanvasGroup>()) if (group.alpha <= 0) visible = false;
                    if (!visible) continue;
                    bool world = graphic.canvas != null && graphic.canvas.renderMode == RenderMode.WorldSpace;
                    if (world && !WorldGraphicVisible(graphic)) continue;
                    if (seen.Count >= 128 || !seen.Add(graphic)) continue;
                    Text legacy = graphic as Text;
                    TMP_Text tmp = graphic as TMP_Text;
                    string original, applied = null;
                    if (legacy != null) {
                        LegacyBinding item;
                        if (!uiLabels.TryGetValue(legacy, out item) || legacy.text != item.Applied) {
                            if (item == null && uiLabels.Count >= 512) continue;
                            if (item != null) RestoreLegacyUI(legacy, item);
                            item = new LegacyBinding { Font = legacy.font, Material = legacy.material };
                            item.Original = legacy.text; item.Applied = null; uiLabels[legacy] = item;
                        }
                        original = item.Original;
                        if (world && Regex.IsMatch(Normalize(original), @"^[A-Za-z]$")) continue;
                        string translated = enabled && !silent ? StartupTranslation(Normalize(original)) : null;
                        if (translated != null && item.Applied == null) ApplyLegacyUI(legacy, item, translated, world);
                        applied = item.Applied;
                    } else {
                        TextBinding item;
                        if (!labels.TryGetValue(tmp, out item) || tmp.text != item.Applied) {
                            if (item == null && labels.Count >= 512) continue;
                            labels[tmp] = item = new TextBinding { Original = tmp.text };
                        }
                        original = item.Original;
                        if (world && Regex.IsMatch(Normalize(original), @"^[A-Za-z]$")) continue;
                        string translated = enabled && !silent ? StartupTranslation(Normalize(original)) : null;
                        if (translated != null && AttachFont(tmp, translated)) tmp.text = item.Applied = translated;
                        applied = item.Applied;
                    }
                    string text = Normalize(original);
                    if (text.Length == 0 || !Safe(text) || Regex.IsMatch(text, @"^[\d\s.,%+\-]+$") || raw.Length + text.Length > 16000) continue;
                    if (owner == null) owner = graphic;
                    raw.Append(text).Append('\n'); shown.Append(applied == null ? text : Normalize(applied)).Append('\n');
                    identity.Append(graphic.GetInstanceID()).Append(':').Append(text.Length).Append(':').Append(text).Append('|');
                }
            }
            string key = (enabled ? "1:" : "0:") + identity.ToString();
            if (key == uiKey) return;
            uiKey = key;
            if (owner == null || silent) return;
            string originalPage = raw.ToString().TrimEnd(), displayPage = shown.ToString().TrimEnd();
            Emit(++sequence, owner, "ui", originalPage, "", enabled && originalPage != displayPage ? displayPage : "");
        }
        // Tick 扫描唯一剧情菜单，不扫描全局TMP和普通UI
        internal static void Tick()
        {
            if (!attempted) LoadSnapshot();
            if (!configured) return;
            if (!menuReadable) return;
            foreach (UnityEngine.Object obj in UnityEngine.Object.FindObjectsOfType(menuType))
            {
                Component manager = obj as Component;
                if (manager == null || !manager.gameObject.activeInHierarchy) continue;
                IList active = optionsField.GetValue(manager) as IList;
                if (active == null || active.Count == 0 || active.Count > 128) continue;
                TMP_Text header = headerField.GetValue(manager) as TMP_Text;
                StringBuilder original = new StringBuilder(), applied = new StringBuilder(), identity = new StringBuilder();
                string headerOriginal = ApplyLabel(header, (capabilities & 8)!=0);
                if (!String.IsNullOrEmpty(headerOriginal)) {
                    original.Append(headerOriginal).Append('\n');
                    applied.Append(header.text).Append('\n');
                    identity.Append(headerOriginal).Append('|');
                }
                foreach (object option in active)
                {
                    Option item;
                    if (!options.TryGetValue(option, out item) || item.Text == null || item.Clicked) continue;
                    identity.Append(item.Owner.GetInstanceID()).Append(':').Append(item.Revision).Append(':').Append(item.ID).Append(':').Append(item.Original).Append('|');
                    original.Append(item.Original).Append('\n');
                    string translation = enabled && (capabilities & 8)!=0 ? Lookup(item.Original) : null;
                    if (translation != null && AttachFont(item.Text, translation)) { item.Applied = translation; item.Text.text = translation; }
                    applied.Append(item.Applied ?? item.Original).Append('\n');
                }
                string key = identity.ToString(), previous;
                if (key.Length == 0 || menuKeys.TryGetValue(manager, out previous) && previous == key) continue;
                menuKeys[manager] = key;
                string raw=original.ToString().TrimEnd(), shown=applied.ToString().TrimEnd();
                Emit(++sequence, manager, "options", raw, "", enabled && shown!=raw ? shown : "");
            }
        }

        // AttachFont 为显示对象建立独立字库和材质，不改游戏共享字库
        static bool AttachFont(TMP_Text text, string value)
        {
            if (text == null || text.font == null) { Diagnose("font:no-display-font"); return false; }
            foreach (char c in value) if (!Char.IsWhiteSpace(c) && !text.font.HasCharacter(c, true) && (chinese == null || !chinese.HasCharacter(c))) { Diagnose(chinese == null ? "font:chinese-unavailable" : "font:missing-glyph"); return false; }
            if (chinese == null) return true;
            for (int i=fonts.Count-1;i>=0;i--) {
                FontBinding b=fonts[i];
                if (b.Text==null || b.Text.font!=b.Private) {
                    owned.Remove(b.Private); if(b.Private!=null) UnityEngine.Object.Destroy(b.Private); fonts.RemoveAt(i);
                } else if (b.Text==text) return true;
            }
            if (fonts.Count >= 512 || chinese == null) { Diagnose("font:binding-limit"); return false; }
            TMP_FontAsset copy = UnityEngine.Object.Instantiate(text.font);
            copy.fallbackFontAssets = new List<TMP_FontAsset>();
            copy.fallbackFontAssets.Add(chinese);
            if (text.font.fallbackFontAssets != null) copy.fallbackFontAssets.AddRange(text.font.fallbackFontAssets);
            FontBinding binding = new FontBinding { Text = text, Original = text.font, OriginalMaterial = text.fontSharedMaterial, Private = copy };
            fonts.Add(binding); owned.Add(copy);
            text.font = copy;
            return true;
        }

        // Prune 回收已销毁显示对象，不丢弃仍能被点击的原文绑定
        static void Prune()
        {
            List<object> dead=new List<object>();
            foreach(KeyValuePair<object,Option> item in options) if(item.Value.Owner==null) dead.Add(item.Key);
            foreach(object key in dead) options.Remove(key);
            dead.Clear();
            foreach(KeyValuePair<object,Page> item in pages) if((item.Value.Window as Component)==null) dead.Add(item.Key);
            foreach(object key in dead) pages.Remove(key);
            dead.Clear();
            foreach(object key in menuKeys.Keys) if((key as Component)==null) dead.Add(key);
            foreach(object key in dead) menuKeys.Remove(key);
            List<TMP_Text> textKeys=new List<TMP_Text>();
            foreach(TMP_Text key in labels.Keys) if(key==null) textKeys.Add(key);
            foreach(TMP_Text key in textKeys) labels.Remove(key);
            List<Text> legacyKeys = new List<Text>();
            foreach (Text key in uiLabels.Keys) if (key == null) legacyKeys.Add(key);
            foreach (Text key in legacyKeys) uiLabels.Remove(key);
            for(int i=fonts.Count-1;i>=0;i--) if(fonts[i].Text==null) {
                TMP_FontAsset copy=fonts[i].Private;owned.Remove(copy);if(copy!=null) UnityEngine.Object.Destroy(copy);fonts.RemoveAt(i);
            }
        }

        // ReadString 读取有界UTF16并拒绝非法代理项
        static string ReadString(BinaryReader reader, int max)
        {
            uint length = reader.ReadUInt32();
            if (length > max) throw new InvalidDataException();
            byte[] bytes = reader.ReadBytes(checked((int)length * 2));
            if (bytes.Length != length * 2) throw new EndOfStreamException();
            return new UnicodeEncoding(false, false, true).GetString(bytes);
        }
        // Hex 生成摘要的稳定小写表示
        static string Hex(byte[] bytes)
        {
            return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
        }
        // Hash 核对不可变载荷和原始来源，长度错误不能冒充空映射
        static byte[] Hash(string path)
        {
            using (SHA256 hash = SHA256.Create()) using (FileStream file = File.OpenRead(path)) return hash.ComputeHash(file);
        }
        // LoadSnapshot 在Unity主线程完成来源、载荷、字库加载后才声明就绪
        static void LoadSnapshot()
        {
            if (!LeaseValid()) return;
            attempted = true;
            string ticket = Path.Combine(cache, "TeamNimbus.active.bin");
            if (!File.Exists(ticket)) { attempted = false; return; }
            try
            {
                byte[] expected, fp, td, activation;
                string name;
                using (BinaryReader r = new BinaryReader(File.OpenRead(ticket)))
                {
                    if (Encoding.ASCII.GetString(r.ReadBytes(8)) != "TNCA0002") throw new InvalidDataException();
                    expected = r.ReadBytes(32); fp = r.ReadBytes(32); td = r.ReadBytes(32); activation = r.ReadBytes(32);
                    name = ReadString(r, 128);
                    if (r.BaseStream.Position != r.BaseStream.Length || Path.GetFileName(name) != name || !name.StartsWith("backfill_data.") || !name.EndsWith(".tnbf")) throw new InvalidDataException();
                }
                byte[] authorized = new byte[32];
                Marshal.Copy(new IntPtr(leaseMemory.ToInt64() + 32), authorized, 0, 32);
                if (Hex(expected) != Hex(authorized)) { attempted = false; return; }
                if ((capabilities & 4) == 0) throw new InvalidDataException("presentation unavailable");
                if (Hex(Hash(Path.Combine(cache, "backfill.active.json"))) != Hex(activation)) throw new InvalidDataException();
                string resource = Path.Combine(cache, name);
                if (new FileInfo(resource).Length > (64L << 20) || Hex(Hash(resource)) != Hex(expected)) throw new InvalidDataException();
                using (BinaryReader r = new BinaryReader(File.OpenRead(resource)))
                {
                    if (Encoding.ASCII.GetString(r.ReadBytes(8)) != "DCTN0001" || Hex(r.ReadBytes(32)) != Hex(fp) || Hex(r.ReadBytes(32)) != Hex(td)) throw new InvalidDataException();
                    string data = Path.GetFullPath(Application.dataPath);
                    uint count = r.ReadUInt32();
                    if (count == 0 || count > 4096) throw new InvalidDataException();
                    StringBuilder inventory = new StringBuilder();
                    HashSet<string> paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    for (uint i = 0; i < count; i++)
                    {
                        string rel = ReadString(r, 1024), sha = Hex(r.ReadBytes(32));
                        string path = Path.GetFullPath(Path.Combine(data, rel));
                        if (Path.IsPathRooted(rel) || !path.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !paths.Add(path) || Hex(Hash(path)) != sha) throw new InvalidDataException();
                        inventory.Append(rel.Replace('\\', '/')).Append(':').Append(sha).Append('\n');
                    }
                    using (SHA256 hash = SHA256.Create()) if (Hex(hash.ComputeHash(Encoding.UTF8.GetBytes(inventory.ToString()))) != Hex(fp)) throw new InvalidDataException();
                    uint pairs = r.ReadUInt32(); if (pairs > 1000000) throw new InvalidDataException();
                    for (uint i = 0; i < pairs; i++)
                    {
                        string original = ReadString(r, 16000), translated = ReadString(r, 16000);
                        if (!Safe(tokens.Replace(original,"x")) || !Safe(tokens.Replace(translated,"x")) || translations.ContainsKey(original)) throw new InvalidDataException();
                        translations.Add(original, translated);
                    }
                    LoadFont(r);
                    if (r.BaseStream.Position != r.BaseStream.Length) throw new InvalidDataException();
                }
                digest = Hex(expected); loaded = enabled = true;
            }
            catch (Exception e) { failure = e.GetType().Name; translations.Clear(); loaded = enabled = false; }
        }

        // LoadFont 加载预编译位图字形；缺字仅导致相应整句回退
        static void LoadFont(BinaryReader r)
        {
            int size = r.ReadInt32(), count = r.ReadInt32();
            if (size == 0 && count == 0) { Diagnose("font:empty-payload"); return; }
            if (size < 128 || size > 4096 || (size & (size - 1)) != 0 || count < 0 || count > 20000) throw new InvalidDataException();
            TMP_Glyph[] glyphs = new TMP_Glyph[count];
            for (int i = 0; i < count; i++)
            {
                glyphs[i] = new TMP_Glyph { id = r.ReadInt32(), x = r.ReadSingle(), y = r.ReadSingle(), width = r.ReadSingle(), height = r.ReadSingle(), xOffset = r.ReadSingle(), yOffset = r.ReadSingle(), xAdvance = r.ReadSingle(), scale = 1 };
                if (glyphs[i].id < 32 || glyphs[i].id > 65535 || glyphs[i].width < 0 || glyphs[i].height < 0 || glyphs[i].x < 0 || glyphs[i].y < 0 || glyphs[i].x + glyphs[i].width > size || glyphs[i].y + glyphs[i].height > size) throw new InvalidDataException();
            }
            byte[] pixels = r.ReadBytes(checked(size * size));
            if (pixels.Length != size * size) throw new EndOfStreamException();
            // 字形载荷结构已完整消费，Unity对象创建失败只使缺字整句回退
            try {
            Shader shader = Shader.Find("TextMeshPro/Bitmap");
            if (shader == null || !shader.isSupported) { Diagnose("font:bitmap-shader-unavailable"); return; }
            Texture2D atlas = new Texture2D(size, size, TextureFormat.Alpha8, false);
            owned.Add(atlas);
            atlas.LoadRawTextureData(pixels); atlas.Apply();
            Material material = new Material(shader); owned.Add(material); material.mainTexture = atlas;
            material.name = "DangoTeamNimbusChineseBitmap";
            chinese = ScriptableObject.CreateInstance<TMP_FontAsset>();
            chinese.name = "DangoTeamNimbusChinese";
            owned.Add(chinese);
            chinese.fontAssetType = TMP_FontAsset.FontAssetTypes.Bitmap;
            chinese.atlas = atlas; chinese.material = material;
            chinese.AddFaceInfo(new FaceInfo { Name = "DangoTeamNimbusChinese", PointSize = 20, Scale = 1, LineHeight = 24, Ascender = 20, Descender = -4, AtlasWidth = size, AtlasHeight = size });
            // 旧TMP不会为CreateInstance初始化序列化字距表，必须先提供空表
            chinese.AddKerningInfo(new KerningTable());
            chinese.fallbackFontAssets = new List<TMP_FontAsset>();
            chinese.AddGlyphInfo(glyphs); chinese.ReadFontDefinition();
            Diagnose("font:bitmap-ready");
            } catch (Exception e) { Diagnose("font:init-" + e.GetType().Name); chinese=null; }
        }

        // Stop 恢复本组件持有且仍有效的显示对象，外部重写不覆盖
        public static void Stop()
        {
            enabled = loaded = false;
            foreach (Page page in pages.Values) {
                if (page.Applied == null || page.Reveal == null) continue;
                TMP_Text current = revealType.GetField("_text", Fields).GetValue(page.Reveal) as TMP_Text;
                if (current == null || Normalize(current.text) != Normalize(page.Applied)) continue;
                // 原生消费者重新开启原文显示，停止原来的逐字协程再按原逻辑恢复
                MethodInfo start = Method(revealType, "StartRevealingNewString", typeof(void), typeof(string));
                if (start != null) try { start.Invoke(page.Reveal, new object[] { page.Original }); } catch { }
            }
            foreach (KeyValuePair<TMP_Text, TextBinding> item in labels)
                if (item.Key != null && item.Value.Applied != null && item.Key.text == item.Value.Applied) item.Key.text = item.Value.Original;
            foreach (KeyValuePair<Text, LegacyBinding> item in uiLabels) if (item.Key != null) {
                RestoreLegacyUI(item.Key, item.Value);
            }
            foreach (KeyValuePair<object, BubbleBinding> item in bubbles) {
                BubbleBinding b = item.Value;
                if (b.Text == null || b.Text.font != bubbleFont) continue;
                b.Text.font = b.Font;
                // 从原入口恢复，避免旧中文message在下一帧继续逐字覆盖
                FieldInfo message = bubbleType.GetField("message", Fields), alpha = bubbleType.GetField("alphaStart", Fields);
                if (message != null && alpha != null) { message.SetValue(item.Key, b.Original); alpha.SetValue(item.Key, Math.Max(0, b.Original.Length - 1)); b.Text.text = b.Original; }
            }
            foreach (Option item in options.Values) if (item.Text != null && item.Applied != null && item.Text.text == item.Applied) item.Text.text = item.Original;
            foreach (FontBinding b in fonts) if (b.Text != null && b.Text.font == b.Private) { b.Text.font = b.Original; b.Text.fontSharedMaterial = b.OriginalMaterial; }
            fonts.Clear(); pages.Clear(); options.Clear(); translations.Clear(); packets.Clear(); menuKeys.Clear();
            labels.Clear(); speakers.Clear(); bubbles.Clear();
            uiLabels.Clear(); uiKey = null;
            templates.Clear();
            foreach (UnityEngine.Object obj in owned) if (obj != null) UnityEngine.Object.Destroy(obj);
            owned.Clear(); chinese = null; bubbleFont = null;
            if (leaseMemory != IntPtr.Zero) { UnmapViewOfFile(leaseMemory); leaseMemory = IntPtr.Zero; }
            if (lease != IntPtr.Zero) { CloseHandle(lease); lease = IntPtr.Zero; }
            if (host != IntPtr.Zero) { CloseHandle(host); host = IntPtr.Zero; }
            attempted = true;
        }

        [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);
        [DllImport("kernel32.dll")] static extern IntPtr MapViewOfFile(IntPtr handle, uint access, uint high, uint low, UIntPtr size);
        [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr memory);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr process, out long created, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);

        // OpenLease 将快照绑定当前Helper进程及其创建时间，拒绝复用旧进程租约
        static void OpenLease()
        {
            lease = OpenFileMapping(4, false, "Local\\DangoTeamNimbusLease_" + GetCurrentProcessId());
            if (lease == IntPtr.Zero) return;
            leaseMemory = MapViewOfFile(lease, 4, 0, 0, new UIntPtr(64));
            if (leaseMemory == IntPtr.Zero) { CloseHandle(lease); lease = IntPtr.Zero; return; }
            uint pid = (uint)Marshal.ReadInt32(leaseMemory, 4);
            hostCreated = Marshal.ReadInt64(leaseMemory, 16);
            host = OpenProcess(0x101000, false, pid);
        }

        // LeaseValid 同时校验目标PID、Helper创建时间和撤销标志
        static bool LeaseValid()
        {
            long created, exit, kernel, user;
            return leaseMemory != IntPtr.Zero && host != IntPtr.Zero && Marshal.ReadInt32(leaseMemory) == 0x544e4c31 &&
                (uint)Marshal.ReadInt32(leaseMemory, 8) == GetCurrentProcessId() && Marshal.ReadInt32(leaseMemory, 24) == 1 &&
                WaitForSingleObject(host, 0) == 258 && GetProcessTimes(host, out created, out exit, out kernel, out user) && created == hostCreated;
        }

        // Pump 在主线程持续核对授权撤销并恢复显示
        public sealed class Pump : MonoBehaviour
        {
            void Update()
            {
                if (configured && leaseMemory != IntPtr.Zero && (!LeaseValid() || enabled && !File.Exists(Path.Combine(cache, "TeamNimbus.active.bin")))) Stop();
            }
        }
    }
}
