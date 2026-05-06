/*
 * MainWindow.xaml.cs
 * ─────────────────────────────────────────────────────────────────────────────
 * Code-behind da janela principal.
 * Comunica-se com o orquestrador C++ via Named Pipe (IPCClient).
 * Toda atualização de UI vem de um DispatcherTimer que lê o IPCClient.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
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

// ─── ViewModel de uma textura (para a lista) ──────────────────────────────────
public class TextureViewModel
{
    public string Name        { get; set; } = "";
    public string Resolution  { get; set; } = "";
    public string SizeMB      { get; set; } = "";
    public string Priority    { get; set; } = "";
    public string Compression { get; set; } = "";
    public string SavedMB     { get; set; } = "";
}

// ─── ViewModel do gráfico (LiveCharts2) ──────────────────────────────────────
public class ChartViewModel : INotifyPropertyChanged
{
    private readonly ObservableCollection<ObservableValue> _values = new();

    public ISeries[] ChartSeries { get; }
    public Axis[]    XAxes       { get; }
    public Axis[]    YAxes       { get; }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ChartViewModel()
    {
        // Pré-popula com 60 pontos em zero
        for (int i = 0; i < 60; i++)
            _values.Add(new ObservableValue(0));

        ChartSeries = new ISeries[]
        {
            new LineSeries<ObservableValue>
            {
                Values          = _values,
                Name            = "VRAM %",
                Stroke          = new SolidColorPaint(SKColor.Parse("#4F8EF7"), 2),
                Fill            = new LinearGradientPaint(
                    new[] { SKColor.Parse("#334F8EF7"), SKColor.Parse("#004F8EF7") },
                    new SKPoint(0, 0), new SKPoint(0, 1)),
                GeometrySize    = 0,
                LineSmoothness  = 0.5,
            }
        };

        XAxes = new Axis[]
        {
            new Axis
            {
                Labels          = null,
                IsVisible       = false,
                SeparatorsPaint = null,
            }
        };

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

    public void AddPoint(double percent)
    {
        _values.RemoveAt(0);
        _values.Add(new ObservableValue(percent));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow
// ─────────────────────────────────────────────────────────────────────────────

public partial class MainWindow : Window
{
    // ── State ─────────────────────────────────────────────────────────────────
    private readonly ChartViewModel _chartVm = new();
    private readonly ObservableCollection<TextureViewModel> _textures = new();
    private readonly DispatcherTimer _uiTimer   = new();
    private readonly DispatcherTimer _clockTimer = new();

    // IPC com o orquestrador C++ (Named Pipe)
    private IPCClient?       _ipc;
    private CancellationTokenSource? _ipcCts;

    // Processo do orquestrador (se lançado por nós)
    private Process? _orchProcess;

    // Injetor gerenciado (chama vram_injector.exe via Process)
    private Process?  _watchProcess;
    private bool      _watchRunning;

    // Tab ativa
    private FrameworkElement? _activePanel;

    // ─────────────────────────────────────────────────────────────────────────
    // Construtor
    // ─────────────────────────────────────────────────────────────────────────

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _chartVm;

        ListTextures.ItemsSource = _textures;

        // Tab padrão
        _activePanel = PanelConfig;

        // Timer de atualização da UI (500ms)
        _uiTimer.Interval = TimeSpan.FromMilliseconds(500);
        _uiTimer.Tick    += OnUITick;

        // Timer do relógio
        _clockTimer.Interval = TimeSpan.FromSeconds(1);
        _clockTimer.Tick    += (_, _) =>
            TxtClock.Text = DateTime.Now.ToString("HH:mm:ss");
        _clockTimer.Start();

        // Inicia IPC e orquestrador automaticamente se configurado
        if (ChkAutoStart.IsChecked == true)
            StartOrchestrator();

        AppendLog("Interface iniciada.");
        SetStatus("Pronto. Configure os parâmetros e injete a DLL no jogo.");
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        _uiTimer.Stop();
        _clockTimer.Stop();
        _ipcCts?.Cancel();
        _watchProcess?.Kill();
        _orchProcess?.Kill();
        base.OnClosing(e);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Orquestrador C++ — lança vram_optimizer.exe em background
    // ─────────────────────────────────────────────────────────────────────────

    private void StartOrchestrator()
    {
        string orchPath = Path.Combine(
            AppDomain.CurrentDomain.BaseDirectory, "vram_optimizer.exe");

        if (!File.Exists(orchPath))
        {
            AppendLog($"[AVISO] vram_optimizer.exe não encontrado em:\n  {orchPath}");
            AppendLog("  Compile o projeto C++ e coloque o .exe na mesma pasta que a UI.");
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
                    RedirectStandardError  = true,
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
            ConnectIPC();
        }
        catch (Exception ex)
        {
            AppendLog($"[ERRO] Não foi possível iniciar o orquestrador: {ex.Message}");
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // IPC — conecta ao named pipe do orquestrador
    // ─────────────────────────────────────────────────────────────────────────

    private void ConnectIPC()
    {
        _ipcCts = new CancellationTokenSource();
        _ipc    = new IPCClient();

        Task.Run(async () =>
        {
            bool connected = await _ipc.ConnectAsync(_ipcCts.Token);
            Dispatcher.InvokeAsync(() =>
            {
                if (connected)
                {
                    SetHookStatus(connected: true, "Orquestrador conectado");
                    _uiTimer.Start();
                    AppendLog("IPC conectado ao orquestrador.");
                }
                else
                {
                    AppendLog("[AVISO] Não foi possível conectar ao orquestrador via pipe.");
                }
            });
        });
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Timer de UI — lê dados do IPC e atualiza controles
    // ─────────────────────────────────────────────────────────────────────────

    private async void OnUITick(object? sender, EventArgs e)
    {
        if (_ipc == null || !_ipc.IsConnected) return;

        var snapshot = await _ipc.RequestSnapshotAsync();
        if (snapshot == null) return;

        UpdateVRAMDisplay(snapshot);
        UpdateStats(snapshot);
        _chartVm.AddPoint(snapshot.VRAMPercent);

        // Atualiza lista de texturas se a tab estiver visível
        if (PanelTextures.Visibility == Visibility.Visible)
            RefreshTextureList(snapshot.Textures);

        // Indicador de hook
        bool hookConnected = snapshot.HookConnected;
        SetHookStatus(hookConnected,
            hookConnected ? "Hook conectado" : "Hook desconectado");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Atualização dos controles de VRAM
    // ─────────────────────────────────────────────────────────────────────────

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

        // Cor da porcentagem e da barra conforme o nível
        var (barColor, textColor) = pct switch
        {
            >= 95 => (Brushes.Red,    (Brush)FindResource("BrCrit")),
            >= 80 => (Brushes.Orange, (Brush)FindResource("BrWarn")),
            _     => (Brushes.Green,  (Brush)FindResource("BrOk")),
        };
        TxtVRAMPct.Foreground = textColor;

        // Largura da barra proporcional ao container
        VRAMBar.Dispatcher.InvokeAsync(() =>
        {
            double maxW = ((Border)VRAMBar.Parent).ActualWidth;
            VRAMBar.Width      = Math.Max(0, Math.Min(maxW, maxW * pct / 100.0));
            VRAMBar.Background = barColor;
        }, DispatcherPriority.Render);
    }

    private void UpdateStats(OrchestratorSnapshot snap)
    {
        TxtStatTextures.Text = $"{snap.CompressedTextures} / {snap.TotalTextures}";
        TxtStatSaved.Text    = $"{snap.TotalSavedBytes / (1024.0*1024.0):0.1} MB";
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
                SizeMB      = $"{t.SizeBytes / (1024.0*1024.0):0.1} MB",
                Priority    = t.Priority switch
                {
                    1 => "Crítica",
                    2 => "Alta",
                    3 => "Média",
                    4 => "Baixa",
                    5 => "Descartável",
                    _ => t.Priority.ToString()
                },
                Compression = t.CompressionLevel == 0 ? "—" : t.CompressionName,
                SavedMB     = t.SavedBytes > 0
                              ? $"{t.SavedBytes/(1024.0*1024.0):0.1} MB"
                              : "—",
            });
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Navegação entre tabs
    // ─────────────────────────────────────────────────────────────────────────

    private void ShowTab(FrameworkElement panel, Button activeBtn)
    {
        PanelConfig.Visibility   = Visibility.Collapsed;
        PanelTextures.Visibility = Visibility.Collapsed;
        PanelGraph.Visibility    = Visibility.Collapsed;
        PanelLog.Visibility      = Visibility.Collapsed;
        panel.Visibility         = Visibility.Visible;
        _activePanel             = panel;

        foreach (var btn in new[] { TabBtnConfig, TabBtnTextures,
                                    TabBtnGraph, TabBtnLog })
            btn.Style = (Style)FindResource("BtnSecondary");

        activeBtn.Style = (Style)FindResource("BtnPrimary");
    }

    private void TabConfig_Click   (object s, RoutedEventArgs e) => ShowTab(PanelConfig,   TabBtnConfig);
    private void TabTextures_Click (object s, RoutedEventArgs e) => ShowTab(PanelTextures, TabBtnTextures);
    private void TabGraph_Click    (object s, RoutedEventArgs e) => ShowTab(PanelGraph,    TabBtnGraph);
    private void TabLog_Click      (object s, RoutedEventArgs e) => ShowTab(PanelLog,      TabBtnLog);

    // ─────────────────────────────────────────────────────────────────────────
    // Controles de injeção
    // ─────────────────────────────────────────────────────────────────────────

    private void BtnInject_Click(object sender, RoutedEventArgs e)
    {
        string proc    = TxtProcessName.Text.Trim();
        string dll     = TxtDllPath.Text.Trim();
        bool   watch   = ChkWatchMode.IsChecked == true;
        string injPath = Path.Combine(
            AppDomain.CurrentDomain.BaseDirectory, "vram_injector.exe");

        if (!File.Exists(injPath))
        {
            AppendLog("[ERRO] vram_injector.exe não encontrado. Compile o projeto C++.");
            return;
        }

        string args = watch
            ? $"--watch \"{proc}\" --dll \"{dll}\""
            : $"--inject \"{proc}\" --dll \"{dll}\"";

        AppendLog($"Injetando em {proc} (modo: {(watch ? "watch" : "direto")})...");

        _watchProcess = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName               = injPath,
                Arguments              = args,
                UseShellExecute        = false,
                RedirectStandardOutput = true,
                RedirectStandardError  = true,
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
        _watchRunning = true;

        SetStatus($"Injetor rodando — alvo: {proc}");
    }

    private void BtnEject_Click(object sender, RoutedEventArgs e)
    {
        string proc    = TxtProcessName.Text.Trim();
        string dll     = TxtDllPath.Text.Trim();
        string injPath = Path.Combine(
            AppDomain.CurrentDomain.BaseDirectory, "vram_injector.exe");

        if (_watchProcess != null && !_watchProcess.HasExited)
        {
            _watchProcess.Kill();
            _watchProcess = null;
            _watchRunning = false;
        }

        RunInjectorCommand($"--eject \"{proc}\" --dll \"{dll}\"", injPath);
        AppendLog($"Ejetando de {proc}...");
        SetStatus("Ejeção solicitada.");
    }

    private void RunInjectorCommand(string args, string injPath)
    {
        var p = new Process
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
        p.OutputDataReceived += (_, ev) =>
        {
            if (ev.Data != null)
                Dispatcher.InvokeAsync(() => AppendLog(ev.Data));
        };
        p.Start();
        p.BeginOutputReadLine();
    }

    private void BtnBrowseDll_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = "DLL files (*.dll)|*.dll|All files (*.*)|*.*",
            Title  = "Selecionar vram_hook.dll",
        };
        if (dlg.ShowDialog() == true)
            TxtDllPath.Text = dlg.FileName;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Configurações
    // ─────────────────────────────────────────────────────────────────────────

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
        // Envia configuração atualizada ao orquestrador via IPC
        if (_ipc == null || !_ipc.IsConnected)
        {
            AppendLog("[AVISO] Orquestrador não conectado. Inicie-o primeiro.");
            return;
        }

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
        SetStatus("Configurações enviadas ao orquestrador.");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Texturas
    // ─────────────────────────────────────────────────────────────────────────

    private void BtnRestoreAll_Click(object sender, RoutedEventArgs e)
    {
        _ipc?.SendCommandAsync("restore_all");
        AppendLog("Solicitando restauração de todas as texturas...");
    }

    private void BtnRefreshTextures_Click(object sender, RoutedEventArgs e)
    {
        // A lista é atualizada automaticamente pelo timer; força um ciclo
        AppendLog("Lista de texturas atualizada.");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Log
    // ─────────────────────────────────────────────────────────────────────────

    private void BtnClearLog_Click(object sender, RoutedEventArgs e)
    {
        TxtLog.Clear();
    }

    private void AppendLog(string msg)
    {
        string line = $"[{DateTime.Now:HH:mm:ss}] {msg}\n";
        TxtLog.AppendText(line);
        TxtLog.ScrollToEnd();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Helpers de status
    // ─────────────────────────────────────────────────────────────────────────

    private void SetStatus(string msg) => TxtStatusBar.Text = msg;

    private void SetHookStatus(bool connected, string msg)
    {
        EllipseHookStatus.Fill = connected
            ? (Brush)FindResource("BrOk")
            : (Brush)FindResource("BrTextSec");
        TxtHookStatus.Text       = msg;
        TxtHookStatus.Foreground = connected
            ? (Brush)FindResource("BrOk")
            : (Brush)FindResource("BrTextSec");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IPCClient — comunica com o orquestrador C++ via Named Pipe
// ─────────────────────────────────────────────────────────────────────────────

// Estruturas de dados trocadas via pipe (espelham as do C++)
public record TextureInfo(
    string Name, uint Width, uint Height,
    long SizeBytes, long SavedBytes,
    int Priority, int CompressionLevel, string CompressionName);

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

public class IPCClient
{
    // Nome do pipe de status: o orquestrador envia snapshots em JSON simples
    private const string PIPE_STATUS = "vram_opt_status";
    private const string PIPE_UI_CMD = "vram_opt_ui_cmd";

    private NamedPipeClientStream? _pipeStatus;
    private NamedPipeClientStream? _pipeCmd;
    private StreamReader?          _reader;

    public bool IsConnected =>
        _pipeStatus?.IsConnected == true;

    public async Task<bool> ConnectAsync(CancellationToken ct)
    {
        try
        {
            _pipeStatus = new NamedPipeClientStream(
                ".", PIPE_STATUS, PipeDirection.In,
                PipeOptions.Asynchronous);
            await _pipeStatus.ConnectAsync(5000, ct);

            _pipeCmd = new NamedPipeClientStream(
                ".", PIPE_UI_CMD, PipeDirection.Out,
                PipeOptions.Asynchronous);
            await _pipeCmd.ConnectAsync(2000, ct);

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
            // O orquestrador envia uma linha JSON por tick
            string? line = await _reader.ReadLineAsync();
            if (line == null) return null;
            return ParseSnapshot(line);
        }
        catch { return null; }
    }

    public async Task SendConfigAsync(OrchestratorConfig cfg)
    {
        if (_pipeCmd == null || !_pipeCmd.IsConnected) return;
        // Serialização simples key=value por linha
        var sb = new StringBuilder();
        sb.AppendLine($"CONFIG warn={cfg.ThresholdWarning}");
        sb.AppendLine($"CONFIG low={cfg.ThresholdLow}");
        sb.AppendLine($"CONFIG med={cfg.ThresholdMedium}");
        sb.AppendLine($"CONFIG emerg={cfg.ThresholdEmergency}");
        sb.AppendLine($"CONFIG max_tex={cfg.MaxCompressPerTick}");
        sb.AppendLine($"CONFIG interval={cfg.MonitorIntervalMs}");
        sb.AppendLine($"CONFIG format={cfg.FormatPreference}");
        sb.AppendLine($"CONFIG half_res={(cfg.AllowHalfRes ? 1 : 0)}");
        sb.AppendLine($"CONFIG quarter_res={(cfg.AllowQuarterRes ? 1 : 0)}");
        sb.AppendLine($"CONFIG skip_critical={(cfg.SkipCritical ? 1 : 0)}");
        var bytes = Encoding.UTF8.GetBytes(sb.ToString());
        await _pipeCmd.WriteAsync(bytes);
        await _pipeCmd.FlushAsync();
    }

    public async void SendCommandAsync(string cmd)
    {
        if (_pipeCmd == null || !_pipeCmd.IsConnected) return;
        var bytes = Encoding.UTF8.GetBytes(cmd + "\n");
        await _pipeCmd.WriteAsync(bytes);
        await _pipeCmd.FlushAsync();
    }

    // Parse simples de JSON manual (evita dep em System.Text.Json para manter leve)
    // Formato enviado pelo C++:
    // {"pct":42.5,"peak":75.0,"used":1073741824,"total":2147483648,...}
    private static OrchestratorSnapshot? ParseSnapshot(string json)
    {
        try
        {
            double Get(string key)
            {
                int i = json.IndexOf($"\"{key}\":", StringComparison.Ordinal);
                if (i < 0) return 0;
                int start = json.IndexOf(':', i) + 1;
                int end   = json.IndexOfAny(new[] {',', '}'}, start);
                return double.Parse(json[start..end].Trim());
            }
            string GetStr(string key)
            {
                int i = json.IndexOf($"\"{key}\":\"", StringComparison.Ordinal);
                if (i < 0) return "";
                int start = json.IndexOf('"', json.IndexOf(':', i) + 1) + 1;
                int end   = json.IndexOf('"', start);
                return json[start..end];
            }

            return new OrchestratorSnapshot(
                VRAMPercent:       Get("pct"),
                PeakPercent:       Get("peak"),
                UsedBytes:         (long)Get("used"),
                TotalBytes:        (long)Get("total"),
                FreeBytes:         (long)Get("free"),
                Backend:           GetStr("backend"),
                HookConnected:     (int)Get("hook") == 1,
                TotalSavedBytes:   (long)Get("saved"),
                TotalTextures:     (int)Get("total_tex"),
                CompressedTextures:(int)Get("comp_tex"),
                ActionsFired:      (int)Get("actions"),
                Textures:          new List<TextureInfo>() // detalhes via request separado
            );
        }
        catch { return null; }
    }
}
