// What differs between the Windows launcher (windows/, WINDOWS_LAUNCHER defined) and the Linux one (linux/): where
// the settings and saves are, the Python and the tools the setup runs with, the names of the files, shortcuts, the
// trash, opening a folder.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

namespace Rac1Launcher
{
    static class Platform
    {
#if WINDOWS_LAUNCHER
        public static readonly bool Windows = true;
        public const string ExeName = "rac1-native.exe";
        public const string LauncherAsset = "rac1-launcher.exe";  // (the release's file the launcher updates itself from)
        public const string LauncherFile = "rac1-launcher.exe";   // (its name in the game folder)
        public const string TrashName = "the Recycle Bin";
        public const string SetupCommand = "setup.cmd";
#else
        public static readonly bool Windows = false;
        public const string ExeName = "rac1-native";
        public const string LauncherAsset = "rac1-launcher-linux-x64";
        public const string LauncherFile = "rac1-launcher";
        public const string TrashName = "the trash";
        public const string SetupCommand = "./setup.sh";
#endif

        static string Home => Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);

        static string Xdg(string variable, string fallback)
        {
            string v = Environment.GetEnvironmentVariable(variable);
            return !string.IsNullOrEmpty(v) && Path.IsPathRooted(v) ? v : Path.Combine(Home, fallback);
        }

        /// <summary>The settings' folder, as the game's (platform.h config_dir): %APPDATA%\rac1-native, or
        /// $XDG_CONFIG_HOME/rac1-native (~/.config/rac1-native).</summary>
        public static string ConfigDir => Windows
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "rac1-native")
            : Path.Combine(Xdg("XDG_CONFIG_HOME", ".config"), "rac1-native");

        /// <summary>The data folder, where the memory card is (docs/saves.md): %APPDATA%\rac1-native, or
        /// $XDG_DATA_HOME/rac1-native (~/.local/share/rac1-native).</summary>
        public static string DataDir => Windows ? ConfigDir : Path.Combine(Xdg("XDG_DATA_HOME", ".local/share"), "rac1-native");

        /// <summary>Where the launcher puts the game when it was never told. On Windows a folder of the player's own, without
        /// spaces or other letters than plain ASCII where it can (the build's tools like those best); on Linux
        /// ~/.local/share/rac1-native/game.</summary>
        public static string DefaultGameDir()
        {
            if (!Windows) return Path.Combine(DataDir, "game");
            string local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "rac1-native");
            if (GameDir.Plain(local)) return local;
            string drive = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows)) ?? @"C:\";
            return Path.Combine(drive, "rac1-native");
        }

        /// <summary>The Python the setup runs with: on Windows the private MSYS2's (tools\windows\toolchain.ps1), on Linux
        /// the system's python3.</summary>
        public static string Python(GameDir g) => Windows ? g.P(".toolchain", "msys64", "clang64", "bin", "python.exe") : "python3";

        /// <summary>True when tools\setup.py can be run (--check): on Windows once the private MSYS2 is there.</summary>
        public static bool CanRunSetup(GameDir g) => !Windows || File.Exists(Python(g)) || Which("python3") != null;

        public static Dictionary<string, string> PythonEnv(GameDir g)
        {
            var env = new Dictionary<string, string>
            {
                ["RAC1_LAUNCHER"] = "1",
                ["PYTHONUNBUFFERED"] = "1",
                ["PYTHONIOENCODING"] = "utf-8",
                ["PYTHONUTF8"] = "1",
            };
            if (Windows)
            {
                env["PATH"] = g.P(".toolchain", "msys64", "clang64", "bin") + ";" + Environment.GetEnvironmentVariable("PATH");
                env["MSYSTEM"] = null;
            }
            return env;
        }

        /// <summary>A program on the PATH, or null.</summary>
        public static string Which(string name)
        {
            foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
            {
                if (dir == "") continue;
                string p = Path.Combine(dir, name);
                if (File.Exists(p)) return p;
                if (Windows && File.Exists(p + ".exe")) return p + ".exe";
            }
            return null;
        }

        public static void OpenFolder(string dir)
        {
            Directory.CreateDirectory(dir);
            if (Windows) Process.Start(new ProcessStartInfo("explorer.exe", Quote(dir)));
            else Process.Start(new ProcessStartInfo("xdg-open", Quote(dir)) { UseShellExecute = false });
        }

        public static void OpenUrl(string url)
        {
            if (Windows) Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            else Process.Start(new ProcessStartInfo("xdg-open", Quote(url)) { UseShellExecute = false });
        }

        /// <summary>A file to the Recycle Bin (Windows) or the desktop's trash (Linux: gio trash, as the file manager
        /// does); deleted when there is no trash.</summary>
        public static void Trash(string file)
        {
#if WINDOWS_LAUNCHER
            Microsoft.VisualBasic.FileIO.FileSystem.DeleteFile(file, Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
                                                               Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
#else
            if (Which("gio") != null)
            {
                using (var p = Process.Start(new ProcessStartInfo("gio", "trash " + Quote(file)) { UseShellExecute = false }))
                {
                    p.WaitForExit();
                    if (p.ExitCode == 0) return;
                }
            }
            // (no gio: the trash as the freedesktop.org specification has it, which every file manager reads)
            try
            {
                string trash = Path.Combine(Xdg("XDG_DATA_HOME", ".local/share"), "Trash");
                Directory.CreateDirectory(Path.Combine(trash, "files"));
                Directory.CreateDirectory(Path.Combine(trash, "info"));
                string name = Path.GetFileName(file);
                for (int n = 2; File.Exists(Path.Combine(trash, "files", name)) || File.Exists(Path.Combine(trash, "info", name + ".trashinfo")); n++)
                    name = Path.GetFileNameWithoutExtension(file) + "." + n + Path.GetExtension(file);
                string full = Path.GetFullPath(file);
                string url = string.Join("/", full.Split('/').Select(Uri.EscapeDataString));
                File.WriteAllText(Path.Combine(trash, "info", name + ".trashinfo"),
                                  $"[Trash Info]\nPath={url}\nDeletionDate={DateTime.Now:yyyy-MM-ddTHH:mm:ss}\n");
                File.Move(full, Path.Combine(trash, "files", name));
                return;
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            File.Delete(file);
#endif
        }

        /// <summary>A command-line argument, quoted the way Windows' programs (and .NET on Linux) split them.</summary>
        public static string Quote(string s)
        {
            if (s.Length > 0 && s.IndexOfAny(new[] { ' ', '\t', '"', '&', '(', ')', '\'' }) < 0) return s;
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

        /// <summary>The launcher copied into the game folder (so it stays when the downloaded one is deleted), and a
        /// shortcut to it: in the Start menu (and on the desktop when asked), or in the desktop's application menu
        /// (~/.local/share/applications). Returns the launcher's copy.</summary>
        public static string InstallLauncher(GameDir g, string runningExe, bool desktop)
        {
            string target = g.P(LauncherFile);
            if (!string.Equals(Path.GetFullPath(target), Path.GetFullPath(runningExe), StringComparison.OrdinalIgnoreCase))
            {
                Directory.CreateDirectory(g.Dir);
                File.Copy(runningExe, target, true);
            }
#if WINDOWS_LAUNCHER
            string name = "Ratchet & Clank (rac1-native).lnk";
            WindowsShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), name), target, g.Dir);
            if (desktop)
                WindowsShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), name), target, g.Dir);
#else
            File.SetUnixFileMode(target, (UnixFileMode)0b111_101_101);
            string data = Xdg("XDG_DATA_HOME", ".local/share");
            string icon = Path.Combine(data, "icons", "hicolor", "256x256", "apps", "rac1-native.png");
            Directory.CreateDirectory(Path.GetDirectoryName(icon));
            using (var s = typeof(Platform).Assembly.GetManifestResourceStream("launcher.png"))
            using (var f = File.Create(icon))
                s?.CopyTo(f);
            string entry = "[Desktop Entry]\nType=Application\nName=Ratchet & Clank (rac1-native)\n"
                         + "Comment=Ratchet & Clank, the PC port: install, update and play\n"
                         + $"Exec=\"{target}\"\nPath={g.Dir}\nIcon=rac1-native\nTerminal=false\nCategories=Game;\n";
            string apps = Path.Combine(data, "applications");
            Directory.CreateDirectory(apps);
            File.WriteAllText(Path.Combine(apps, "rac1-native.desktop"), entry);
            if (desktop)
            {
                string dir = DesktopDir();
                if (dir != null)
                {
                    string d = Path.Combine(dir, "rac1-native.desktop");
                    File.WriteAllText(d, entry);
                    File.SetUnixFileMode(d, (UnixFileMode)0b111_101_101);
                }
            }
#endif
            return target;
        }

#if WINDOWS_LAUNCHER
        static void WindowsShortcut(string lnk, string target, string workDir)
        {
            var flags = System.Reflection.BindingFlags.InvokeMethod;
            var set = System.Reflection.BindingFlags.SetProperty;
            var type = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(type);
            object sc = type.InvokeMember("CreateShortcut", flags, null, shell, new object[] { lnk });
            var st = sc.GetType();
            st.InvokeMember("TargetPath", set, null, sc, new object[] { target });
            st.InvokeMember("WorkingDirectory", set, null, sc, new object[] { workDir });
            st.InvokeMember("Description", set, null, sc, new object[] { "Ratchet & Clank, the PC port (rac1-native)" });
            st.InvokeMember("IconLocation", set, null, sc, new object[] { target + ",0" });
            st.InvokeMember("Save", flags, null, sc, null);
        }
#else
        /// <summary>The desktop folder (xdg-user-dir DESKTOP), or null.</summary>
        static string DesktopDir()
        {
            try
            {
                var p = Process.Start(new ProcessStartInfo("xdg-user-dir", "DESKTOP") { UseShellExecute = false, RedirectStandardOutput = true });
                string dir = p.StandardOutput.ReadToEnd().Trim();
                p.WaitForExit();
                if (dir != "" && dir != Home && Directory.Exists(dir)) return dir;
            }
            catch (Exception) { }
            string d = Path.Combine(Home, "Desktop");
            return Directory.Exists(d) ? d : null;
        }
#endif

        /// <summary>A new launcher (fresh, downloaded beside exe) in the running one's place. Windows cannot replace a
        /// running program, only rename it: it goes to exe.old, deleted at the next start (Program.cs).</summary>
        public static void ReplaceLauncher(string exe, string fresh)
        {
#if WINDOWS_LAUNCHER
            File.Delete(exe + ".old");
            File.Move(exe, exe + ".old");
            File.Move(fresh, exe);
#else
            File.SetUnixFileMode(fresh, (UnixFileMode)0b111_101_101);
            File.Move(fresh, exe, true);  // (a running program's file can be replaced on Linux: it keeps the old one)
#endif
        }

        /// <summary>The system packages a Linux setup found missing, as tools/setup.py prints them ("    sudo apt install
        /// a b c", check_system), made into a command the launcher can run as root with pkexec; null for another line.</summary>
        public static string PackageInstallCommand(string setupLine)
        {
            var m = System.Text.RegularExpressions.Regex.Match(setupLine.Trim(),
                @"^sudo (apt install|dnf install|pacman -S --needed) ([a-zA-Z0-9.+_ -]+)$");
            if (!m.Success) return null;
            string pkgs = m.Groups[2].Value.Trim();
            switch (m.Groups[1].Value)
            {
                case "apt install": return "apt-get update && apt-get install -y " + pkgs;
                case "dnf install": return "dnf install -y " + pkgs;
                default: return "pacman -S --needed --noconfirm " + pkgs;
            }
        }
    }
}
