using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using UnityEngine;

namespace Dango.UnityMono
{
    public static partial class Reader
    {
        static IntPtr liveMapping,liveMemory,liveClient;
        static long liveClientBirth,liveEpoch,liveSequence,liveCommand;
        static bool liveEnabled,liveAcknowledged;
        static float nextLive;
        static string liveOriginal,liveTranslation;
        static readonly List<LiveLabel> liveLabels=new List<LiveLabel>();

        sealed class LiveLabel {internal Label Label;internal long Revision;internal bool Rendered;}

        // ClearLiveTargets 撤销本页全部显示目标，隐藏或局部失败后不能重用旧网格
        static void ClearLiveTargets() {
            foreach(LiveLabel item in liveLabels){item.Label.Target=null;Restore(item.Label);item.Rendered=false;}
        }

        // LiveValid 同时核对实时开关代次、客户端创建时间和当前Helper租约
        static bool LiveValid() {
            if(!liveEnabled||liveMemory==IntPtr.Zero||liveClient==IntPtr.Zero)return false;
            long created,exit,kernel,user;
            return Marshal.ReadInt32(liveMemory,44)==1&&Marshal.ReadInt64(liveMemory,48)==liveEpoch&&
                WaitForSingleObject(liveClient,0)==258&&GetProcessTimes(liveClient,out created,out exit,out kernel,out user)&&created==liveClientBirth;
        }

        // ReleaseLiveFont 先撤销显示再销毁本轮字形，避免旧网格引用已释放图集
        static void ReleaseLiveFont() {
            foreach(Label label in labels.Values){Restore(label);label.Target=null;}
            foreach(UnityEngine.Object item in owned)if(item!=null)UnityEngine.Object.Destroy(item);
            owned.Clear();chinese=null;glyphs.Clear();
            if(uiFont!=null){UnityEngine.Object.Destroy(uiFont);uiFont=null;}
        }

        // CloseLive 清除易失实时通道，不改写全量资源或游戏原始字段
        static void CloseLive() {
            liveEnabled=false;liveEpoch=0;liveSequence=0;liveCommand=0;liveLabels.Clear();liveTranslation=null;liveOriginal=null;
            if(liveMemory!=IntPtr.Zero){UnmapViewOfFile(liveMemory);liveMemory=IntPtr.Zero;}
            if(liveMapping!=IntPtr.Zero){CloseHandle(liveMapping);liveMapping=IntPtr.Zero;}
            if(liveClient!=IntPtr.Zero){CloseHandle(liveClient);liveClient=IntPtr.Zero;}
        }

        // LiveAck 仅在主线程完成绘制或确认局部回退后提交当前页结果
        static void LiveAck(int status) {
            if(liveMemory==IntPtr.Zero||liveSequence==0||liveAcknowledged)return;
            int stamp=Marshal.ReadInt32(liveMemory,512);
            Marshal.WriteInt32(liveMemory,512,stamp+1);Thread.MemoryBarrier();
            Marshal.WriteInt64(liveMemory,520,liveEpoch);Marshal.WriteInt64(liveMemory,528,liveSequence);Marshal.WriteInt32(liveMemory,536,status);
            Thread.MemoryBarrier();Marshal.WriteInt32(liveMemory,512,stamp+2);liveAcknowledged=true;
        }

        // OpenLive 由核验后的Helper租约派生映射名，不读取磁盘上的旧注入授权
        static void OpenLive() {
            if(leaseMemory==IntPtr.Zero||!LeaseValid())return;
            uint helper=(uint)Marshal.ReadInt32(leaseMemory,4);
            liveMapping=OpenFileMapping(6,false,"Local\\DangoUnityMonoLive_"+GetCurrentProcessId()+"_"+helper);
            if(liveMapping==IntPtr.Zero)return;
            liveMemory=MapViewOfFile(liveMapping,6,0,0,new UIntPtr(4096));
            if(liveMemory==IntPtr.Zero){CloseLive();return;}
            byte[] magic=new byte[8];Marshal.Copy(liveMemory,magic,0,8);
            if(Encoding.ASCII.GetString(magic)!="DMLV0001"||(uint)Marshal.ReadInt32(liveMemory,12)!=helper||
                (uint)Marshal.ReadInt32(liveMemory,16)!=GetCurrentProcessId()||Marshal.ReadInt64(liveMemory,32)!=hostCreated){CloseLive();return;}
            liveClientBirth=Marshal.ReadInt64(liveMemory,24);
            liveClient=OpenProcess(0x101000,false,(uint)Marshal.ReadInt32(liveMemory,8));
            long created,exit,kernel,user;
            if(liveClient==IntPtr.Zero||WaitForSingleObject(liveClient,0)!=258||!GetProcessTimes(liveClient,out created,out exit,out kernel,out user)||created!=liveClientBirth)CloseLive();
        }

        // PollLive 每四分之一秒检查控制槽，仅在新回复出现时读取有界不可变载荷
        static void PollLive() {
            if(loaded){if(liveMemory!=IntPtr.Zero)CloseLive();return;}
            float now=Time.realtimeSinceStartup;if(now<nextLive)return;nextLive=now+0.25f;
            if(liveMemory==IntPtr.Zero)OpenLive();if(liveMemory==IntPtr.Zero)return;
            if(!LeaseValid()){ReleaseLiveFont();CloseLive();return;}
            int stamp=Marshal.ReadInt32(liveMemory,40);if((stamp&1)!=0)return;
            byte[] command=new byte[316];Marshal.Copy(new IntPtr(liveMemory.ToInt64()+44),command,0,command.Length);Thread.MemoryBarrier();
            if(Marshal.ReadInt32(liveMemory,40)!=stamp)return;
            bool enabled=BitConverter.ToInt32(command,0)==1;
            long epoch=BitConverter.ToInt64(command,4),page=BitConverter.ToInt64(command,12);
            if(epoch!=liveEpoch||enabled!=liveEnabled) {
                ReleaseLiveFont();liveLabels.Clear();liveOriginal=null;liveTranslation=null;liveSequence=0;liveCommand=0;liveAcknowledged=false;
                liveEpoch=epoch;liveEnabled=enabled;packets.Clear();
                foreach(Label label in labels.Values)label.Published=null;
            }
            if(!enabled)return;
            if(!LiveValid()){ReleaseLiveFont();CloseLive();return;}
            if(page==0||page==liveCommand)return;liveCommand=page;
            if(page!=liveSequence)return;
            if(!LivePageValid()){LiveAck(2);return;}
            try {
                string name=Encoding.Unicode.GetString(command,60,256).TrimEnd('\0');
                if(Path.GetFileName(name)!=name||!name.StartsWith("mono.live.")||!name.EndsWith(".bin")||name.IndexOfAny(new char[]{':','\0','/','\\'})>=0)throw new IOException();
                string path=Path.Combine(cache,name);
                FileInfo info=new FileInfo(path);if(info.Length>20L<<20||info.Length<36||(info.Attributes&FileAttributes.ReparsePoint)!=0)throw new IOException();
                byte[] expected=new byte[32];Array.Copy(command,28,expected,0,32);
                byte[] payload=File.ReadAllBytes(path);
                using(var hash=System.Security.Cryptography.SHA256.Create())if(Hex(hash.ComputeHash(payload))!=Hex(expected))throw new IOException();
                using(BinaryReader reader=new BinaryReader(new MemoryStream(payload))) {
                    if(Encoding.ASCII.GetString(reader.ReadBytes(8))!="DMLP0001"||reader.ReadInt64()!=liveEpoch||reader.ReadInt64()!=liveSequence)throw new IOException();
                    string original=ReadString(reader,16000),translation=ReadString(reader,4096);
                    if(original!=liveOriginal||!Safe(original)||!Safe(translation))throw new IOException();
                    int size=reader.ReadInt32();if(size<8||size>20<<20||size!=reader.BaseStream.Length-reader.BaseStream.Position)throw new IOException();
                    ReleaseLiveFont();LoadFont(reader);
                    foreach(LiveLabel item in liveLabels)item.Rendered=false;
                    if(reader.BaseStream.Position!=reader.BaseStream.Length)throw new IOException();
                    liveTranslation=translation;
                }
            }catch(Exception error){ReleaseLiveFont();liveTranslation=null;LiveAck(2);Diagnose("live:reply:"+error.GetType().Name);}
        }

        // BindLivePage 只绑定单个明确原文及其真实显示对象，多段歧义继续普通读取
        static long BindLivePage(long page,List<Label> accepted,List<string> texts) {
            if(loaded||!LiveValid())return 0;
            LiveAck(2);ClearLiveTargets();liveLabels.Clear();liveTranslation=null;liveOriginal=null;liveSequence=0;liveAcknowledged=false;
            if(texts.Count!=1||!Safe(texts[0]))return 0;
            foreach(Label label in accepted) {
                if(Normalize(label.Full??label.Raw)!=texts[0])return 0;
                liveLabels.Add(new LiveLabel{Label=label,Revision=label.Revision});
            }
            liveOriginal=texts[0];liveSequence=page;return liveEpoch;
        }

        // LivePageValid 按对象引用、逻辑页代次、可见性及正文语义淘汰旧回复
        static bool LivePageValid() {
            if(!LiveValid()||liveLabels.Count==0)return false;
            foreach(LiveLabel item in liveLabels) {
                Label label=item.Label;
                if(label.Text==null||!labels.ContainsKey(label.Text)||label.Revision!=item.Revision||!Visible(label.Text)||!ContentText(label)||Normalize(label.Full??label.Raw)!=liveOriginal)return false;
            }
            return true;
        }

        // UpdateLiveTargets 原文吐字完整后才显示译文，不修改打印器计时或VM状态
        static void UpdateLiveTargets() {
            if(loaded||!liveEnabled||liveSequence==0)return;
            if(!LivePageValid()){LiveAck(2);ClearLiveTargets();liveTranslation=null;liveLabels.Clear();return;}
            if(liveTranslation==null)return;
            foreach(LiveLabel item in liveLabels)if(item.Label.Raw!=(item.Label.Full??item.Label.Raw))return;
            foreach(LiveLabel item in liveLabels)item.Label.Target=liveTranslation;
        }

        // LiveRendered 使用实际绘制返回确认，不将收到译文当成游戏内注入成功
        static void LiveRendered(Label label,bool applied) {
            if(liveTranslation==null)return;
            bool all=true;
            foreach(LiveLabel item in liveLabels){if(item.Label==label){if(!applied){LiveAck(2);ClearLiveTargets();liveTranslation=null;return;}item.Rendered=true;}if(!item.Rendered)all=false;}
            if(all)LiveAck(1);
        }
    }
}
