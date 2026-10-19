// The launcher's window: Play (install, set up, update, play), Saves and Settings. Everything long (downloads, the
// setup) runs in the background with its progress shown; the setup's own messages are under "Show the details".
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Rac1Launcher
{
    sealed class MainForm : Form
    {
        enum State { Checking, NotInstalled, NeedsSetup, Ready }

        readonly Settings settings = Settings.Load();
        GameDir game;
        State state = State.Checking;
        GitHub.Release latest;
        string mainCommit;  // (the newest commit on main, for the development channel)
        Runner runner;  // (the task running now, or null)
        CancellationTokenSource cancel;
        Process gameProc;
        readonly ConcurrentQueue<string> pending = new ConcurrentQueue<string>();
        readonly StringBuilder fullLog = new StringBuilder();
        StreamWriter logFile;
        readonly List<string> errors = new List<string>();
        bool sawPythonCrash, sawStepMarks;
        static readonly Version LauncherVersion = Assembly.GetExecutingAssembly().GetName().Version;

        // Play
        readonly Label lblStatus = new Label(), lblDetail = new Label(), lblStep = new Label(), lblUpdate = new Label();
        readonly Button btnMain = new Button(), btnCancel = new Button(), btnUpdate = new Button();
        readonly Panel updatePanel = new Panel();
        readonly ProgressBar bar = new ProgressBar();
        readonly CheckBox chkLog = new CheckBox();
        readonly TextBox txtLog = new TextBox();
        // Saves
        readonly ListView lstSaves = new ListView();
        readonly Label lblCard = new Label();
        // Settings
        readonly TextBox txtDir = new TextBox(), txtIso = new TextBox();
        readonly ComboBox cmbJobs = new ComboBox(), cmbChannel = new ComboBox();
        readonly TabControl tabs = new TabControl();
        readonly List<Control> busyDisabled = new List<Control>();

        static readonly Color Accent = Color.FromArgb(232, 120, 24);
        static readonly Color HeaderBack = Color.FromArgb(28, 36, 64);

        public MainForm()
        {
            Text = "Ratchet & Clank - rac1-native";
            AutoScaleMode = AutoScaleMode.None;  // (scaled by hand: Sz, Pd, Pt, S)
            using (var g = CreateGraphics()) k = g.DpiX / 96f;  // (DeviceDpi is 96 on .NET Framework without an .exe.config)
            Font = new Font("Segoe UI", 9.75f);
            ClientSize = Sz(820, 600);
            MinimumSize = Sz(640, 520);
            StartPosition = FormStartPosition.CenterScreen;
            try
            {
                using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("launcher.ico"))
                    if (s != null) Icon = new Icon(s);
            }
            catch (Exception) { }

            game = new GameDir(ChooseGameDir());
            BuildUi();
            NoMnemonics(this);


            var flush = new System.Windows.Forms.Timer { Interval = 120 };
            flush.Tick += (s, e) => FlushLog();
            flush.Start();

            Shown += (s, e) => { RefreshState(); CheckUpdates(false); };
            FormClosing += OnClosing;
        }

        /// <summary>The game's folder: the one the launcher is in when it is a game folder (the launcher copied beside
        /// the game, or put in a downloaded zip's folder), else the one in the settings, else the default.</summary>
        string ChooseGameDir()
        {
            string own = Path.GetDirectoryName(Application.ExecutablePath);
            if (new GameDir(own).Downloaded) return own;
            if (!string.IsNullOrEmpty(settings.GameDir)) return settings.GameDir;
            return GameDir.DefaultDir();
        }

        // --- The window -------------------------------------------------------------------------------------

        void BuildUi()
        {
            var header = new Panel { Dock = DockStyle.Top, Height = S(78), BackColor = HeaderBack, Padding = Pd(18, 10, 18, 8) };
            var title = new Label
            {
                Text = "Ratchet & Clank", ForeColor = Color.White, AutoSize = true, Location = Pt(16, 10),
                Font = new Font("Segoe UI Semibold", 20f),
            };
            var sub = new Label
            {
                Text = "rac1-native, the PC port   ·   launcher " + VersionText(LauncherVersion), ForeColor = Color.FromArgb(190, 200, 225),
                AutoSize = true, Location = Pt(19, 50),
            };
            header.Controls.Add(title);
            header.Controls.Add(sub);

            tabs.Dock = DockStyle.Fill;
            tabs.Padding = Pt(14, 5);
            tabs.TabPages.Add(PlayPage());
            tabs.TabPages.Add(SavesPage());
            tabs.TabPages.Add(SettingsPage());
            tabs.SelectedIndexChanged += (s, e) => { if (tabs.SelectedIndex == 1) RefreshSaves(); };
            Controls.Add(tabs);
            Controls.Add(header);
        }

        TabPage PlayPage()
        {
            var page = new TabPage("Play") { Padding = Pd(18), UseVisualStyleBackColor = true };
            var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 8 };
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            for (int i = 0; i < 7; i++) t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            t.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            lblStatus.AutoSize = true;
            lblStatus.Font = new Font("Segoe UI Semibold", 15f);
            lblStatus.Margin = Pd(0, 0, 0, 6);
            lblStatus.Text = "Looking at the game folder...";
            lblDetail.AutoSize = true;
            lblDetail.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            lblDetail.Margin = Pd(0, 0, 0, 12);

            updatePanel.BackColor = Color.FromArgb(255, 244, 214);
            updatePanel.Dock = DockStyle.Top;
            updatePanel.Height = S(46);
            updatePanel.Padding = Pd(10, 8, 10, 8);
            updatePanel.Margin = Pd(0, 0, 0, 12);
            updatePanel.Visible = false;
            lblUpdate.AutoSize = false;
            lblUpdate.Dock = DockStyle.Fill;
            lblUpdate.TextAlign = ContentAlignment.MiddleLeft;
            btnUpdate.Text = "Update";
            btnUpdate.Dock = DockStyle.Right;
            btnUpdate.Width = S(110);
            btnUpdate.Click += (s, e) => DoUpdate();
            updatePanel.Controls.Add(lblUpdate);
            updatePanel.Controls.Add(btnUpdate);

            var buttons = new FlowLayoutPanel { AutoSize = true, Margin = Pd(0, 0, 0, 10), WrapContents = false };
            btnMain.Size = Sz(230, 56);
            btnMain.Font = new Font("Segoe UI Semibold", 13f);
            btnMain.BackColor = Accent;
            btnMain.ForeColor = Color.White;
            btnMain.FlatStyle = FlatStyle.Flat;
            btnMain.FlatAppearance.BorderSize = 0;
            btnMain.Enabled = false;
            btnMain.Text = "...";
            btnMain.Click += (s, e) => MainClick();
            btnCancel.Text = "Cancel";
            btnCancel.Size = Sz(110, 56);
            btnCancel.Margin = Pd(12, 3, 3, 3);
            btnCancel.Visible = false;
            btnCancel.Click += (s, e) => CancelTask();
            buttons.Controls.Add(btnMain);
            buttons.Controls.Add(btnCancel);

            lblStep.AutoSize = true;
            lblStep.Margin = Pd(0, 4, 0, 4);
            bar.Dock = DockStyle.Top;
            bar.Height = S(18);
            bar.Visible = false;
            bar.Margin = Pd(0, 0, 0, 10);

            chkLog.Text = "Show the details";
            chkLog.AutoSize = true;
            chkLog.CheckedChanged += (s, e) => txtLog.Visible = chkLog.Checked;
            txtLog.Multiline = true;
            txtLog.ReadOnly = true;
            txtLog.ScrollBars = ScrollBars.Both;
            txtLog.WordWrap = false;
            txtLog.Font = new Font("Consolas", 9f);
            txtLog.Dock = DockStyle.Fill;
            txtLog.BackColor = Color.FromArgb(24, 24, 28);
            txtLog.ForeColor = Color.FromArgb(220, 220, 220);
            txtLog.Visible = false;
            txtLog.MaxLength = 0;

            t.Controls.Add(lblStatus, 0, 0);
            t.Controls.Add(lblDetail, 0, 1);
            t.Controls.Add(updatePanel, 0, 2);
            t.Controls.Add(buttons, 0, 3);
            t.Controls.Add(lblStep, 0, 4);
            t.Controls.Add(bar, 0, 5);
            t.Controls.Add(chkLog, 0, 6);
            t.Controls.Add(txtLog, 0, 7);
            page.Controls.Add(t);
            return page;
        }

        TabPage SavesPage()
        {
            var page = new TabPage("Saves") { Padding = Pd(18), UseVisualStyleBackColor = true };
            var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3 };
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            t.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            lblCard.AutoSize = true;
            lblCard.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            lblCard.Margin = Pd(0, 0, 0, 8);
            lstSaves.View = View.Details;
            lstSaves.FullRowSelect = true;
            lstSaves.MultiSelect = false;
            lstSaves.HideSelection = false;
            lstSaves.Dock = DockStyle.Fill;
            lstSaves.Columns.Add("Disc", S(240));
            lstSaves.Columns.Add("Slot", S(60));
            lstSaves.Columns.Add("Last saved", S(200));
            lstSaves.Columns.Add("Size", S(90), HorizontalAlignment.Right);
            var row = new FlowLayoutPanel { AutoSize = true, Margin = Pd(0, 10, 0, 0) };
            row.Controls.Add(Btn("Refresh", (s, e) => RefreshSaves()));
            row.Controls.Add(Btn("Open the folder", (s, e) => { Directory.CreateDirectory(Saves.CardDir); OpenFolder(Saves.CardDir); }));
            row.Controls.Add(Btn("Back up all...", (s, e) => BackupSaves()));
            row.Controls.Add(Btn("Restore a backup...", (s, e) => RestoreSaves()));
            row.Controls.Add(Btn("Delete the slot", (s, e) => DeleteSave()));
            t.Controls.Add(lblCard, 0, 0);
            t.Controls.Add(lstSaves, 0, 1);
            t.Controls.Add(row, 0, 2);
            page.Controls.Add(t);
            return page;
        }

        TabPage SettingsPage()
        {
            var page = new TabPage("Settings") { Padding = Pd(18), UseVisualStyleBackColor = true, AutoScroll = true };
            var t = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3 };
            t.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            t.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            txtDir.ReadOnly = true;
            txtDir.Dock = DockStyle.Fill;
            txtDir.Text = game.Dir;
            var dirButtons = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
            dirButtons.Controls.Add(Btn("Change...", (s, e) => ChangeGameDir(), true));
            dirButtons.Controls.Add(Btn("Open", (s, e) => { Directory.CreateDirectory(game.Dir); OpenFolder(game.Dir); }));
            AddRow(t, "Game folder", txtDir, dirButtons);

            txtIso.ReadOnly = true;
            txtIso.Dock = DockStyle.Fill;
            txtIso.Text = settings.Iso;
            AddRow(t, "Disc image", txtIso, Btn("Change...", (s, e) => { if (AskIso(true) != null) txtIso.Text = settings.Iso; }, true));

            cmbChannel.DropDownStyle = ComboBoxStyle.DropDownList;
            cmbChannel.Items.AddRange(new object[] { "Stable: the releases (recommended)", "Development: the newest changes on main" });
            cmbChannel.SelectedIndex = Channel.Development ? 1 : 0;
            cmbChannel.SelectedIndexChanged += (s, e) => ChangeChannel(cmbChannel.SelectedIndex == 1);
            AddRow(t, "Update channel", cmbChannel, new Label
            {
                Text = "(the same setting as the game's Options > Advanced > Update Channel)", AutoSize = true, Anchor = AnchorStyles.Left,
                ForeColor = SystemColors.GrayText,
            });

            cmbJobs.DropDownStyle = ComboBoxStyle.DropDownList;
            cmbJobs.Items.AddRange(new object[] { "Automatic", "1 (slowest, the safest)", "2", "4", "8", "16" });
            var jobValues = new[] { 0, 1, 2, 4, 8, 16 };
            cmbJobs.SelectedIndex = Math.Max(0, Array.IndexOf(jobValues, settings.Jobs));
            cmbJobs.Width = S(200);
            cmbJobs.SelectedIndexChanged += (s, e) => { settings.Jobs = jobValues[cmbJobs.SelectedIndex]; settings.Save(); };
            AddRow(t, "Parallel jobs", cmbJobs, new Label
            {
                Text = "(for the setup: fewer is slower, but uses less memory)", AutoSize = true, Anchor = AnchorStyles.Left,
                ForeColor = SystemColors.GrayText,
            });

            var actions = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, Margin = Pd(0, 18, 0, 0) };
            actions.Controls.Add(Btn("Check for updates", (s, e) => CheckUpdates(true), true));
            actions.Controls.Add(Btn("Update now", (s, e) => DoUpdate(), true));
            actions.Controls.Add(Btn("Set up again from scratch", (s, e) => DoRepair(), true));
            actions.Controls.Add(Btn("Create the shortcuts", (s, e) => { CreateShortcuts(true); }, true));
            actions.Controls.Add(Btn("Open the setup's logs", (s, e) => { Directory.CreateDirectory(game.P("build")); OpenFolder(game.P("build")); }));
            actions.Controls.Add(Btn("Report a problem", (s, e) => OpenUrl(GitHub.Page + "/issues")));

            var note = new Label
            {
                AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = Pd(0, 18, 0, 0), ForeColor = SystemColors.GrayText,
                Text = "The game's own options (graphics, controls) are in the game's Options menu. "
                     + "Your settings and saves are in " + Settings.ConfigDir + ", not in the game folder: "
                     + "setting the game up again never touches them.",
            };
            var outer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1 };
            outer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.Controls.Add(t, 0, 0);
            outer.Controls.Add(actions, 0, 1);
            outer.Controls.Add(note, 0, 2);
            page.Controls.Add(outer);
            return page;
        }

        void AddRow(TableLayoutPanel t, string label, Control field, Control right)
        {
            int r = t.RowCount++;
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            t.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = Pd(0, 8, 12, 8) }, 0, r);
            field.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            field.Margin = Pd(0, 6, 8, 6);
            t.Controls.Add(field, 1, r);
            t.Controls.Add(right, 2, r);
        }

        Button Btn(string text, EventHandler click, bool disabledWhileBusy = false)
        {
            var b = new Button { Text = text, AutoSize = true, Padding = Pd(8, 3, 8, 3), Margin = Pd(0, 3, 8, 3) };
            b.Click += click;
            if (disabledWhileBusy) busyDisabled.Add(b);
            return b;
        }

        /// <summary>"&" shown as itself in every label ("Ratchet & Clank"), not as a keyboard shortcut's mark.</summary>
        static void NoMnemonics(Control c)
        {
            if (c is Label l) l.UseMnemonic = false;
            foreach (Control child in c.Controls) NoMnemonics(child);
        }

        // Sizes in pixels at 96 DPI made the screen's (the manifest makes the launcher DPI-aware; the fonts, in points,
        // already follow the screen)
        readonly float k = 1f;
        int S(int px) => (int)Math.Round(px * k);
        Size Sz(int w, int h) => new Size(S(w), S(h));
        Padding Pd(int l, int t, int r, int b) => new Padding(S(l), S(t), S(r), S(b));
        Padding Pd(int all) => new Padding(S(all));
        Point Pt(int x, int y) => new Point(S(x), S(y));

        static string VersionText(Version v) => v == null ? "?" : $"{v.Major}.{v.Minor}.{v.Build}";

        // --- The state --------------------------------------------------------------------------------------

        /// <summary>What the game folder holds, and what the Play button does about it.</summary>
        void RefreshState()
        {
            if (runner != null) return;
            var g = game;
            Task.Run(() =>
            {
                State st;
                string due = "";
                if (!g.Downloaded) st = State.NotInstalled;
                else if (!g.Built) st = State.NeedsSetup;
                else if (File.Exists(g.Python))
                {
                    // (tools\setup.py --check, as play.cmd: a new version unpacked over this one, or a setup that did
                    // not finish; exit status 10 when the setup has something to do)
                    var lines = new List<string>();
                    int rc;
                    using (var r = new Runner())
                        rc = r.Run(g.Python, Quote(g.P("tools", "setup.py")) + " --check", g.Dir, PythonEnv(g), l => lines.Add(l), Encoding.UTF8);
                    st = rc == 10 ? State.NeedsSetup : State.Ready;
                    due = string.Join("\n", lines.Take(4));
                }
                else st = State.Ready;  // (set up from an MSYS2 shell of the player's own: the game just starts)
                BeginInvoke((Action)(() => { if (g == game) ShowState(st, due); }));
            });
        }

        void ShowState(State st, string due)
        {
            state = st;
            cmbChannel.SelectedIndex = Channel.Development ? 1 : 0;  // (the game may have changed it)
            btnMain.Enabled = runner == null;
            var v = game.SourceVersion;
            switch (st)
            {
                case State.NotInstalled:
                    lblStatus.Text = "Not installed yet";
                    lblDetail.Text = "Install downloads the game's latest version from GitHub and builds it on this PC from your own "
                                   + "Ratchet & Clank disc image: the North American disc (SCUS-97199) or the European one (SCES-50916), "
                                   + "as a plain .iso file.\n\nThe first time takes a while (often 20 to 40 minutes) and needs an internet "
                                   + "connection and about 10 GB of free space. It goes into:\n" + game.Dir;
                    btnMain.Text = "Install";
                    break;
                case State.NeedsSetup:
                    lblStatus.Text = "The setup needs to run";
                    lblDetail.Text = (game.Built ? "A new version was downloaded, or the last setup did not finish." : "The game is downloaded but not built yet.")
                                   + (string.IsNullOrWhiteSpace(due) ? "" : "\n" + due)
                                   + "\nThe setup builds only what changed.";
                    btnMain.Text = "Set up";
                    break;
                case State.Ready:
                    lblStatus.Text = "Ready to play";
                    string commit = game.InstalledCommit;
                    lblDetail.Text = "Version " + VersionText(v)
                                   + (commit != null ? $" (development: main at {commit.Substring(0, Math.Min(7, commit.Length))})" : "")
                                   + (game.IsGitClone ? " (a git clone)" : "") + ", in " + game.Dir;
                    btnMain.Text = "Play";
                    break;
            }
            ShowUpdate();
        }

        void ShowUpdate()
        {
            if (runner != null) return;
            string msg = UpdateMessage();
            updatePanel.Visible = msg != null && game.Downloaded;
            if (msg != null) lblUpdate.Text = msg + " The update keeps your saves and settings.";
        }

        /// <summary>What is newer on the chosen channel than what is installed, or null (nothing newer, or not known).
        /// Stable: a release newer than the sources' version. Development: main, when it moved since the commit the last
        /// download unpacked (a stable install on the development channel: main is always newer).</summary>
        string UpdateMessage()
        {
            var v = game.SourceVersion;
            if (Channel.Development)
            {
                if (mainCommit == null || game.IsGitClone && game.InstalledCommit == null) return null;
                string have = game.InstalledCommit;
                if (have == mainCommit) return null;
                return have == null ? $"You are on the development channel: main ({mainCommit.Substring(0, 7)}) has the newest changes."
                                    : $"There are new changes on main ({mainCommit.Substring(0, 7)}; you have {have.Substring(0, Math.Min(7, have.Length))}).";
            }
            if (latest?.Version != null && v != null && latest.Version > v)
                return $"Version {VersionText(latest.Version)} is out (you have {VersionText(v)}).";
            return null;
        }

        void ChangeChannel(bool development)
        {
            if (development == Channel.Development) return;
            if (development && MessageBox.Show(this, "The development channel gets the newest changes on main, before they are released: "
                                                   + "new fixes sooner, but also new bugs. The game's own Update Channel option changes too.\n\n"
                                                   + "Switch to the development channel?", "Update channel", MessageBoxButtons.YesNo,
                                               MessageBoxIcon.Question) != DialogResult.Yes)
            {
                cmbChannel.SelectedIndex = 0;
                return;
            }
            try { Channel.Development = development; }
            catch (Exception e)
            {
                MessageBox.Show(this, "The setting could not be saved: " + e.Message, "Update channel");
                cmbChannel.SelectedIndex = Channel.Development ? 1 : 0;
                return;
            }
            mainCommit = null;
            ShowUpdate();
            CheckUpdates(false);
        }

        void CheckUpdates(bool userAsked)
        {
            Task.Run(() =>
            {
                try
                {
                    var r = GitHub.Latest();
                    string main = Channel.Development ? GitHub.MainCommit() : null;
                    string fresh = DownloadNewLauncher(r);
                    BeginInvoke((Action)(() =>
                    {
                        latest = r;
                        if (main != null) mainCommit = main;
                        ShowUpdate();
                        if (fresh != null && SwapLauncher(fresh)) return;
                        if (userAsked)
                        {
                            var v = game.SourceVersion;
                            string newer = UpdateMessage();
                            string msg = !game.Downloaded ? (main != null ? $"Install gets main ({main.Substring(0, 7)})."
                                                                          : $"The latest version is {r.Tag}: Install gets it.")
                                : newer != null ? newer + " Use Update on the Play page."
                                : main != null && game.IsGitClone && game.InstalledCommit == null
                                    ? $"This folder is a git clone: Update brings it to main's newest changes ({main.Substring(0, 7)})."
                                : main != null ? $"You have the newest changes on main ({main.Substring(0, 7)})."
                                : $"You have the latest version ({VersionText(v)}).";
                            MessageBox.Show(this, msg, "Updates", MessageBoxButtons.OK, MessageBoxIcon.Information);
                        }
                    }));
                }
                catch (Exception e)
                {
                    if (userAsked)
                        BeginInvoke((Action)(() => MessageBox.Show(this, "GitHub could not be reached (" + e.Message + ").\nCheck the internet connection.",
                                                                   "Updates", MessageBoxButtons.OK, MessageBoxIcon.Warning)));
                }
            });
        }

        /// <summary>The latest release's rac1-launcher.exe when it is newer than this launcher, downloaded beside it
        /// (in the background); null when there is none.</summary>
        static string DownloadNewLauncher(GitHub.Release rel)
        {
            if (rel.LauncherUrl == null || rel.Version == null) return null;
            if (LauncherVersion.Major == 0 && LauncherVersion.Minor == 0 && LauncherVersion.Build == 0) return null;  // (a build of one's own)
            if (rel.Version <= new Version(LauncherVersion.Major, LauncherVersion.Minor, LauncherVersion.Build)) return null;
            string fresh = Application.ExecutablePath + ".new";
            try
            {
                GitHub.Download(rel.LauncherUrl, fresh, (a, b) => { }, CancellationToken.None);
                if (new FileInfo(fresh).Length < 32 * 1024) throw new InvalidDataException("the download is too small");
                return fresh;
            }
            catch (Exception)
            {
                try { File.Delete(fresh); } catch (Exception) { }
                return null;
            }
        }

        /// <summary>The new launcher replaces this one, which then starts it. True when it did (the window closes).</summary>
        bool SwapLauncher(string fresh)
        {
            string exe = Application.ExecutablePath;
            if (runner != null || (gameProc != null && !gameProc.HasExited)) return false;  // (next time)
            try
            {
                File.Delete(exe + ".old");
                File.Move(exe, exe + ".old");  // (a running program can be renamed, not replaced)
                File.Move(fresh, exe);
            }
            catch (Exception)
            {
                try { File.Delete(fresh); } catch (Exception) { }
                return false;
            }
            Process.Start(new ProcessStartInfo(exe) { UseShellExecute = false });
            Close();
            return true;
        }

        // --- The Play button --------------------------------------------------------------------------------

        void MainClick()
        {
            switch (state)
            {
                case State.NotInstalled: DoInstall(); break;
                case State.NeedsSetup: DoSetup(""); break;
                case State.Ready: Play(); break;
            }
        }

        /// <summary>The disc image: the one already chosen (or the one the game uses), else asks for it.</summary>
        string AskIso(bool always = false)
        {
            if (!always && File.Exists(settings.Iso)) return settings.Iso;
            if (!always)
            {
                string known = game.KnownIso();
                if (known != null) return known;
            }
            using (var d = new OpenFileDialog
            {
                Title = "Choose your Ratchet & Clank disc image (.iso)",
                Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*",
                CheckFileExists = true,
            })
            {
                if (File.Exists(settings.Iso)) d.InitialDirectory = Path.GetDirectoryName(settings.Iso);
                if (d.ShowDialog(this) != DialogResult.OK) return null;
                string ext = Path.GetExtension(d.FileName).ToLowerInvariant();
                if (new[] { ".bin", ".cue", ".chd", ".cso", ".zso", ".gz", ".7z", ".zip", ".rar", ".mdf", ".nrg" }.Contains(ext))
                {
                    MessageBox.Show(this, $"This is a {ext} file. The setup needs a plain .iso dump of the disc "
                                        + "(a compressed image or a .bin/.cue pair does not work): convert it to .iso first.",
                                    "Disc image", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return null;
                }
                if (new FileInfo(d.FileName).Length < 100L * 1024 * 1024)
                {
                    MessageBox.Show(this, "This file is too small to be the game's disc image.", "Disc image",
                                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return null;
                }
                settings.Iso = d.FileName;
                settings.Save();
                return d.FileName;
            }
        }

        void DoInstall()
        {
            string iso = AskIso();
            if (iso == null) return;
            try
            {
                Directory.CreateDirectory(game.Dir);
                var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(game.Dir)));
                if (drive.AvailableFreeSpace < 10L << 30 &&
                    MessageBox.Show(this, $"The drive {drive.Name} has {drive.AvailableFreeSpace >> 30} GB free; the game needs about 10 GB.\n"
                                        + "Install anyway? (Settings > Game folder changes where it goes.)", "Free space",
                                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    return;
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The game folder cannot be made here (" + e.Message + "). Choose another one in Settings.",
                                "Install", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            settings.GameDir = game.Dir;
            settings.Save();
            RunTask("Installing", r =>
            {
                if (!Download(r)) return false;
                BeginInvoke((Action)(() => CreateShortcuts(false)));
                return Setup(r, iso, "");
            });
        }

        /// <summary>The source zip of the chosen channel (the latest release, or main), unpacked into the game folder.</summary>
        bool Download(Runner r)
        {
            Step("Finding the latest version");
            string label, url;
            string[] version;
            try
            {
                if (Channel.Development)
                {
                    string sha = GitHub.MainCommit();
                    mainCommit = sha;
                    label = "main " + sha.Substring(0, 7);
                    url = $"{GitHub.Page}/archive/{sha}.zip";  // (that commit, even if main moves meanwhile)
                    version = new[] { "describe " + sha.Substring(0, 7), "commit " + sha, "branch main" };
                }
                else
                {
                    var rel = latest ?? GitHub.Latest();
                    latest = rel;
                    label = rel.Tag;
                    url = $"{GitHub.Page}/archive/refs/tags/{rel.Tag}.zip";
                    version = new[] { "describe " + rel.Tag, "commit ", "branch " };
                }
            }
            catch (Exception e) { Fail("GitHub could not be reached (" + e.Message + "). Check the internet connection, then try again."); return false; }
            string zip = Path.Combine(Path.GetTempPath(), $"rac1-native-{label.Replace(' ', '-')}.zip");
            Step($"Downloading the game ({label})");
            Log($"Downloading {url}");
            try
            {
                GitHub.Download(url, zip, (done, total) =>
                {
                    if (total > 0) Progress((int)(done * 100 / total));
                    StepDetail($"Downloading the game ({label}): {done >> 20} MB");
                }, cancel.Token);
                Step("Unpacking");
                Install.Unpack(zip, game.Dir, version, Log);
            }
            catch (OperationCanceledException) { return false; }
            catch (Exception e) { Fail("The download failed (" + e.Message + "). Check the internet connection, then try again."); return false; }
            finally { try { File.Delete(zip); } catch (Exception) { } }
            return true;
        }

        void DoSetup(string extra)
        {
            string iso = AskIso();
            if (iso == null) return;
            RunTask("Setting up", r => Setup(r, iso, extra));
        }

        void DoRepair()
        {
            if (!game.Downloaded) { DoInstall(); return; }
            if (MessageBox.Show(this, "This generates the game's code and data again from your disc image, and rebuilds the game. "
                                    + "It takes as long as the first setup. Your saves and settings are kept.\n\nSet up again?",
                                "Set up again", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            tabs.SelectedIndex = 0;
            DoSetup("--force");
        }

        void DoUpdate()
        {
            if (!game.Downloaded) { DoInstall(); return; }
            if (game.IsGitClone == false && !File.Exists(game.Python))
            {
                // (never set up: the new version's zip over this one, then the setup)
                string iso0 = AskIso();
                if (iso0 == null) return;
                tabs.SelectedIndex = 0;
                latest = null;
                RunTask("Updating", r => Download(r) && Setup(r, iso0, ""));
                return;
            }
            string iso = AskIso();
            if (iso == null) return;
            tabs.SelectedIndex = 0;
            RunTask("Updating", r => Setup(r, iso, "--update --channel " + Channel.Name));
        }

        /// <summary>What setup.cmd does: the tools (tools\windows\toolchain.ps1), then tools\setup.py.</summary>
        bool Setup(Runner r, string iso, string extra)
        {
            var g = game;
            Step("Preparing the tools that build the game");
            StepDetail("Preparing the tools that build the game (the first time: about 1 GB to download, several minutes)");
            string ps = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
            int rc = r.Run(ps, "-NoProfile -ExecutionPolicy Bypass -File " + Quote(g.ToolchainScript), g.Dir,
                           new Dictionary<string, string> { ["MSYSTEM"] = null }, line =>
                           {
                               Log(line);
                               if (line.TrimStart().StartsWith("ERROR")) errors.Add(line.Trim());
                               var m = Regex.Match(line, @"^  ([A-Z][^:]{3,80})$");
                               if (m.Success && !line.Contains("ERROR")) StepDetail("Preparing the tools: " + m.Groups[1].Value.TrimEnd('.'));
                           });
            if (r.Cancelled) return false;
            if (rc != 0) { Fail(LastError() ?? "Preparing the tools failed."); return false; }
            RepairCertificates(r, g);
            Step(extra.StartsWith("--update") ? "Downloading the update" : "Starting the setup");
            sawStepMarks = false;

            int jobs = settings.Jobs;
            for (int attempt = 0; ; attempt++)
            {
                sawPythonCrash = false;
                errors.Clear();
                string args = Quote(g.P("tools", "setup.py")) + " --no-play --iso " + Quote(iso)
                            + (jobs > 0 ? " -j " + jobs : "") + (extra != "" ? " " + extra : "");
                Log("> python " + args);
                rc = r.Run(g.Python, args, g.Dir, PythonEnv(g), OnSetupLine, Encoding.UTF8);
                if (r.Cancelled) return false;
                if (rc == 0) break;
                // (Python itself crashed in a parallel job, GitHub issue #72: once more with one job at a time)
                if (attempt == 0 && sawPythonCrash && jobs != 1 && !extra.StartsWith("--update"))
                {
                    Log("Python crashed while generating the code: trying again with one job at a time.");
                    jobs = 1;
                    continue;
                }
                Fail(LastError() ?? "The setup failed.");
                return false;
            }
            Step("Done");
            Progress(100);
            Log("The game is ready.");
            return true;
        }

        /// <summary>The certificates the build downloads with, for a release whose toolchain.ps1 does not make them
        /// yet in a folder with a space in its path (GitHub issue #79; tools\windows\toolchain.ps1 does it since 0.4.1).</summary>
        void RepairCertificates(Runner r, GameDir g)
        {
            string pem = g.P(".toolchain", "msys64", "clang64", "etc", "ssl", "cert.pem");
            if (File.Exists(pem) && new FileInfo(pem).Length >= 1024) return;
            Log("Making the certificates for the downloads");
            string own = g.P(".toolchain", "msys64", "usr", "ssl", "certs", "ca-bundle.crt");
            if (!File.Exists(own)) return;
            foreach (var dst in new[] { pem, g.P(".toolchain", "msys64", "clang64", "etc", "ssl", "certs", "ca-bundle.crt") })
            {
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.Copy(own, dst, true);
            }
        }

        void OnSetupLine(string line)
        {
            if (line.StartsWith("@step "))
            {
                var m = Regex.Match(line, @"^@step (\d+) (\d+) (.*)$");
                if (m.Success) Step($"Step {m.Groups[1].Value} of {m.Groups[2].Value}: {m.Groups[3].Value}");
                sawStepMarks = true;
                return;
            }
            if (line.StartsWith("@progress "))
            {
                if (int.TryParse(line.Substring(10), out int p)) Progress(p);
                return;
            }
            if (!sawStepMarks)
            {
                // (a setup.py from before 0.4.1 prints no @step lines: its "[n/total] step" headings instead)
                var m = Regex.Match(line, @"^\[(\d+)/(\d+)\] (.+)$");
                if (m.Success) Step($"Step {m.Groups[1].Value} of {m.Groups[2].Value}: {m.Groups[3].Value}");
            }
            if (line.Contains("Fatal Python error")) sawPythonCrash = true;
            if (line.TrimStart().StartsWith("ERROR")) errors.Add(line.Trim());
            else if (errors.Count > 0 && line.StartsWith("         ")) errors[errors.Count - 1] += "\n" + line.Trim();
            Log(line);
        }

        string LastError()
        {
            if (errors.Count == 0) return null;
            return Regex.Replace(errors[errors.Count - 1], @"^ERROR\s+", "");
        }

        static Dictionary<string, string> PythonEnv(GameDir g) => new Dictionary<string, string>
        {
            ["PATH"] = g.ClangBin + ";" + Environment.GetEnvironmentVariable("PATH"),
            ["MSYSTEM"] = null,
            ["RAC1_LAUNCHER"] = "1",
            ["PYTHONUNBUFFERED"] = "1",
            ["PYTHONIOENCODING"] = "utf-8",
            ["PYTHONUTF8"] = "1",
        };

        void Play()
        {
            if (gameProc != null && !gameProc.HasExited) return;
            try
            {
                gameProc = Process.Start(new ProcessStartInfo(game.Exe) { WorkingDirectory = game.Dir, UseShellExecute = false });
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The game could not be started (" + e.Message + ").", "Play", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            var started = DateTime.Now;
            btnMain.Enabled = false;
            btnMain.Text = "Playing...";
            WindowState = FormWindowState.Minimized;
            gameProc.EnableRaisingEvents = true;
            gameProc.Exited += (s, e) => BeginInvoke((Action)(() =>
            {
                int code = gameProc.ExitCode;
                WindowState = FormWindowState.Normal;
                Activate();
                btnMain.Enabled = true;
                ShowState(state, "");
                CheckUpdates(false);  // (the channel may have changed in the game's options)
                if (code != 0 && (DateTime.Now - started).TotalSeconds < 20)
                    MessageBox.Show(this, $"The game closed right after starting (exit code {code}). If it keeps happening, "
                                        + "try Settings > Set up again from scratch, or report it (Settings > Report a problem).",
                                    "Play", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }));
        }

        // --- Running a task ---------------------------------------------------------------------------------

        void RunTask(string what, Func<Runner, bool> work)
        {
            if (runner != null) return;
            runner = new Runner();
            cancel = new CancellationTokenSource();
            errors.Clear();
            tabs.SelectedIndex = 0;
            btnMain.Enabled = false;
            btnMain.Text = what + "...";
            lblStatus.Text = what + "...";
            lblDetail.Text = "This takes a while (the first time, often 20 to 40 minutes). You can use the PC meanwhile; "
                           + "keep this window open. Cancel stops it, and the next run continues from where it stopped.";
            btnCancel.Visible = true;
            btnCancel.Enabled = true;
            bar.Visible = true;
            bar.Style = ProgressBarStyle.Marquee;
            updatePanel.Visible = false;
            foreach (var c in busyDisabled) c.Enabled = false;
            OpenLogFile();
            Log($"=== {what}: {DateTime.Now:yyyy-MM-dd HH:mm:ss}, launcher {VersionText(LauncherVersion)}, {Environment.OSVersion}, folder {game.Dir}");
            var r = runner;
            Task.Run(() =>
            {
                bool ok;
                try { ok = work(r); }
                catch (Exception e) { Fail("Something went wrong: " + e.Message); Log(e.ToString()); ok = false; }
                BeginInvoke((Action)(() => TaskDone(ok, r.Cancelled)));
            });
        }

        void TaskDone(bool ok, bool cancelled)
        {
            FlushLog();
            runner.Dispose();
            runner = null;
            btnCancel.Visible = false;
            bar.Visible = false;
            foreach (var c in busyDisabled) c.Enabled = true;
            logFile?.Dispose();
            logFile = null;
            lblStep.Text = cancelled ? "Cancelled." : ok ? "" : lblStep.Text;
            RefreshState();
            if (ok)
            {
                lblStep.Text = "";
                if (MessageBox.Show(this, "Ratchet & Clank is ready. Start the game now?", "Ready", MessageBoxButtons.YesNo,
                                    MessageBoxIcon.Information) == DialogResult.Yes)
                {
                    state = State.Ready;
                    Play();
                }
            }
            else if (!cancelled)
            {
                string msg = failure ?? "The setup failed.";
                failure = null;
                var res = MessageBox.Show(this, msg + "\n\nYour saves and settings are safe. You can try again with the button on the Play page."
                                              + "\n\nCopy the full log to the clipboard, for a bug report on GitHub?",
                                          "Something went wrong", MessageBoxButtons.YesNo, MessageBoxIcon.Error);
                chkLog.Checked = true;
                if (res == DialogResult.Yes)
                {
                    try { Clipboard.SetText(fullLog.ToString()); } catch (Exception) { }
                    OpenUrl(GitHub.Page + "/issues");
                }
            }
        }

        void CancelTask()
        {
            if (runner == null) return;
            if (MessageBox.Show(this, "Stop now? What was done so far is kept: the next run continues from there.", "Cancel",
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            btnCancel.Enabled = false;
            cancel?.Cancel();
            runner?.Cancel();
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (runner == null) return;
            if (MessageBox.Show(this, "The setup is still running. Stop it and close? It continues from where it stopped next time.",
                                "Close", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
            {
                e.Cancel = true;
                return;
            }
            cancel?.Cancel();
            runner?.Cancel();
        }

        // --- Progress and the log (from any thread) ---------------------------------------------------------

        string failure;
        void Fail(string msg) { failure = msg; Log("ERROR  " + msg); }

        void Step(string text) => BeginInvoke((Action)(() =>
        {
            lblStep.Text = text;
            bar.Style = ProgressBarStyle.Marquee;
        }));

        void StepDetail(string text) => BeginInvoke((Action)(() => lblStep.Text = text));

        void Progress(int pct) => BeginInvoke((Action)(() =>
        {
            bar.Style = ProgressBarStyle.Continuous;
            bar.Value = Math.Max(0, Math.Min(100, pct));
        }));

        void Log(string line) => pending.Enqueue(line);

        void OpenLogFile()
        {
            try
            {
                Directory.CreateDirectory(game.P("build"));
                logFile = new StreamWriter(game.P("build", "launcher.log"), true, new UTF8Encoding(false)) { AutoFlush = true };
            }
            catch (Exception) { logFile = null; }
        }

        void FlushLog()
        {
            if (pending.IsEmpty) return;
            var sb = new StringBuilder();
            while (pending.TryDequeue(out string line))
            {
                sb.Append(line).Append("\r\n");
                try { logFile?.WriteLine(line); } catch (Exception) { }
            }
            fullLog.Append(sb);
            if (fullLog.Length > 4 << 20) fullLog.Remove(0, fullLog.Length - (2 << 20));
            if (txtLog.TextLength > 1 << 20) txtLog.Text = txtLog.Text.Substring(txtLog.TextLength - (512 << 10));
            txtLog.AppendText(sb.ToString());
        }

        // --- Saves ------------------------------------------------------------------------------------------

        void RefreshSaves()
        {
            lblCard.Text = "Your saves are in " + Saves.CardDir + ". Back them up before trying something new; "
                         + "a deleted slot goes to the Recycle Bin.";
            lstSaves.Items.Clear();
            try
            {
                foreach (var s in Saves.List())
                {
                    var it = new ListViewItem(new[] { s.Region, s.Slot.ToString(), s.Saved.ToString("g"), $"{(s.Size + 1023) / 1024} KB" }) { Tag = s };
                    lstSaves.Items.Add(it);
                }
            }
            catch (Exception e) { lblCard.Text += "\n(" + e.Message + ")"; }
            if (lstSaves.Items.Count == 0)
                lstSaves.Items.Add(new ListViewItem(new[] { "No saves yet", "", "", "" }));
        }

        void BackupSaves()
        {
            if (!Directory.Exists(Saves.CardDir)) { MessageBox.Show(this, "There are no saves yet.", "Saves"); return; }
            using (var d = new SaveFileDialog
            {
                Title = "Back up the saves", Filter = "Zip archive (*.zip)|*.zip",
                FileName = $"rac1-saves-{DateTime.Now:yyyy-MM-dd}.zip",
                InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    Saves.Backup(d.FileName);
                    MessageBox.Show(this, "The saves were backed up to\n" + d.FileName, "Saves", MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
                catch (Exception e) { MessageBox.Show(this, "The backup failed: " + e.Message, "Saves", MessageBoxButtons.OK, MessageBoxIcon.Error); }
            }
        }

        void RestoreSaves()
        {
            if (gameProc != null && !gameProc.HasExited) { MessageBox.Show(this, "Close the game first.", "Saves"); return; }
            using (var d = new OpenFileDialog { Title = "Restore saves from a backup", Filter = "Zip archive (*.zip)|*.zip" })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                if (MessageBox.Show(this, "The saves in the backup replace the ones with the same slot now. "
                                        + "The saves as they are now are backed up first, into\n" + Saves.BackupDir + "\n\nRestore?",
                                    "Saves", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                    return;
                try
                {
                    Saves.Restore(d.FileName);
                    RefreshSaves();
                    MessageBox.Show(this, "The saves were restored.", "Saves", MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
                catch (Exception e) { MessageBox.Show(this, "The restore failed: " + e.Message, "Saves", MessageBoxButtons.OK, MessageBoxIcon.Error); }
            }
        }

        void DeleteSave()
        {
            if (lstSaves.SelectedItems.Count == 0 || !(lstSaves.SelectedItems[0].Tag is SaveSlot s))
            {
                MessageBox.Show(this, "Choose a slot in the list first.", "Saves");
                return;
            }
            if (gameProc != null && !gameProc.HasExited) { MessageBox.Show(this, "Close the game first.", "Saves"); return; }
            if (MessageBox.Show(this, $"Delete slot {s.Slot} ({s.Region}, saved {s.Saved:g})? It goes to the Recycle Bin.",
                                "Saves", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                return;
            try { Saves.Delete(s); }
            catch (Exception e) { MessageBox.Show(this, "The slot could not be deleted: " + e.Message, "Saves"); }
            RefreshSaves();
        }

        // --- Settings ---------------------------------------------------------------------------------------

        void ChangeGameDir()
        {
            using (var d = new FolderBrowserDialog
            {
                Description = "Choose the game's folder: an empty folder for a new install, or a folder that already has the game.",
                SelectedPath = Directory.Exists(game.Dir) ? game.Dir : "",
                ShowNewFolderButton = true,
            })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                string dir = d.SelectedPath;
                var g = new GameDir(dir);
                if (!g.Downloaded && Directory.Exists(dir) && Directory.EnumerateFileSystemEntries(dir).Any())
                {
                    string sub = Path.Combine(dir, "rac1-native");
                    if (MessageBox.Show(this, "This folder is not empty. Install into a new folder inside it?\n" + sub, "Game folder",
                                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                        return;
                    dir = sub;
                }
                if (!GameDir.Plain(dir))
                    MessageBox.Show(this, "This folder's path has spaces or special letters in it. It should work, but if the setup has "
                                        + "trouble, a plain path (like C:\\Games\\rac1-native) is the first thing to try.",
                                    "Game folder", MessageBoxButtons.OK, MessageBoxIcon.Information);
                game = new GameDir(dir);
                settings.GameDir = dir;
                settings.Save();
                txtDir.Text = dir;
                RefreshState();
            }
        }

        /// <summary>The launcher copied into the game folder (so it stays there when the downloaded one is deleted),
        /// and a shortcut to it in the Start menu and (when the player wants one) on the desktop.</summary>
        void CreateShortcuts(bool userAsked)
        {
            try
            {
                string target = game.P("rac1-launcher.exe");
                if (!string.Equals(Path.GetFullPath(target), Path.GetFullPath(Application.ExecutablePath), StringComparison.OrdinalIgnoreCase))
                {
                    Directory.CreateDirectory(game.Dir);
                    File.Copy(Application.ExecutablePath, target, true);
                }
                string name = "Ratchet & Clank (rac1-native).lnk";
                MakeShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), name), target);
                string desktop = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), name);
                if (userAsked || !File.Exists(desktop) &&
                    MessageBox.Show(this, "Put a shortcut to the game on the desktop?", "Shortcut", MessageBoxButtons.YesNo,
                                    MessageBoxIcon.Question) == DialogResult.Yes)
                    MakeShortcut(desktop, target);
                if (userAsked)
                    MessageBox.Show(this, "The shortcuts are on the desktop and in the Start menu.", "Shortcuts",
                                    MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception e)
            {
                if (userAsked) MessageBox.Show(this, "The shortcuts could not be made: " + e.Message, "Shortcuts");
                else Log("The shortcuts could not be made: " + e.Message);
            }
        }

        void MakeShortcut(string lnk, string target)
        {
            var type = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(type);
            object sc = type.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
            var st = sc.GetType();
            st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
            st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { game.Dir });
            st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { "Ratchet & Clank, the PC port (rac1-native)" });
            st.InvokeMember("IconLocation", BindingFlags.SetProperty, null, sc, new object[] { target + ",0" });
            st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
        }

        static void OpenFolder(string dir) => Process.Start(new ProcessStartInfo("explorer.exe", Quote(dir)));
        static void OpenUrl(string url) => Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });

        /// <summary>A command-line argument, quoted for Windows' parsing (CommandLineToArgvW).</summary>
        static string Quote(string s)
        {
            if (s.Length > 0 && s.IndexOfAny(new[] { ' ', '\t', '"', '&', '(', ')' }) < 0) return s;
            var sb = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in s)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') sb.Append('\\', slashes * 2 + 1);
                else sb.Append('\\', slashes);
                slashes = 0;
                sb.Append(c);
            }
            sb.Append('\\', slashes * 2).Append('"');
            return sb.ToString();
        }
    }
}
