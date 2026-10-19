// Runs the setup's programs (PowerShell for the tools, Python for tools\setup.py) without a console window, their
// output line by line to the launcher, all of them in a Windows job: Cancel, or closing the launcher, ends the
// whole tree (pacman, CMake, the compilers, the game's --setup) and leaves nothing running behind.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Rac1Launcher
{
    sealed class Runner : IDisposable
    {
        readonly IntPtr job;
        volatile bool cancelled;
        public bool Cancelled => cancelled;

        public Runner()
        {
            job = CreateJobObject(IntPtr.Zero, null);
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags = 0x2000;  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            int size = Marshal.SizeOf(info);
            IntPtr p = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(info, p, false);
                SetInformationJobObject(job, 9 /* JobObjectExtendedLimitInformation */, p, (uint)size);
            }
            finally { Marshal.FreeHGlobal(p); }
        }

        /// <summary>Runs a program to its end; each line of its output (stdout and stderr) to onLine. Returns its exit
        /// code (-1 when cancelled).</summary>
        public int Run(string exe, string args, string cwd, IDictionary<string, string> env, Action<string> onLine,
                       Encoding encoding = null)
        {
            if (cancelled) return -1;
            var psi = new ProcessStartInfo(exe, args)
            {
                WorkingDirectory = cwd,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                RedirectStandardInput = true,  // (no one to answer: the setup's questions take their default)
            };
            if (encoding != null)
            {
                psi.StandardOutputEncoding = encoding;
                psi.StandardErrorEncoding = encoding;
            }
            if (env != null)
                foreach (var kv in env)
                {
                    if (kv.Value == null) psi.EnvironmentVariables.Remove(kv.Key);
                    else psi.EnvironmentVariables[kv.Key] = kv.Value;
                }
            using (var proc = new Process { StartInfo = psi })
            {
                var done = new CountdownEvent(2);
                proc.OutputDataReceived += (s, e) => { if (e.Data == null) done.Signal(); else onLine(e.Data); };
                proc.ErrorDataReceived += (s, e) => { if (e.Data == null) done.Signal(); else onLine(e.Data); };
                try { proc.Start(); }
                catch (Win32Exception e) { onLine($"ERROR  Could not start {exe}: {e.Message}"); return -2; }
                AssignProcessToJobObject(job, proc.Handle);
                proc.StandardInput.Close();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
                proc.WaitForExit();
                done.Wait(5000);
                return cancelled ? -1 : proc.ExitCode;
            }
        }

        public void Cancel()
        {
            cancelled = true;
            TerminateJobObject(job, 1);
        }

        public void Dispose() => CloseHandle(job);

        [StructLayout(LayoutKind.Sequential)]
        struct JOBOBJECT_BASIC_LIMIT_INFORMATION
        {
            public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct IO_COUNTERS
        {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount,
                         WriteTransferCount, OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll")]
        static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint length);
        [DllImport("kernel32.dll")]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll")]
        static extern bool TerminateJobObject(IntPtr job, uint exitCode);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);
    }
}
