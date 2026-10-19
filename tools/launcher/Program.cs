using System;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace Rac1Launcher
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            // (what a self-update left behind: the previous launcher, renamed while it was running)
            try { File.Delete(Application.ExecutablePath + ".old"); } catch (Exception) { }

            using (var one = new Mutex(true, "rac1-native-launcher", out bool first))
            {
                if (!first)
                {
                    MessageBox.Show("The launcher is already open.", "Ratchet & Clank", MessageBoxButtons.OK,
                                    MessageBoxIcon.Information);
                    return;
                }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm());
            }
        }
    }
}
