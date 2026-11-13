using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;

namespace Dango.UnityMono {
    public static partial class Reader {
        sealed class Instruction { internal OpCode Op; internal object Value; }
        static readonly Dictionary<Assembly,HashSet<Type>> timelineChoices=new Dictionary<Assembly,HashSet<Type>>();
        static readonly HashSet<Assembly> timelineAssemblies=new HashSet<Assembly>();

        // Instructions decodes actual metadata operands; tokens and type names
        // are not fixed across builds. Unknown/ambiguous IL produces no contract.
        static List<Instruction> Instructions(MethodBase method) {
            List<Instruction> result=new List<Instruction>();
            if(method==null||method.ContainsGenericParameters)return result;
            try {
                MethodBody body=method.GetMethodBody();if(body==null)return result;
                byte[] bytes=body.GetILAsByteArray();if(bytes.Length>100000)return result;
                for(int p=0;p<bytes.Length;) {
                    short key=bytes[p++];if(key==0xfe)key=(short)(0xfe00|bytes[p++]);OpCode op;
                    if(!opcodes.TryGetValue(key,out op))return new List<Instruction>();
                    int start=p,size=0;
                    switch(op.OperandType) {
                    case OperandType.InlineNone:break;
                    case OperandType.ShortInlineBrTarget:case OperandType.ShortInlineI:case OperandType.ShortInlineVar:size=1;break;
                    case OperandType.InlineVar:size=2;break;
                    case OperandType.InlineI8:case OperandType.InlineR:size=8;break;
                    case OperandType.InlineSwitch:size=4+4*BitConverter.ToInt32(bytes,p);break;
                    default:size=4;break;
                    }
                    if(size<0||p+size>bytes.Length)return new List<Instruction>();p+=size;
                    object value=null;
                    if(op.OperandType==OperandType.InlineField)value=method.Module.ResolveField(BitConverter.ToInt32(bytes,start),method.DeclaringType.GetGenericArguments(),method.GetGenericArguments());
                    else if(op.OperandType==OperandType.InlineMethod)value=method.Module.ResolveMethod(BitConverter.ToInt32(bytes,start),method.DeclaringType.GetGenericArguments(),method.GetGenericArguments());
                    else if(op.OperandType==OperandType.InlineString)value=method.Module.ResolveString(BitConverter.ToInt32(bytes,start));
                    else if(size==1)value=(int)bytes[start];
                    result.Add(new Instruction{Op=op,Value=value});
                }
            }catch{return new List<Instruction>();}
            return result;
        }

        static bool Call(Instruction instruction,string owner,string name) {
            MethodBase method=instruction.Value as MethodBase;
            return (instruction.Op==OpCodes.Call||instruction.Op==OpCodes.Callvirt)&&method!=null&&method.DeclaringType!=null&&method.DeclaringType.FullName==owner&&method.Name==name;
        }
        static bool TextSetter(Instruction instruction) {
            MethodInfo method=instruction.Value as MethodInfo;
            return instruction.Op==OpCodes.Callvirt&&method!=null&&method.DeclaringType!=null&&DisplayType(method.DeclaringType)&&method.Name=="set_text"&&method.GetParameters().Length==1&&method.GetParameters()[0].ParameterType==typeof(string);
        }
        static bool Argument(Instruction i,int n) {
            return n==0?i.Op==OpCodes.Ldarg_0:n==1?i.Op==OpCodes.Ldarg_1:n==2?i.Op==OpCodes.Ldarg_2:n==3?i.Op==OpCodes.Ldarg_3:(i.Op==OpCodes.Ldarg_S&&i.Value is int&&(int)i.Value==n);
        }
        static bool CollectionCall(Instruction instruction,string name) {
            MethodBase method=instruction.Value as MethodBase;
            return instruction.Op==OpCodes.Callvirt&&method!=null&&method.DeclaringType==typeof(List<string>)&&method.Name==name;
        }

        // TimelineChoiceTypes follows a real command dispatcher into its display
        // consumers. Shutdown-only dispatchers and unrelated menu labels do not
        // establish a choice contract. It never invokes game methods/callbacks.
        static HashSet<Type> TimelineChoiceTypes(Assembly assembly) {
            HashSet<Type> found;if(timelineChoices.TryGetValue(assembly,out found))return found;
            found=new HashSet<Type>();timelineChoices[assembly]=found;
            string assemblyName=assembly.GetName().Name;
            if(assemblyName=="mscorlib"||assemblyName=="netstandard"||assemblyName.StartsWith("System")||assemblyName.StartsWith("Unity")||assemblyName.StartsWith("Mono."))return found;
            List<MethodInfo> dispatchers=new List<MethodInfo>();
            try {
                foreach(Type type in assembly.GetTypes()) {
                    if(type.ContainsGenericParameters)continue;
                    foreach(MethodInfo method in type.GetMethods(Flags|BindingFlags.DeclaredOnly)) {
                        bool print=false,choice=false,pair=false;
                        foreach(Instruction instruction in Instructions(method)) {
                            string literal=instruction.Value as string;print|=literal=="PrintText";choice|=literal=="SetSELECT";
                            MethodBase called=instruction.Value as MethodBase;
                            if(called!=null&&called.DeclaringType==type) {
                                ParameterInfo[] args=called.GetParameters();
                                if(args.Length==2&&args[0].ParameterType==typeof(string)&&args[1].ParameterType==typeof(string))pair=true;
                            }
                        }
                        if(print&&choice&&pair)dispatchers.Add(method);
                    }
                }
                if(dispatchers.Count!=1)return found;
                timelineAssemblies.Add(assembly);
                HashSet<MethodBase> visited=new HashSet<MethodBase>();List<MethodBase> pending=new List<MethodBase>{dispatchers[0]};
                for(int depth=0;depth<7&&pending.Count>0;depth++) {
                    List<MethodBase> next=new List<MethodBase>();
                    foreach(MethodBase method in pending) {
                        if(!visited.Add(method)||visited.Count>512)continue;
                        List<Instruction> code=Instructions(method);bool objectName=false,text=false;
                        foreach(Instruction i in code) {
                            objectName|=Call(i,"UnityEngine.Object","set_name");text|=TextSetter(i);
                            MethodBase called=i.Value as MethodBase;
                            if(called!=null&&called.DeclaringType!=null&&called.DeclaringType.Assembly==assembly&&!called.IsConstructor)next.Add(called);
                        }
                        if(objectName&&text&&InheritsType(method.DeclaringType,typeof(UnityEngine.MonoBehaviour)))found.Add(method.DeclaringType);
                    }
                    pending=next;
                }
            }catch{found.Clear();timelineAssemblies.Remove(assembly);}
            return found;
        }

        // TimelineContracts accepts a precise List<string>[Count-1] -> optional
        // escaped-newline replacement -> TMP setter, not arbitrary List getters.
        static void TimelineContracts(Type type,List<Contract> found) {
            bool choice=TimelineChoiceTypes(type.Assembly).Contains(type);
            if(!timelineAssemblies.Contains(type.Assembly))return;
            foreach(MethodInfo method in type.GetMethods(Flags)) {
                List<Instruction> code=Instructions(method);
                for(int i=0;i<code.Count;i++) {
                    if(!TextSetter(code[i]))continue;
                    int end=i;
                    if(end>=3&&Call(code[end-1],"System.String","Replace")&&(code[end-3].Value as string)=="\\n"&&(code[end-2].Value as string)=="\n")end-=3;
                    int begin=end-10;
                    if(begin>=0&&Argument(code[begin],0)&&code[begin+1].Op==OpCodes.Ldfld&&Argument(code[begin+2],0)&&code[begin+3].Op==OpCodes.Ldfld&&Argument(code[begin+4],0)&&code[begin+5].Op==OpCodes.Ldfld&&CollectionCall(code[begin+6],"get_Count")&&code[begin+7].Op==OpCodes.Ldc_I4_1&&code[begin+8].Op==OpCodes.Sub&&CollectionCall(code[begin+9],"get_Item")) {
                        FieldInfo target=code[begin+1].Value as FieldInfo,page=code[begin+3].Value as FieldInfo,count=code[begin+5].Value as FieldInfo;
                        if(target!=null&&DisplayType(target.FieldType)&&page!=null&&page==count&&page.FieldType==typeof(List<string>)&&!page.IsStatic&&!target.IsStatic)found.Add(new Contract{Page=page,Target=target,Latest=true});
                    }
                    if(choice&&i>=3&&Argument(code[i-3],0)&&code[i-2].Op==OpCodes.Ldfld) {
                        ParameterInfo[] args=method.GetParameters();FieldInfo target=code[i-2].Value as FieldInfo;
                        for(int arg=1;arg<=args.Length;arg++)if(args[arg-1].ParameterType==typeof(string)&&Argument(code[i-1],arg)&&target!=null&&DisplayType(target.FieldType)&&!target.IsStatic)found.Add(new Contract{Target=target,Choice=true});
                    }
                }
            }
        }
    }
}
