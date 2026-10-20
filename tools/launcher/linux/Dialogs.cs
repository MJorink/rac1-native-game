// The Linux launcher's small windows: a message, a yes / no question, a path typed in (when the desktop has no file
// chooser the launcher can open). Their text can be selected and copied (a command to run, an error for a report).
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Rac1Launcher
{
    static class Dialogs
    {
        static Window Frame(string title, Control body, params Button[] buttons)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Right,
                                       Margin = new Thickness(0, 18, 0, 0) };
            foreach (var b in buttons) row.Children.Add(b);
            var panel = new StackPanel { Margin = new Thickness(22) };
            panel.Children.Add(body);
            panel.Children.Add(row);
            return new Window
            {
                Title = title, Content = panel, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false,
                WindowStartupLocation = WindowStartupLocation.CenterOwner, ShowInTaskbar = false,
            };
        }

        static Control Text(string text) => new SelectableTextBlock
        {
            Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = 520, FontSize = 14,
        };

        static Button Button(string text, bool primary = false)
        {
            var b = new Button { Content = text, MinWidth = 90, HorizontalContentAlignment = HorizontalAlignment.Center };
            if (primary) b.Classes.Add("accent");
            return b;
        }

        public static Task Say(Window owner, string title, string text)
        {
            var ok = Button("OK", true);
            var w = Frame(title, Text(text), ok);
            ok.Click += (s, e) => w.Close();
            return w.ShowDialog(owner);
        }

        public static Task<bool> Ask(Window owner, string title, string text, string yes = "Yes", string no = "No")
        {
            var y = Button(yes, true);
            var n = Button(no);
            var w = Frame(title, Text(text), y, n);
            y.Click += (s, e) => w.Close(true);
            n.Click += (s, e) => w.Close(false);
            return w.ShowDialog<bool>(owner);
        }

        /// <summary>A path typed in (or pasted); null when cancelled.</summary>
        public static Task<string> AskPath(Window owner, string title, string text, string start)
        {
            var box = new TextBox { Text = start ?? "", Width = 520, Margin = new Thickness(0, 12, 0, 0) };
            var body = new StackPanel();
            body.Children.Add(Text(text));
            body.Children.Add(box);
            var ok = Button("OK", true);
            var cancel = Button("Cancel");
            var w = Frame(title, body, ok, cancel);
            ok.Click += (s, e) => w.Close(box.Text?.Trim().Trim('"', '\''));
            cancel.Click += (s, e) => w.Close(null);
            return w.ShowDialog<string>(owner);
        }
    }
}
