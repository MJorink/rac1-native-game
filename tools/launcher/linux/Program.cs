using System;
using System.Threading;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Themes.Fluent;

[assembly: System.Runtime.Versioning.SupportedOSPlatform("linux")]

namespace Rac1Launcher
{
    sealed class App : Application
    {
        public override void Initialize() => Styles.Add(new FluentTheme());

        public override void OnFrameworkInitializationCompleted()
        {
            if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
                desktop.MainWindow = new MainWindow();
            base.OnFrameworkInitializationCompleted();
        }
    }

    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            using (var one = new Mutex(true, "rac1-native-launcher", out bool first))
            {
                if (!first)
                {
                    Console.Error.WriteLine("The launcher is already open.");
                    return 1;
                }
                return AppBuilder.Configure<App>()
                                 .UsePlatformDetect()
                                 .WithInterFont()  // (its own font: the same text on every distribution)
                                 .StartWithClassicDesktopLifetime(args);
            }
        }
    }
}
