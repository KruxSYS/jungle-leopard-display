using System;
using System.Media;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using JLDisplayManager.Themes;

namespace JLDisplayManager.Views;

/// <summary>
/// The app's message box, drawn in the theme. System.Windows.MessageBox is the
/// system's light dialog and takes no styling at all, which put one white
/// window in an otherwise dark app.
///
/// The signatures are MessageBox.Show's, so a file opts in with
/// <c>using MessageBox = JLDisplayManager.Views.Dialog;</c> and no call site
/// changes. Only the button sets and images the app actually uses are drawn.
/// </summary>
public static class Dialog
{
    public static MessageBoxResult Show(Window? owner, string text, string caption,
        MessageBoxButton buttons = MessageBoxButton.OK,
        MessageBoxImage image = MessageBoxImage.None)
        => Build(owner, text, caption, buttons, image);

    public static MessageBoxResult Show(string text, string caption,
        MessageBoxButton buttons = MessageBoxButton.OK,
        MessageBoxImage image = MessageBoxImage.None)
        => Build(null, text, caption, buttons, image);

    private static MessageBoxResult Build(Window? owner, string text, string caption,
        MessageBoxButton buttons, MessageBoxImage image)
    {
        // Before the app's resources exist (a crash in OnStartup before
        // InitializeComponent) there is no theme to draw with.
        if (Application.Current?.TryFindResource("Bg") == null)
            return MessageBox.Show(text, caption, buttons, image);

        if (owner is { IsVisible: false }) owner = null;

        var result = buttons is MessageBoxButton.OK ? MessageBoxResult.OK
                   : buttons is MessageBoxButton.YesNo ? MessageBoxResult.No
                   : MessageBoxResult.Cancel;

        var win = new Window
        {
            Title = caption,
            Owner = owner,
            SizeToContent = SizeToContent.WidthAndHeight,
            ResizeMode = ResizeMode.NoResize,
            ShowInTaskbar = owner == null,
            WindowStartupLocation = owner == null
                ? WindowStartupLocation.CenterScreen
                : WindowStartupLocation.CenterOwner,
            Background = Theme.Brush("Panel"),
            MinWidth = 340,
        };

        // A bar down the left edge carries the kind of message: red for
        // something that went wrong, cyan for everything else. (Amber is
        // reserved by the brand for release status and is not used in the UI.)
        bool bad = image is MessageBoxImage.Error or MessageBoxImage.Warning;
        var bar = new Border { Width = 3, Background = Theme.Brush(bad ? "Error" : "Accent") };

        var body = new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            MaxWidth = 440,
            Foreground = Theme.Brush("TextSoft"),
            Margin = new Thickness(16, 16, 20, 18),
        };

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(16, 0, 16, 14),
        };

        void Add(string label, MessageBoxResult r, bool primary, bool isDefault, bool isCancel)
        {
            var b = new Button
            {
                Content = label,
                MinWidth = 76,
                Margin = new Thickness(8, 0, 0, 0),
                Style = (Style)Application.Current.FindResource(primary ? "BtnPrimary" : "Btn"),
                IsDefault = isDefault,
                IsCancel = isCancel,
            };
            b.Click += (_, _) => { result = r; win.Close(); };
            row.Children.Add(b);
        }

        switch (buttons)
        {
            case MessageBoxButton.OKCancel:
                Add("OK", MessageBoxResult.OK, true, true, false);
                Add("Cancel", MessageBoxResult.Cancel, false, false, true);
                break;
            case MessageBoxButton.YesNo:
                Add("Yes", MessageBoxResult.Yes, true, true, false);
                Add("No", MessageBoxResult.No, false, false, true);
                break;
            case MessageBoxButton.YesNoCancel:
                Add("Yes", MessageBoxResult.Yes, true, true, false);
                Add("No", MessageBoxResult.No, false, false, false);
                Add("Cancel", MessageBoxResult.Cancel, false, false, true);
                break;
            default:
                Add("OK", MessageBoxResult.OK, true, true, true);
                break;
        }

        var stack = new StackPanel();
        stack.Children.Add(body);
        stack.Children.Add(row);

        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        Grid.SetColumn(stack, 1);
        grid.Children.Add(bar);
        grid.Children.Add(stack);
        win.Content = grid;

        win.Loaded += (_, _) =>
        {
            if (row.Children[0] is Button first) Keyboard.Focus(first);
        };

        (image switch
        {
            MessageBoxImage.Error => SystemSounds.Hand,
            MessageBoxImage.Warning => SystemSounds.Exclamation,
            MessageBoxImage.Question => SystemSounds.Question,
            MessageBoxImage.Information => SystemSounds.Asterisk,
            _ => null,
        })?.Play();

        win.ShowDialog();
        return result;
    }
}
