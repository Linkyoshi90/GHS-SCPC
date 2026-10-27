// "GHS-SCPC.exe": unpacks the user's disc image into game\, builds the port once (tools\setup.ps1), then plays
// (starts the runner itself, like tools\launch.ps1, so Play also works under Wine/Proton). Build it with the C# compiler that comes with Windows (.NET Framework 4, C# 5), from the repo root:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /target:winexe /win32icon:ghs.ico /out:GHS-SCPC.exe tools\launcher.cs
// (ghs.ico: the exe's icon; the window takes it from the exe.)
// Updates: in a git clone it fetches on start and offers "Update" when origin/main has new commits (see Update()).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Media;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

static class Launcher
{
    // The recompiled code and overrides\ are made for this exact program (Europe, SLES-51933, v1.00).
    const string Elf = "SLES_519.33";
    const string ElfSha1 = "8676819AA0EDC9F1B5EFCDD1C0C97934DA5B6528";

    static readonly string Root = AppDomain.CurrentDomain.BaseDirectory;
    static readonly string GameDir = Path.Combine(Root, "game");
    static readonly string Runner = Path.Combine(Root, @"PS2Recomp\out\build\ps2xRuntime\Release\ps2EntryRunner.exe");
    static readonly string Exe = Path.Combine(Root, "GHS-SCPC.exe"), OldExe = Path.Combine(Root, "GHS-SCPC.old.exe");

    static Form form;
    static Label status;
    static BloodBar progress;
    static TextBox log;
    static Button button, musicButton, nextButton, jpButton, updateButton, graphicsButton;
    // Where the game draws (launcher.ini graphics=, tools\launch.ps1 reads it too): the GPU through D3D11 or Vulkan
    // (GHS_GS_API=vulkan; only in a build made with the Vulkan SDK), or the CPU (GHS_GS_GPU=0).
    static readonly string[][] Backends = { new[] { "d3d11", "Direct3D 11" }, new[] { "vulkan", "Vulkan" }, new[] { "cpu", "CPU (slow)" } };
    static StepCard[] steps;
    static Action buttonAction;
    static Process running; // the build, so closing the window can stop it

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr hwnd, string app, string idList);
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    // Dark cartoon-horror look (the game's logo colours: blue GREGORY, dripping red HORROR, cracked yellow SHOW).
    public static readonly Color Back = Color.FromArgb(15, 11, 19), Card = Color.FromArgb(27, 20, 34), Ink = Color.FromArgb(230, 221, 238),
        Dim = Color.FromArgb(150, 138, 162), Blood = Color.FromArgb(200, 40, 48), Bone = Color.FromArgb(242, 200, 70), Ghost = Color.FromArgb(90, 160, 255);
    public static float Scale = 1f;
    public static int S(float px) { return (int)Math.Round(px * Scale); }

    [STAThread]
    static void Main()
    {
        SetProcessDPIAware();
        Application.EnableVisualStyles();
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) Scale = g.DpiX / 96f;
        form = new Form { Text = "GHS:SCPC", ClientSize = new Size(S(940), S(730)), MinimumSize = new Size(S(800), S(680)),
                          StartPosition = FormStartPosition.CenterScreen, BackColor = Back, ForeColor = Ink };
        try { form.Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        var title = new TitlePanel { Dock = DockStyle.Top, Height = S(215) };

        // Walkthrough: the three steps, the current one lit.
        steps = new[]
        {
            new StepCard("1", "Unpack your disc", "Choose the game's disc image (.iso, European release SLES-51933). " +
                         "The game files are copied into the game folder; your disc image stays untouched."),
            new StepCard("2", "Build the game", "One time, 30-60 minutes. Needs Git and the free Visual Studio C++ Build Tools (the button installs them) " +
                         "and an internet connection. The log on the right shows the progress."),
            new StepCard("3", "Play", "Starts the game. In game: F2 settings, F5 cheats, F6 snapshot. " +
                         "Optional: \"Japanese voices...\" below adds the Japanese voice recordings (Japanese disc, Python 3 + numpy)."),
        };
        var stepColumn = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(S(14), S(10), S(6), S(6)), BackColor = Back };
        stepColumn.Controls.Add(new Label { Text = "HOW TO PLAY", ForeColor = Dim, Font = new Font("Segoe UI", 9, FontStyle.Bold), AutoSize = true, Margin = new Padding(S(2), 0, 0, S(6)) }, 0, 0);
        for (int i = 0; i < steps.Length; i++)
        {
            steps[i].Dock = DockStyle.Fill;
            steps[i].Margin = new Padding(0, 0, 0, S(10));
            stepColumn.Controls.Add(steps[i], 0, i + 1);
            stepColumn.RowStyles.Add(new RowStyle(SizeType.Percent, 33.3f));
        }
        stepColumn.RowStyles.Insert(0, new RowStyle(SizeType.AutoSize));
        graphicsButton = MakeButton("", Ghost, false);
        graphicsButton.Width = S(240);
        graphicsButton.Click += delegate { NextGraphics(); };
        stepColumn.Controls.Add(graphicsButton, 0, 4);
        stepColumn.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        UpdateGraphicsButton();

        status = new Label { Dock = DockStyle.Top, Height = S(62), Font = new Font("Segoe UI", 10.5f), ForeColor = Ink, Padding = new Padding(0, S(4), 0, 0) };
        progress = new BloodBar { Dock = DockStyle.Top, Height = S(16), Visible = false };
        log = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 9),
                            BackColor = Color.FromArgb(12, 9, 15), ForeColor = Dim, BorderStyle = BorderStyle.None };
        var logFrame = new Panel { Dock = DockStyle.Fill, Padding = new Padding(S(8)), BackColor = Color.FromArgb(12, 9, 15), Margin = new Padding(0) };
        logFrame.Controls.Add(log);
        var spacer = new Panel { Dock = DockStyle.Top, Height = S(10), BackColor = Back };
        var right = new Panel { Dock = DockStyle.Fill, Padding = new Padding(S(8), S(12), S(16), S(6)), BackColor = Back };
        right.Controls.Add(logFrame);
        right.Controls.Add(spacer);
        right.Controls.Add(progress);
        right.Controls.Add(status);

        var middle = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, BackColor = Back };
        middle.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(370)));
        middle.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        middle.Controls.Add(stepColumn, 0, 0);
        middle.Controls.Add(right, 1, 0);

        button = MakeButton("", Blood, true);
        button.Click += delegate { buttonAction(); };
        musicButton = MakeButton("", Ghost, false);
        musicButton.Click += delegate { Music.Toggle(); UpdateMusicButtons(); if (Music.On) Log(Music.Describe()); };
        nextButton = MakeButton("Next track", Ghost, false);
        nextButton.Click += delegate { Music.Next(); UpdateMusicButtons(); Log(Music.Describe()); };
        var tips = new ToolTip();
        tips.SetToolTip(musicButton, "Background music from the game (needs the unpacked game files)");
        tips.SetToolTip(nextButton, "Play the next of the game's music tracks");
        tips.SetToolTip(graphicsButton, "Where the game draws its graphics, from the next start: the graphics card through Direct3D 11 (default) " +
                                        "or Vulkan, or the CPU (slow). Vulkan is offered when the game was built with the Vulkan SDK installed.");
        jpButton = MakeButton("Japanese voices...", Ghost, false);
        jpButton.Width = S(170);
        jpButton.Click += delegate { ChooseJapaneseIso(); };
        tips.SetToolTip(jpButton, "Optional: the Japanese voice recordings with the English game (F2 -> Japanese voices in game). " +
                                  "Needs the Japanese disc image, Python 3 and numpy.");
        updateButton = MakeButton("Update", Bone, false);
        updateButton.Click += delegate { Update(); };
        ShowUpdate(false);
        tips.SetToolTip(updateButton, "Lights up when GitHub has a new version (checked at start, in a git clone): " +
                                      "gets it (the log lists what changed) and rebuilds what it needs");
        var bottom = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = S(64), ColumnCount = 5, RowCount = 1, Padding = new Padding(S(14), S(10), S(16), S(12)), BackColor = Card };
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        bottom.Controls.Add(musicButton, 0, 0);
        bottom.Controls.Add(nextButton, 1, 0);
        bottom.Controls.Add(jpButton, 2, 0);
        bottom.Controls.Add(updateButton, 3, 0);
        bottom.Controls.Add(button, 4, 0);

        form.Controls.Add(middle);
        form.Controls.Add(bottom);
        form.Controls.Add(title);
        form.AcceptButton = button;
        form.FormClosing += OnClosing;
        form.HandleCreated += delegate
        {
            // Dark title bar and scroll bars where Windows has them (10 1809+; elsewhere these calls do nothing).
            int on = 1;
            try { DwmSetWindowAttribute(form.Handle, 20, ref on, 4); } catch (DllNotFoundException) { } catch (EntryPointNotFoundException) { }
        };
        log.HandleCreated += delegate { try { SetWindowTheme(log.Handle, "DarkMode_Explorer", null); } catch (DllNotFoundException) { } catch (EntryPointNotFoundException) { } };
        form.Shown += delegate { ShowState(); if (Music.On && Music.Available()) { Music.Start(); Log(Music.Describe()); } UpdateMusicButtons(); CheckForUpdates(); };
        // Back from the Git / C++ tools installers: check again (only while idle at the build step).
        form.Activated += delegate { if (running == null && button.Enabled && steps[1].State == StepState.Current) ShowState(); };
        Application.Run(form);
    }

    static Button MakeButton(string text, Color accent, bool main)
    {
        var b = new Button { Text = text, AutoSize = false, Height = S(40), Width = S(main ? 240 : 132), FlatStyle = FlatStyle.Flat,
                             BackColor = main ? Color.FromArgb(70, 14, 20) : Back, ForeColor = Ink, Cursor = Cursors.Hand,
                             Font = new Font("Segoe UI", main ? 11f : 9.5f, FontStyle.Bold), Margin = new Padding(0, 0, S(10), 0) };
        b.FlatAppearance.BorderColor = accent;
        b.FlatAppearance.BorderSize = 2;
        b.FlatAppearance.MouseOverBackColor = main ? Color.FromArgb(110, 20, 28) : Color.FromArgb(28, 34, 56);
        b.FlatAppearance.MouseDownBackColor = main ? Color.FromArgb(140, 24, 34) : Color.FromArgb(36, 44, 72);
        return b;
    }

    static void UpdateMusicButtons()
    {
        bool can = Music.Available();
        musicButton.Enabled = can;
        nextButton.Enabled = can && Music.On;
        musicButton.FlatAppearance.BorderColor = can ? Ghost : Color.FromArgb(60, 52, 70);
        nextButton.FlatAppearance.BorderColor = nextButton.Enabled ? Ghost : Color.FromArgb(60, 52, 70);
        musicButton.Text = !can ? "♪ Music (unpack first)" : Music.On ? "♪ Music: on" : "♪ Music: off";
        musicButton.Width = S(can ? 132 : 190);
    }

    // The saved backend, D3D11 when it is Vulkan but the build has no Vulkan kernels (gs_spirv: made by a build with the SDK).
    static string GraphicsChoice()
    {
        string g = Settings.Get("graphics", "d3d11");
        bool vulkan = File.Exists(Path.Combine(Root, @"PS2Recomp\out\build\ps2xRuntime\gs_spirv\gs_spirv.cpp"));
        return Backends.Any(b => b[0] == g) && (g != "vulkan" || vulkan) ? g : "d3d11";
    }

    static void NextGraphics()
    {
        int i = Array.FindIndex(Backends, b => b[0] == GraphicsChoice());
        string next = Backends[(i + 1) % Backends.Length][0];
        Settings.Set("graphics", next);
        if (GraphicsChoice() != next) // no Vulkan in this build: on to the CPU
            Settings.Set("graphics", next = Backends[(i + 2) % Backends.Length][0]);
        UpdateGraphicsButton();
        Log("Graphics: " + Backends.First(b => b[0] == next)[1] + " (from the next start)");
    }

    static void UpdateGraphicsButton()
    {
        string g = GraphicsChoice();
        graphicsButton.Text = "Graphics: " + Backends.First(b => b[0] == g)[1];
    }

    // Lit (and clickable) only while GitHub has a newer version.
    static void ShowUpdate(bool available)
    {
        updateButton.Enabled = available;
        updateButton.FlatAppearance.BorderColor = available ? Bone : Color.FromArgb(60, 52, 70);
    }

    static void ShowState()
    {
        bool files = GameFilesOk(), built = files && File.Exists(Runner);
        steps[0].State = files ? StepState.Done : StepState.Current;
        steps[1].State = !files ? StepState.Locked : built ? StepState.Done : StepState.Current;
        steps[2].State = built ? StepState.Current : StepState.Locked;
        if (!files)
            Offer("Choose the game's disc image (.iso) to unpack the game files.\nOnly the European disc (SLES-51933) is supported so far.", "Choose disc image...", ChooseIso);
        else if (!built)
        {
            var missing = MissingTools();
            string needs = "Building needs " + string.Join(" and ", missing.Select(t => t[0])) + (missing.Count == 1 ? " (free)" : ", both free") + ". ";
            string names = string.Join(" + ", missing.Select(t => t[1]));
            if (missing.Count > 0 && File.Exists(Winget))
                Offer(needs + "The button installs " + (missing.Count == 1 ? "it" : "them") + " with Windows' package manager (winget); " +
                      "it shows what it will do first.", "Install " + names, () => ConfirmInstall(missing));
            else if (missing.Count > 0) // no winget (Wine, older Windows): the download pages
                Offer(needs + "The button opens the download " + (missing.Count == 1 ? "page" : "pages") + "; install, then come back here." +
                      (missing.Any(t => t[1] == "C++ tools") ? "\nIn the Build Tools installer, tick \"Desktop development with C++\"." : ""),
                      "Get " + names, () => { foreach (var t in missing) Process.Start(t[2]); });
            else
                Offer("The game files are in place. Building the game takes 30-60 minutes (one time).", "Build the game", Build);
        }
        else
            Offer("Ready. The hotel is waiting for you...", "Play", Play);
        jpButton.Enabled = files;
        jpButton.Text = File.Exists(Path.Combine(GameDir, "ghs_jp_voice.txt")) ? "Japanese voices ✔" : "Japanese voices...";
        jpButton.FlatAppearance.BorderColor = files ? Ghost : Color.FromArgb(60, 52, 70);
        UpdateMusicButtons();
    }

    static void Offer(string text, string label, Action action)
    {
        status.Text = text;
        button.Text = label;
        buttonAction = action;
        button.Enabled = true;
        progress.Visible = false;
    }

    static readonly string Winget = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Microsoft\WindowsApps\winget.exe");

    // What building needs and isn't installed: {what, button word, download page, winget arguments, what the confirm
    // window says about it}. Build Tools 2026 (vs18, as the port is built with): the compiler, Windows SDK and CMake
    // without the Visual Studio editor; --passive shows the installer's progress without asking anything.
    static readonly string[] GitTool = { "Git", "Git", "https://git-scm.com/download/win",
        "install --id Git.Git --exact --source winget --silent --accept-package-agreements --accept-source-agreements",
        "Git for Windows (about 70 MB). License: GNU GPL v2." };
    static readonly string[] CppTool = { "the Visual Studio C++ Build Tools", "C++ tools", "https://visualstudio.microsoft.com/visual-cpp-build-tools/",
        "install --id Microsoft.VisualStudio.BuildTools --exact --source winget --accept-package-agreements --accept-source-agreements " +
        "--override \"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended\"",
        "Visual Studio Build Tools 2026: the C++ compiler, Windows SDK and CMake, without the Visual Studio editor (several GB; " +
        "its own progress window shows the download; Windows asks for administrator permission). " +
        "License: https://visualstudio.microsoft.com/license-terms/" };

    // The exact winget commands, and only on "Yes" one after the other (output in the log); then checked again.
    static void ConfirmInstall(List<string[]> tools)
    {
        string text = "The launcher will run these commands with Windows' package manager (winget):\n\n" +
                      string.Join("\n\n", tools.Select(t => "• " + t[4] + "\n   winget " + t[3])) +
                      "\n\n\"Yes\" installs " + (tools.Count == 1 ? "it" : "them") + " and accepts " + (tools.Count == 1 ? "its license" : "their licenses") + ". Install now?";
        if (MessageBox.Show(form, text, "Install the build tools", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;
        InstallNext(tools, 0);
    }

    static void InstallNext(List<string[]> tools, int i)
    {
        if (i == tools.Count)
        {
            ShowState(); // found now: "Build the game"; still missing: the button again
            return;
        }
        Busy("Installing " + tools[i][1] + " with winget (this can take a while) ...");
        RunProcess(Winget, tools[i][3], code =>
        {
            Log(code == 0 ? tools[i][1] + " installed." : "winget ended with code 0x" + code.ToString("X8") + " for " + tools[i][1] + " (see above).");
            InstallNext(tools, i + 1);
        });
    }

    static List<string[]> MissingTools()
    {
        // An installer changes the system PATH, not this process's: read it again, so Git installed while the
        // launcher was open is found, and setup.ps1 (started from here) finds it too.
        Environment.SetEnvironmentVariable("PATH", Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.Machine) + ";" +
                                                   Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.User));
        var missing = new List<string[]>();
        bool git = Environment.GetEnvironmentVariable("PATH").Split(';').Any(dir =>
        {
            try { return dir.Trim().Length > 0 && File.Exists(Path.Combine(dir.Trim().Trim('"'), "git.exe")); }
            catch (ArgumentException) { return false; } // a malformed PATH entry
        });
        if (!git)
            missing.Add(GitTool);
        if (!HasCppTools())
            missing.Add(CppTool);
        return missing;
    }

    // The test setup.ps1 makes: a Visual Studio or Build Tools with the C++ compiler and its CMake.
    static bool HasCppTools()
    {
        string vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Microsoft Visual Studio\Installer\vswhere.exe");
        if (!File.Exists(vswhere))
            return false;
        var p = Process.Start(new ProcessStartInfo(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
                                                            "-find Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true });
        string output = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        return output.Trim().Length > 0;
    }

    static bool GameFilesOk()
    {
        string elf = Path.Combine(GameDir, Elf);
        return File.Exists(elf) && Sha1(File.ReadAllBytes(elf)) == ElfSha1;
    }

    static void ChooseIso()
    {
        var dlg = new OpenFileDialog { Title = "The game's disc image (SLES-51933)", Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*" };
        if (dlg.ShowDialog(form) != DialogResult.OK) return;
        Busy("Unpacking " + Path.GetFileName(dlg.FileName) + " ...");
        progress.Visible = true;
        string iso = dlg.FileName;
        Task.Run(() => Unpack(iso)).ContinueWith(t => form.BeginInvoke((Action)delegate
        {
            if (t.Exception != null) { Log("Error: " + t.Exception.InnerException.Message); ShowState(); }
            else Build();
        }));
    }

    static void Unpack(string isoPath)
    {
        using (var iso = new Iso(isoPath))
        {
            var files = new List<KeyValuePair<string, Iso.Entry>>();
            iso.Collect(iso.Root, "", files);
            var cnf = files.FirstOrDefault(f => f.Key == "SYSTEM.CNF");
            if (cnf.Value == null) throw new InvalidDataException("This is not a PlayStation 2 disc (no SYSTEM.CNF).");
            var boot = Regex.Match(Encoding.ASCII.GetString(iso.ReadFile(cnf.Value)), @"BOOT2\s*=\s*cdrom0:\\?([^;\r\n]+)");
            string elf = boot.Success ? boot.Groups[1].Value.Split('\\').Last().Trim() : "?";
            if (elf != Elf)
            {
                if (elf.StartsWith("SLPM") || elf.StartsWith("SLPS"))
                    throw new InvalidDataException("This is the Japanese release (" + elf + "). Only the European disc (SLES-51933) is supported so far.");
                throw new InvalidDataException("This disc boots " + elf + ", not the European release this port is made for (SLES-51933).");
            }
            var elfEntry = files.First(f => f.Key == Elf).Value;
            if (Sha1(iso.ReadFile(elfEntry)) != ElfSha1)
                throw new InvalidDataException(Elf + " on this disc is a different version than the one this port is made for (v1.00). Check the image against redump.org.");

            long total = files.Sum(f => (long)f.Value.Size), done = 0;
            foreach (var f in files)
            {
                Log("  " + f.Key + " (" + (f.Value.Size >> 20) + " MB)");
                string dest = Path.Combine(GameDir, f.Key);
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                using (var output = File.Create(dest))
                    iso.CopyFile(f.Value, output, n =>
                    {
                        done += n;
                        int value = (int)(done * 1000 / Math.Max(1, total));
                        form.BeginInvoke((Action)delegate { progress.Value = value; });
                    });
            }
            Log("Unpacked " + files.Count + " files into game\\.");
        }
    }

    static void Build()
    {
        Busy("Building the game (30-60 minutes, one time). Needs git, Visual Studio with C++, and internet.");
        RunScript("setup.ps1", code =>
        {
            if (code != 0 || !File.Exists(Runner)) Log("The build did not finish. Fix the problem above, then press the button to try again.");
            ShowState();
        });
    }

    static void Play()
    {
        // VU1 programs the game ran on the slow path last time (game\vu1_untranslated): translate them once, then play
        string newDir = Path.Combine(GameDir, "vu1_untranslated"), known = Path.Combine(Root, @"recomp\vu1images");
        int fresh = Directory.Exists(newDir) ? Directory.GetFiles(newDir, "*.bin").Count(f => !File.Exists(Path.Combine(known, Path.GetFileName(f)))) : 0;
        // The speed-up rebuilds the runner, so it needs Visual Studio (missing under Proton: those programs stay slow)
        string vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Microsoft Visual Studio\Installer\vswhere.exe");
        if (fresh > 0 && File.Exists(vswhere))
        {
            Busy("Speeding up " + fresh + " graphics program(s) found in your last session (a few minutes, one time) ...");
            RunScript("add_vu1_images.ps1", code => Start());
        }
        else Start();
    }

    static void Start()
    {
        Busy("Starting ...");
        // What tools\launch.ps1 does without debug switches, but without PowerShell (Wine/Proton has only a stub).
        // cmd writes the logs: redirecting through .NET would tie the game's output to pipes owned by this window.
        string recomp = Path.Combine(Root, "recomp");
        Directory.CreateDirectory(recomp);
        foreach (string name in new[] { "live.log", "live.err.log", "port_trace.log" }) // keep the previous run's logs
        {
            string old = Path.Combine(recomp, name), prev = Path.Combine(recomp, name.StartsWith("live") ? "live.prev" + name.Substring(4) : "port_trace.prev.log");
            try { if (File.Exists(old)) { File.Delete(prev); File.Move(old, prev); } } catch (IOException) { } // still open: the game runs already
        }
        var psi = new ProcessStartInfo("cmd.exe", "/c \"\"" + Runner + "\" \"" + Path.Combine(GameDir, Elf) + "\" >\"" + Path.Combine(recomp, "live.log") + "\" 2>\"" + Path.Combine(recomp, "live.err.log") + "\"\"")
        { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = GameDir };
        psi.EnvironmentVariables["GHS_MC_ROOT"] = Path.Combine(recomp, "memcards");
        string graphics = GraphicsChoice();
        psi.EnvironmentVariables.Remove("GHS_GS_API");
        psi.EnvironmentVariables.Remove("GHS_GS_GPU");
        if (graphics == "vulkan") psi.EnvironmentVariables["GHS_GS_API"] = "vulkan";
        if (graphics == "cpu") psi.EnvironmentVariables["GHS_GS_GPU"] = "0";
        var p = Process.Start(psi);
        // cmd lives as long as the game, so an exit within a few seconds with an error means it did not start
        Task.Run(() => p.WaitForExit(5000)).ContinueWith(t => form.BeginInvoke((Action)delegate
        {
            if (!t.Result || p.ExitCode == 0) form.Close();
            else { Log("The game did not start (see recomp\\live.err.log)."); ShowState(); }
        }));
    }

    // Updates through git (the README's install is a clone; a ZIP download has no .git and no update check).
    static void CheckForUpdates()
    {
        try { File.Delete(OldExe); } catch (IOException) { } catch (UnauthorizedAccessException) { } // the launcher an update replaced
        if (!Directory.Exists(Path.Combine(Root, ".git"))) return;
        Task.Run(() =>
        {
            string output;
            if (Git("fetch -q origin", out output) != 0 || Git("log --format=%s HEAD..origin/main", out output) != 0) return (string)null;
            return output;
        }).ContinueWith(t => form.BeginInvoke((Action)delegate
        {
            if (string.IsNullOrEmpty(t.Result)) return;
            string[] commits = t.Result.Split('\n');
            Log("Update available (" + commits.Length + " change" + (commits.Length == 1 ? "" : "s") + "), press Update:");
            foreach (string c in commits) Log("  - " + c.Trim());
            ShowUpdate(true);
        }));
    }

    // Fast-forwards to origin/main, rebuilds what the changed files need (tools\setup.ps1 only redoes what changed) and
    // restarts the launcher when the update replaced it.
    static void Update()
    {
        if (running != null) return;
        if (Process.GetProcessesByName("ps2EntryRunner").Length > 0) { Log("Close the game first, then press Update."); return; }
        ShowUpdate(false);
        Busy("Updating ...");
        Task.Run(() => PullUpdate()).ContinueWith(t => form.BeginInvoke((Action)delegate
        {
            if (t.Exception != null) { Log("The update failed: " + t.Exception.InnerException.Message); ShowState(); return; }
            Action done = delegate
            {
                if (!t.Result.Contains("GHS-SCPC.exe")) { ShowState(); return; }
                Log("Restarting the launcher ...");
                Process.Start(Exe);
                form.Close();
            };
            if (File.Exists(Runner) && t.Result.Any(NeedsBuild))
            {
                Busy("Updated. Rebuilding what changed (a few minutes; longer when the recompiler changed) ...");
                RunScript("setup.ps1", code =>
                {
                    if (code == 0) done();
                    else Offer("The rebuild did not finish (see the log).", "Build again", Build);
                });
            }
            else { Log("Updated."); done(); }
        }));
    }

    // The changed paths. Windows can rename the running exe but not overwrite it: when the update brings a new launcher,
    // this one moves aside (deleted on the next start) and git writes the new one in its place.
    static string[] PullUpdate()
    {
        string output;
        if (Git("status --porcelain --untracked-files=no", out output) != 0 || output != "")
            throw new InvalidOperationException("the port's files have local changes, update with git yourself:\n" + output);
        Git("diff --name-only HEAD origin/main", out output);
        string[] changed = output.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries).Select(p => p.Trim()).ToArray();
        bool self = changed.Contains("GHS-SCPC.exe");
        if (self)
        {
            File.Delete(OldExe);
            File.Move(Exe, OldExe);
            Git("checkout -- GHS-SCPC.exe", out output); // the current version back in place, so the merge sees a clean tree
        }
        if (Git("merge --ff-only -q origin/main", out output) != 0)
        {
            if (self) { File.Delete(Exe); File.Move(OldExe, Exe); }
            throw new InvalidOperationException(output);
        }
        return changed;
    }

    // What the built game depends on: the game fixes, the runtime patch, recompiler inputs and VU1 recipes, build scripts.
    static bool NeedsBuild(string path)
    {
        return path.StartsWith("overrides/") || path.StartsWith("patches/") || path.StartsWith("recomp/") || (path.StartsWith("tools/") && path.EndsWith(".ps1"));
    }

    // git in the port's folder: the exit code (-1: git is missing) and its output (on failure with the error text).
    static int Git(string arguments, out string output)
    {
        var psi = new ProcessStartInfo("git", arguments)
        {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Root,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        psi.EnvironmentVariables["GIT_TERMINAL_PROMPT"] = "0"; // never wait on a password prompt nobody sees
        try
        {
            using (var p = Process.Start(psi))
            {
                var error = p.StandardError.ReadToEndAsync();
                output = p.StandardOutput.ReadToEnd().Trim();
                p.WaitForExit();
                if (p.ExitCode != 0) output = (output + "\n" + error.Result).Trim();
                return p.ExitCode;
            }
        }
        catch (System.ComponentModel.Win32Exception) { output = ""; return -1; }
    }

    static string ScriptArgs(string script)
    {
        return "-NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(Root, "tools", script) + "\"";
    }

    // Optional Japanese voices: tools\ghs_jp_voice.py copies the Japanese disc's voice pool to game\NGYSTM_V.BIN and
    // writes game\ghs_jp_voice.txt (which English clip is which Japanese recording). Needs Python 3 with numpy.
    static void ChooseJapaneseIso()
    {
        var dlg = new OpenFileDialog { Title = "Japanese disc image (SLPM-65324): Japanese voices", Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*" };
        if (dlg.ShowDialog(form) != DialogResult.OK) return;
        string[] python = FindPython();
        if (python == null)
        {
            Log("Japanese voices need Python 3 with numpy. Install Python from https://www.python.org/downloads/ " +
                "(tick \"Add python.exe to PATH\"), then run in a command prompt:  py -m pip install numpy");
            status.Text = "Japanese voices: Python 3 with numpy is missing (see the log).";
            return;
        }
        button.Enabled = jpButton.Enabled = false;
        Busy("Adding the Japanese voices (a few minutes) ...");
        RunProcess(python[0], python[1] + "\"" + Path.Combine(Root, @"tools\ghs_jp_voice.py") + "\" \"" + dlg.FileName + "\"", code =>
        {
            Log(code == 0 ? "Japanese voices added: switch them on in game with F2 -> \"Japanese voices\"."
                          : "Adding the Japanese voices failed (see above).");
            ShowState();
        });
    }

    // {program, its first arguments} of a Python 3 that has numpy (the py launcher first, then python on the PATH).
    static string[] FindPython()
    {
        foreach (var candidate in new[] { new[] { "py", "-3 " }, new[] { "python", "" } })
        {
            try
            {
                var p = Process.Start(new ProcessStartInfo(candidate[0], candidate[1] + "-c \"import numpy\"")
                    { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true });
                if (p.WaitForExit(30000) && p.ExitCode == 0) return candidate;
            }
            catch (System.ComponentModel.Win32Exception) { } // not installed
        }
        return null;
    }

    static void RunScript(string script, Action<int> then) { RunProcess("powershell.exe", ScriptArgs(script), then); }

    static void RunProcess(string program, string arguments, Action<int> then)
    {
        var psi = new ProcessStartInfo(program, arguments)
        {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Root,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var eof = new CountdownEvent(2);
        DataReceivedEventHandler onLine = (s, e) => { if (e.Data != null) Log(e.Data); else eof.Signal(); };
        p.OutputDataReceived += onLine;
        p.ErrorDataReceived += onLine;
        p.Exited += delegate
        {
            // Bounded: build helpers that outlive the script (MSBuild nodes, mspdbsrv) can inherit the pipes and hold them open.
            eof.Wait(5000);
            form.BeginInvoke((Action)delegate { running = null; then(p.ExitCode); });
        };
        running = p;
        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
    }

    static void OnClosing(object sender, FormClosingEventArgs e)
    {
        var p = running;
        if (p == null || p.HasExited || e.CloseReason != CloseReason.UserClosing) return;
        if (MessageBox.Show(form, "Stop what is running (see the log)? Pressing the button again later continues it.", "GHS:SCPC", MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
        Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id) { UseShellExecute = false, CreateNoWindow = true }).WaitForExit();
    }

    static void Busy(string text)
    {
        status.Text = text;
        button.Enabled = false;
        Log(text);
    }

    static void Log(string line)
    {
        if (form.InvokeRequired) { form.BeginInvoke((Action)delegate { Log(line); }); return; }
        log.AppendText(line + Environment.NewLine);
    }

    public static GraphicsPath Round(Rectangle r, int radius)
    {
        int d = Math.Max(2, Math.Min(radius * 2, Math.Min(r.Width, r.Height)));
        var p = new GraphicsPath();
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static string Sha1(byte[] data)
    {
        using (var sha = SHA1.Create()) return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "");
    }
}

// The header: a night sky with a moon over the hotel's roofline, and the title in the spirit of the game's logo (blue
// GREGORY, dripping red HORROR, cracked yellow SHOW), each letter tilted a little like a cartoon.
class TitlePanel : Control
{
    static FontFamily[] families;

    public TitlePanel()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.UserPaint, true);
    }

    static FontFamily Pick(params string[] names)
    {
        if (families == null) families = FontFamily.Families;
        foreach (string n in names)
            foreach (FontFamily f in families)
                if (f.Name.Equals(n, StringComparison.OrdinalIgnoreCase)) return f;
        return FontFamily.GenericSansSerif;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        int w = Width, h = Height;
        using (var sky = new LinearGradientBrush(new Rectangle(0, 0, w, h), Color.FromArgb(18, 10, 28), Color.FromArgb(48, 10, 22), LinearGradientMode.Vertical))
            g.FillRectangle(sky, 0, 0, w, h);
        var rnd = new Random(7);
        for (int i = 0; i < 70; i++)
            using (var star = new SolidBrush(Color.FromArgb(rnd.Next(60, 200), 230, 220, 255)))
            {
                float s = rnd.Next(1, 3) * Launcher.Scale;
                g.FillEllipse(star, rnd.Next(w), rnd.Next(h * 2 / 3), s, s);
            }
        // Moon with a glow and a few craters.
        float mx = w - Launcher.S(120), my = Launcher.S(62), mr = Launcher.S(38);
        for (int i = 6; i >= 1; i--)
            using (var glow = new SolidBrush(Color.FromArgb(14, 255, 240, 190)))
                g.FillEllipse(glow, mx - mr - i * 7 * Launcher.Scale, my - mr - i * 7 * Launcher.Scale, 2 * (mr + i * 7 * Launcher.Scale), 2 * (mr + i * 7 * Launcher.Scale));
        using (var moon = new SolidBrush(Color.FromArgb(242, 232, 182))) g.FillEllipse(moon, mx - mr, my - mr, 2 * mr, 2 * mr);
        using (var crater = new SolidBrush(Color.FromArgb(222, 208, 150)))
        {
            g.FillEllipse(crater, mx - mr * 0.45f, my - mr * 0.35f, mr * 0.42f, mr * 0.36f);
            g.FillEllipse(crater, mx + mr * 0.15f, my + mr * 0.2f, mr * 0.3f, mr * 0.26f);
            g.FillEllipse(crater, mx - mr * 0.1f, my + mr * 0.45f, mr * 0.18f, mr * 0.16f);
        }
        // The hotel: a roofline with a tower, and a few lit windows.
        float roof = h - Launcher.S(42);
        var hotel = new PointF[]
        {
            new PointF(0, h), new PointF(0, roof + Launcher.S(14)), new PointF(w * 0.08f, roof + Launcher.S(14)), new PointF(w * 0.08f, roof),
            new PointF(w * 0.30f, roof), new PointF(w * 0.30f, roof - Launcher.S(22)), new PointF(w * 0.335f, roof - Launcher.S(44)),
            new PointF(w * 0.37f, roof - Launcher.S(22)), new PointF(w * 0.37f, roof), new PointF(w * 0.72f, roof), new PointF(w * 0.72f, roof + Launcher.S(8)),
            new PointF(w, roof + Launcher.S(8)), new PointF(w, h)
        };
        using (var black = new SolidBrush(Color.FromArgb(9, 5, 12))) g.FillPolygon(black, hotel);
        var lit = new Random(3);
        using (var window = new SolidBrush(Color.FromArgb(210, 232, 180, 80)))
            for (float x = w * 0.1f; x < w * 0.98f; x += Launcher.S(26))
                if (lit.Next(4) == 0 && (x < w * 0.28f || x > w * 0.39f))
                    g.FillRectangle(window, x, roof + Launcher.S(22), Launcher.S(8), Launcher.S(10));
        using (var window = new SolidBrush(Color.FromArgb(230, 255, 210, 110)))
            g.FillEllipse(window, w * 0.335f - Launcher.S(5), roof - Launcher.S(26), Launcher.S(10), Launcher.S(10)); // the tower's round window

        // "GHS:SCPC" on one line, bottoms aligned: G, H and S in the logo's three colours (H dripping, S cracked),
        // then a pale ghostly ":SCPC". (The full title isn't spelled out.)
        FontFamily heavy = Pick("Arial Black", "Impact"), tall = Pick("Impact", "Arial Black");
        var pieces = new[]
        {
            new { Text = "G", Family = heavy, Em = 80f, Light = Color.FromArgb(150, 210, 255), Dark = Color.FromArgb(30, 80, 220), Ink = Color.FromArgb(8, 10, 34), Drips = false, Cracks = false },
            new { Text = "H", Family = tall, Em = 96f, Light = Color.FromArgb(255, 90, 80), Dark = Color.FromArgb(150, 0, 18), Ink = Color.FromArgb(30, 0, 4), Drips = true, Cracks = false },
            new { Text = "S", Family = heavy, Em = 80f, Light = Color.FromArgb(255, 238, 120), Dark = Color.FromArgb(240, 160, 0), Ink = Color.FromArgb(40, 20, 0), Drips = false, Cracks = true },
            new { Text = ":", Family = heavy, Em = 64f, Light = Color.FromArgb(235, 228, 245), Dark = Color.FromArgb(150, 136, 190), Ink = Color.FromArgb(16, 10, 28), Drips = false, Cracks = false },
            new { Text = "SCPC", Family = tall, Em = 76f, Light = Color.FromArgb(240, 234, 250), Dark = Color.FromArgb(140, 124, 190), Ink = Color.FromArgb(16, 10, 28), Drips = false, Cracks = false },
        };
        float spacing = Launcher.S(10), total = -spacing, bottom = Launcher.S(124);
        var sizes = new List<SizeF>();
        foreach (var p in pieces)
        {
            SizeF size = Measure(p.Text, p.Family, Launcher.S(p.Em));
            sizes.Add(size);
            total += size.Width + spacing;
        }
        float left = w / 2f - total / 2;
        for (int i = 0; i < pieces.Length; i++)
        {
            var p = pieces[i];
            float top = p.Text == ":" ? bottom - sizes[i].Height - Launcher.S(6) : bottom - sizes[i].Height;
            Word(g, p.Text, p.Family, Launcher.S(p.Em), left + sizes[i].Width / 2, top, 5f, p.Light, p.Dark, p.Ink, p.Drips, p.Cracks);
            left += sizes[i].Width + spacing;
        }
        using (var sub = new Font("Segoe UI", 9.5f, FontStyle.Italic))
            TextRenderer.DrawText(g, "PC port launcher", sub, new Rectangle(0, Launcher.S(183), w, Launcher.S(22)),
                                  Color.FromArgb(185, 168, 215), TextFormatFlags.HorizontalCenter);
    }

    // The width Word lays a text out to, and its tallest letter.
    static SizeF Measure(string text, FontFamily family, float em)
    {
        float width = -em * 0.04f, height = 0;
        foreach (char c in text)
            using (var p = new GraphicsPath())
            {
                p.AddString(c.ToString(), family, (int)FontStyle.Regular, em, new PointF(0, 0), StringFormat.GenericTypographic);
                RectangleF b = p.GetBounds();
                width += b.Width + em * 0.04f;
                height = Math.Max(height, b.Height);
            }
        return new SizeF(width, height);
    }

    // One word: letters laid out centred on cx, tilted alternately, outlined in ink, filled with a vertical gradient;
    // drips hang from some letters (HORROR), cracks run over others (SHOW).
    static void Word(Graphics g, string text, FontFamily family, float em, float cx, float top, float tilt,
                     Color light, Color dark, Color ink, bool drips, bool cracks)
    {
        var letters = new List<GraphicsPath>();
        var widths = new List<float>();
        float gap = em * 0.04f, total = 0;
        foreach (char c in text)
        {
            var p = new GraphicsPath();
            p.AddString(c.ToString(), family, (int)FontStyle.Regular, em, new PointF(0, 0), StringFormat.GenericTypographic);
            RectangleF b = p.GetBounds();
            using (var m = new Matrix()) { m.Translate(-b.X, -b.Y); p.Transform(m); }
            letters.Add(p);
            widths.Add(b.Width);
            total += b.Width + gap;
        }
        total -= gap;
        float x = cx - total / 2;
        var rnd = new Random(text.Length * 31 + (int)em);
        RectangleF all = RectangleF.Empty;
        for (int i = 0; i < letters.Count; i++)
        {
            RectangleF b = letters[i].GetBounds();
            float angle = (i % 2 == 0 ? -tilt : tilt) + (float)(rnd.NextDouble() - 0.5) * 2f, lift = (float)(rnd.NextDouble() - 0.5) * em * 0.06f;
            using (var m = new Matrix())
            {
                m.Translate(x, top + lift);
                m.RotateAt(angle, new PointF(b.Width / 2, b.Height / 2));
                letters[i].Transform(m);
            }
            if (drips && i % 2 == 0)
            {
                RectangleF lb = letters[i].GetBounds();
                float dw = em * 0.075f, len = em * (0.16f + (float)rnd.NextDouble() * 0.22f), dx = lb.X + lb.Width * (0.3f + (float)rnd.NextDouble() * 0.4f);
                letters[i].AddRectangle(new RectangleF(dx - dw / 2, lb.Bottom - em * 0.08f, dw, len));
                letters[i].AddEllipse(dx - dw * 0.85f, lb.Bottom - em * 0.08f + len - dw * 0.9f, dw * 1.7f, dw * 1.7f);
                letters[i].FillMode = FillMode.Winding;
            }
            all = all.IsEmpty ? letters[i].GetBounds() : RectangleF.Union(all, letters[i].GetBounds());
            x += widths[i] + gap;
        }
        using (var outline = new Pen(ink, em * 0.11f) { LineJoin = LineJoin.Round })
        using (var shadow = new Pen(Color.FromArgb(120, 0, 0, 0), em * 0.11f) { LineJoin = LineJoin.Round })
        {
            foreach (GraphicsPath p in letters)
            {
                using (var m = new Matrix()) { m.Translate(em * 0.04f, em * 0.05f); var s = (GraphicsPath)p.Clone(); s.Transform(m); g.DrawPath(shadow, s); s.Dispose(); }
                g.DrawPath(outline, p);
            }
        }
        using (var fill = new LinearGradientBrush(new RectangleF(all.X, all.Y - 1, all.Width, all.Height + 2), light, dark, LinearGradientMode.Vertical))
            foreach (GraphicsPath p in letters)
                g.FillPath(fill, p);
        if (cracks)
            using (var crack = new Pen(Color.FromArgb(150, 60, 30, 0), Math.Max(1.5f, em * 0.035f)))
            {
                var rc = new Random(11);
                foreach (GraphicsPath p in letters)
                {
                    RectangleF lb = p.GetBounds();
                    g.SetClip(p);
                    var pts = new List<PointF>();
                    float yy = lb.Y + lb.Height * (0.25f + (float)rc.NextDouble() * 0.4f);
                    for (float xx = lb.X - 2; xx < lb.Right + 2; xx += lb.Width / 4)
                        pts.Add(new PointF(xx, yy + (float)(rc.NextDouble() - 0.5) * em * 0.18f));
                    if (pts.Count > 1) g.DrawLines(crack, pts.ToArray());
                    g.ResetClip();
                }
            }
        foreach (GraphicsPath p in letters) p.Dispose();
    }
}

enum StepState { Locked, Current, Done }

// One step of the walkthrough: a numbered card, lit red while it's the step to do, green once done.
class StepCard : Control
{
    readonly string number, title, body;
    StepState state;
    public StepState State { get { return state; } set { state = value; Invalidate(); } }

    public StepCard(string number, string title, string body)
    {
        this.number = number; this.title = title; this.body = body;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.UserPaint, true);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Launcher.Back);
        Color border = state == StepState.Done ? Color.FromArgb(70, 160, 100) : state == StepState.Current ? Launcher.Blood : Color.FromArgb(60, 52, 70);
        Color fill = state == StepState.Current ? Color.FromArgb(42, 16, 24) : Launcher.Card;
        using (var path = Launcher.Round(new Rectangle(1, 1, Width - 3, Height - 3), Launcher.S(12)))
        using (var brush = new SolidBrush(fill))
        using (var pen = new Pen(border, state == StepState.Current ? 2.5f * Launcher.Scale : 1.5f * Launcher.Scale))
        {
            g.FillPath(brush, path);
            g.DrawPath(pen, path);
        }
        int d = Launcher.S(32);
        var bubble = new Rectangle(Launcher.S(12), Launcher.S(12), d, d);
        using (var brush = new SolidBrush(state == StepState.Locked ? Color.FromArgb(48, 42, 56) : border)) g.FillEllipse(brush, bubble);
        using (var f = new Font("Segoe UI", 11f, FontStyle.Bold))
            TextRenderer.DrawText(g, state == StepState.Done ? "✔" : number, f, bubble, state == StepState.Locked ? Launcher.Dim : Color.White,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        int left = Launcher.S(54);
        using (var f = new Font("Segoe UI", 11f, FontStyle.Bold))
            TextRenderer.DrawText(g, title, f, new Rectangle(left, Launcher.S(12), Width - left - Launcher.S(10), d),
                                  state == StepState.Locked ? Launcher.Dim : Launcher.Ink, TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
        string tag = state == StepState.Done ? "DONE" : state == StepState.Current ? "NOW" : "";
        if (tag.Length > 0)
            using (var f = new Font("Segoe UI", 7.5f, FontStyle.Bold))
                TextRenderer.DrawText(g, tag, f, new Rectangle(0, Launcher.S(14), Width - Launcher.S(12), Launcher.S(16)), border, TextFormatFlags.Right);
        using (var f = new Font("Segoe UI", 8.75f))
            TextRenderer.DrawText(g, body, f, new Rectangle(left, Launcher.S(48), Width - left - Launcher.S(12), Height - Launcher.S(54)),
                                  state == StepState.Locked ? Color.FromArgb(110, 100, 122) : Color.FromArgb(196, 186, 206), TextFormatFlags.WordBreak | TextFormatFlags.Left);
    }
}

// The progress bar, filled with blood.
class BloodBar : Control
{
    int value;
    public int Maximum = 1000;
    public int Value { get { return value; } set { this.value = Math.Max(0, Math.Min(Maximum, value)); Invalidate(); } }

    public BloodBar()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.UserPaint, true);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Launcher.Back);
        var r = new Rectangle(0, 0, Width - 1, Height - 1);
        using (var path = Launcher.Round(r, Height / 2))
        using (var bg = new SolidBrush(Color.FromArgb(36, 26, 42)))
            g.FillPath(bg, path);
        int filled = (int)((long)(Width - 1) * value / Math.Max(1, Maximum));
        if (filled > Height)
            using (var path = Launcher.Round(new Rectangle(0, 0, filled, Height - 1), Height / 2))
            using (var blood = new LinearGradientBrush(r, Color.FromArgb(235, 60, 66), Color.FromArgb(110, 6, 18), LinearGradientMode.Vertical))
                g.FillPath(blood, path);
    }
}

// launcher.ini: key=value lines (music, track, graphics); Set rewrites the file keeping the other keys.
static class Settings
{
    static readonly string Ini = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "launcher.ini");

    static List<string> Lines()
    {
        try { return File.ReadAllLines(Ini).ToList(); }
        catch (IOException) { return new List<string>(); }
        catch (UnauthorizedAccessException) { return new List<string>(); }
    }

    public static string Get(string key, string fallback)
    {
        string line = Lines().FirstOrDefault(l => l.StartsWith(key + "="));
        return line == null ? fallback : line.Substring(key.Length + 1).Trim();
    }

    public static void Set(string key, string value)
    {
        var lines = Lines().Where(l => !l.StartsWith(key + "=")).ToList();
        lines.Add(key + "=" + value);
        try { File.WriteAllLines(Ini, lines); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
}

// The game's music behind the launcher, from the unpacked game files: NGYSTM_U.BIN holds raw 16-bit PCM streams (start
// sectors as u64 at EXE vaddr 0x342000, 6-byte format records at 0x340350; format 5 = stereo 48 kHz in 0x200-byte L/R
// blocks). 964-968 are the looping music tracks (loop flag set), 986-988 longer pieces. On/off and the track are kept in
// launcher.ini. The game itself is untouched: the launcher only reads the files.
static class Music
{
    static readonly int[] Tracks = { 964, 965, 966, 967, 968, 986, 987, 988 };
    static readonly string Root = AppDomain.CurrentDomain.BaseDirectory;
    static SoundPlayer player;
    static int track, generation;
    public static bool On = true;

    static string Elf { get { return Path.Combine(Root, @"game\SLES_519.33"); } }
    static string Pool { get { return Path.Combine(Root, @"game\NGYSTM_U.BIN"); } }

    static Music()
    {
        On = Settings.Get("music", "1") != "0";
        int.TryParse(Settings.Get("track", "0"), out track);
        track = Math.Max(0, Math.Min(Tracks.Length - 1, track));
    }

    public static bool Available() { return File.Exists(Elf) && File.Exists(Pool); }
    public static string Describe() { return "♪ Music: track " + (track + 1) + " of " + Tracks.Length + " (game stream " + Tracks[track] + ")"; }

    public static void Toggle()
    {
        On = !On;
        if (On) Start(); else Stop();
        Save();
    }

    public static void Next()
    {
        track = (track + 1) % Tracks.Length;
        Save();
        if (On) Start();
    }

    public static void Start()
    {
        int clip = Tracks[track], gen = ++generation;
        Task.Run(() => Wav(clip)).ContinueWith(t =>
        {
            if (t.Exception != null || t.Result == null || gen != generation || !On) return;
            Stop();
            player = new SoundPlayer(new MemoryStream(t.Result));
            player.PlayLooping();
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    static void Stop()
    {
        if (player == null) return;
        player.Stop();
        player.Dispose();
        player = null;
    }

    static void Save()
    {
        Settings.Set("music", On ? "1" : "0");
        Settings.Set("track", track.ToString());
    }

    static long Exe(long vaddr) { return vaddr - 0x100000 + 0x80; }

    // The stream as a WAV file in memory, at 55 % volume (it's background music).
    static byte[] Wav(int clip)
    {
        long start, end;
        using (var f = File.OpenRead(Elf))
        {
            var b = new byte[16];
            f.Position = Exe(0x340350) + 6L * clip;
            if (f.ReadByte() != 5) return null; // not stereo 48 kHz: leave it
            f.Position = Exe(0x342000) + 8L * clip;
            if (f.Read(b, 0, 16) != 16) return null;
            start = BitConverter.ToInt64(b, 0);
            end = BitConverter.ToInt64(b, 8);
        }
        int bytes = (int)((end - start) * 2048) / 0x400 * 0x400;
        if (bytes <= 0 || bytes > 64 << 20) return null;
        var raw = new byte[bytes];
        using (var f = File.OpenRead(Pool))
        {
            f.Position = start * 2048;
            for (int n = 0; n < bytes; )
            {
                int r = f.Read(raw, n, bytes - n);
                if (r <= 0) return null;
                n += r;
            }
        }
        var wav = new byte[44 + bytes];
        Action<int, string> tag = (at, s) => Encoding.ASCII.GetBytes(s, 0, 4, wav, at);
        Action<int, int> u32 = (at, v) => BitConverter.GetBytes(v).CopyTo(wav, at);
        tag(0, "RIFF"); u32(4, 36 + bytes); tag(8, "WAVE"); tag(12, "fmt ");
        u32(16, 16); BitConverter.GetBytes((short)1).CopyTo(wav, 20); BitConverter.GetBytes((short)2).CopyTo(wav, 22);
        u32(24, 48000); u32(28, 48000 * 4); BitConverter.GetBytes((short)4).CopyTo(wav, 32); BitConverter.GetBytes((short)16).CopyTo(wav, 34);
        tag(36, "data"); u32(40, bytes);
        int o = 44;
        for (int blk = 0; blk < bytes; blk += 0x400)
            for (int s = 0; s < 256; s++)
            {
                short l = (short)(BitConverter.ToInt16(raw, blk + 2 * s) * 0.55f), r = (short)(BitConverter.ToInt16(raw, blk + 0x200 + 2 * s) * 0.55f);
                wav[o++] = (byte)l; wav[o++] = (byte)(l >> 8); wav[o++] = (byte)r; wav[o++] = (byte)(r >> 8);
            }
        return wav;
    }
}

// Reads files straight from an ISO 9660 image (2048-byte sectors; PS2 DVDs carry ISO 9660 next to UDF). No mounting needed.
class Iso : IDisposable
{
    public class Entry { public string Name; public long Lba; public uint Size; public bool IsDir; }

    readonly FileStream fs;
    public readonly Entry Root;

    public Iso(string path)
    {
        fs = File.OpenRead(path);
        byte[] pvd = Read(16, 2048);
        if (pvd[0] != 1 || Encoding.ASCII.GetString(pvd, 1, 5) != "CD001")
            throw new InvalidDataException("Not a DVD image with an ISO 9660 file system. Use a plain .iso dump of the disc (not .bin/.cue, .chd or .cso).");
        Root = Parse(pvd, 156);
    }

    static Entry Parse(byte[] b, int p)
    {
        string name = Encoding.ASCII.GetString(b, p + 33, b[p + 32]);
        int version = name.IndexOf(';');
        if (version >= 0) name = name.Substring(0, version).TrimEnd('.');
        return new Entry { Name = name, Lba = BitConverter.ToUInt32(b, p + 2), Size = BitConverter.ToUInt32(b, p + 10), IsDir = (b[p + 25] & 2) != 0 };
    }

    // All files below dir, with their paths (backslash-separated) relative to the disc root.
    public void Collect(Entry dir, string prefix, List<KeyValuePair<string, Entry>> files)
    {
        byte[] data = Read(dir.Lba, (int)dir.Size);
        for (int p = 0; p < data.Length; )
        {
            int len = data[p];
            if (len == 0) { p = (p / 2048 + 1) * 2048; continue; } // records never cross a sector: rest of this one is padding
            Entry e = Parse(data, p);
            p += len;
            if (e.Name == "\0" || e.Name == "\u0001") continue; // "." and ".."
            if (e.IsDir) Collect(e, prefix + e.Name + "\\", files);
            else files.Add(new KeyValuePair<string, Entry>(prefix + e.Name, e));
        }
    }

    public byte[] ReadFile(Entry e) { return Read(e.Lba, (int)e.Size); }

    public void CopyFile(Entry e, Stream output, Action<long> progress)
    {
        fs.Position = e.Lba * 2048;
        var buffer = new byte[1 << 20];
        for (long left = e.Size; left > 0; )
        {
            int n = fs.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
            if (n <= 0) throw new EndOfStreamException("The disc image is cut short (incomplete download or copy?).");
            output.Write(buffer, 0, n);
            left -= n;
            progress(n);
        }
    }

    byte[] Read(long lba, int length)
    {
        fs.Position = lba * 2048;
        var b = new byte[length];
        for (int n = 0; n < length; )
        {
            int r = fs.Read(b, n, length - n);
            if (r <= 0) throw new EndOfStreamException("The disc image is cut short (incomplete download or copy?).");
            n += r;
        }
        return b;
    }

    public void Dispose() { fs.Dispose(); }
}
