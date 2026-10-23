using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using TMPro;
using UnityEngine;
using UnityEngine.TextCore.LowLevel;

namespace Dango.YarnSpinner
{
    // Display 在独立主线程组件中更新显示副本，原对话对象、回调和存档保持原值
    public sealed partial class Display : MonoBehaviour
    {
        const BindingFlags InstanceFields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        static Type chatType, messageType;
        static FieldInfo chatLine, chatParent, messageText;
        static PropertyInfo messageRecord;
        static FieldInfo payload, sender;
        static Display pump;
        public static bool ChatSupported { get { return chatSupported; } }
        static bool chatSupported, phoneSupported, customPhoneSupported;
        static long latestSequence;
        static readonly List<Binding> bindings = new List<Binding>();
        static readonly List<Binding> applied = new List<Binding>();
        static WeakReference line;
        static string original, speaker;
        static IntPtr mapping, memory, host;
        static long hostTime;
        long revision, epoch;
        bool displayEnabled;
        string translation;
        long translatedSequence;
        TMP_FontAsset chinese, symbols;
        Font source, symbolSource;
        readonly HashSet<char> glyphs = new HashSet<char>();
        int mainThread;

        sealed class Binding
        {
            internal WeakReference Owner, View, Line;
            internal TMP_Text Text;
            internal string Original, Applied;
            internal long Sequence;
            internal FieldInfo Current;
            internal PropertyInfo Record;
            internal object OriginalRecord;
            internal TMP_FontAsset Font, FallbackFont;
            internal Material Material, Replacement;
            internal int Visible;
            internal bool Auxiliary, CacheBody, PostedChoice;
            internal PropertyInfo Option;
            internal object OriginalOption;
            internal Transform OptionParent;
            internal WeakReference ChoiceHover;
            internal ChoiceFonts ChoiceFonts;
            internal string RecordPayload, RecordSender, RecordKind;
            internal TMP_Text[] GuardTexts;
            internal string[] GuardValues;
            internal double LastQuery;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);
        [DllImport("kernel32.dll")] static extern IntPtr MapViewOfFile(IntPtr handle, uint access, uint high, uint low, UIntPtr bytes);
        [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr address);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
        [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint millis);
        [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr handle, out long created, out long ended, out long kernel, out long user);

        // Validate 分别核对聊天及电话显示布局，不把失败传递给只读监听
        internal static bool Validate(Assembly yarn, Assembly game)
        {
            try
            {
                chatSupported = game != null && Reader.CheckDisplay(game, ChatContracts);
                if (chatSupported)
                {
                    chatType = game.GetType("CupidChatroom.Yarn.ChatroomView", true);
                    messageType = game.GetType("MessageBox", true);
                    chatLine = Field(chatType, "_currentLine", Reader.LineType);
                    chatParent = Field(chatType, "_currentTransform", typeof(Transform));
                    messageText = Field(messageType, "_messageText", typeof(TMP_Text));
                    messageRecord = messageType.GetProperty("Record");
                    Type record = messageRecord == null ? null : messageRecord.PropertyType;
                    payload = record == null ? null : Field(record, "payload", typeof(string));
                    sender = record == null ? null : Field(record, "sender", typeof(string));
                    chatSupported = chatLine != null && chatParent != null && messageText != null && payload != null && sender != null;
                }
                phoneSupported = Reader.CheckDisplay(yarn, PhoneContracts);
                customPhoneSupported = phoneSupported && game != null && Reader.CheckDisplay(game, CustomPhoneContracts);
                ValidateInterface(game);
                ValidateChoices(yarn, game);
                return chatSupported || phoneSupported;
            }
            catch { chatSupported = phoneSupported = false; return false; }
        }

        static FieldInfo Field(Type type, string name, Type expected)
        { FieldInfo field = type.GetField(name, InstanceFields); return field != null && !field.IsStatic && field.FieldType == expected ? field : null; }

        // Observe 为每个已验证显示视图登记页身份，重复监听页也要登记其他消费者
        internal static void Observe(object value, object view, long sequence, bool body)
        {
            if ((!chatSupported && !phoneSupported) || sequence <= 0) return;
            try
            {
                if (!EnsurePump()) return;
                if (Thread.CurrentThread.ManagedThreadId != pump.mainThread) return;
                if (sequence < latestSequence) return;
                if (sequence != latestSequence)
                {
                    pump.ReleasePhones();
                    latestSequence = sequence; bindings.Clear();
                    line = body ? new WeakReference(value) : null;
                    original = body ? Reader.DisplayText(value) : null;
                    speaker = body ? Reader.DisplaySpeaker(value) : null;
                }
                if (!body || !phoneSupported) return;
                Type type = view.GetType();
                if (type.FullName != "Yarn.Unity.LineView" && !(customPhoneSupported && type.FullName == "CupidChatroom.Yarn.ChatroomLineView")) return;
                FieldInfo current = Field(type, "currentLine", Reader.LineType);
                FieldInfo text = Field(type, "lineText", typeof(TextMeshProUGUI));
                if (current == null || text == null) return;
                TMP_Text target = text.GetValue(view) as TMP_Text;
                if (target == null || bindings.Exists(b => b.Text == target)) return;
                bindings.Add(new Binding { Owner = new WeakReference(view), View = new WeakReference(view), Line = new WeakReference(value), Text = target,
                    Sequence = sequence, Current = current });
            }
            catch { }
        }

        // BindMessage 只绑定当前原页在同频道生成的正文气泡，记录在原方法之后核对
        public static void BindMessage(object owner, string text)
        {
            if (!chatSupported || pump == null || Thread.CurrentThread.ManagedThreadId != pump.mainThread || line == null || text != original) return;
            try
            {
                Component box = owner as Component; if (box == null || owner.GetType() != messageType) return;
                PropertyInfo singleton = chatType.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
                object view = singleton == null ? null : singleton.GetValue(null, null);
                if (view == null || !System.Object.ReferenceEquals(chatLine.GetValue(view), line.Target) || (chatParent.GetValue(view) as Transform) != box.transform.parent) return;
                TMP_Text target = messageText.GetValue(owner) as TMP_Text;
                if (target == null || bindings.Exists(b => b.Text == target)) return;
                bindings.Add(new Binding { Owner = new WeakReference(owner), View = new WeakReference(view), Line = line, Text = target,
                    Original = text, Sequence = latestSequence, Record = messageRecord });
            }
            catch { }
        }

        internal static void Invalidate()
        { bindings.Clear(); latestSequence = 0; line = null; if (pump != null) { pump.translation = null; pump.ReleasePhones(); } }

        // OpenChannel 验证目标与Host的创建时间和独立会话标记，不信任磁盘激活文件
        static bool OpenChannel()
        {
            if (memory != IntPtr.Zero) return true;
            long created, ended, kernel, user;
            if (!GetProcessTimes(GetCurrentProcess(), out created, out ended, out kernel, out user)) return false;
            mapping = OpenFileMapping(6, false, "Local\\Dango.YarnSpinner." + GetCurrentProcessId() + "." + created);
            if (mapping == IntPtr.Zero) return false;
            memory = MapViewOfFile(mapping, 6, 0, 0, new UIntPtr(131072));
            if (memory == IntPtr.Zero) { CloseChannel(); return false; }
            byte[] header = new byte[48]; Marshal.Copy(memory, header, 0, header.Length);
            bool nonce = false; for (int i = 32; i < 48; i++) nonce |= header[i] != 0;
            if (Encoding.ASCII.GetString(header, 0, 8) != "DYRT0002" || BitConverter.ToUInt32(header, 8) != GetCurrentProcessId() || BitConverter.ToInt64(header, 16) != created || !nonce)
            { CloseChannel(); return false; }
            host = OpenProcess(0x101000, false, BitConverter.ToUInt32(header, 12));
            hostTime = BitConverter.ToInt64(header, 24);
            if (host == IntPtr.Zero || !GetProcessTimes(host, out created, out ended, out kernel, out user) || created != hostTime || WaitForSingleObject(host, 0) != 258)
            { CloseChannel(); return false; }
            return true;
        }

        static void CloseChannel()
        {
            if (memory != IntPtr.Zero) UnmapViewOfFile(memory);
            if (mapping != IntPtr.Zero) CloseHandle(mapping);
            if (host != IntPtr.Zero) CloseHandle(host);
            memory = mapping = host = IntPtr.Zero;
        }

        // Update 只在游戏主线程消费有界快照，字体及显示写入不进入Hook等待路径
        void Update()
        {
            if (Thread.CurrentThread.ManagedThreadId != mainThread) return;
            try
            {
                if (host == IntPtr.Zero || WaitForSingleObject(host, 0) != 258)
                { Restore(); CloseChannel(); UnityEngine.Object.Destroy(gameObject); return; }
                if (Reader.Restoring) Invalidate();
                for (int i = applied.Count - 1; i >= 0; i--)
                    if (applied[i].Text == null || applied[i].Owner.Target == null) { RestoreBinding(applied[i]); applied.RemoveAt(i); }
                int stamp = Marshal.ReadInt32(memory, 48);
                if ((stamp & 1) == 0)
                {
                    long next = Marshal.ReadInt64(memory, 56);
                    if (next > revision)
                    {
                        byte[] snapshot = new byte[26146]; Marshal.Copy(memory, snapshot, 0, snapshot.Length); Thread.MemoryBarrier();
                        if (stamp == Marshal.ReadInt32(memory, 48)) Consume(snapshot);
                    }
                }
                if (displayEnabled && translation != null && translatedSequence == latestSequence && !Reader.Restoring) Apply();
                if (displayEnabled && InterfaceSupported && !Reader.Restoring)
                { try { UpdateInterface(); } catch { InterfaceSupported = false; RestoreInterface(); } }
            }
            catch { Ack(4, 0); displayEnabled = false; Restore(); }
        }

        void Consume(byte[] snapshot)
        {
            long next = BitConverter.ToInt64(snapshot, 56), mode = BitConverter.ToInt64(snapshot, 64);
            if (next <= revision || mode < epoch || BitConverter.ToUInt32(snapshot, 80) > 1) return;
            revision = next;
            uint fontLength = BitConverter.ToUInt32(snapshot, 84), length = BitConverter.ToUInt32(snapshot, 88);
            if (fontLength > 1023 || length > 12000) { Ack(4, 0); return; }
            if (mode != epoch)
            {
                Restore(); epoch = mode; translation = null; translatedSequence = 0;
                displayEnabled = BitConverter.ToUInt32(snapshot, 80) == 1;
                interfaceChinese = BitConverter.ToUInt32(snapshot, 92) == 1;
                if (displayEnabled)
                {
                    string path = Encoding.Unicode.GetString(snapshot, 96, (int)fontLength * 2);
                    if (path.IndexOf('\0') >= 0 || !System.IO.Path.IsPathRooted(path) || !System.IO.File.Exists(path)) { displayEnabled = false; Ack(4, 0); return; }
                    source = new Font(path);
                    chinese = TMP_FontAsset.CreateFontAsset(source, 90, 9, GlyphRenderMode.SDFAA, 2048, 2048, AtlasPopulationMode.Dynamic, true);
                    if (chinese == null) { displayEnabled = false; Ack(4, 0); return; }
                    chinese.hideFlags = HideFlags.HideAndDontSave;
                    Ack(1, 0);
                }
                else Ack(2, 0);
                if (!displayEnabled || length == 0) return;
            }
            long page = BitConverter.ToInt64(snapshot, 72);
            if (!displayEnabled || page != latestSequence || page <= 0 || (line == null || !line.IsAlive) && !bindings.Exists(b => b.Record != null && b.Text != null)) { Ack(5, page); return; }
            string value = Encoding.Unicode.GetString(snapshot, 2144, (int)length * 2);
            if (!SafeText(value)) { Ack(5, page); return; }
            translation = value; translatedSequence = page;
        }

        static bool SafeText(string value)
        {
            if (String.IsNullOrWhiteSpace(value) || value.Length > 12000) return false;
            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                if ((Char.IsControl(c) && c != '\n' && c != '\r' && c != '\t') || (c == '<' && i + 1 < value.Length && (Char.IsLetter(value[i + 1]) || value[i + 1] == '/' || value[i + 1] == '#')) || c == '\0') return false;
                if (Char.IsHighSurrogate(c)) { if (++i >= value.Length || !Char.IsLowSurrogate(value[i])) return false; }
                else if (Char.IsLowSurrogate(c)) return false;
            }
            return true;
        }

        bool Ready(Binding b)
        {
            Component owner = b.Owner.Target as Component;
            if (owner == null || b.Text == null || !owner.gameObject.activeInHierarchy || b.Sequence != latestSequence) return false;
            if (b.Record != null)
            {
                object record = b.Record.GetValue(owner, null);
                if (record == null || (string)payload.GetValue(record) != b.Original || (string)sender.GetValue(record) != speaker) return false;
                if (b.OriginalRecord != null && !System.Object.ReferenceEquals(record, b.OriginalRecord)) return false;
                b.OriginalRecord = record; return b.Text.text == b.Original || b.Text.text == b.Applied;
            }
            if (!System.Object.ReferenceEquals(b.Current.GetValue(owner), b.Line.Target) || b.Text.maxVisibleCharacters != Int32.MaxValue) return false;
            string text = b.Text.text;
            // 首期电话只接受未带角色前缀或复杂排版的完整正文，未知呈现整句回退
            if (text != original && text != b.Applied) return false;
            b.Original = original; return true;
        }

        void Apply()
        {
            bool written = false;
            foreach (Binding b in bindings)
            {
                if (!Ready(b) || (b.Applied == translation && b.Text.text == translation)) continue;
                if (!WriteDisplay(b, translation)) { Ack(5, latestSequence); translation = null; return; }
                written = true;
            }
            if (written) Ack(3, latestSequence);
        }

        // ReleasePhones 在新页原方法执行前恢复电话字体和同页原显示，气泡可继续保留译文
        void ReleasePhones()
        {
            for (int i = applied.Count - 1; i >= 0; i--) if (applied[i].Current != null) { RestoreBinding(applied[i]); applied.RemoveAt(i); }
        }

        // WriteDisplay 共用有界字体及私有材质写入，失败保留原文
        bool WriteDisplay(Binding b, string value)
        {
            if (!SafeText(value) || b.Text == null || chinese == null) return false;
            foreach (char c in value) if (!Char.IsWhiteSpace(c)) glyphs.Add(c);
            if (glyphs.Count > 4096) return false;
            // 此版本TryAddCharacters在没有新增字形时也返回false，最终按字符实际可用性判断
            string missing; chinese.TryAddCharacters(value, out missing, false);
            TMP_FontAsset originalFont = b.Applied == null ? b.Text.font : b.Font;
            bool fallback = false;
            foreach (char c in value)
            {
                if (Char.IsWhiteSpace(c) || chinese.HasCharacter(c, false, false)) continue;
                if (originalFont == null || Char.IsSurrogate(c)) return false;
                if (!originalFont.HasCharacter(c, true, false) && !EnsureSymbol(c)) return false;
                fallback = true;
            }
            if (chinese.atlasTextures.Length + (symbols == null ? 0 : symbols.atlasTextures.Length) > 4) return false;
            if (b.Applied == null)
            {
                if (applied.Count >= 2048) return false;
                Material material = b.Text.fontSharedMaterial;
                if (material == null || !material.HasProperty("_GradientScale") || !material.HasProperty("_MainTex")) return false;
                b.Font = b.Text.font; b.Material = material; b.Visible = b.Text.maxVisibleCharacters;
                b.Replacement = new Material(material); b.Replacement.hideFlags = HideFlags.HideAndDontSave;
                b.Replacement.mainTexture = chinese.atlasTextures[0];
                foreach (string property in new string[] { "_GradientScale", "_TextureWidth", "_TextureHeight", "_WeightNormal", "_WeightBold" })
                    if (b.Replacement.HasProperty(property) && chinese.material.HasProperty(property)) b.Replacement.SetFloat(property, chinese.material.GetFloat(property));
                applied.Add(b);
            }
            // 每个显示副本持有自己的回退表，不改游戏字体或共享中文字体
            if (fallback)
            {
                if (b.FallbackFont != null) UnityEngine.Object.Destroy(b.FallbackFont);
                b.FallbackFont = UnityEngine.Object.Instantiate(chinese);
                b.FallbackFont.hideFlags = HideFlags.HideAndDontSave;
                b.FallbackFont.fallbackFontAssetTable = new List<TMP_FontAsset> { originalFont };
                if (symbols != null) b.FallbackFont.fallbackFontAssetTable.Add(symbols);
            }
            TMP_FontAsset displayFont = fallback ? b.FallbackFont : chinese;
            if (!WriteChoiceFonts(b, displayFont)) { RestoreBinding(b); applied.Remove(b); return false; }
            if (b.ChoiceFonts == null) { b.Text.font = displayFont; b.Text.fontSharedMaterial = b.Replacement; }
            b.Text.text = value; b.Text.maxVisibleCharacters = Int32.MaxValue; b.Applied = value; return true;
        }

        // EnsureSymbol 为原字体也缺少的音标装饰建立独立Arial图集，缺字或预算不足拒绝
        bool EnsureSymbol(char value)
        {
            if (!(value >= '\u02b0' && value <= '\u02ff' || value >= '\u1d00' && value <= '\u1dff')) return false;
            if (symbols == null)
            {
                if (chinese.atlasTextures.Length >= 4) return false;
                if (symbolSource != null) UnityEngine.Object.Destroy(symbolSource);
                symbolSource = Font.CreateDynamicFontFromOSFont("Arial", 90);
                if (symbolSource == null) return false;
                symbols = TMP_FontAsset.CreateFontAsset(symbolSource, 90, 9, GlyphRenderMode.SDFAA, 1024, 1024, AtlasPopulationMode.Dynamic, false);
                if (symbols == null) return false;
                symbols.hideFlags = HideFlags.HideAndDontSave;
            }
            string missing; symbols.TryAddCharacters(value.ToString(), out missing, false);
            return symbols.atlasTextures.Length == 1 && symbols.HasCharacter(value, false, false);
        }

        void RestoreBinding(Binding b)
        {
            try
            {
                Component owner = b.Owner.Target as Component;
                bool current = owner != null && b.Text != null;
                if (current && b.Current != null) current = System.Object.ReferenceEquals(b.Current.GetValue(owner), b.Line.Target);
                if (current && b.Record != null) current = System.Object.ReferenceEquals(b.Record.GetValue(owner, null), b.OriginalRecord);
                if (current && b.Auxiliary) current = AuxiliaryIdentity(b, false);
                if (current && b.Text.text == b.Applied) { b.Text.text = b.Original; b.Text.maxVisibleCharacters = b.Visible; }
                RestoreChoiceFonts(b);
                if (b.Text != null && (b.Text.font == chinese || b.FallbackFont != null && b.Text.font == b.FallbackFont)) { b.Text.font = b.Font; b.Text.fontSharedMaterial = b.Material; }
                if (b.Replacement != null) UnityEngine.Object.Destroy(b.Replacement);
                if (b.FallbackFont != null) UnityEngine.Object.Destroy(b.FallbackFont);
                b.Applied = null; b.Replacement = null; b.FallbackFont = null;
            }
            catch { }
        }

        void Ack(int status, long page)
        {
            if (memory == IntPtr.Zero || revision <= 0) return;
            int stamp = Marshal.ReadInt32(memory, 52); Marshal.WriteInt32(memory, 52, stamp + 1); Thread.MemoryBarrier();
            Marshal.WriteInt64(memory, 26152, revision); Marshal.WriteInt64(memory, 26160, page); Marshal.WriteInt32(memory, 26168, status);
            Thread.MemoryBarrier(); Marshal.WriteInt32(memory, 52, stamp + 2);
        }

        // Restore 仅恢复仍绑定同一显示内容的对象，不覆盖已被游戏复用的新页面
        void Restore()
        {
            displayEnabled = false; translation = null;
            pendingCache = null; auxiliary.Clear(); nextScan = 0; cacheCursor = 0;
            foreach (Binding b in applied) RestoreBinding(b);
            applied.Clear(); glyphs.Clear();
            if (chinese != null) { foreach (Texture2D atlas in chinese.atlasTextures) if (atlas != null) UnityEngine.Object.Destroy(atlas); if (chinese.material != null) UnityEngine.Object.Destroy(chinese.material); UnityEngine.Object.Destroy(chinese); }
            if (source != null) UnityEngine.Object.Destroy(source);
            if (symbols != null) { foreach (Texture2D atlas in symbols.atlasTextures) if (atlas != null) UnityEngine.Object.Destroy(atlas); if (symbols.material != null) UnityEngine.Object.Destroy(symbols.material); UnityEngine.Object.Destroy(symbols); }
            if (symbolSource != null) UnityEngine.Object.Destroy(symbolSource);
            chinese = symbols = null; source = symbolSource = null;
        }

        void OnDestroy()
        { Restore(); bindings.Clear(); latestSequence = 0; line = null; CloseChannel(); if (pump == this) pump = null; }
    }
}
