using System;
using System.Collections.Generic;
using System.Reflection;
using TMPro;
using UnityEngine;

namespace Dango.YarnSpinner
{
    public sealed partial class Display
    {
        static Type choiceType, recorderType, choiceHoverType;
        static PropertyInfo choiceValue, optionsContainer;
        static FieldInfo choiceText, choiceLine, choiceAvailable, recorderText;
        static FieldInfo hoverText, normalFont, hoverFont;
        static Type themedHoverType;
        static FieldInfo themedText;
        static FieldInfo[] themedFonts;
        public static bool ChoicesSupported { get; private set; }

        // ValidateChoices 独立验证选项显示、选择回调和原文记录消费者，不关闭正文或界面
        static void ValidateChoices(Assembly yarn, Assembly game)
        {
            ChoicesSupported = false;
            try
            {
                if (!chatSupported || game == null || !Reader.CheckDisplay(yarn, ChoiceViewContracts) || !Reader.CheckDisplay(game, ChoiceChatContracts)) return;
                choiceType = yarn.GetType("Yarn.Unity.OptionView", true);
                choiceValue = choiceType.GetProperty("Option");
                choiceText = Field(choiceType, "text", typeof(TextMeshProUGUI));
                optionsContainer = chatType.GetProperty("CurrentOptionsContainer");
                if (choiceValue == null || optionsContainer == null || optionsContainer.PropertyType != typeof(Transform) || choiceText == null) return;
                choiceLine = Field(choiceValue.PropertyType, "Line", Reader.LineType);
                choiceAvailable = Field(choiceValue.PropertyType, "IsAvailable", typeof(bool));
                recorderType = game.GetType("CupidChatroom.Yarn.CallChoiceRecorder", true);
                recorderText = Field(recorderType, "_text", typeof(TMP_Text));
                choiceHoverType = game.GetType("ChoiceHover", true);
                hoverText = Field(choiceHoverType, "_text", typeof(TMP_Text));
                normalFont = Field(choiceHoverType, "_normalFontAsset", typeof(TMP_FontAsset));
                hoverFont = Field(choiceHoverType, "_hoverFontAsset", typeof(TMP_FontAsset));
                themedHoverType = game.GetType("CupidChatroom.Theming.ThemedChoiceHover", true);
                themedText = Field(themedHoverType, "_text", typeof(TMP_Text));
                themedFonts = new FieldInfo[] { Field(themedHoverType, "_normalFontAsset", typeof(TMP_FontAsset)), Field(themedHoverType, "_hoverFontAsset", typeof(TMP_FontAsset)), Field(themedHoverType, "_nightNormalFontAsset", typeof(TMP_FontAsset)), Field(themedHoverType, "_nightHoverFontAsset", typeof(TMP_FontAsset)) };
                ChoicesSupported = choiceLine != null && choiceAvailable != null && recorderText != null && hoverText != null && normalFont != null && hoverFont != null && themedText != null && Array.TrueForAll(themedFonts, field => field != null);
            }
            catch { ChoicesSupported = false; }
        }

        // ScanChoices 仅替换当前聊天菜单的精确显示副本，读取显示文字的电话记录器继续原文
        void ScanChoices()
        {
            if (!ChoicesSupported) return;
            try
            {
                object view = chatType.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static).GetValue(null, null);
                Transform container = view == null ? null : optionsContainer.GetValue(view, null) as Transform;
                if (container == null || !container.gameObject.activeInHierarchy) return;
                UnityEngine.Object[] choices = Resources.FindObjectsOfTypeAll(choiceType);
                UnityEngine.Object[] recorders = Resources.FindObjectsOfTypeAll(recorderType);
                UnityEngine.Object[] hovers = Resources.FindObjectsOfTypeAll(choiceHoverType);
                if (themedHoverType != null)
                {
                    UnityEngine.Object[] themed = Resources.FindObjectsOfTypeAll(themedHoverType);
                    var all = new List<UnityEngine.Object>(hovers); all.AddRange(themed); hovers = all.ToArray();
                }
                if (choices.Length > 128 || recorders.Length > 256 || hovers.Length > 256) return;
                foreach (UnityEngine.Object item in choices)
                {
                    Component owner = item as Component;
                    if (owner == null || owner.GetType() != choiceType || owner.transform.parent != container || !owner.gameObject.activeInHierarchy) continue;
                    object option = choiceValue.GetValue(owner, null);
                    if (option == null || !(bool)choiceAvailable.GetValue(option)) continue;
                    TMP_Text text = choiceText.GetValue(owner) as TMP_Text;
                    if (text == null || OriginalDisplay(text) != Reader.DisplayText(choiceLine.GetValue(option))) continue;
                    bool recordsDisplay = false;
                    foreach (UnityEngine.Object recorder in recorders) if ((recorder as Component) != null && (recorderText.GetValue(recorder) as TMP_Text) == text) { recordsDisplay = true; break; }
                    if (recordsDisplay)
                    {
                        Binding old = auxiliary.Find(entry => entry.Text == text);
                        if (old != null) RemoveAuxiliary(old);
                        continue;
                    }
                    Component hover = null;
                    bool compatible = true;
                    foreach (UnityEngine.Object candidate in hovers)
                    {
                        Component component = candidate as Component;
                        if (component == null) continue;
                        FieldInfo textField = themedHoverType != null && themedHoverType.IsInstanceOfType(component) ? themedText : hoverText;
                        if ((textField.GetValue(component) as TMP_Text) != text) continue;
                        if (hover != null || component.GetType() != choiceHoverType && component.GetType() != themedHoverType || !component.gameObject.scene.IsValid()) { compatible = false; break; }
                        hover = component;
                    }
                    Binding previous = auxiliary.Find(entry => entry.Text == text);
                    if (!compatible || previous != null && !System.Object.ReferenceEquals(previous.ChoiceHover == null ? null : previous.ChoiceHover.Target, hover))
                    {
                        if (previous != null) RemoveAuxiliary(previous);
                        if (!compatible) continue;
                    }
                    Binding b = RegisterAuxiliary(owner, text, false, null, null);
                    if (b != null) { b.Option = choiceValue; b.OriginalOption = option; b.OptionParent = container; b.ChoiceHover = hover == null ? null : new WeakReference(hover); }
                }
            }
            catch { ChoicesSupported = false; }
        }

        sealed class ChoiceFonts
        {
            internal Component Owner;
            internal FieldInfo[] Fields;
            internal TMP_FontAsset[] Originals, Copies;
        }

        // CloneChoiceFont 保留原正常或悬停字体的样式，仅在私有副本中追加中文回退
        static TMP_FontAsset CloneChoiceFont(TMP_FontAsset original, TMP_FontAsset fallback)
        {
            if (original == null || original.material == null || fallback == null) return null;
            TMP_FontAsset copy = UnityEngine.Object.Instantiate(original);
            try
            {
                copy.hideFlags = HideFlags.HideAndDontSave;
                copy.fallbackFontAssetTable = new List<TMP_FontAsset> { fallback };
                if (original.fallbackFontAssetTable != null) copy.fallbackFontAssetTable.AddRange(original.fallbackFontAssetTable);
                copy.material = new Material(original.material);
                copy.material.hideFlags = HideFlags.HideAndDontSave;
                return copy;
            }
            catch { UnityEngine.Object.Destroy(copy); throw; }
        }

        // ChoiceFontsCurrent 核对悬停对象和两个显示字段，拒绝外部改写或对象复用
        static bool ChoiceFontsCurrent(Binding b)
        {
            if (b.ChoiceHover == null) return b.ChoiceFonts == null;
            Component owner = b.ChoiceHover.Target as Component;
            if (owner == null || ((owner.GetType() == themedHoverType ? themedText : hoverText).GetValue(owner) as TMP_Text) != b.Text) return false;
            ChoiceFonts fonts = b.ChoiceFonts;
            if (fonts == null) return true;
            if (fonts.Owner != owner) return false;
            for (int i = 0; i < fonts.Fields.Length; ++i) if ((fonts.Fields[i].GetValue(owner) as TMP_FontAsset) != fonts.Copies[i]) return false;
            return true;
        }

        // WriteChoiceFonts 只为已确认选项替换悬停字体引用，事件仍执行原显示与选择逻辑
        bool WriteChoiceFonts(Binding b, TMP_FontAsset fallback)
        {
            if (b.ChoiceHover == null) return true;
            if (!ChoiceFontsCurrent(b)) return false;
            if (b.ChoiceFonts == null)
            {
                Component owner = b.ChoiceHover.Target as Component;
                FieldInfo[] fields = owner.GetType() == themedHoverType ? themedFonts : new FieldInfo[] { normalFont, hoverFont };
                ChoiceFonts fonts = new ChoiceFonts { Owner = owner, Fields = fields, Originals = new TMP_FontAsset[fields.Length], Copies = new TMP_FontAsset[fields.Length] };
                for (int i = 0; i < fields.Length; ++i) fonts.Originals[i] = fields[i].GetValue(owner) as TMP_FontAsset;
                if (fonts.Originals[0] == null || fonts.Originals[1] == null || Array.IndexOf(fonts.Originals, b.Text.font) < 0) return false;
                b.ChoiceFonts = fonts;
                try
                {
                    for (int i = 0; i < fields.Length; ++i)
                    {
                        if (fonts.Originals[i] == null) continue;
                        fonts.Copies[i] = CloneChoiceFont(fonts.Originals[i], fallback);
                        if (fonts.Copies[i] == null) { RestoreChoiceFonts(b); return false; }
                    }
                    for (int i = 0; i < fields.Length; ++i) if (fonts.Originals[i] != null) fields[i].SetValue(owner, fonts.Copies[i]);
                }
                catch { RestoreChoiceFonts(b); return false; }
            }
            ChoiceFonts active = b.ChoiceFonts;
            for (int i = 0; i < active.Copies.Length; ++i) if (active.Copies[i] != null) active.Copies[i].fallbackFontAssetTable[0] = fallback;
            int index = Array.IndexOf(active.Originals, b.Text.font);
            if (index < 0) index = Array.IndexOf(active.Copies, b.Text.font);
            if (index < 0 || active.Copies[index] == null) return false;
            TMP_FontAsset target = active.Copies[index];
            b.Text.font = target; b.Text.fontSharedMaterial = target.material;
            return true;
        }

        // RestoreChoiceFonts 仅恢复仍属于本绑定的私有引用，不改共享字体及其图集
        static void RestoreChoiceFonts(Binding b)
        {
            ChoiceFonts fonts = b.ChoiceFonts;
            if (fonts == null) return;
            try
            {
                if (fonts.Owner != null)
                {
                    for (int i = 0; i < fonts.Fields.Length; ++i)
                        if (fonts.Copies[i] != null && (fonts.Fields[i].GetValue(fonts.Owner) as TMP_FontAsset) == fonts.Copies[i]) fonts.Fields[i].SetValue(fonts.Owner, fonts.Originals[i]);
                }
                if (b.Text != null)
                {
                    int index = Array.IndexOf(fonts.Copies, b.Text.font);
                    TMP_FontAsset original = index < 0 ? null : fonts.Originals[index];
                    if (original != null) { b.Text.font = original; b.Text.fontSharedMaterial = original == b.Font ? b.Material : original.material; }
                }
            }
            finally
            {
                foreach (TMP_FontAsset copy in fonts.Copies)
                    if (copy != null) { if (copy.material != null) UnityEngine.Object.Destroy(copy.material); UnityEngine.Object.Destroy(copy); }
                b.ChoiceFonts = null;
            }
        }
    }
}
