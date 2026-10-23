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
using UnityEngine;

namespace Dango.UnityMono
{
    // The bridge touches Unity objects only from the verified canvas frame callback.
    public static class Reader
    {
        const BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;
        static Type tmp, ui, mesh, canvasGroup;
        static string cache, digest, failure;
        static bool configured, loaded, attempted, invoking, subscribed, rendering;
        static float nextScan, nextTick;
        static long sequence;
        static int sceneFrames;
        static IntPtr lease, leaseMemory, host;
        static long hostCreated;
        static readonly Dictionary<string,string> translations = new Dictionary<string,string>(StringComparer.Ordinal);
        static readonly Dictionary<Component,Label> labels = new Dictionary<Component,Label>();
        static readonly List<Source> sources = new List<Source>();
        static readonly Queue<string> packets = new Queue<string>();
        static readonly Queue<string> diagnostics = new Queue<string>();
        static readonly HashSet<string> diagnosed = new HashSet<string>();
        static readonly List<UnityEngine.Object> owned = new List<UnityEngine.Object>();
        static UnityEngine.Object chinese;
        static Font uiFont;
        static bool dynamicFont;
        static readonly HashSet<int> glyphs = new HashSet<int>();
        static readonly Regex styles = new Regex(@"</?(?:b|i|u|s|color|size|alpha)(?:[ =][^<>]*)?>",RegexOptions.IgnoreCase);
        static readonly Dictionary<short,OpCode> opcodes = new Dictionary<short,OpCode>();
        public static string Status { get { return loaded ? "ready " + digest : attempted ? "failed" : ""; } }
        public static string Diagnostic { get { return diagnostics.Count == 0 ? "" : diagnostics.Dequeue(); } }

        sealed class Label {
            internal Component Text;
            internal string Raw, Published, Full;
            internal float Changed;
            internal bool Applied;
            internal bool Visible;
            internal string Target;
            internal FieldInfo TextField;
            internal UnityEngine.Object Font;
            internal Material Material;
        }
        sealed class Source { internal Component Owner; internal FieldInfo Field; internal List<Component> Targets; }
        sealed class Contract { internal FieldInfo Page,Target; }

        static void Diagnose(string value) { if(diagnosed.Count<64 && diagnosed.Add(value)) diagnostics.Enqueue(value); }
        static Type Find(string name) {
            Type result=null;
            foreach(Assembly a in AppDomain.CurrentDomain.GetAssemblies()) {
                Type t=a.GetType(name,false); if(t==null) continue;
                if(result!=null && result!=t) return null; result=t;
            }
            return result;
        }
        static FieldInfo Field(Type t,string name) {
            for(;t!=null;t=t.BaseType) { FieldInfo f=t.GetField(name,Flags|BindingFlags.DeclaredOnly);if(f!=null)return f; }return null;
        }
        static object Get(object obj,string name) {
            if(obj==null)return null; Type t=obj.GetType();PropertyInfo p=t.GetProperty(name,Flags);
            return p!=null?p.GetValue(obj,null):Field(t,name)!=null?Field(t,name).GetValue(obj):null;
        }
        static void Set(object obj,string name,object value) {
            PropertyInfo p=obj.GetType().GetProperty(name,Flags);
            if(p!=null && p.CanWrite) p.SetValue(obj,ConvertValue(value,p.PropertyType),null);
            else { FieldInfo f=Field(obj.GetType(),name);if(f==null)throw new MissingFieldException(name);f.SetValue(obj,ConvertValue(value,f.FieldType)); }
        }
        static object ConvertValue(object value,Type type) { return value!=null&&!type.IsInstanceOfType(value)&&value is IConvertible ? Convert.ChangeType(value,type) : value; }
        static MethodInfo Method(Type t,string name,int count) {
            MethodInfo result=null;
            foreach(MethodInfo m in t.GetMethods(Flags)) if(m.Name==name && m.GetParameters().Length==count) {if(result!=null)return null;result=m;}
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
                foreach(Component group in p.GetComponents<Component>())if(group!=null&&canvasGroup.IsInstanceOfType(group)) {
                    Behaviour b=group as Behaviour;if(b!=null&&!b.enabled)continue;
                    object alpha=Get(group,"alpha");if(alpha!=null)opacity*=Convert.ToSingle(alpha);
                    object skip=Get(group,"ignoreParentGroups");if(skip is bool && (bool)skip)ignore=true;
                }
                if(opacity<=0.001f)return false;if(ignore)break;
            }
            return true;
        }
        static bool Safe(string s) {
            if(String.IsNullOrEmpty(s)||s.Length>16000 || s.IndexOfAny(new char[]{'\0','{','}','<','>'})>=0)return false;
            for(int i=0;i<s.Length;i++) if(Char.IsSurrogate(s[i])||Char.IsControl(s[i]) && s[i]!='\n' && s[i]!='\t')return false;
            return true;
        }
        static bool Readable(string s) {
            if(String.IsNullOrEmpty(s)||s.Length>16000)return false;
            foreach(char c in s)if(Char.IsLetter(c))return true;return false;
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
                if(unique.Contains(s)){accepted.Add(l);continue;}
                string tr;bool hit=translations.TryGetValue(s,out tr);
                // Account for both text and exact segments, including JSON escapes.
                int cost=2*Escape(s).Length+Escape(hit?tr:s).Length+16;
                if(cost>23000){Diagnose("capture:packet-too-large");continue;}
                if(budget+cost>23000||texts.Count>=256)continue;
                budget+=cost;unique.Add(s);texts.Add(s);translated.Add(hit?tr:s);anyTranslated|=hit;accepted.Add(l);
            }
            if(texts.Count==0)return;
            string[] escaped=new string[texts.Count];for(int i=0;i<texts.Count;i++)escaped[i]="\""+Escape(texts[i])+"\"";
            string packet="{\"sequence\":"+(++sequence)+",\"owner\":"+((uint)accepted[0].Text.GetInstanceID())+",\"kind\":\"ui\",\"text\":\""+Escape(String.Join("\n",texts.ToArray()))+"\",\"segments\":["+String.Join(",",escaped)+"],\"speaker\":\"\",\"translation\":\""+Escape(anyTranslated?String.Join("\n",translated.ToArray()):"")+"\",\"backfill\":"+(loaded?"true":"false")+"}";
            if(packet.Length>=24500){Diagnose("capture:packet-too-large");return;}
            foreach(Label l in accepted)l.Published=Normalize(l.Full??l.Raw);
            packets.Enqueue(packet);
        }

        // Resolve complete typewriter sources by actual IL Substring -> standard text consumer.
        static bool DisplayType(Type t) {return tmp!=null&&tmp.IsAssignableFrom(t)||ui!=null&&ui.IsAssignableFrom(t)||mesh!=null&&mesh.IsAssignableFrom(t);}
        static List<Contract> PageFields(Type type) {
            List<Contract> found=new List<Contract>();
            foreach(MethodInfo method in type.GetMethods(Flags)) {
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
                        FieldInfo f=method.Module.ResolveField(BitConverter.ToInt32(code,start));
                        if(DisplayType(f.FieldType)||f.FieldType.IsArray&&DisplayType(f.FieldType.GetElementType()))target=f;
                        last=f.FieldType==typeof(string)?f:null;
                    }
                    if(op==OpCodes.Call || op==OpCodes.Callvirt) {
                        MethodBase m=method.Module.ResolveMethod(BitConverter.ToInt32(code,start));string own=m.DeclaringType.FullName;
                        if(m.Name=="Substring" && own=="System.String" && last!=null&&target!=null)pending=new Contract{Page=last,Target=target};
                        if((own=="UnityEngine.UI.Text"||own.StartsWith("TMPro.")) && (m.Name=="set_text"||m.Name=="SetText")) {if(pending!=null)found.Add(pending);pending=null;target=null;}
                        last=null;
                    }
                    if(op.FlowControl==FlowControl.Branch || op.FlowControl==FlowControl.Cond_Branch){last=null;target=null;pending=null;}
                }}catch{continue;}
            }return found;
        }
        static void Scan() {
            if(opcodes.Count==0)foreach(FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Static|BindingFlags.Public)) {object v=f.GetValue(null);if(v is OpCode)opcodes[((OpCode)v).Value]=(OpCode)v;}
            foreach(Type t in new Type[]{tmp,ui,mesh})if(t!=null)foreach(UnityEngine.Object o in Resources.FindObjectsOfTypeAll(t)) {
                Component c=o as Component;if(c==null || labels.ContainsKey(c))continue;
                // Input controls own editable state and never receive display translation.
                bool input=false;for(Transform p=c.transform;p!=null;p=p.parent)foreach(Component parent in p.GetComponents<Component>())if(parent!=null && parent.GetType().Name.IndexOf("InputField",StringComparison.Ordinal)>=0)input=true;
                if(input)continue;
                string fieldName=(t==ui||t==mesh)?"m_Text":"m_text";
                FieldInfo field=Field(c.GetType(),fieldName);if(field==null || field.FieldType!=typeof(string))continue;
                if(labels.Count>=8192)break;
                labels[c]=new Label{Text=c,TextField=field,Changed=Time.realtimeSinceStartup};
            }
            sources.Clear();Dictionary<Type,List<Contract>> types=new Dictionary<Type,List<Contract>>();
            foreach(UnityEngine.Object o in Resources.FindObjectsOfTypeAll(typeof(MonoBehaviour))) {
                Component c=o as Component;if(c==null || c.GetType().Namespace=="Dango.UnityMono")continue;
                Type t=c.GetType();if(t.Namespace!=null && (t.Namespace.StartsWith("UnityEngine")||t.Namespace.StartsWith("TMPro")))continue;
                List<Contract> fields;if(!types.TryGetValue(t,out fields)){fields=PageFields(t);types[t]=fields;}
                if(fields.Count==0)continue;
                foreach(Contract contract in fields) {
                    FieldInfo f=contract.Target;if(f.IsStatic||!f.DeclaringType.IsAssignableFrom(t)||!contract.Page.DeclaringType.IsAssignableFrom(t))continue;
                    List<Component> targets=new List<Component>();
                    object v;try{v=f.GetValue(c);}catch{continue;}
                    Component target=v as Component;if(target!=null&&labels.ContainsKey(target))targets.Add(target);
                    Array array=v as Array;if(array!=null&&array.Length<=128)foreach(object item in array){target=item as Component;if(target!=null&&labels.ContainsKey(target))targets.Add(target);}
                    sources.Add(new Source{Owner=c,Field=contract.Page,Targets=targets});
                }
            }
            List<Component> dead=new List<Component>();foreach(Component c in labels.Keys)if(c==null)dead.Add(c);foreach(Component c in dead)labels.Remove(c);
            Diagnose("read:standard-renderers-and-source-fields");
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
            FieldInfo chars=Field(c.GetType(),"m_char_buffer");
            Array priorChars=chars==null?null:chars.GetValue(c) as Array;
            if(priorChars!=null)priorChars=(Array)priorChars.Clone();
            bool legacy=ui!=null&&ui.IsInstanceOfType(c)||mesh!=null&&mesh.IsInstanceOfType(c);
            try {
                if(!legacy) {
                    if(chinese==null)return false;
                    foreach(char ch in translation)if(!Char.IsWhiteSpace(ch)&&(!glyphs.Contains(ch)||dynamicFont&&!HasGlyph(ch)))return false;
                    Set(c,"font",chinese);if(visible!=null)visible.SetValue(c,Int32.MaxValue);
                } else {
                    if(uiFont==null)uiFont=Font.CreateDynamicFontFromOSFont(new string[]{"Microsoft YaHei","SimSun"},24);
                    if(uiFont==null)return false;
                    foreach(char ch in translation)if(!Char.IsWhiteSpace(ch)&&!uiFont.HasCharacter(ch))return false;
                    Set(c,"font",uiFont);
                }
                l.TextField.SetValue(c,translation);
                if(source!=null)source.SetValue(c,Enum.Parse(source.FieldType,"Text"));
                if(parse!=null)parse.SetValue(c,true);
                if(!legacy) {
                    MethodInfo force=Method(c.GetType(),"ForceMeshUpdate",0);
                    if(force!=null)force.Invoke(c,null);
                    else {force=Method(c.GetType(),"ForceMeshUpdate",2);if(force==null)return false;force.Invoke(c,new object[]{false,true});}
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
                if(!l.Applied){l.Font=font as UnityEngine.Object;l.Material=material as Material;}l.Applied=true;return true;
            } catch(Exception e){Diagnose("display:"+e.GetType().Name);return false;}
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
                if(chars!=null&&priorChars!=null)chars.SetValue(c,priorChars);
                if(parse!=null)parse.SetValue(c,true);
            }
        }
        static void Restore(Label l) {
            if(!l.Applied||l.Text==null)return;l.Applied=false;
            try {
                Set(l.Text,"font",l.Font);if(l.Material!=null)Set(l.Text,"fontSharedMaterial",l.Material);
                MethodInfo dirty=Method(l.Text.GetType(),"SetAllDirty",0);if(dirty!=null)dirty.Invoke(l.Text,null);
                MethodInfo force=Method(l.Text.GetType(),"ForceMeshUpdate",0);if(force!=null)force.Invoke(l.Text,null);
            }catch(Exception e){Diagnose("restore:"+e.GetType().Name);}
        }
        // Run after the normal CanvasUpdateRegistry rebuild; an entry-hook draw would
        // otherwise be overwritten by Unity's following willRenderCanvases callbacks.
        static void Render() {
            if(rendering)return;rendering=true;
            try {
                foreach(Label l in labels.Values) {
                    if(loaded && LeaseValid() && Visible(l.Text) && l.Target!=null && (l.TextField.GetValue(l.Text) as string)==l.Raw) {
                        if(!Draw(l,l.Target)){Restore(l);Diagnose("display:local-original-fallback");}
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
                if(loaded&&(!LeaseValid()||!File.Exists(Path.Combine(cache,"TeamNimbus.active.bin"))))Stop();
                if(!attempted&&LeaseValid())LoadSnapshot();
                float now=Time.realtimeSinceStartup;
                if(now>=nextScan){nextScan=now+2;Scan();}
                if(now>=nextTick) {
                    nextTick=now+0.05f;sceneFrames++;
                    foreach(Label l in labels.Values) {
                        bool visible=Visible(l.Text);
                        if(visible!=l.Visible){l.Visible=visible;l.Changed=now;l.Published=null;}
                        if(!visible){l.Published=null;l.Full=null;l.Target=null;continue;}
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
                        l.Target=null;
                        if(l.Full!=null&&l.Full.Length>0 || now-l.Changed>=0.3f) {
                            // Capture source pages immediately; stable generic labels are a fallback.
                            candidates.Add(l);
                            string tr;
                            if(loaded && l.Raw==full && translations.TryGetValue(Normalize(full),out tr) && Safe(Normalize(full))&&Safe(tr)) {
                                l.Target=tr;
                            }
                        }
                    }
                    Publish(candidates);
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
            for(int i=0;i<count;i++) {
                int id=r.ReadInt32();float x=r.ReadSingle(),y=r.ReadSingle(),w=r.ReadSingle(),h=r.ReadSingle(),xo=r.ReadSingle(),yo=r.ReadSingle(),advance=r.ReadSingle();
                if(id<32||id>65535||Single.IsNaN(x+y+w+h+xo+yo+advance)||Single.IsInfinity(x+y+w+h+xo+yo+advance)||x<0||y<0||w<0||h<0||x+w>size||y+h>size)throw new IOException();
                glyphs.Add(id);
                if(values!=null) {
                    object g=Activator.CreateInstance(glyph);Set(g,"id",id);Set(g,"x",x);Set(g,"y",y);Set(g,"width",w);Set(g,"height",h);Set(g,"xOffset",xo);Set(g,"yOffset",yo);Set(g,"xAdvance",advance);Set(g,"scale",1f);values.SetValue(g,i);
                }
            }
            byte[] pixels=r.ReadBytes(checked(size*size));if(pixels.Length!=size*size)throw new EndOfStreamException();
            try {
                if(asset==null)return;
                if(face==null||values==null){LoadDynamicFont(asset);return;}
                Shader shader=Shader.Find("TextMeshPro/Bitmap");if(shader==null||!shader.isSupported){Diagnose("font:bitmap-shader-unavailable");return;}
                Texture2D atlas=new Texture2D(size,size,TextureFormat.Alpha8,false);owned.Add(atlas);atlas.LoadRawTextureData(pixels);atlas.Apply();
                Material material=new Material(shader);owned.Add(material);material.mainTexture=atlas;
                UnityEngine.Object font=ScriptableObject.CreateInstance(asset);owned.Add(font);font.name="DangoUnityMonoChinese";
                PropertyInfo type=asset.GetProperty("fontAssetType",Flags);Set(font,"fontAssetType",Enum.Parse(type.PropertyType,"Bitmap"));Set(font,"atlas",atlas);Set(font,"material",material);
                object info=Activator.CreateInstance(face);Set(info,"Name","DangoUnityMonoChinese");Set(info,"PointSize",20f);Set(info,"Scale",1f);Set(info,"LineHeight",24f);Set(info,"Ascender",20f);Set(info,"Descender",-4f);Set(info,"AtlasWidth",(float)size);Set(info,"AtlasHeight",(float)size);
                Method(asset,"AddFaceInfo",1).Invoke(font,new object[]{info});
                Type kern=Find("TMPro.KerningTable");if(kern!=null)Method(asset,"AddKerningInfo",1).Invoke(font,new object[]{Activator.CreateInstance(kern)});
                Set(font,"fallbackFontAssets",Activator.CreateInstance(typeof(List<>).MakeGenericType(asset)));
                Method(asset,"AddGlyphInfo",1).Invoke(font,new object[]{values});Method(asset,"ReadFontDefinition",0).Invoke(font,null);chinese=font;Diagnose("font:legacy-bitmap-ready");
            }catch(Exception e){chinese=null;Diagnose("font:"+e.GetType().Name);}
        }
        static bool HasGlyph(char c) {
            if(chinese==null)return false;
            MethodInfo m=chinese.GetType().GetMethod("HasCharacter",Flags,null,new Type[]{typeof(char),typeof(bool),typeof(bool)},null);
            if(m!=null)return (bool)m.Invoke(chinese,new object[]{c,true,true});
            m=chinese.GetType().GetMethod("HasCharacter",Flags,null,new Type[]{typeof(int)},null);
            return m!=null&&(bool)m.Invoke(chinese,new object[]{(int)c});
        }
        static void LoadDynamicFont(Type asset) {
            Font sourceFont=Font.CreateDynamicFontFromOSFont(new string[]{"Microsoft YaHei","SimSun"},32);if(sourceFont==null)return;owned.Add(sourceFont);
            foreach(MethodInfo m in asset.GetMethods(BindingFlags.Static|BindingFlags.Public)) {
                ParameterInfo[] ps=m.GetParameters();if(m.Name!="CreateFontAsset"||ps.Length==0||ps[0].ParameterType!=typeof(Font))continue;
                object[] args=new object[ps.Length];args[0]=sourceFont;bool valid=true;
                for(int i=1;i<ps.Length;i++){if(!ps[i].IsOptional){valid=false;break;}args[i]=ps[i].DefaultValue;}
                if(!valid)continue;
                chinese=m.Invoke(null,args) as UnityEngine.Object;if(chinese==null)continue;owned.Add(chinese);dynamicFont=true;Diagnose("font:dynamic-tmp-ready");return;
            }
            Diagnose("font:unsupported-tmp-schema");
        }
        static void Stop() {
            loaded=false;translations.Clear();packets.Clear();
            foreach(Label l in labels.Values){Restore(l);l.Published=null;l.Target=null;}
            foreach(UnityEngine.Object o in owned)if(o!=null)UnityEngine.Object.Destroy(o);owned.Clear();chinese=null;dynamicFont=false;glyphs.Clear();
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

                if (Hex(Hash(Path.Combine(cache, "backfill.active.json"))) != Hex(activation)) throw new IOException();
                string resource = Path.Combine(cache, name);
                if (new FileInfo(resource).Length > (64L << 20) || Hex(Hash(resource)) != Hex(expected)) throw new IOException();
                using (BinaryReader r = new BinaryReader(File.OpenRead(resource)))
                {
                    if (Encoding.ASCII.GetString(r.ReadBytes(8)) != "DCTN0001" || Hex(r.ReadBytes(32)) != Hex(fp) || Hex(r.ReadBytes(32)) != Hex(td)) throw new IOException();
                    string data = Path.GetFullPath(Application.dataPath);
                    uint count = r.ReadUInt32();
                    if (count == 0 || count > 4096) throw new IOException();
                    StringBuilder inventory = new StringBuilder();
                    HashSet<string> paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    for (uint i = 0; i < count; i++)
                    {
                        string rel = ReadString(r, 1024), sha = Hex(r.ReadBytes(32));
                        string path = Path.GetFullPath(Path.Combine(data, rel));
                        if (Path.IsPathRooted(rel) || !path.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !paths.Add(path) || Hex(Hash(path)) != sha) throw new IOException();
                        inventory.Append(rel.Replace('\\', '/')).Append(':').Append(sha).Append('\n');
                    }
                    using (SHA256 hash = SHA256.Create()) if (Hex(hash.ComputeHash(Encoding.UTF8.GetBytes(inventory.ToString()))) != Hex(fp)) throw new IOException();
                    uint pairs = r.ReadUInt32(); if (pairs > 1000000) throw new IOException();
                    for (uint i = 0; i < pairs; i++)
                    {
                        string original = ReadString(r, 16000), translated = ReadString(r, 16000);
                        if (translations.ContainsKey(original)) throw new IOException();
                        if (Safe(original) && Safe(translated)) translations.Add(original, translated);
                    }
                    LoadFont(r);
                    if (r.BaseStream.Position != r.BaseStream.Length) throw new IOException();
                }
                digest = Hex(expected); loaded = true;
            }
            catch (Exception e) { failure = e.GetType().Name; translations.Clear(); loaded = false; }
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

