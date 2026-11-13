// UnityEngineReference 仅供编译 Dango.UnityMono.Reader 的引用桩，不随组件发布、不进入游戏进程。
// 它复刻旧版完整 UnityEngine 0.0.0.0 中 Reader 实际用到的类型与成员签名，使 Reader 只引用
// 单一 UnityEngine 程序集：旧 Mono 直接解析，Unity 2017.2+ 经 UnityEngine.dll 类型转发解析。
// 新增 Reader 对 UnityEngine 的直接引用时须同步补充此处签名，并核对编译产物的 AssemblyRef/MemberRef。
using System;
[assembly: System.Reflection.AssemblyVersion("0.0.0.0")]
namespace UnityEngine
{
    // Object 提供比较、命名和销毁签名
    public class Object {
        public string name { get { return null; } set { } }
        public static bool operator ==(Object x, Object y) { return false; }
        public static bool operator !=(Object x, Object y) { return false; }
        public override bool Equals(object o) { return false; }
        public override int GetHashCode() { return 0; }
        public static void Destroy(Object o) { }
    }
    // Component 提供节点与组件查询签名
    public class Component : Object {
        public GameObject gameObject { get { return null; } }
        public Transform transform { get { return null; } }
        public Component[] GetComponents(Type type) { return null; }
        public T[] GetComponents<T>() { return null; }
    }
    public sealed class GameObject : Object { public bool activeInHierarchy { get { return false; } } }
    public class Behaviour : Component { public bool enabled { get { return false; } set { } } }
    public class MonoBehaviour : Behaviour { }
    public class Transform : Component { public Transform parent { get { return null; } } }
    public sealed class Time {
        public static int frameCount { get { return 0; } }
        public static float realtimeSinceStartup { get { return 0; } }
    }
    public sealed class Resources { public static Object[] FindObjectsOfTypeAll(Type type) { return null; } }
    public sealed class Application { public static string dataPath { get { return null; } } }
    public class Shader : Object {
        public static Shader Find(string name) { return null; }
        public bool isSupported { get { return false; } }
    }
    public class Texture : Object { }
    // TextureFormat 数值必须与 Unity 一致，常量会被编译进 Reader
    public enum TextureFormat { Alpha8 = 1, ARGB4444 = 2, RGB24 = 3, RGBA32 = 4, ARGB32 = 5 }
    public sealed class Texture2D : Texture {
        public Texture2D(int width, int height, TextureFormat format, bool mipmap) { }
        public void LoadRawTextureData(byte[] data) { }
        public void Apply() { }
    }
    public class Material : Object {
        public Material(Shader shader) { }
        public Texture mainTexture { get { return null; } set { } }
        public Shader shader { get { return null; } }
        public void SetFloat(string name, float value) { }
    }
    public sealed class Font : Object {
        public static Font CreateDynamicFontFromOSFont(string[] names, int size) { return null; }
        public bool HasCharacter(char c) { return false; }
    }
    public sealed class CanvasRenderer : Component {
        public Material GetMaterial(int index) { return null; }
        public void SetTexture(Texture texture) { }
    }
    // Canvas 的事件字段名与旧版一致，帧中继按此字段名插入调用链
    public sealed class Canvas : Behaviour {
        public delegate void WillRenderCanvases();
        public static event WillRenderCanvases willRenderCanvases;
        static void Touch() { if (willRenderCanvases != null) willRenderCanvases(); }
    }
    public class ScriptableObject : Object { public static ScriptableObject CreateInstance(Type type) { return null; } }
}
