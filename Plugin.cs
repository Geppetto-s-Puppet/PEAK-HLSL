// Plugin.cs
using BepInEx;
using BepInEx.Logging;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;


namespace PEAK_SSGI
{
    class CapturePass : ScriptableRenderPass
    {
        public static Action<IntPtr, IntPtr, IntPtr, int, int, int, int>? OnCapture;

        static readonly int s_idColor = Shader.PropertyToID("_PEAK_Color");
        static readonly int s_idDepth = Shader.PropertyToID("_PEAK_Depth");
        static readonly int s_idNormal = Shader.PropertyToID("_PEAK_Normal");

        class PassData
        {
            public TextureHandle colorHandle;
            public TextureHandle depthHandle;
            public TextureHandle normalHandle;
            public int texW, texH, winW, winH;
        }

        public CapturePass()
        {
            renderPassEvent = RenderPassEvent.AfterRenderingPostProcessing;
        }

        public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameContext)
        {
            var resourceData = frameContext.Get<UniversalResourceData>();
            var cameraData = frameContext.Get<UniversalCameraData>();
            var cam = cameraData.camera;

            if (cam.targetTexture != null) return;
            if (cam.cameraType != CameraType.Game) return;

            using var builder = renderGraph.AddUnsafePass<PassData>("PEAK_SSGI_Capture", out var passData);

            passData.colorHandle = resourceData.activeColorTexture;
            passData.depthHandle = resourceData.cameraDepthTexture;
            passData.normalHandle = resourceData.cameraNormalsTexture;
            passData.texW = cameraData.cameraTargetDescriptor.width;
            passData.texH = cameraData.cameraTargetDescriptor.height;
            passData.winW = cam.pixelWidth;
            passData.winH = cam.pixelHeight;

            builder.UseTexture(passData.colorHandle, AccessFlags.Read);
            if (passData.depthHandle.IsValid())
                builder.UseTexture(passData.depthHandle, AccessFlags.Read);
            if (passData.normalHandle.IsValid())
                builder.UseTexture(passData.normalHandle, AccessFlags.Read);

            builder.AllowPassCulling(false);

            builder.SetRenderFunc(static (PassData data, UnsafeGraphContext ctx) =>
            {
                try
                {
                    ctx.cmd.SetGlobalTexture(s_idColor, data.colorHandle);
                    if (data.depthHandle.IsValid())
                        ctx.cmd.SetGlobalTexture(s_idDepth, data.depthHandle);
                    if (data.normalHandle.IsValid())
                        ctx.cmd.SetGlobalTexture(s_idNormal, data.normalHandle);

                    s_lastTexW = data.texW;
                    s_lastTexH = data.texH;
                    s_lastWinW = data.winW;
                    s_lastWinH = data.winH;
                    Interlocked.Exchange(ref s_passExecutedInt, 1);
                }
                catch (Exception e)
                {
                    Plugin.Log.LogError($"[CapturePass] RenderFunc exception: {e}");
                }
            });
        }

        internal static int s_passExecutedInt = 0;
        internal static volatile int s_lastTexW = 0;
        internal static volatile int s_lastTexH = 0;
        internal static volatile int s_lastWinW = 0;
        internal static volatile int s_lastWinH = 0;

        // WaitForEndOfFrame コルーチンから呼ぶこと
        public static void ReadAndSend()
        {
            if (Interlocked.Exchange(ref s_passExecutedInt, 0) == 0) return;

            var colorTex = Shader.GetGlobalTexture(s_idColor);
            var depthTex = Shader.GetGlobalTexture(s_idDepth);
            var normalTex = Shader.GetGlobalTexture(s_idNormal);

            IntPtr colorPtr = IntPtr.Zero;
            IntPtr depthPtr = IntPtr.Zero;
            IntPtr normalPtr = IntPtr.Zero;

            try
            {
                if (colorTex != null) colorPtr = colorTex.GetNativeTexturePtr();
                if (depthTex != null) depthPtr = depthTex.GetNativeTexturePtr();
                if (normalTex != null) normalPtr = normalTex.GetNativeTexturePtr();
            }
            catch (Exception e)
            {
                Plugin.Log.LogWarning($"[ReadAndSend] GetNativeTexturePtr() exception: {e}");
            }

            int winW = s_lastWinW > 0 ? s_lastWinW : Screen.width;
            int winH = s_lastWinH > 0 ? s_lastWinH : Screen.height;
            int texW = s_lastTexW > 0 ? s_lastTexW : winW;
            int texH = s_lastTexH > 0 ? s_lastTexH : winH;

            if (Time.frameCount % 60 == 0)
            {
                Plugin.Log.LogInfo(
                    $"[ReadAndSend] color={colorPtr}({texW}x{texH})" +
                    $"  depth={depthPtr}  normal={normalPtr}" +
                    $"  Screen={winW}x{winH}");
            }

            OnCapture?.Invoke(colorPtr, depthPtr, normalPtr, texW, texH, winW, winH);
        }
    }

    class CaptureFeature : ScriptableRendererFeature
    {
        CapturePass? _pass;

        public override void Create()
        {
            Plugin.Log.LogInfo("[CaptureFeature] Create()");
            _pass = new CapturePass();
        }

        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
        {
            var cam = renderingData.cameraData.camera;
            if (cam.targetTexture != null) return;
            if (cam.cameraType != CameraType.Game) return;
            if (_pass == null) return;

            _pass.ConfigureInput(
                ScriptableRenderPassInput.Color |
                ScriptableRenderPassInput.Depth |
                ScriptableRenderPassInput.Normal);

            renderer.EnqueuePass(_pass);
        }
    }

    [BepInPlugin("com.alex.peakhlsl.plugin", "PEAK HLSL", "1.0.0")]
    public partial class Plugin : BaseUnityPlugin
    {
        internal static ManualLogSource Log { get; private set; } = null!;

        // ──── ネイティブエクスポート ────
        [DllImport("com.alex.peakhlsl.native.dll")] static extern void NativeOverlay_Start();
        [DllImport("com.alex.peakhlsl.native.dll")] static extern void NativeOverlay_Show();
        [DllImport("com.alex.peakhlsl.native.dll")] static extern void NativeOverlay_Hide();
        [DllImport("com.alex.peakhlsl.native.dll")] static extern int NativeOverlay_AcquireUnityDevice(IntPtr sampleResourcePtr);
        [DllImport("com.alex.peakhlsl.native.dll")] static extern int NativeOverlay_GetStatus();

        /// <summary>
        /// シェーダーモードを変更する (0=非表示, 1=パススルー, 2=深度, 3=法線)
        /// </summary>
        [DllImport("com.alex.peakhlsl.native.dll")] static extern void NativeOverlay_SetMode(int mode);

        /// <summary>
        /// カラー/深度/法線テクスチャを更新する。
        /// unityD3D12Device / unityFence は AcquireUnityDevice で取得済みのため不要。
        /// </summary>
        [DllImport("com.alex.peakhlsl.native.dll")]
        static extern void NativeOverlay_UpdateTextures(
            IntPtr colorPtr, IntPtr depthPtr, IntPtr normalPtr,
            int screenW, int screenH);

        // ──── 定数 ────
        const int VK_SLASH = 0xBF;
        [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vKey);

        static readonly string[] ModeNames = { "OFF", "Passthrough", "Depth", "Normals" };

        // ──── 状態 ────
        int _shaderMode = 0;   // 0=非表示, 1..3=シェーダー
        bool _prevDown = false;
        bool _featureAdded = false;
        IntPtr _lastColorPtr = IntPtr.Zero;
        int _lastStatus = -1;

        RenderTexture? _deviceProbeRT = null;

        // depthTextureMode をカメラ毎に一度だけ設定する
        HashSet<Camera> _depthSetCameras = new();

        // ──── Awake ────
        void Awake()
        {
            Log = Logger;

            LogRuntimeEnvironment();

            NativeOverlay_Start();
            StartCoroutine(AcquireDeviceAfterStart());

            CapturePass.OnCapture = (col, dep, nor, texW, texH, winW, winH) =>
            {
                if (col != _lastColorPtr)
                {
                    _lastColorPtr = col;
                    Log.LogInfo($"[Capture] color={col}  depth={dep}  normal={nor}" +
                                $"  tex={texW}x{texH}  win={winW}x{winH}");
                }
                // unityD3D12Device / unityFence は AcquireUnityDevice で解決済み → 省略
                NativeOverlay_UpdateTextures(col, dep, nor, winW, winH);
            };

            StartCoroutine(EndOfFrameCapture());

            Log.LogInfo($"Plugin PEAK HLSL is loaded!");
        }

        void OnDestroy()
        {
            CapturePass.OnCapture = null;
            if (_deviceProbeRT != null) { _deviceProbeRT.Release(); _deviceProbeRT = null; }
        }

        // ──── LateUpdate ────
        void LateUpdate()
        {
            // カメラに深度モードを設定（初回のみ）
            foreach (var cam in Camera.allCameras)
            {
                if (cam.cameraType == CameraType.Game && !_depthSetCameras.Contains(cam))
                {
                    cam.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.DepthNormals;
                    _depthSetCameras.Add(cam);
                    Log.LogInfo($"[LateUpdate] Set depthTextureMode for camera {cam.name}");
                }
            }

            // フレーム5でフィーチャー注入
            if (!_featureAdded && Time.frameCount == 5)
            {
                _featureAdded = true;
                PatchURPAsset();
                TryAddFeature();
                LogRenderInfo();
            }

            // ステータスログ（60フレームに1回）
            if (Time.frameCount % 60 == 0)
            {
                int s = NativeOverlay_GetStatus();
                if (s != _lastStatus) { _lastStatus = s; LogStatus("Status", s); }
            }

            // "/" キーでシェーダーモードをサイクル
            bool down = (GetAsyncKeyState(VK_SLASH) & 0x8000) != 0;
            if (down && !_prevDown)
            {
                _shaderMode = (_shaderMode + 1) % ModeNames.Length;

                NativeOverlay_SetMode(_shaderMode);

                if (_shaderMode == 0) NativeOverlay_Hide();
                else NativeOverlay_Show();

                Log.LogInfo($"[Shader] mode={_shaderMode} ({ModeNames[_shaderMode]})");
                LogStatus("NativeStatus", NativeOverlay_GetStatus());
            }
            _prevDown = down;
        }

        // ──── コルーチン ────
        IEnumerator EndOfFrameCapture()
        {
            while (true)
            {
                yield return new WaitForEndOfFrame();
                CapturePass.ReadAndSend();
            }
        }

        IEnumerator AcquireDeviceAfterStart()
        {
            yield return null;
            yield return new WaitForSeconds(0.05f);

            if (_deviceProbeRT == null || !_deviceProbeRT.IsCreated())
            {
                _deviceProbeRT = new RenderTexture(4, 4, 0, RenderTextureFormat.ARGB32);
                _deviceProbeRT.enableRandomWrite = true;
                _deviceProbeRT.Create();
                yield return null;
                yield return new WaitForEndOfFrame();
            }

            IntPtr probePtr = _deviceProbeRT.GetNativeTexturePtr();
            Log.LogInfo($"[Device] probeRT={probePtr}, calling AcquireUnityDevice...");

            const int maxRetries = 3;
            int attempt = 0;
            int result = -9999;
            for (; attempt < maxRetries; attempt++)
            {
                try { result = NativeOverlay_AcquireUnityDevice(probePtr); }
                catch (Exception e) { Log.LogError($"[Device] AcquireUnityDevice threw: {e}"); result = -9999; }

                Log.LogInfo($"[Device] AcquireUnityDevice attempt={attempt} returned {result}");
                if (result == 0) break;

                yield return new WaitForSeconds(0.1f);
            }

            if (result != 0)
            {
                Log.LogError($"[Device] AcquireUnityDevice failed after {attempt} attempts (last={result})");
                yield break;
            }

            yield return null;
            int status = NativeOverlay_GetStatus();
            LogStatus("AfterAcquire", status);

            if ((status & 4) == 0)
                Log.LogInfo("[Device] Unity device acquired successfully!");
            else
                Log.LogWarning("[Device] Still using own device after acquire!");
        }

        // ──── ログユーティリティ ────
        void LogRuntimeEnvironment()
        {
            try
            {
                string urpAssemblyVersion = "unknown";
                try
                {
                    var asm = typeof(UniversalRenderPipelineAsset).Assembly;
                    urpAssemblyVersion = asm.GetName().Version?.ToString() ?? asm.GetName().FullName;
                }
                catch { }

                var gdev = SystemInfo.graphicsDeviceType;
                Log.LogInfo($"[Env] UnityVersion={Application.unityVersion}  GraphicsAPI={gdev}  URP={urpAssemblyVersion}");

                if (gdev == GraphicsDeviceType.Direct3D12)
                    Log.LogWarning("[Env] D3D12: Native texture pointer = ID3D12Resource*");
            }
            catch (Exception e)
            {
                Log.LogWarning($"[Env] {e}");
            }
        }

        static void LogStatus(string tag, int s)
        {
            bool initDone = (s & 1) != 0;
            bool initOK = (s & 2) != 0;
            bool usingOwn = (s & 4) != 0;
            bool srvReady = (s & 8) != 0;
            int mode = (s >> 4) & 0xF;
            int devErr = (s >> 8) & 0xFF;
            Log.LogInfo($"[{tag}] initDone={initDone} initOK={initOK}" +
                        $" usingOwn={usingOwn} srvReady={srvReady} mode={mode} devErr=0x{devErr:X2}");
        }

        void LogRenderInfo()
        {
            try
            {
                var pl = GraphicsSettings.currentRenderPipeline as UniversalRenderPipelineAsset;
                if (pl != null)
                    Log.LogInfo($"[URP] renderScale={pl.renderScale}" +
                                $"  Screen={Screen.width}x{Screen.height}" +
                                $"  API={SystemInfo.graphicsDeviceType}");
            }
            catch (Exception e) { Log.LogError($"[URP] {e.Message}"); }
        }

        // ──── URP アセットパッチ ────
        void PatchURPAsset()
        {
            try
            {
                var pl = GraphicsSettings.currentRenderPipeline;
                if (pl == null) return;
                var bf = BindingFlags.NonPublic | BindingFlags.Instance;

                foreach (string name in new[] { "m_RequireDepthTexture", "m_RequireOpaqueTexture" })
                {
                    var fi = pl.GetType().GetField(name, bf);
                    if (fi?.FieldType == typeof(bool) && !(bool)fi.GetValue(pl))
                    { fi.SetValue(pl, true); Log.LogInfo($"[URP] {name}: false→true"); }
                    else if (fi == null)
                        Log.LogWarning($"[URP] {name} not found");
                }

                foreach (var f in pl.GetType().GetFields(bf))
                {
                    string n = f.Name.ToLower();
                    if (f.FieldType == typeof(bool) &&
                        (n.Contains("opaque") || n.Contains("normal") || n.Contains("depth")))
                    {
                        if (!(bool)f.GetValue(pl))
                        { f.SetValue(pl, true); Log.LogInfo($"[URP] {f.Name}: false→true"); }
                    }
                }
            }
            catch (Exception e) { Log.LogError($"[URP] {e}"); }
        }

        // ──── フィーチャー注入 ────
        void TryAddFeature()
        {
            try
            {
                var pl = GraphicsSettings.currentRenderPipeline as UniversalRenderPipelineAsset;
                if (pl == null) { Log.LogWarning("[Feature] URP asset null"); return; }

                var renderer = pl.scriptableRenderer;
                if (renderer == null) { Log.LogWarning("[Feature] scriptableRenderer null"); return; }

                var bf = BindingFlags.NonPublic | BindingFlags.Instance;
                FieldInfo? fiFeatures = null;
                for (var t = renderer.GetType(); t != null; t = t.BaseType)
                {
                    fiFeatures ??= t.GetField("m_RendererFeatures", bf);
                    if (fiFeatures != null) break;
                }

                if (fiFeatures == null)
                {
                    Log.LogWarning("[Feature] m_RendererFeatures not found; falling back");
                    TryAddFeatureViaData();
                    return;
                }

                var features = fiFeatures.GetValue(renderer) as List<ScriptableRendererFeature>;
                if (features == null) { Log.LogWarning("[Feature] list null"); return; }

                foreach (var f in features)
                    if (f is CaptureFeature) { Log.LogInfo("[Feature] already exists"); return; }

                var feat = ScriptableObject.CreateInstance<CaptureFeature>();
                feat.name = "PEAK_SSGI_Capture";
                DontDestroyOnLoad(feat);
                feat.Create();
                features.Add(feat);
                Log.LogInfo($"[Feature] Injected into {renderer.GetType().Name}");
            }
            catch (Exception e) { Log.LogError($"[Feature] {e}"); }
        }

        void TryAddFeatureViaData()
        {
            try
            {
                var pipeline = GraphicsSettings.currentRenderPipeline;
                if (pipeline == null) return;
                var bf = BindingFlags.NonPublic | BindingFlags.Instance;
                var dataList = pipeline.GetType().GetField("m_RendererDataList", bf)
                    ?.GetValue(pipeline) as UnityEngine.Object[];
                if (dataList == null) return;

                for (int i = 0; i < dataList.Length; i++)
                {
                    var rd = dataList[i] as ScriptableRendererData;
                    if (rd == null) continue;
                    if (rd.name.ToLower().Contains("rendertexture")) continue;

                    bool exists = false;
                    foreach (var f in rd.rendererFeatures)
                        if (f is CaptureFeature) { exists = true; break; }
                    if (exists) continue;

                    FieldInfo? fiF = null, fiM = null;
                    for (var t = rd.GetType(); t != null; t = t.BaseType)
                    {
                        fiF ??= t.GetField("m_RendererFeatures", bf);
                        fiM ??= t.GetField("m_RendererFeatureMap", bf);
                    }
                    if (fiF == null) continue;

                    var feat = ScriptableObject.CreateInstance<CaptureFeature>();
                    feat.name = "PEAK_SSGI_Capture";
                    DontDestroyOnLoad(feat);
                    feat.Create();
                    (fiF.GetValue(rd) as List<ScriptableRendererFeature>)?.Add(feat);
                    (fiM?.GetValue(rd) as List<long>)?.Add(0L);
                    typeof(ScriptableRendererData)
                        .GetMethod("OnValidate", BindingFlags.NonPublic | BindingFlags.Instance)
                        ?.Invoke(rd, null);
                    Log.LogInfo($"[Feature] Added to RendererData [{i}] {rd.name}");
                }
            }
            catch (Exception e) { Log.LogError($"[Feature] (via data) {e}"); }
        }
    }
}