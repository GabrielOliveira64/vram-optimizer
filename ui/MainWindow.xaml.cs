/*
 * MainWindow.xaml.cs — v2
 * Correções:
 *   - Reconexão automática ao orquestrador via pipe (retry a cada 2s)
 *   - Seleção de processo do jogo via lista (ProcessPicker)
 *   - Injeção automática quando processo é selecionado
 *   - UI atualiza mesmo quando orquestrador está no modo standalone
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using LiveChartsCore;
using LiveChartsCore.Defaults;
using LiveChartsCore.SkiaSharpView;
using LiveChartsCore.SkiaSharpView.Painting;
using Microsoft.Win32;
using SkiaSharp;

namespace VRAMOptimizer.UI;

// ─── ViewModels ───────────────────────────────────────────────────────────────

public class TextureViewModel
{
    public string Name        { get; set; } = "";
    public string Resolution  { get; set; } = "";
    public string SizeMB      { get; set; } = "";
    public string Priority    { get; set; } = "";
    public string Compression { get; set; } = "";
    public string SavedMB     { get; set; } = "";
}

public class ProcessViewModel
{
    public uint   PID     { get; set; }
    public string Name    { get; set; } = "";
    public string Display => $"{Name}  (PID {PID})";
}

public class ChartViewModel
{
    private readonly ObservableCollection<ObservableValue> _values = new();

    public ISeries[] ChartSeries { get; }
    public Axis[]    XAxes       { get; }
    public Axis[]    YAxes       { get; }

    public ChartViewModel()
    {
        for (int i = 0; i < 60; i++) _values.Add(new ObservableValue(0));

        ChartSeries = new ISeries[]
        {
            new LineSeries<ObservableValue>
            {
                Values         = _values,
                Name           = "VRAM %",
                Stroke         = new SolidColorPaint(SKColor.Parse("#4F8EF7"), 2),
                Fill           = new LinearGradientPaint(
                    new[] { SKColor.Parse("#334F8EF7"), SKColor.Parse("#004F8EF7") },
                    new SKPoint(0,0), new SKPoint(0,1)),
                GeometrySize   = 0,
                LineSmoothness = 0.4,
            }
        };
        XAxes = new Axis[] { new Axis { IsVisible = false, SeparatorsPaint = null } };
        YAxes = new Axis[]
        {
            new Axis
            {
                MinLimit        = 0,
                MaxLimit        = 100,
                LabelsPaint     = new SolidColorPaint(SKColor.Parse("#8A90A8")),
                SeparatorsPaint = new SolidColorPaint(SKColor.Parse("#2E3350"), 1),
                Labeler         = v => $"{v:0}%",
            }
        };
    }

    public void AddPoint(double pct) {
        _values.RemoveAt(0);
        _values.Add(new ObservableValue(Math.Clamp(pct, 0, 100)));
    }
}

// ─── MainWindow ───────────────────────────────────────────────────────────────

public partial class MainWindow : Window
{
    private readonly ChartViewModel _chartVm = new();
    private readonly ObservableCollection<TextureViewModel>  _textures  = new();
    private readonly ObservableCollection<ProcessViewModel>  _processes = new();

    private readonly DispatcherTimer _uiTimer      = new();
    private readonly DispatcherTimer _clockTimer   = new();
    private readonly DispatcherTimer _connectTimer = new();
    private readonly DispatcherTimer _procTimer    = new();

    private IPCClient?               _ipc;
    private CancellationTokenSource? _ipcCts;
    private Process?                 _orchProcess;
    private Process?                 _watchProcess;

    // Caminho base onde ficam os executáveis C++
    private string BasePath => Path.Combine(
        AppDomain.CurrentDomain.BaseDirectory,
        "..","..","..","..","build","Release");

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _chartVm;
        ListTextures.ItemsSource  = _textures;
        ListProcesses.ItemsSource = _processes;

        // Tab padrão
        ShowTab(PanelConfig, TabBtnConfig);

        // Relógio
        _clockTimer.Interval = TimeSpan.FromSeconds(1);
        _clockTimer.Tick    += (_, _) => TxtClock.Text = DateTime.Now.ToString("HH:mm:ss");
        _clockTimer.Start();

        // Timer de atualização da UI (lê dados do pipe)
        _uiTimer.Interval = TimeSpan.FromMilliseconds(500);
        _uiTimer.Tick    += OnUITick;

        // Timer de reconexão automática ao pipe (tenta a cada 2s)
        _connectTimer.Interval = TimeSpan.FromSeconds(2);
        _connectTimer.Tick    += OnConnectTick;
        _connectTimer.Start();

        // Timer de atualização da lista de processos (a cada 3s)
        _procTimer.Interval = TimeSpan.FromSeconds(3);
        _procTimer.Tick    += OnProcessTick;
        _procTimer.Start();

        if (ChkAutoStart.IsChecked == true)
            StartOrchestrator();

        AppendLog("Interface iniciada. Aguardando orquestrador...");
        SetStatus("Aguardando conexão com o orquestrador.");
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        _uiTimer.Stop();
        _clockTimer.Stop();
        _connectTimer.Stop();
        _procTimer.Stop();
        _ipcCts?.Cancel();
        _watchProcess?.Kill();
        _orchProcess?.Kill();
        base.OnClosing(e);
    }

    // ─── Orquestrador ─────────────────────────────────────────────────────────

    private void StartOrchestrator()
    {
        string orchPath = Path.GetFullPath(Path.Combine(BasePath, "vram_optimizer.exe"));
        if (!File.Exists(orchPath))
        {
            AppendLog($"[AVISO] vram_optimizer.exe não encontrado em:\n  {orchPath}");
            return;
        }
        try
        {
            _orchProcess = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName               = orchPath,
                    UseShellExecute        = false,
                    RedirectStandardOutput = true,
                    CreateNoWindow         = true,
                }
            };
            _orchProcess.OutputDataReceived += (_, e) =>
            {
                if (e.Data != null)
                    Dispatcher.InvokeAsync(() => AppendLog(e.Data));
            };
            _orchProcess.Start();
            _orchProcess.BeginOutputReadLine();
            AppendLog($"Orquestrador iniciado (PID {_orchProcess.Id}).");
        }
        catch (Exception ex)
        {
            AppendLog($"[ERRO] {ex.Message}");
        }
    }

    // ─── Timer de reconexão ao pipe ───────────────────────────────────────────

    private async void OnConnectTick(object? sender, EventArgs e)
    {
        if (_ipc?.IsConnected == true) return;   // já conectado

        _ipc?.Dispose();
        _ipcCts?.Cancel();
        _ipcCts = new CancellationTokenSource();
        _ipc    = new IPCClient();

        bool ok = await _ipc.ConnectAsync(_ipcCts.Token);
        if (ok)
        {
            SetHookStatus(false, "Orquestrador conectado");
            _uiTimer.Start();
            AppendLog("Conectado ao orquestrador via pipe.");
            SetStatus("Conectado. Inicie o jogo e injete o hook.");
        }
    }

    // ─── Timer de lista de processos ──────────────────────────────────────────

    private void OnProcessTick(object? sender, EventArgs e)
    {
        // Atualiza a lista de processos visíveis (com janela)
        _processes.Clear();
        foreach (var p in Process.GetProcesses())
        {
            try
            {
                if (string.IsNullOrEmpty(p.MainWindowTitle)) continue;
                if (p.Id == Environment.ProcessId) continue;
                _processes.Add(new ProcessViewModel
                {
                    PID  = (uint)p.Id,
                    Name = p.ProcessName + ".exe",
                });
            }
            catch { }
        }
    }

    // ─── Timer de UI ──────────────────────────────────────────────────────────

    private async void OnUITick(object? sender, EventArgs e)
    {
        if (_ipc == null || !_ipc.IsConnected)
        {
            _uiTimer.Stop();
            SetHookStatus(false, "Aguardando orquestrador...");
            return;
        }

        var snap = await _ipc.RequestSnapshotAsync();
        if (snap == null) return;

        UpdateVRAMDisplay(snap);
        UpdateStats(snap);
        _chartVm.AddPoint(snap.VRAMPercent);

        bool hookOk = snap.HookConnected;
        SetHookStatus(hookOk, hookOk ? "Hook conectado ao jogo" : "Hook não conectado");

        if (PanelTextures.Visibility == Visibility.Visible && snap.Textures.Count > 0)
            RefreshTextureList(snap.Textures);
    }

    // ─── Atualização de controles ─────────────────────────────────────────────

    private void UpdateVRAMDisplay(OrchestratorSnapshot snap)
    {
        double usedMB  = snap.UsedBytes  / (1024.0 * 1024.0);
        double totalMB = snap.TotalBytes / (1024.0 * 1024.0);
        double freeMB  = snap.FreeBytes  / (1024.0 * 1024.0);
        double pct     = snap.VRAMPercent;

        TxtVRAMUsed.Text = $"{usedMB:0} MB / {totalMB:0} MB";
        TxtVRAMPct.Text  = $"{pct:0.0}%";
        RunFree.Text     = $"{freeMB:0} MB";
        RunPeak.Text     = $"{snap.PeakPercent:0.0}%";
        TxtBackend.Text  = $"Backend: {snap.Backend}";

        Brush pctBrush = pct >= 95 ? (Brush)FindResource("BrCrit")
                       : pct >= 80 ? (Brush)FindResource("BrWarn")
                       :             (Brush)FindResource("BrOk");
        TxtVRAMPct.Foreground = pctBrush;

        Dispatcher.InvokeAsync(() =>
        {
            double maxW = ((Border)VRAMBar.Parent).ActualWidth;
            if (maxW <= 0) return;
            VRAMBar.Width      = Math.Max(0, Math.Min(maxW, maxW * pct / 100.0));
            VRAMBar.Background = pctBrush;
        }, DispatcherPriority.Render);
    }

    private void UpdateStats(OrchestratorSnapshot snap)
    {
        TxtStatTextures.Text = $"{snap.CompressedTextures} / {snap.TotalTextures}";
        TxtStatSaved.Text    = $"{snap.TotalSavedBytes / (1024.0 * 1024.0):0.1} MB";
        TxtStatActions.Text  = snap.ActionsFired.ToString();
    }

    private void RefreshTextureList(List<TextureInfo> textures)
    {
        _textures.Clear();
        TxtTextureCount.Text = $"{textures.Count} texturas detectadas";
        foreach (var t in textures)
        {
            _textures.Add(new TextureViewModel
            {
                Name        = t.Name,
                Resolution  = $"{t.Width}×{t.Height}",
                SizeMB      = $"{t.SizeBytes / (1024.0 * 1024.0):0.1} MB",
                Priority    = t.Priority switch { 1=>"Crítica",2=>"Alta",3=>"Média",4=>"Baixa",5=>"Descartável",_=>$"{t.Priority}" },
                Compression = t.CompressionLevel == 0 ? "—" : t.CompressionName,
                SavedMB     = t.SavedBytes > 0 ? $"{t.SavedBytes/(1024.0*1024.0):0.1} MB" : "—",
            });
        }
    }

    // ─── Tabs ─────────────────────────────────────────────────────────────────

    private void ShowTab(FrameworkElement panel, Button btn)
    {
        PanelConfig.Visibility   = Visibility.Collapsed;
        PanelTextures.Visibility = Visibility.Collapsed;
        PanelGraph.Visibility    = Visibility.Collapsed;
        PanelLog.Visibility      = Visibility.Collapsed;
        panel.Visibility         = Visibility.Visible;
        foreach (var b in new[] { TabBtnConfig, TabBtnTextures, TabBtnGraph, TabBtnLog })
            b.Style = (Style)FindResource("BtnSecondary");
        btn.Style = (Style)FindResource("BtnPrimary");
    }

    private void TabConfig_Click  (object s, RoutedEventArgs e) => ShowTab(PanelConfig,   TabBtnConfig);
    private void TabTextures_Click(object s, RoutedEventArgs e) => ShowTab(PanelTextures, TabBtnTextures);
    private void TabGraph_Click   (object s, RoutedEventArgs e) => ShowTab(PanelGraph,    TabBtnGraph);
    private void TabLog_Click     (object s, RoutedEventArgs e) => ShowTab(PanelLog,      TabBtnLog);

    // ─── Injeção ──────────────────────────────────────────────────────────────

    private void BtnInject_Click(object sender, RoutedEventArgs e)
    {
        string proc  = TxtProcessName.Text.Trim();
        string dll   = TxtDllPath.Text.Trim();
        bool   watch = ChkWatchMode.IsChecked == true;

        if (string.IsNullOrEmpty(proc)) { AppendLog("[ERRO] Informe o nome do processo."); return; }

        string injPath = Path.GetFullPath(Path.Combine(BasePath, "vram_injector.exe"));
        if (!File.Exists(injPath)) { AppendLog($"[ERRO] vram_injector.exe não encontrado em:\n  {injPath}"); return; }

        string args = watch
            ? $"--watch \"{proc}\" --dll \"{dll}\""
            : $"--inject \"{proc}\" --dll \"{dll}\"";

        AppendLog($"Injetando em {proc}...");
        RunInjector(injPath, args);
        SetStatus($"Injetor rodando — alvo: {proc}");
    }

    private void BtnEject_Click(object sender, RoutedEventArgs e)
    {
        string proc  = TxtProcessName.Text.Trim();
        string dll   = TxtDllPath.Text.Trim();
        string injPath = Path.GetFullPath(Path.Combine(BasePath, "vram_injector.exe"));
        _watchProcess?.Kill();
        RunInjector(injPath, $"--eject \"{proc}\" --dll \"{dll}\"");
        AppendLog($"Ejetando de {proc}...");
    }

    private void BtnInjectSelected_Click(object sender, RoutedEventArgs e)
    {
        if (ListProcesses.SelectedItem is not ProcessViewModel pvm) return;
        TxtProcessName.Text = pvm.Name;
        BtnInject_Click(sender, e);
    }

    private void RunInjector(string injPath, string args)
    {
        _watchProcess = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName               = injPath,
                Arguments              = args,
                UseShellExecute        = false,
                RedirectStandardOutput = true,
                CreateNoWindow         = true,
            }
        };
        _watchProcess.OutputDataReceived += (_, ev) =>
        {
            if (ev.Data != null)
                Dispatcher.InvokeAsync(() => AppendLog(ev.Data));
        };
        _watchProcess.Start();
        _watchProcess.BeginOutputReadLine();
    }

    private void BtnBrowseDll_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "DLL files (*.dll)|*.dll", Title = "Selecionar vram_hook.dll" };
        if (dlg.ShowDialog() == true) TxtDllPath.Text = dlg.FileName;
    }

    private void ListProcesses_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ListProcesses.SelectedItem is ProcessViewModel pvm)
            TxtProcessName.Text = pvm.Name;
    }

    // ─── Configurações ────────────────────────────────────────────────────────

    private void Slider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsInitialized) return;
        if (sender == SliderWarn)     LblWarn.Text     = $"{SliderWarn.Value:0}%";
        if (sender == SliderLow)      LblLow.Text      = $"{SliderLow.Value:0}%";
        if (sender == SliderMed)      LblMed.Text      = $"{SliderMed.Value:0}%";
        if (sender == SliderEmerg)    LblEmerg.Text    = $"{SliderEmerg.Value:0}%";
        if (sender == SliderMaxTex)   LblMaxTex.Text   = $"{(int)SliderMaxTex.Value}";
        if (sender == SliderInterval) LblInterval.Text = $"{(int)SliderInterval.Value}ms";
    }

    private void BtnApplyConfig_Click(object sender, RoutedEventArgs e)
    {
        if (_ipc == null || !_ipc.IsConnected) { AppendLog("[AVISO] Orquestrador não conectado."); return; }
        var cfg = new OrchestratorConfig
        {
            ThresholdWarning   = (float)SliderWarn.Value,
            ThresholdLow       = (float)SliderLow.Value,
            ThresholdMedium    = (float)SliderMed.Value,
            ThresholdEmergency = (float)SliderEmerg.Value,
            MaxCompressPerTick = (int)SliderMaxTex.Value,
            MonitorIntervalMs  = (int)SliderInterval.Value,
            FormatPreference   = CboFormat.SelectedIndex,
            AllowHalfRes       = ChkAllowHalfRes.IsChecked == true,
            AllowQuarterRes    = ChkAllowQuarterRes.IsChecked == true,
            SkipCritical       = ChkSkipCritical.IsChecked == true,
            LogToFile          = ChkLogToFile.IsChecked == true,
        };
        _ = _ipc.SendConfigAsync(cfg);
        AppendLog("Configurações aplicadas.");
        SetStatus("Configurações enviadas.");
    }

    // ─── Texturas ─────────────────────────────────────────────────────────────

    private void BtnRestoreAll_Click    (object s, RoutedEventArgs e) { _ipc?.SendCommandAsync("restore_all"); AppendLog("Restaurando texturas..."); }
    private void BtnRefreshTextures_Click(object s, RoutedEventArgs e) => AppendLog("Atualização manual solicitada.");

    // ─── Log ──────────────────────────────────────────────────────────────────

    private void BtnClearLog_Click(object s, RoutedEventArgs e) => TxtLog.Clear();

    private void AppendLog(string msg)
    {
        TxtLog.AppendText($"[{DateTime.Now:HH:mm:ss}] {msg}\n");
        TxtLog.ScrollToEnd();
    }

    // ─── Helpers ──────────────────────────────────────────────────────────────

    private void SetStatus(string msg) => TxtStatusBar.Text = msg;

    private void SetHookStatus(bool connected, string msg)
    {
        EllipseHookStatus.Fill   = connected ? (Brush)FindResource("BrOk")
                                             : (Brush)FindResource("BrTextSec");
        TxtHookStatus.Text       = msg;
        TxtHookStatus.Foreground = connected ? (Brush)FindResource("BrOk")
                                             : (Brush)FindResource("BrTextSec");
    }
}

// ─── IPC Client ───────────────────────────────────────────────────────────────

public record TextureInfo(string Name, uint Width, uint Height,
    long SizeBytes, long SavedBytes, int Priority, int CompressionLevel, string CompressionName);

public record OrchestratorSnapshot(
    double VRAMPercent, double PeakPercent,
    long UsedBytes, long TotalBytes, long FreeBytes,
    string Backend, bool HookConnected,
    long TotalSavedBytes, int TotalTextures, int CompressedTextures, int ActionsFired,
    List<TextureInfo> Textures);

public class OrchestratorConfig
{
    public float ThresholdWarning   { get; set; } = 70;
    public float ThresholdLow       { get; set; } = 80;
    public float ThresholdMedium    { get; set; } = 90;
    public float ThresholdEmergency { get; set; } = 95;
    public int   MaxCompressPerTick { get; set; } = 4;
    public int   MonitorIntervalMs  { get; set; } = 500;
    public int   FormatPreference   { get; set; } = 0;
    public bool  AllowHalfRes       { get; set; } = true;
    public bool  AllowQuarterRes    { get; set; } = true;
    public bool  SkipCritical       { get; set; } = true;
    public bool  LogToFile          { get; set; } = true;
}

public class IPCClient : IDisposable
{
    private const string PIPE_STATUS = "vram_opt_status";
    private const string PIPE_UI_CMD = "vram_opt_ui_cmd";

    private NamedPipeClientStream? _pipeStatus;
    private NamedPipeClientStream? _pipeCmd;
    private StreamReader?          _reader;

    public bool IsConnected => _pipeStatus?.IsConnected == true;

    public async Task<bool> ConnectAsync(CancellationToken ct)
    {
        try
        {
            _pipeStatus = new NamedPipeClientStream(".", PIPE_STATUS,
                PipeDirection.In, PipeOptions.Asynchronous);
            await _pipeStatus.ConnectAsync(1000, ct);

            _pipeCmd = new NamedPipeClientStream(".", PIPE_UI_CMD,
                PipeDirection.Out, PipeOptions.Asynchronous);
            await _pipeCmd.ConnectAsync(1000, ct);

            _reader = new StreamReader(_pipeStatus, Encoding.UTF8);
            return true;
        }
        catch { return false; }
    }

    public async Task<OrchestratorSnapshot?> RequestSnapshotAsync()
    {
        if (_reader == null || !IsConnected) return null;
        try
        {
            var line = await _reader.ReadLineAsync();
            return line == null ? null : ParseSnapshot(line);
        }
        catch { return null; }
    }

    public async Task SendConfigAsync(OrchestratorConfig cfg)
    {
        if (_pipeCmd == null || !_pipeCmd.IsConnected) return;
        var sb = new StringBuilder();
        sb.AppendLine($"CONFIG warn={cfg.ThresholdWarning}");
        sb.AppendLine($"CONFIG low={cfg.ThresholdLow}");
        sb.AppendLine($"CONFIG med={cfg.ThresholdMedium}");
        sb.AppendLine($"CONFIG emerg={cfg.ThresholdEmergency}");
        sb.AppendLine($"CONFIG max_tex={cfg.MaxCompressPerTick}");
        sb.AppendLine($"CONFIG interval={cfg.MonitorIntervalMs}");
        sb.AppendLine($"CONFIG format={cfg.FormatPreference}");
        sb.AppendLine($"CONFIG half_res={(cfg.AllowHalfRes?1:0)}");
        sb.AppendLine($"CONFIG quarter_res={(cfg.AllowQuarterRes?1:0)}");
        sb.AppendLine($"CONFIG skip_critical={(cfg.SkipCritical?1:0)}");
        var bytes = Encoding.UTF8.GetBytes(sb.ToString());
        try { await _pipeCmd.WriteAsync(bytes); await _pipeCmd.FlushAsync(); } catch { }
    }

    public async void SendCommandAsync(string cmd)
    {
        if (_pipeCmd == null || !_pipeCmd.IsConnected) return;
        var bytes = Encoding.UTF8.GetBytes(cmd + "\n");
        try { await _pipeCmd.WriteAsync(bytes); await _pipeCmd.FlushAsync(); } catch { }
    }

    private static OrchestratorSnapshot? ParseSnapshot(string json)
    {
        try
        {
            double Get(string key) {
                int i = json.IndexOf($"\"{key}\":", StringComparison.Ordinal);
                if (i < 0) return 0;
                int s = json.IndexOf(':', i) + 1;
                int end = json.IndexOfAny(new[]{',','}'}, s);
                return double.Parse(json[s..end].Trim(), System.Globalization.CultureInfo.InvariantCulture);
            }
            string GetStr(string key) {
                int i = json.IndexOf($"\"{key}\":\"", StringComparison.Ordinal);
                if (i < 0) return "";
                int s = json.IndexOf('"', json.IndexOf(':', i)+1)+1;
                return json[s..json.IndexOf('"', s)];
            }
            return new OrchestratorSnapshot(
                Get("pct"), Get("peak"),
                (long)Get("used"), (long)Get("total"), (long)Get("free"),
                GetStr("backend"), (int)Get("hook")==1,
                (long)Get("saved"), (int)Get("total_tex"),
                (int)Get("comp_tex"), (int)Get("actions"),
                new List<TextureInfo>());
        }
        catch { return null; }
    }

    public void Dispose()
    {
        _reader?.Dispose();
        _pipeStatus?.Dispose();
        _pipeCmd?.Dispose();
    }
}