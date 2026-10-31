using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using UnityEngine;

namespace Dango.UnityMono
{
    // The bridge touches Unity objects only from the verified canvas frame callback.
    public static partial class Reader
    {
        const BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;
        static Type tmp, ui, mesh, canvasGroup;
        static string cache, digest, failure;
        static bool configured, loaded, attempted, invoking, subscribed, rendering;
        static float nextScan, nextTick, nextSnapshot;
        static int scanPhase=-1,scanIndex,lastScanFrame=-1;
        static UnityEngine.Object[] scanObjects;
        static readonly List<Source> scanSources=new List<Source>();
        static readonly Dictionary<Type,List<Contract>> pageContracts=new Dictionary<Type,List<Contract>>();
        static readonly Dictionary<Type,Members> memberCache=new Dictionary<Type,Members>();
        static readonly Dictionary<Type,object> textSources=new Dictionary<Type,object>();
        static long sequence;
        static int sceneFrames;
        static IntPtr lease, leaseMemory, host;
        static long hostCreated;
        static readonly Dictionary<string,string> translations = new Dictionary<string,string>(StringComparer.Ordinal);
        static readonly Dictionary<string,string> resourceOriginals = new Dictionary<string,string>(StringComparer.Ordinal);
        static readonly Dictionary<Component,Label> labels = new Dictionary<Component,Label>();
        static readonly List<Source> sources = new List<Source>();
        static readonly Queue<string> packets = new Queue<string>();
        static readonly Queue<string> diagnostics = new Queue<string>();
        static readonly HashSet<string> diagnosed = new HashSet<string>();
        static readonly List<UnityEngine.Object> owned = new List<UnityEngine.Object>();
        static UnityEngine.Object chinese;
        static Font uiFont;
        static readonly HashSet<int> glyphs = new HashSet<int>();
        static readonly Regex styles = new Regex(@"</?(?:b|i|u|s|color|size|alpha)(?:[ =][^<>]*)?>",RegexOptions.IgnoreCase);
        static readonly Regex templates = new Regex(@"\{[^{}\r\n]{1,256}\}|<(?:PLAYERNAME|PARTYSIZE|[A-Z_][A-Z_0-9]*)>");
        static readonly Dictionary<short,OpCode> opcodes = new Dictionary<short,OpCode>();
        public static string Status { get { return loaded ? "ready " + digest : attempted ? "failed" : ""; } }
        public static string Diagnostic { get { return diagnostics.Count == 0 ? "" : diagnostics.Dequeue(); } }

        sealed class Label {
            internal Component Text;
            internal string Raw, Published, Full;
            internal float Changed;
            internal bool Applied;
            internal bool Visible;
            internal bool Ready;
            internal string Target;
            internal string Page;
            internal long Revision;
            internal FieldInfo TextField;
            internal UnityEngine.Object Font;
            internal Material Material;
        }
        sealed class Source { internal Component Owner; internal FieldInfo Field; internal List<Component> Targets; }
        sealed class Contract { internal FieldInfo Page,Target; }
        sealed class Members {
            internal readonly Dictionary<string,FieldInfo> Fields=new Dictionary<string,FieldInfo>();
            internal readonly Dictionary<string,PropertyInfo> Properties=new Dictionary<string,PropertyInfo>();
            internal readonly Dictionary<string,MethodInfo> Methods=new Dictionary<string,MethodInfo>();
        }

        static void Diagnose(string value) { if(diagnosed.Count<64 && diagnosed.Add(value)) diagnostics.Enqueue(value); }
        static Type Find(string name) {
            Type result=null;
            foreach(Assembly a in AppDomain.CurrentDomain.GetAssemblies()) {
                Type t=a.GetType(name,false); if(t==null) continue;
                if(result!=null && result!=t) return null; result=t;
            }
            // Unity 6 may defer-load TextCore and TMP modules until the first
            // font asset is touched. Resolve the known module explicitly so a
            // valid type is not mistaken for a missing schema.
            string[] modules;
            if(name.StartsWith("UnityEngine.TextCore.",StringComparison.Ordinal))
                modules=new string[]{"UnityEngine.TextCoreFontEngineModule","UnityEngine.TextCoreTextEngineModule"};
            else if(name.StartsWith("TMPro.",StringComparison.Ordinal))
                modules=new string[]{"Unity.TextMeshPro"};
            else modules=new string[0];
            foreach(string module in modules) {
                try {
                    Assembly a=Assembly.Load(module); Type t=a.GetType(name,false); if(t==null) continue;
                    if(result!=null && result!=t) return null; result=t;
                } catch { }
            }
            return result;
        }
        // CachedMembers 按实际类型身份复用反射元数据，包含不存在与歧义成员的结果
        static Members CachedMembers(Type type) {
            Members members;if(!memberCache.TryGetValue(type,out members)){members=new Members();memberCache.Add(type,members);}return members;
        }
        // Field 缓存继承链字段查找，避免每帧重复枚举同一类型
        static FieldInfo Field(Type t,string name) {
            Members members=CachedMembers(t);FieldInfo field;if(members.Fields.TryGetValue(name,out field))return field;
            for(Type current=t;current!=null;current=current.BaseType) {field=current.GetField(name,Flags|BindingFlags.DeclaredOnly);if(field!=null)break;}
            members.Fields.Add(name,field);return field;
        }
        // Property 缓存属性元数据但不缓存对象属性值，保留动态可见性
        static PropertyInfo Property(Type type,string name) {
            Members members=CachedMembers(type);PropertyInfo property;
            if(!members.Properties.TryGetValue(name,out property)){property=type.GetProperty(name,Flags);members.Properties.Add(name,property);}return property;
        }
        static object Get(object obj,string name) {
            if(obj==null)return null; Type t=obj.GetType();PropertyInfo p=Property(t,name);if(p!=null)return p.GetValue(obj,null);
            FieldInfo field=Field(t,name);return field==null?null:field.GetValue(obj);
        }
        static void Set(object obj,string name,object value) {
            PropertyInfo p=Property(obj.GetType(),name);
            if(p!=null && p.CanWrite) p.SetValue(obj,ConvertValue(value,p.PropertyType),null);
            else { FieldInfo f=Field(obj.GetType(),name);if(f==null)throw new MissingFieldException(name);f.SetValue(obj,ConvertValue(value,f.FieldType)); }
        }
        static object ConvertValue(object value,Type type) { return value!=null&&!type.IsInstanceOfType(value)&&value is IConvertible ? Convert.ChangeType(value,type) : value; }
        // Method 缓存唯一方法签名及歧义结果，不重复分配完整方法数组
        static MethodInfo Method(Type t,string name,int count) {
            Members members=CachedMembers(t);string key=name+":"+count;MethodInfo cached;if(members.Methods.TryGetValue(key,out cached))return cached;
            MethodInfo result=null;
            foreach(MethodInfo m in t.GetMethods(Flags)) if(m.Name==name && m.GetParameters().Length==count) {if(result!=null){members.Methods.Add(key,null);return null;}result=m;}
            members.Methods.Add(key,result);
            return result;
        }
        public static bool Validate() {
            tmp=Find("TMPro.TMP_Text");ui=Find("UnityEngine.UI.Text");mesh=Find("UnityEngine.TextMesh");canvasGroup=Find("UnityEngine.CanvasGroup");
            if(tmp==null)tmp=Find("TMPro.TextMeshProUGUI");
            return tmp!=null || ui!=null || mesh!=null;
        }
        public static void Configure(string path) { cache=path;configured=true; }
        static string Normalize(string s) { return styles.Replace(s.Replace("\r\n","\n").Replace('\r','\n').Replace("<br>","\n").Replace("<br/>","\n"),"").Trim('\ufeff','\0',' ','\t','\n'); }
        static bool Active(Component c) {
            if(c==null || !c.gameObject.activeInHierarchy)return false;
            Behaviour b=c as Behaviour;return b==null || b.enabled;
        }
        // CanvasGroup panels can remain active while their text is transparent.
        // Visibility gates capture and replay only; it never changes game state.
        static bool Visible(Component c) {
            if(!Active(c))return false;
            object scene=Get(c.gameObject,"scene");
            if(scene!=null) {
                MethodInfo valid=Method(scene.GetType(),"IsValid",0);
                if(valid!=null && !(bool)valid.Invoke(scene,null))return false;
            }
            object renderer=Get(c,"canvasRenderer");
            if(renderer!=null) {
                object culled=Get(renderer,"cull");if(culled is bool && (bool)culled)return false;
                MethodInfo alpha=Method(renderer.GetType(),"GetAlpha",0);
                if(alpha!=null && Convert.ToSingle(alpha.Invoke(renderer,null))<=0.001f)return false;
            }
            float opacity=1;
            if(canvasGroup!=null)for(Transform p=c.transform;p!=null;p=p.parent) {
                bool ignore=false;
                foreach(Component group in p.GetComponents(canvasGroup))if(group!=null) {
                    Behaviour b=group as Behaviour;if(b!=null&&!b.enabled)continue;
                    object alpha=Get(group,"alpha");if(alpha!=null)opacity*=Convert.ToSingle(alpha);
                    object skip=Get(group,"ignoreParentGroups");if(skip is bool && (bool)skip)ignore=true;
                }
                if(opacity<=0.001f)return false;if(ignore)break;
            }
            return true;
        }
        static bool Safe(string s) {
            if(String.IsNullOrEmpty(s)||s.Length>16000)return false;
            string checkedText=templates.Replace(s,"x");
            if(checkedText.IndexOfAny(new char[]{'\0','{','}','<','>'})>=0)return false;
            for(int i=0;i<checkedText.Length;i++) if(Char.IsSurrogate(checkedText[i])||Char.IsControl(checkedText[i]) && checkedText[i]!='\n' && checkedText[i]!='\t')return false;
            return true;
        }
        static bool Readable(string s) {
            if(String.IsNullOrEmpty(s)||s.Length>16000)return false;
            foreach(char c in s)if(Char.IsLetter(c))return true;return false;
        }
        // ContentText 仅接收已确认对话与说明正文，交互标签和用途不明的短UI保留原文
        static bool ContentText(Label label) {
            Component c=label.Text;
            for(Transform p=c.transform;p!=null;p=p.parent)foreach(Component control in p.GetComponents<Component>()) {
                if(control==null)continue;
                for(Type type=control.GetType();type!=null;type=type.BaseType) {
                    string name=type.FullName;
                    if(name=="UnityEngine.UI.Selectable"||name=="TMPro.TMP_InputField")return false;
                }
            }
            if(!String.IsNullOrEmpty(label.Full))return true;
            string text=Normalize(label.Raw??"");if(!Readable(text))return false;
            // 自动换行本身不是正文证据，标题菜单同样可能打开该属性
            // 自然语句才进入说明回退，多行菜单列表不单凭换行放行
            // 不依赖游戏名、控件名或固定菜单文字
            // 无句号的按键教程允许长说明段落，短菜单逐行罗列仍排除
            if(text.IndexOf('\n')>=0)foreach(string line in text.Split('\n')) {
                int letters=0;foreach(char ch in line)if(Char.IsLetter(ch))letters++;
                if(letters>=20)return true;
            }
            for(int i=0;i<text.Length;i++) {
                char ch=text[i];
                if(ch=='。'||ch=='！'||ch=='？'||ch=='!'||ch=='?')return true;
                if(ch=='.' && (i+1==text.Length||Char.IsWhiteSpace(text[i+1])) && i>0 && Char.IsLetter(text[i-1]))return true;
            }
            return false;
        }
        static string Escape(string s) {
            StringBuilder b=new StringBuilder();foreach(char c in s ?? "") {
                if(c=='"'||c=='\\')b.Append('\\').Append(c);else if(c<' ')b.Append("\\u").Append(((int)c).ToString("x4"));else b.Append(c);
            }return b.ToString();
        }
        static void Publish(List<Label> candidates) {
            if(packets.Count>=64)return;
            List<Label> accepted=new List<Label>();List<string> texts=new List<string>(),translated=new List<string>();
            HashSet<string> unique=new HashSet<string>(StringComparer.Ordinal);int budget=0;bool anyTranslated=false;
            foreach(Label l in candidates) {
                string s=Normalize(l.Full??l.Raw);if(!Readable(s)||s==l.Published)continue;
                string original=null;bool native=loaded&&resourceOriginals.TryGetValue(s,out original);
                string nativeText=s;
                if(native&&!String.IsNullOrEmpty(original))s=original;
                if(unique.Contains(s)){accepted.Add(l);continue;}
                string tr;bool hit=translations.TryGetValue(s,out tr);
                if(native){hit=true;tr=nativeText;}
                // Account for both text and exact segments, including JSON escapes.
                int cost=2*Escape(s).Length+Escape(hit?tr:s).Length+16;
                if(cost>23000){Diagnose("capture:packet-too-large");continue;}
                if(budget+cost>23000||texts.Count>=256)continue;
                budget+=cost;unique.Add(s);texts.Add(s);translated.Add(hit?tr:s);anyTranslated|=hit;accepted.Add(l);
            }
            if(texts.Count==0)return;
            string[] escaped=new string[texts.Count];for(int i=0;i<texts.Count;i++)escaped[i]="\""+Escape(texts[i])+"\"";
            long pageSequence=++sequence;
            long epoch=BindLivePage(pageSequence,accepted,texts);
            string packet="{\"sequence\":"+pageSequence+",\"liveEpoch\":"+epoch+",\"owner\":"+((uint)RuntimeHelpers.GetHashCode(accepted[0].Text))+",\"kind\":\"ui\",\"text\":\""+Escape(String.Join("\n",texts.ToArray()))+"\",\"segments\":["+String.Join(",",escaped)+"],\"speaker\":\"\",\"translation\":\""+Escape(anyTranslated?String.Join("\n",translated.ToArray()):"")+"\",\"backfill\":"+(loaded?"true":"false")+"}";
            if(packet.Length>=24500){Diagnose("capture:packet-too-large");return;}
            foreach(Label l in accepted)l.Published=Normalize(l.Full??l.Raw);
            packets.Enqueue(packet);
        }

        // InheritsType 仅沿已闭合类的真实继承链核对身份，避免Mono对未绑定泛型执行原生可赋值检查
        static bool InheritsType(Type type,Type parent) {
            if(type==null||parent==null)return false;
            try {
                if(type.IsGenericParameter||type.ContainsGenericParameters||type.HasElementType||type.IsInterface||
                    parent.IsGenericParameter||parent.ContainsGenericParameters||parent.HasElementType||parent.IsInterface)return false;
                for(int depth=0;type!=null&&depth<128;depth++,type=type.BaseType)if(type==parent)return true;
            }catch(Exception){return false;}
            return false;
        }
        // DisplayType 按实际文本基类识别来源字段，不对任意IL字段调用IsAssignableFrom
        static bool DisplayType(Type t) {return InheritsType(t,tmp)||InheritsType(t,ui)||InheritsType(t,mesh);}
        // PageFields 按闭合类型永久缓存来源合同，空结果也不重复解析IL
        static List<Contract> PageFields(Type type) {
            List<Contract> cached;if(pageContracts.TryGetValue(type,out cached))return cached;
            List<Contract> found=new List<Contract>();
            bool renderer=false;
            for(Type current=type;current!=null&&!renderer;current=current.BaseType)foreach(FieldInfo field in current.GetFields(Flags|BindingFlags.DeclaredOnly)) {
                if(field.IsStatic)continue;
                Type target=field.FieldType;if(DisplayType(target)||target.IsArray&&DisplayType(target.GetElementType())){renderer=true;break;}
            }
            if(!renderer){pageContracts.Add(type,found);return found;}
            foreach(MethodInfo method in type.GetMethods(Flags)) {
                if(method.ContainsGenericParameters)continue;
                MethodBody body;try{body=method.GetMethodBody();}catch{continue;}if(body==null)continue;
                byte[] code=body.GetILAsByteArray();if(code.Length>100000)continue;
                FieldInfo last=null,target=null;Contract pending=null;
                try {for(int p=0;p<code.Length;) {
                    short key=code[p++];if(key==0xfe)key=(short)(0xfe00|code[p++]);OpCode op;if(!opcodes.TryGetValue(key,out op))break;
                    int start=p,size=0;
                    switch(op.OperandType) {
                    case OperandType.InlineNone:break;
                    case OperandType.ShortInlineBrTarget:case OperandType.ShortInlineI:case OperandType.ShortInlineVar:size=1;break;
                    case OperandType.InlineVar:size=2;break;
                    case OperandType.InlineI8:case OperandType.InlineR:size=8;break;
                    case OperandType.InlineSwitch:size=4+4*BitConverter.ToInt32(code,p);break;
                    default:size=4;break;
                    }
                    if(p+size>code.Length || size<0)break;p+=size;
                    if(op==OpCodes.Ldfld) {
                        FieldInfo f=method.Module.ResolveField(BitConverter.ToInt32(code,start),method.DeclaringType.GetGenericArguments(),method.GetGenericArguments());
                        if(f==null){last=null;target=null;pending=null;continue;}
                        if(DisplayType(f.FieldType)||f.FieldType.IsArray&&DisplayType(f.FieldType.GetElementType()))target=f;
                        last=f.FieldType==typeof(string)?f:null;
                    }
                    if(op==OpCodes.Call || op==OpCodes.Callvirt) {
                        MethodBase m=method.Module.ResolveMethod(BitConverter.ToInt32(code,start),method.DeclaringType.GetGenericArguments(),method.GetGenericArguments());
                        if(m==null||m.DeclaringType==null){last=null;target=null;pending=null;continue;}
                        string own=m.DeclaringType.FullName;if(own==null){last=null;target=null;pending=null;continue;}
                        if(m.Name=="Substring" && own=="System.String" && last!=null&&target!=null)pending=new Contract{Page=last,Target=target};
                        if((own=="UnityEngine.UI.Text"||own.StartsWith("TMPro.")) && (m.Name=="set_text"||m.Name=="SetText")) {if(pending!=null)found.Add(pending);pending=null;target=null;}
                        last=null;
                    }
                    if(op.FlowControl==FlowControl.Branch || op.FlowControl==FlowControl.Cond_Branch){last=null;target=null;pending=null;}
                }}catch{continue;}
            }pageContracts.Add(type,found);return found;
        }
        // Scan 将发现和绑定拆到不同帧，保留上一轮完整来源直至新一轮提交
        static void Scan() {
            int frame=Time.frameCount;if(lastScanFrame==frame)return;lastScanFrame=frame;
            if(opcodes.Count==0)foreach(FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Static|BindingFlags.Public)) {object v=f.GetValue(null);if(v is OpCode)opcodes[((OpCode)v).Value]=(OpCode)v;}
            if(scanPhase<0){scanPhase=0;scanIndex=0;scanObjects=null;scanSources.Clear();}
            Type kind=scanPhase==0?tmp:scanPhase==1?ui:scanPhase==2?mesh:typeof(MonoBehaviour);
            if(scanObjects==null){
                try{scanObjects=kind==null?new UnityEngine.Object[0]:Resources.FindObjectsOfTypeAll(kind);}
                catch(Exception e){scanObjects=null;scanSources.Clear();scanPhase=-1;nextScan=Time.realtimeSinceStartup+2;Diagnose("scan:"+e.GetType().Name);}
                return;
            }
            long started=System.Diagnostics.Stopwatch.GetTimestamp();int processed=0;
            while(scanIndex<scanObjects.Length && processed++<128) {
                Component c=scanObjects[scanIndex++] as Component;
                if(c!=null)try{if(scanPhase==3)ScanSource(c);else ScanLabel(c,kind);}catch(Exception e){Diagnose("scan:"+e.GetType().Name);}
                if(System.Diagnostics.Stopwatch.GetTimestamp()-started>=System.Diagnostics.Stopwatch.Frequency/500)break;
            }
            if(scanIndex<scanObjects.Length)return;
            scanObjects=null;scanIndex=0;scanPhase++;
            if(scanPhase<4)return;
            sources.Clear();sources.AddRange(scanSources);scanSources.Clear();scanPhase=-1;nextScan=Time.realtimeSinceStartup+2;
            List<Component> dead=new List<Component>();foreach(Component c in labels.Keys)if(c==null)dead.Add(c);foreach(Component c in dead)labels.Remove(c);
            foreach(Label label in labels.Values)label.Ready=true;
        }
        // ScanLabel 只登记实际文本字段，输入控件继续排除
        static void ScanLabel(Component c,Type kind) {
            if(labels.ContainsKey(c))return;
            bool input=false;for(Transform p=c.transform;p!=null;p=p.parent)foreach(Component parent in p.GetComponents<Component>())if(parent!=null && parent.GetType().Name.IndexOf("InputField",StringComparison.Ordinal)>=0)input=true;
            if(input)return;
            string fieldName=(kind==ui||kind==mesh)?"m_Text":"m_text";
            FieldInfo field=Field(c.GetType(),fieldName);if(field==null || field.FieldType!=typeof(string))return;
            if(labels.Count>=8192)return;
            labels[c]=new Label{Text=c,TextField=field,Changed=Time.realtimeSinceStartup};
        }
        // ScanSource 使用类型缓存重新绑定当前实例，不复用旧场景对象或目标字段值
        static void ScanSource(Component c) {
            if(c.GetType().Namespace=="Dango.UnityMono")return;
            Type t=c.GetType();if(t.Namespace!=null && (t.Namespace.StartsWith("UnityEngine")||t.Namespace.StartsWith("TMPro")))return;
            List<Contract> fields=PageFields(t);if(fields.Count==0)return;
            foreach(Contract contract in fields) {
                FieldInfo f=contract.Target;if(f.IsStatic||contract.Page.IsStatic||!InheritsType(t,f.DeclaringType)||!InheritsType(t,contract.Page.DeclaringType))continue;
                List<Component> targets=new List<Component>();
                object v;try{v=f.GetValue(c);}catch{continue;}
                Component target=v as Component;if(target!=null&&labels.ContainsKey(target))targets.Add(target);
                Array array=v as Array;if(array!=null&&array.Length<=128)foreach(object item in array){target=item as Component;if(target!=null&&labels.ContainsKey(target))targets.Add(target);}
                if(targets.Count>0)scanSources.Add(new Source{Owner=c,Field=contract.Page,Targets=targets});
            }
        }

        // TmpTextSource 按实际TMP枚举解析字符串输入语义，不调用可能改写剧情状态的派生text属性
        static object TmpTextSource(Type type) {
            object value;if(textSources.TryGetValue(type,out value))return value;
            if(type.IsEnum)foreach(string name in new string[]{"TextString","String","Text","TextInputBox"})
                if(Enum.IsDefined(type,name)){value=Enum.Parse(type,name);break;}
            textSources.Add(type,value);return value;
        }
        // TmpInputState 保存新旧TMP的解析缓冲副本，防止译文污染原文逐字播放或SetText输入
        static Dictionary<FieldInfo,object> TmpInputState(Component c) {
            Dictionary<FieldInfo,object> state=new Dictionary<FieldInfo,object>();
            foreach(string name in new string[]{"m_char_buffer","m_TextProcessingArray","m_InternalTextProcessingArraySize","m_TextBackingArray","m_IsTextBackingStringDirty"}) {
                FieldInfo field=Field(c.GetType(),name);if(field==null)continue;
                object value=field.GetValue(c);Array array=value as Array;
                if(array!=null)value=array.Clone();
                else if(value!=null&&name=="m_TextBackingArray"&&field.FieldType.IsValueType) {
                    FieldInfo buffer=Field(field.FieldType,"m_Array");array=buffer==null?null:buffer.GetValue(value) as Array;
                    if(array!=null)buffer.SetValue(value,array.Clone());
                }
                state.Add(field,value);
            }
            return state;
        }
        // Rebuild a display copy, then restore original fields before the game resumes.
        // No source table, controller timer, branch value or getter text is overwritten.
        static bool Draw(Label l,string translation) {
            Component c=l.Text;object original=l.TextField.GetValue(c);
            object font=Get(c,"font"), material=Get(c,"fontSharedMaterial");
            FieldInfo visible=Field(c.GetType(),"m_maxVisibleCharacters"),fontField=Field(c.GetType(),"m_fontAsset");
            object priorVisible=visible==null?null:visible.GetValue(c);
            FieldInfo source=Field(c.GetType(),"m_inputSource"),parse=Field(c.GetType(),"m_isInputParsingRequired");
            object priorSource=source==null?null:source.GetValue(c);
            bool legacy=ui!=null&&ui.IsInstanceOfType(c)||mesh!=null&&mesh.IsInstanceOfType(c);
            Dictionary<FieldInfo,object> input=legacy?null:TmpInputState(c);
            Dictionary<FieldInfo,object> layout=new Dictionary<FieldInfo,object>();
            if(!legacy)foreach(string name in new string[]{"m_lineSpacing","m_paragraphSpacing","m_lineSpacingMax","m_lineSpacingDelta","m_fontSize","m_charWidthAdjDelta"}) {
                FieldInfo f=Field(c.GetType(),name);if(f!=null)layout.Add(f,f.GetValue(c));
            }
            string stage="font";
            try {
                if(!l.Applied){l.Font=font as UnityEngine.Object;l.Material=material as Material;}l.Applied=true;
                if(!legacy) {
                    if(chinese==null)return false;
                    foreach(char ch in translation)if(!Char.IsWhiteSpace(ch)&&!glyphs.Contains(ch))return false;
                    Set(c,"font",chinese);
                    Material targetMaterial=Get(chinese,"material") as Material;
                    Material activeMaterial=Get(c,"fontSharedMaterial") as Material;
                    if(targetMaterial==null)return false;
                    if(activeMaterial==null||activeMaterial.mainTexture!=targetMaterial.mainTexture||activeMaterial.shader!=targetMaterial.shader) {
                        Set(c,"fontSharedMaterial",targetMaterial);
                    }
                    if(visible!=null)visible.SetValue(c,Int32.MaxValue);
                    ConfigureTmpSpacing(c,font);
                } else {
                    if(uiFont==null)uiFont=Font.CreateDynamicFontFromOSFont(new string[]{"Microsoft YaHei","SimSun"},24);
                    if(uiFont==null)return false;
                    foreach(char ch in translation)if(!Char.IsWhiteSpace(ch)&&!uiFont.HasCharacter(ch))return false;
                    Set(c,"font",uiFont);
                }
                stage="text-input";
                if(source!=null){object textSource=TmpTextSource(source.FieldType);if(textSource==null)throw new NotSupportedException("TMP string input source");source.SetValue(c,textSource);}
                l.TextField.SetValue(c,translation);
                FieldInfo backingDirty=legacy?null:Field(c.GetType(),"m_IsTextBackingStringDirty");if(backingDirty!=null)backingDirty.SetValue(c,false);
                if(parse!=null)parse.SetValue(c,true);
                if(!legacy) {
                    stage="mesh";
                    MethodInfo force=Method(c.GetType(),"ForceMeshUpdate",0);
                    if(force!=null)force.Invoke(c,null);
                    else {force=Method(c.GetType(),"ForceMeshUpdate",2);if(force==null)return false;force.Invoke(c,new object[]{false,true});}
                    // 旧TMP的ForceMeshUpdate只重建网格；在恢复原字段前同步提交配套材质
                    // 保留引擎的UpdateMaterial路径，以继续使用Canvas遮罩和裁剪材质
                    MethodInfo updateMaterial=Method(c.GetType(),"UpdateMaterial",0);
                    if(updateMaterial==null)return false;
                    stage="material";
                    updateMaterial.Invoke(c,null);
                    if(!SubmitTmpAtlas(c))return false;
                } else if(ui!=null&&ui.IsInstanceOfType(c)) {
                    MethodInfo dirty=Method(c.GetType(),"SetVerticesDirty",0);if(dirty!=null)dirty.Invoke(c,null);
                    MethodInfo rebuild=Method(c.GetType(),"Rebuild",1);if(rebuild==null)return false;
                    rebuild.Invoke(c,new object[]{Enum.ToObject(rebuild.GetParameters()[0].ParameterType,3)});
                } else {
                    // TextMesh has no Canvas rebuild contract.  Assigning its
                    // public text/font pair is safe for the current render
                    // tick; if the legacy private refresh method is present,
                    // invoke it before the original value is restored.
                    MethodInfo refresh=Method(c.GetType(),"FontTextureChanged",0);
                    if(refresh!=null)refresh.Invoke(c,null);
                }
                return true;
            } catch(Exception e){if(e is TargetInvocationException&&e.InnerException!=null)e=e.InnerException;Diagnose("display:"+stage+":"+e.GetType().Name);return false;}
            finally {
                l.TextField.SetValue(c,original);
                // Direct restoration avoids dirtying the private mesh after rendering.
                FieldInfo ff=legacy?Field(c.GetType(),"m_FontData"):fontField;
                if(!legacy&&ff!=null)ff.SetValue(c,font);
                FieldInfo shared=Field(c.GetType(),"m_sharedMaterial");if(shared!=null)shared.SetValue(c,material);
                if(legacy&&ff!=null){object fd=ff.GetValue(c);FieldInfo f=Field(fd.GetType(),"m_Font");if(f!=null)f.SetValue(fd,font);}
                if(mesh!=null&&mesh.IsInstanceOfType(c))Set(c,"font",font);
                if(visible!=null)visible.SetValue(c,priorVisible);
                if(source!=null)source.SetValue(c,priorSource);
                if(input!=null)foreach(KeyValuePair<FieldInfo,object> value in input)value.Key.SetValue(c,value.Value);
                foreach(KeyValuePair<FieldInfo,object> value in layout)value.Key.SetValue(c,value.Value);
                if(parse!=null)parse.SetValue(c,true);
            }
        }
        // SubmitTmpAtlas 同步Canvas独立纹理槽，保留TMP提交的遮罩材质；其UpdateMaterial并不更新该槽
        static bool SubmitTmpAtlas(Component c) {
            CanvasRenderer renderer=Get(c,"canvasRenderer") as CanvasRenderer;
            // 世界空间TMP由MeshRenderer绘制，没有Canvas纹理覆盖槽
            if(renderer==null)return true;
            Material expected=Get(c,"fontSharedMaterial") as Material;
            if(expected==null||expected.mainTexture==null)return false;
            Material submitted=renderer.GetMaterial(0);
            if(submitted==null||submitted.shader!=expected.shader) {
                Diagnose("display:canvas-material-mismatch");return false;
            }
            // TextMeshProUGUI.UpdateMaterial() only sets the CanvasRenderer material;
            // Unity's Graphic path also submits the atlas through SetTexture().
            renderer.SetTexture(expected.mainTexture);
            return true;
        }
        // ConfigureTmpSpacing 按字体度量换算TMP的设计行距，译文行高至少容纳完整字面
        static void ConfigureTmpSpacing(Component c,object originalFont) {
            object originalFace=Get(originalFont,"fontInfo"),targetFace=Get(chinese,"fontInfo");
            bool modern=targetFace==null;
            if(modern){originalFace=Get(originalFont,"faceInfo");targetFace=Get(chinese,"faceInfo");}
            if(originalFace==null||targetFace==null)return;
            FieldInfo line=Field(c.GetType(),"m_lineSpacing"),paragraph=Field(c.GetType(),"m_paragraphSpacing");
            if(line==null||paragraph==null)return;
            float originalPoint=Convert.ToSingle(Get(originalFace,modern?"pointSize":"PointSize")),targetPoint=Convert.ToSingle(Get(targetFace,modern?"pointSize":"PointSize"));
            float originalScale=Convert.ToSingle(Get(originalFace,modern?"scale":"Scale")),targetScale=Convert.ToSingle(Get(targetFace,modern?"scale":"Scale"));
            if(originalPoint<=0||targetPoint<=0||originalScale<=0||targetScale<=0)return;
            float ratio=targetPoint/originalPoint*originalScale/targetScale;
            float targetHeight=Convert.ToSingle(Get(targetFace,modern?"lineHeight":"LineHeight"));
            float extent=Convert.ToSingle(Get(targetFace,modern?"ascentLine":"Ascender"))-Convert.ToSingle(Get(targetFace,modern?"descentLine":"Descender"));
            float minimum=Math.Max(targetHeight,extent);
            float designed=(Convert.ToSingle(Get(originalFace,modern?"lineHeight":"LineHeight"))+Convert.ToSingle(line.GetValue(c)))*ratio;
            float adjusted=Math.Max(minimum,designed)-targetHeight;
            float paragraphValue=Math.Max(minimum-targetHeight-adjusted,Convert.ToSingle(paragraph.GetValue(c))*ratio);
            if(Single.IsNaN(adjusted+paragraphValue)||Single.IsInfinity(adjusted+paragraphValue)||minimum<=0)throw new IOException("TMP spacing metrics");
            line.SetValue(c,adjusted);paragraph.SetValue(c,paragraphValue);
            // 不允许自动字号先通过负行距压缩；仍沿用原控件的字号范围与等比缩放
            FieldInfo limit=Field(c.GetType(),"m_lineSpacingMax"),delta=Field(c.GetType(),"m_lineSpacingDelta");
            if(limit!=null)limit.SetValue(c,0f);if(delta!=null)delta.SetValue(c,0f);
        }
        static void Restore(Label l) {
            if(!l.Applied||l.Text==null)return;l.Applied=false;
            try {
                Set(l.Text,"font",l.Font);if(l.Material!=null)Set(l.Text,"fontSharedMaterial",l.Material);
                MethodInfo dirty=Method(l.Text.GetType(),"SetAllDirty",0);if(dirty!=null)dirty.Invoke(l.Text,null);
                MethodInfo force=Method(l.Text.GetType(),"ForceMeshUpdate",0);if(force!=null)force.Invoke(l.Text,null);
                else {force=Method(l.Text.GetType(),"ForceMeshUpdate",2);if(force!=null)force.Invoke(l.Text,new object[]{false,true});}
                // 关闭或局部回退时，原文网格也必须与原字体材质同时恢复
                MethodInfo updateMaterial=Method(l.Text.GetType(),"UpdateMaterial",0);if(updateMaterial!=null)updateMaterial.Invoke(l.Text,null);
                CanvasRenderer renderer=Get(l.Text,"canvasRenderer") as CanvasRenderer;Material originalMaterial=Get(l.Text,"fontSharedMaterial") as Material;
                if(renderer!=null&&originalMaterial!=null&&originalMaterial.mainTexture!=null)renderer.SetTexture(originalMaterial.mainTexture);
                if(tmp!=null&&tmp.IsInstanceOfType(l.Text))SubmitTmpAtlas(l.Text);
            }catch(Exception e){Diagnose("restore:"+e.GetType().Name);}
        }
        // Run after the normal CanvasUpdateRegistry rebuild; an entry-hook draw would
        // otherwise be overwritten by Unity's following willRenderCanvases callbacks.
        static void Render() {
            if(rendering||!loaded&&!liveEnabled)return;rendering=true;
            try {
                bool leaseValid=LeaseValid()&&(loaded||LiveValid());
                foreach(Label l in labels.Values) {
                    if(l.Target!=null && leaseValid && Visible(l.Text) && ContentText(l) && (l.TextField.GetValue(l.Text) as string)==l.Raw) {
                        bool applied=Draw(l,l.Target);
                        if(!applied){Restore(l);Diagnose("display:local-original-fallback");}
                        if(!loaded)LiveRendered(l,applied);
                    } else Restore(l);
                }
            }catch(Exception e){Diagnose("render:"+e.GetType().Name);}
            finally{rendering=false;}
        }
        public static string Frame() {
            if(!configured||invoking)return "";invoking=true;
            try {
                if(!subscribed){Canvas.willRenderCanvases+=Render;subscribed=true;}
                if(leaseMemory==IntPtr.Zero)OpenLease();
                if((loaded||liveEnabled)&&(!LeaseValid()||loaded&&!File.Exists(Path.Combine(cache,"TeamNimbus.active.bin"))))Stop();
                PollLive();
                float now=Time.realtimeSinceStartup;
                if(!attempted&&now>=nextSnapshot){nextSnapshot=now+0.25f;if(LeaseValid())LoadSnapshot();}
                if(scanPhase>=0||now>=nextScan)Scan();
                if(now>=nextTick) {
                    nextTick=now+0.05f;sceneFrames++;
                    foreach(Label l in labels.Values) {
                        bool visible=Visible(l.Text);
                        if(visible!=l.Visible){l.Visible=visible;l.Changed=now;l.Published=null;}
                        if(!visible){l.Published=null;l.Full=null;l.Target=null;l.Page=null;l.Revision++;continue;}
                        string raw=l.TextField.GetValue(l.Text) as string ?? "";
                        if(raw!=l.Raw){l.Raw=raw;l.Changed=now;}
                        l.Full=null;
                    }
                    foreach(Source s in sources)if(Active(s.Owner)) {
                        string full=s.Field.GetValue(s.Owner) as string;if(String.IsNullOrEmpty(full))continue;
                        foreach(Component target in s.Targets){Label l;if(target==null||!labels.TryGetValue(target,out l)||!l.Visible)continue;
                            if(!String.IsNullOrEmpty(l.Raw)&&full.StartsWith(l.Raw,StringComparison.Ordinal)) {if(l.Full!=null&&l.Full!=full)l.Full="";else l.Full=full;}
                        }
                    }
                    foreach(Label l in labels.Values)if(l.Full=="")l.Full=null;
                    List<Label> candidates=new List<Label>();
                    foreach(Label l in labels.Values)if(l.Visible) {
                        string full=l.Full??l.Raw;
                        if(l.Page!=full){l.Page=full;l.Revision++;}
                        l.Target=null;
                        if(!l.Ready)continue;
                        if(!ContentText(l)){l.Published=null;continue;}
                        if(l.Full!=null&&l.Full.Length>0 || now-l.Changed>=0.3f) {
                            // Capture source pages immediately; stable generic labels are a fallback.
                            candidates.Add(l);
                            string tr;
                            if(loaded && !resourceOriginals.ContainsKey(Normalize(full)) && l.Raw==full && translations.TryGetValue(Normalize(full),out tr) && Safe(Normalize(full))&&Safe(tr)) {
                                l.Target=tr;
                            }
                        }
                    }
                    Publish(candidates);
                    UpdateLiveTargets();
                }
                return packets.Count>0?packets.Dequeue():"";
            }catch(Exception e){Diagnose("frame:"+e.GetType().Name);return "";}
            finally{invoking=false;}
        }

        static void LoadFont(BinaryReader r) {
            int size=r.ReadInt32(),count=r.ReadInt32();
            if(size==0&&count==0)return;
            if(size<128||size>4096||(size&(size-1))!=0||count<0||count>20000)throw new IOException();
            Type glyph=Find("TMPro.TMP_Glyph"),asset=Find("TMPro.TMP_FontAsset"),face=Find("TMPro.FaceInfo");
            Array values=glyph==null?null:Array.CreateInstance(glyph,count);
            List<float[]> records=new List<float[]>(count);
            for(int i=0;i<count;i++) {
                int id=r.ReadInt32();float x=r.ReadSingle(),y=r.ReadSingle(),w=r.ReadSingle(),h=r.ReadSingle(),xo=r.ReadSingle(),yo=r.ReadSingle(),advance=r.ReadSingle();
                if(id<32||id>65535||Single.IsNaN(x+y+w+h+xo+yo+advance)||Single.IsInfinity(x+y+w+h+xo+yo+advance)||x<0||y<0||w<0||h<0||x+w>size||y+h>size)throw new IOException();
                glyphs.Add(id);
                records.Add(new float[]{id,x,y,w,h,xo,yo,advance});
                if(values!=null) {
                    object g=Activator.CreateInstance(glyph);Set(g,"id",id);Set(g,"x",x);Set(g,"y",y);Set(g,"width",w);Set(g,"height",h);Set(g,"xOffset",xo);Set(g,"yOffset",yo);Set(g,"xAdvance",advance);Set(g,"scale",1f);values.SetValue(g,i);
                }
            }
            byte[] pixels=r.ReadBytes(checked(size*size));if(pixels.Length!=size*size)throw new EndOfStreamException();
            string stage="schema";
            try {
                if(asset==null)return;
                if(face==null||values==null){LoadModernFont(asset,size,pixels,records);return;}
                stage="shader";
                Shader shader=Shader.Find("TextMeshPro/Bitmap");if(shader==null||!shader.isSupported){Diagnose("font:bitmap-shader-unavailable");return;}
                stage="atlas";
                Texture2D atlas=new Texture2D(size,size,TextureFormat.Alpha8,false);owned.Add(atlas);atlas.LoadRawTextureData(pixels);atlas.Apply();
                Material material=new Material(shader);owned.Add(material);material.mainTexture=atlas;
                stage="asset";
                UnityEngine.Object font=ScriptableObject.CreateInstance(asset);owned.Add(font);font.name="DangoUnityMonoChinese";
                // 旧版TMP将枚举公开为字段，统一按实际成员读取和写入
                object type=Get(font,"fontAssetType");if(type==null||!type.GetType().IsEnum)throw new MissingFieldException("fontAssetType");
                Set(font,"fontAssetType",Enum.Parse(type.GetType(),"Bitmap"));Set(font,"atlas",atlas);Set(font,"material",material);
                object info=Activator.CreateInstance(face);Set(info,"Name","DangoUnityMonoChinese");Set(info,"PointSize",20f);Set(info,"Scale",1f);Set(info,"LineHeight",24f);Set(info,"Ascender",20f);Set(info,"Descender",-4f);Set(info,"AtlasWidth",(float)size);Set(info,"AtlasHeight",(float)size);
                stage="definition";
                Method(asset,"AddFaceInfo",1).Invoke(font,new object[]{info});
                Type kern=Find("TMPro.KerningTable");if(kern!=null)Method(asset,"AddKerningInfo",1).Invoke(font,new object[]{Activator.CreateInstance(kern)});
                Set(font,"fallbackFontAssets",Activator.CreateInstance(typeof(List<>).MakeGenericType(asset)));
                Method(asset,"AddGlyphInfo",1).Invoke(font,new object[]{values});Method(asset,"ReadFontDefinition",0).Invoke(font,null);chinese=font;
            }catch(Exception e){if(e is TargetInvocationException&&e.InnerException!=null)e=e.InnerException;chinese=null;Diagnose("font:"+stage+":"+e.GetType().Name+":"+e.Message);}
        }
        // LoadModernFont 为TextCore版TMP构造静态中文图集，不依赖OS动态字体的原始字面数据
        static void LoadModernFont(Type asset,int size,byte[] pixels,List<float[]> records) {
            Type glyph=Find("UnityEngine.TextCore.Glyph"),metrics=Find("UnityEngine.TextCore.GlyphMetrics"),rect=Find("UnityEngine.TextCore.GlyphRect"),face=Find("UnityEngine.TextCore.FaceInfo"),character=Find("TMPro.TMP_Character");
            if(glyph==null||metrics==null||rect==null||face==null||character==null)throw new MissingMemberException("TextCore font schema glyph="+(glyph!=null)+" metrics="+(metrics!=null)+" rect="+(rect!=null)+" face="+(face!=null)+" character="+(character!=null));
            Shader shader=Shader.Find("TextMeshPro/Mobile/Bitmap");if(shader==null||!shader.isSupported)shader=Shader.Find("TextMeshPro/Bitmap");
            if(shader==null||!shader.isSupported)throw new MissingMemberException("TMP bitmap shader");
            Texture2D atlas=new Texture2D(size,size,TextureFormat.Alpha8,false);owned.Add(atlas);atlas.LoadRawTextureData(pixels);atlas.Apply();
            Material material=new Material(shader);owned.Add(material);material.mainTexture=atlas;
            material.SetFloat("_TextureWidth",size);material.SetFloat("_TextureHeight",size);
            UnityEngine.Object font=ScriptableObject.CreateInstance(asset);owned.Add(font);font.name="DangoUnityMonoChinese";
            // Older TMP releases expose the asset version field, while Unity 6's
            // bundled TMP omits it entirely.  The version is metadata only; do
            // not reject an otherwise valid runtime font schema because this
            // optional field is absent.
            if(Field(asset,"m_Version")!=null)Set(font,"m_Version","1.1.0");
            object info=Activator.CreateInstance(face);Set(info,"familyName","DangoUnityMonoChinese");Set(info,"pointSize",20);Set(info,"scale",1f);Set(info,"lineHeight",24f);Set(info,"ascentLine",20f);Set(info,"descentLine",-4f);Set(info,"capLine",20f);Set(info,"meanLine",10f);Set(font,"faceInfo",info);
            Set(font,"atlasPopulationMode",Enum.Parse(Get(font,"atlasPopulationMode").GetType(),"Static"));
            Set(font,"atlasTextures",new Texture2D[]{atlas});Set(font,"atlasWidth",size);Set(font,"atlasHeight",size);Set(font,"atlasPadding",0);
            Set(font,"atlasRenderMode",Enum.Parse(Get(font,"atlasRenderMode").GetType(),"RASTER"));Set(font,"material",material);
            IList glyphTable=(IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(glyph));
            IList characterTable=(IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(character));
            foreach(float[] row in records) {
                uint index=(uint)row[0];
                object measure=Activator.CreateInstance(metrics,new object[]{row[3],row[4],row[5],row[6],row[7]});
                object area=Activator.CreateInstance(rect,new object[]{(int)row[1],size-(int)row[2]-(int)row[4],(int)row[3],(int)row[4]});
                object entry=Activator.CreateInstance(glyph,new object[]{index,measure,area,1f,0});glyphTable.Add(entry);
                characterTable.Add(Activator.CreateInstance(character,new object[]{index,entry}));
            }
            Set(font,"glyphTable",glyphTable);Set(font,"characterTable",characterTable);
            MethodInfo definition=Method(asset,"ReadFontAssetDefinition",0);if(definition==null)throw new MissingMethodException("ReadFontAssetDefinition");
            definition.Invoke(font,null);
            MethodInfo has=asset.GetMethod("HasCharacter",Flags,null,new Type[]{typeof(int)},null);
            if(has==null)throw new MissingMethodException("HasCharacter");
            foreach(float[] row in records)if(!(bool)has.Invoke(font,new object[]{(int)row[0]}))throw new IOException("TMP glyph lookup");
            chinese=font;
        }
        static void Stop() {
            CloseLive();
            loaded=false;translations.Clear();resourceOriginals.Clear();packets.Clear();
            foreach(Label l in labels.Values){Restore(l);l.Published=null;l.Target=null;}
            foreach(UnityEngine.Object o in owned)if(o!=null)UnityEngine.Object.Destroy(o);owned.Clear();chinese=null;glyphs.Clear();
            if(uiFont!=null){UnityEngine.Object.Destroy(uiFont);uiFont=null;}
            if(leaseMemory!=IntPtr.Zero){UnmapViewOfFile(leaseMemory);leaseMemory=IntPtr.Zero;}
            if(lease!=IntPtr.Zero){CloseHandle(lease);lease=IntPtr.Zero;}
            if(host!=IntPtr.Zero){CloseHandle(host);host=IntPtr.Zero;}attempted=false;
        }

        // ReadString 读取有界UTF16并拒绝非法代理项
        static string ReadString(BinaryReader reader, int max)
        {
            uint length = reader.ReadUInt32();
            if (length > max) throw new IOException();
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
                    if (Encoding.ASCII.GetString(r.ReadBytes(8)) != "TNCA0002") throw new IOException();
                    expected = r.ReadBytes(32); fp = r.ReadBytes(32); td = r.ReadBytes(32); activation = r.ReadBytes(32);
                    name = ReadString(r, 128);
                    if (r.BaseStream.Position != r.BaseStream.Length || Path.GetFileName(name) != name || !name.StartsWith("backfill_data.") || !name.EndsWith(".tnbf")) throw new IOException();
                }
                byte[] authorized = new byte[32];
                Marshal.Copy(new IntPtr(leaseMemory.ToInt64() + 32), authorized, 0, 32);
                if (Hex(expected) != Hex(authorized)) { attempted = false; return; }

                if(liveMemory!=IntPtr.Zero){ReleaseLiveFont();CloseLive();}

                if (Hex(Hash(Path.Combine(cache, "backfill.active.json"))) != Hex(activation)) throw new IOException();
                string resource = Path.Combine(cache, name);
                if (new FileInfo(resource).Length > (64L << 20) || Hex(Hash(resource)) != Hex(expected)) throw new IOException();
                using (BinaryReader r = new BinaryReader(File.OpenRead(resource)))
                {
                    ReadSnapshot(r,fp,td,Path.GetFullPath(Application.dataPath));
                }
                digest = Hex(expected); loaded = true;
            }
            catch (Exception e)
            {
                failure = e.GetType().Name + (String.IsNullOrEmpty(e.Message) ? "" : ":" + e.Message);
                Diagnose("snapshot:" + failure);
                translations.Clear();resourceOriginals.Clear(); loaded = false;
            }
        }

        // ReadSnapshot 共用新旧快照协议并独立校验原始身份和已安装来源
        static IOException SnapshotError(string detail) { return new IOException("snapshot:" + detail); }
        static void ReadSnapshot(BinaryReader r,byte[] fp,byte[] td,string data)
        {
            string format=Encoding.ASCII.GetString(r.ReadBytes(8));
            bool staticResources=format=="DCTN0002";
            if ((!staticResources&&format!="DCTN0001") || Hex(r.ReadBytes(32)) != Hex(fp) || Hex(r.ReadBytes(32)) != Hex(td)) throw SnapshotError("header");
            uint count = r.ReadUInt32();
            if (count == 0 || count > 4096) throw SnapshotError("source-count");
            StringBuilder inventory = new StringBuilder();
            HashSet<string> paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (uint i = 0; i < count; i++)
            {
                string rel = ReadString(r, 1024), sha = Hex(r.ReadBytes(32));
                string installed=staticResources?Hex(r.ReadBytes(32)):sha;
                string path = Path.GetFullPath(Path.Combine(data, rel));
                if (Path.IsPathRooted(rel) || !path.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !paths.Add(path) || Hex(Hash(path)) != installed) throw SnapshotError("source:" + rel);
                inventory.Append(rel.Replace('\\', '/')).Append(':').Append(sha).Append('\n');
            }
            using (SHA256 hash = SHA256.Create()) if (Hex(hash.ComputeHash(Encoding.UTF8.GetBytes(inventory.ToString()))) != Hex(fp)) throw SnapshotError("inventory");
            uint pairs = r.ReadUInt32(); if (pairs > 1000000) throw SnapshotError("pair-count");
            for (uint i = 0; i < pairs; i++)
            {
                string original = ReadString(r, 16000), translated = ReadString(r, 16000);
                if (translations.ContainsKey(original)) throw SnapshotError("duplicate-pair");
                if (Safe(original) && Safe(translated)) translations.Add(original, translated);
            }
            LoadFont(r);
            if(staticResources) {
                uint nativePairs=r.ReadUInt32();if(nativePairs>1000000)throw SnapshotError("native-pair-count");
                for(uint i=0;i<nativePairs;i++) {
                    string original=ReadString(r,16000),translated=ReadString(r,16000), prior;
                    if(!translations.ContainsKey(original)||translations[original]!=translated)throw SnapshotError("native-pair:" + i);
                    if(resourceOriginals.TryGetValue(translated,out prior)&&prior!=original)resourceOriginals[translated]="";
                    else resourceOriginals[translated]=original;
                }
                uint descriptorSize=r.ReadUInt32();if(descriptorSize>8<<20)throw SnapshotError("descriptor-size");
                byte[] descriptor=r.ReadBytes((int)descriptorSize);
                if(descriptor.Length!=descriptorSize||r.ReadUInt32()!=descriptorSize||Encoding.ASCII.GetString(r.ReadBytes(8))!="DMRS0001")throw SnapshotError("descriptor");
            }
            if (r.BaseStream.Position != r.BaseStream.Length) throw SnapshotError("trailing-data");
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


    }
}

