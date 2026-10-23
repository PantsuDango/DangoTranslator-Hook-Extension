using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using TMPro;
using UnityEngine;

namespace Dango.YarnSpinner
{
    // Display 的界面扩展只查询已有译文，独立于剧情页和实时翻译队列
    public sealed partial class Display
    {
        public static bool InterfaceSupported { get; private set; }
        static Type historyType, dmType, directType, memberType, timeType, characterType;
        static FieldInfo recordKind, mcUsername, characterInstance;
        readonly List<Binding> auxiliary = new List<Binding>();
        Binding pendingCache;
        long nextCacheID, pendingCacheID;
        double pendingSince, nextScan;
        int cacheCursor;
        bool interfaceChinese;
        static readonly Regex Clock = new Regex("^(?:(Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday) at )?([0-9]{1,2}):([0-9]{2})(AM|PM)$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        static readonly string[] Days = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
        static readonly string[] ChineseDays = { "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日" };

        // ValidateInterface 单独核对字段与完整消费者契约，失败保留原正文注入
        static void ValidateInterface(Assembly game)
        {
            InterfaceSupported = false;
            try
            {
                if (!chatSupported || game == null || !Reader.CheckDisplay(game, InterfaceContracts)) return;
                historyType = game.GetType("HistoryEntry", true); dmType = game.GetType("DMButton", true);
                directType = game.GetType("DirectMessageManager", true); memberType = game.GetType("MemberInfo", true);
                timeType = game.GetType("TimeDisplay", true); characterType = game.GetType("CharacterManager", true);
                Type record = messageRecord.PropertyType;
                recordKind = Field(record, "kind", typeof(string));
                mcUsername = Field(characterType, "_mcUsername", typeof(string));
                characterInstance = characterType.GetField("Instance", BindingFlags.Public | BindingFlags.Static);
                if (recordKind == null || mcUsername == null || characterInstance == null || characterInstance.FieldType != characterType) return;
                foreach (string name in new string[] { "_username", "_message", "_time", "_channel" }) if (Field(historyType, name, typeof(TMP_Text)) == null) return;
                if (Field(historyType, "_textRoot", typeof(GameObject)) == null || Field(dmType, "_usernameText", typeof(TMP_Text)) == null ||
                    Field(dmType, "_username", typeof(string)) == null || Field(memberType, "_username", typeof(TMP_Text)) == null ||
                    Field(directType, "_usernameText", typeof(TMP_Text)) == null || Field(timeType, "_label", typeof(TMP_Text)) == null ||
                    Field(messageType, "_username", typeof(TMP_Text)) == null || Field(messageType, "_timeText", typeof(TMP_Text)) == null) return;
                InterfaceSupported = true;
            }
            catch { InterfaceSupported = false; }
        }

        // Frame 在已验证的Unity Update入口建立显示组件，无需等待新剧情
        public static void Frame(object owner)
        {
            if (!InterfaceSupported || owner == null || owner.GetType() != chatType) return;
            try { Component component = owner as Component; if (component != null && component.gameObject.scene.IsValid()) EnsurePump(); }
            catch { }
        }

        static bool EnsurePump()
        {
            if (pump != null) return Thread.CurrentThread.ManagedThreadId == pump.mainThread;
            if (!OpenChannel()) return false;
            GameObject root = new GameObject("Dango Yarn display"); root.hideFlags = HideFlags.HideAndDontSave;
            UnityEngine.Object.DontDestroyOnLoad(root); pump = root.AddComponent<Display>();
            pump.mainThread = Thread.CurrentThread.ManagedThreadId; return true;
        }

        // OriginalDisplay 读取仍绑定对象的原显示，避免把已经注入的中文当成原文
        static string OriginalDisplay(TMP_Text text)
        {
            if (text == null) return "";
            foreach (Binding b in applied) if (b.Text == text && text.text == b.Applied) return b.Original ?? "";
            return text.text ?? "";
        }

        static bool AuxiliaryIdentity(Binding b, bool checkFonts = true)
        {
            Component owner = b.Owner.Target as Component;
            if (owner == null || b.Text == null) return false;
            if (b.Record != null)
            {
                object record = b.Record.GetValue(owner, null);
                if (!System.Object.ReferenceEquals(record, b.OriginalRecord) || record == null ||
                    (string)payload.GetValue(record) != b.RecordPayload || (string)sender.GetValue(record) != b.RecordSender || (string)recordKind.GetValue(record) != b.RecordKind) return false;
            }
            if (b.Option != null && (!System.Object.ReferenceEquals(b.Option.GetValue(owner, null), b.OriginalOption) || owner.transform.parent != b.OptionParent ||
                !(bool)choiceAvailable.GetValue(b.OriginalOption) || Reader.DisplayText(choiceLine.GetValue(b.OriginalOption)) != b.Original)) return false;
            if (checkFonts && !ChoiceFontsCurrent(b)) return false;
            if (b.GuardTexts != null)
                for (int i = 0; i < b.GuardTexts.Length; i++) if (b.GuardTexts[i] == null || OriginalDisplay(b.GuardTexts[i]) != b.GuardValues[i]) return false;
            return b.Text.text == b.Original || b.Text.text == b.Applied;
        }

        bool AuxiliaryReady(Binding b)
        {
            Component owner = b.Owner.Target as Component;
            return owner != null && owner.gameObject.activeInHierarchy && b.Text != null && b.Text.gameObject.activeInHierarchy &&
                owner.gameObject.scene.IsValid() && AuxiliaryIdentity(b);
        }

        void RemoveAuxiliary(Binding b)
        { RestoreBinding(b); applied.Remove(b); auxiliary.Remove(b); if (pendingCache == b) pendingCache = null; }

        // RestoreInterface 可选界面异常只撤销辅助显示，不关闭已经验证的正文注入
        void RestoreInterface()
        {
            pendingCache = null;
            foreach (Binding b in auxiliary.ToArray()) RemoveAuxiliary(b);
            auxiliary.Clear();
        }

        // RegisterAuxiliary 按对象与当前显示身份绑定，每次对象复用都撤销旧结果
        Binding RegisterAuxiliary(Component owner, TMP_Text text, bool body, object record, TMP_Text[] guards)
        {
            if (owner == null || text == null || !owner.gameObject.scene.IsValid() || !text.gameObject.activeInHierarchy) return null;
            if (bindings.Exists(b => b.Text == text)) return null;
            Binding old = auxiliary.Find(b => b.Text == text);
            if (old != null && AuxiliaryIdentity(old)) return old;
            if (old != null) RemoveAuxiliary(old);
            if (applied.Exists(b => b.Text == text) || auxiliary.Count >= 2048) return null;
            string originalText = text.text ?? "";
            if (String.IsNullOrWhiteSpace(originalText) || originalText.Length > 12000) return null;
            Binding binding = new Binding { Owner = new WeakReference(owner), Text = text, Original = originalText,
                Auxiliary = true, CacheBody = body, LastQuery = -10 };
            if (record != null)
            {
                binding.Record = messageRecord; binding.OriginalRecord = record;
                binding.RecordPayload = (string)payload.GetValue(record); binding.RecordSender = (string)sender.GetValue(record); binding.RecordKind = (string)recordKind.GetValue(record);
                binding.PostedChoice = body && binding.RecordSender == "MC";
            }
            if (guards != null)
            {
                binding.GuardTexts = guards; binding.GuardValues = new string[guards.Length];
                for (int i = 0; i < guards.Length; i++) binding.GuardValues[i] = OriginalDisplay(guards[i]);
            }
            auxiliary.Add(binding); return binding;
        }

        static TMP_Text TextField(Component owner, string name)
        { return owner.GetType().GetField(name, InstanceFields).GetValue(owner) as TMP_Text; }

        static string PlayerName()
        { object manager = characterInstance.GetValue(null); return manager == null ? "" : (string)mcUsername.GetValue(manager) ?? ""; }

        // LocalClock 只格式化已确认英文星期与AM/PM模板，不修改时间模型
        static string LocalClock(string text)
        {
            Match match = Clock.Match(text ?? ""); if (!match.Success) return null;
            int hour = Int32.Parse(match.Groups[2].Value), minute = Int32.Parse(match.Groups[3].Value);
            if (hour < 1 || hour > 12 || minute > 59) return null;
            string day = "";
            for (int i = 0; i < Days.Length; i++) if (String.Equals(match.Groups[1].Value, Days[i], StringComparison.OrdinalIgnoreCase)) day = ChineseDays[i] + " ";
            return day + (String.Equals(match.Groups[4].Value, "AM", StringComparison.OrdinalIgnoreCase) ? "上午" : "下午") + hour + ":" + match.Groups[3].Value;
        }

        void RegisterClock(Component owner, TMP_Text text, object record)
        {
            if (!interfaceChinese || text == null) return;
            string value = LocalClock(OriginalDisplay(text)); if (value == null) return;
            Binding b = RegisterAuxiliary(owner, text, false, record, null);
            if (b != null && AuxiliaryReady(b) && b.Applied != value) WriteDisplay(b, value);
        }

        void RegisterName(Component owner, TMP_Text text, object record, string player)
        {
            if (text == null || OriginalDisplay(text) == player || (record != null && (string)sender.GetValue(record) == "MC")) return;
            RegisterAuxiliary(owner, text, false, record, null);
        }

        void ScanMessages(string player)
        {
            UnityEngine.Object[] objects = Resources.FindObjectsOfTypeAll(messageType); if (objects.Length > 4096) return;
            foreach (UnityEngine.Object item in objects)
            {
                Component owner = item as Component; if (owner == null || !owner.gameObject.scene.IsValid()) continue;
                object record = messageRecord.GetValue(owner, null); if (record == null) continue;
                string kind = (string)recordKind.GetValue(record);
                TMP_Text body = messageText.GetValue(owner) as TMP_Text;
                if (kind == "text" && body != null && OriginalDisplay(body) == (string)payload.GetValue(record)) RegisterAuxiliary(owner, body, true, record, null);
                RegisterName(owner, TextField(owner, "_username"), record, player);
                RegisterClock(owner, TextField(owner, "_timeText"), record);
            }
        }

        void ScanHistory(string player)
        {
            UnityEngine.Object[] objects = Resources.FindObjectsOfTypeAll(historyType); if (objects.Length > 4096) return;
            foreach (UnityEngine.Object item in objects)
            {
                Component owner = item as Component; if (owner == null || !owner.gameObject.scene.IsValid()) continue;
                TMP_Text name = TextField(owner, "_username"), time = TextField(owner, "_time"), channel = TextField(owner, "_channel");
                GameObject textRoot = owner.GetType().GetField("_textRoot", InstanceFields).GetValue(owner) as GameObject;
                if (textRoot != null && textRoot.activeInHierarchy && name != null && time != null && channel != null)
                {
                    Binding b = RegisterAuxiliary(owner, TextField(owner, "_message"), true, null, new TMP_Text[] { name, time, channel });
                    if (b != null) b.PostedChoice = OriginalDisplay(name) == player;
                }
                RegisterName(owner, name, null, player); RegisterClock(owner, time, null);
                if (channel != null && interfaceChinese && OriginalDisplay(channel) == "server")
                { Binding b = RegisterAuxiliary(owner, channel, false, null, null); if (b != null && AuxiliaryReady(b)) WriteDisplay(b, "聊天室"); }
                else RegisterName(owner, channel, null, player);
            }
        }

        static string Hierarchy(Transform target)
        {
            string result = target.name;
            for (int depth = 0; target.parent != null && depth < 32; depth++) { target = target.parent; result = target.name + "/" + result; }
            return result;
        }

        void ScanFixedLabels()
        {
            if (!interfaceChinese) return;
            string[] paths = { "== CHATROOM ==/ChatroomOverlay/Background/Left Column/MembersTitle", "== CHATROOM ==/ChatroomOverlay/Background/Left Column/ServerText",
                "== CHATROOM ==/ChatroomOverlay/Background/Left Column/UserText", "== CHATROOM ==/ChatroomOverlay/Background/Left Column/ServerButton/ServerName",
                "PcCanva/Wallpaper/CupidButton/CupidText", "Canvas/TypeToSearchText", "OverlayCanvas/SettingScreen/TypeToSearchText", "OverlayCanvas/SaveLoadScreen/TypeToSearchText" };
            string[] originals = { "DIRECT MESSAGES", "servers", "user profile", "CUPID's\nchatroom", "Cupid's Chatroom", "TYPE HERE TO SEARCH", "TYPE HERE TO SEARCH", "TYPE HERE TO SEARCH" };
            string[] translated = { "私信", "聊天室", "个人资料", "丘比特的\n聊天室", "丘比特聊天室", "在此输入以搜索", "在此输入以搜索", "在此输入以搜索" };
            TextMeshProUGUI[] texts = Resources.FindObjectsOfTypeAll<TextMeshProUGUI>(); if (texts.Length > 4096) return;
            foreach (TMP_Text text in texts)
            {
                if (text == null || !text.gameObject.scene.IsValid() || text.GetType() != typeof(TextMeshProUGUI)) continue;
                string path = Hierarchy(text.transform), originalText = OriginalDisplay(text);
                for (int i = 0; i < paths.Length; i++) if ((path == paths[i] || path.EndsWith("/" + paths[i], StringComparison.Ordinal)) && originalText == originals[i])
                { Binding b = RegisterAuxiliary(text, text, false, null, null); if (b != null && AuxiliaryReady(b) && b.Applied != translated[i]) WriteDisplay(b, translated[i]); }
            }
        }

        void ScanInterface()
        {
            string player = PlayerName(); ScanMessages(player); ScanHistory(player); ScanFixedLabels(); ScanChoices();
            foreach (Type type in new Type[] { dmType, directType, memberType, timeType })
            {
                UnityEngine.Object[] objects = Resources.FindObjectsOfTypeAll(type); if (objects.Length > 4096) continue;
                foreach (UnityEngine.Object item in objects)
                {
                    Component owner = item as Component; if (owner == null || !owner.gameObject.scene.IsValid()) continue;
                    if (type == timeType) RegisterClock(owner, TextField(owner, "_label"), null);
                    else
                    {
                        if (type == dmType && (string)type.GetField("_username", InstanceFields).GetValue(owner) == "MC") continue;
                        RegisterName(owner, TextField(owner, type == memberType ? "_username" : "_usernameText"), null, player);
                    }
                }
            }
        }

        // UpdateInterface 每次只发一条精确缓存查询，未命中限速重查，不保存负缓存
        void UpdateInterface()
        {
            double now = Time.realtimeSinceStartup;
            for (int i = auxiliary.Count - 1; i >= 0; i--) if (!AuxiliaryReady(auxiliary[i])) RemoveAuxiliary(auxiliary[i]);
            if (now >= nextScan) { nextScan = now + 0.75; ScanInterface(); }
            if (pendingCache != null)
            {
                int stamp = Marshal.ReadInt32(memory, 65536);
                if ((stamp & 1) == 0)
                {
                    byte[] snapshot = new byte[24036]; Marshal.Copy(IntPtr.Add(memory, 65536), snapshot, 0, snapshot.Length); Thread.MemoryBarrier();
                    if (stamp == Marshal.ReadInt32(memory, 65536) && BitConverter.ToInt64(snapshot, 8) == pendingCacheID && BitConverter.ToInt64(snapshot, 16) == epoch)
                    {
                        uint length = BitConverter.ToUInt32(snapshot, 4);
                        if (length <= 12000 && AuxiliaryReady(pendingCache) && length > 0) WriteDisplay(pendingCache, Encoding.Unicode.GetString(snapshot, 32, (int)length * 2));
                        pendingCache = null;
                    }
                }
                if (pendingCache != null && now - pendingSince < 2) return;
                pendingCache = null;
            }
            for (int count = 0; count < auxiliary.Count; count++)
            {
                if (cacheCursor >= auxiliary.Count) cacheCursor = 0;
                Binding b = auxiliary[cacheCursor++];
                if (b.Applied != null || now - b.LastQuery < 3 || !AuxiliaryReady(b)) continue;
                string value = b.Original;
                if (String.IsNullOrWhiteSpace(value) || value.Length > 12000) continue;
                int stamp = Marshal.ReadInt32(memory, 32768); Marshal.WriteInt32(memory, 32768, stamp + 1); Thread.MemoryBarrier();
                byte[] bytes = Encoding.Unicode.GetBytes(value); Marshal.WriteInt32(memory, 32772, value.Length);
                pendingCacheID = ++nextCacheID; Marshal.WriteInt64(memory, 32776, pendingCacheID); Marshal.WriteInt64(memory, 32784, epoch);
                Marshal.WriteInt32(memory, 32792, b.PostedChoice ? 1 : 0);
                Marshal.Copy(bytes, 0, IntPtr.Add(memory, 32800), bytes.Length);
                Thread.MemoryBarrier(); Marshal.WriteInt32(memory, 32768, stamp + 2);
                b.LastQuery = pendingSince = now; pendingCache = b; return;
            }
        }
    }
}
