using System.Reflection;

namespace JLDisplayManager.Services;

/// <summary>
/// Who makes this and where it lives. The one place in the manager that names
/// the repository: change it here and the About card follows.
/// </summary>
public static class AppInfo
{
    public const string Product = "Jungle Leopard Display";
    public const string Publisher = "KruxSYS";
    public const string RepoUrl = "https://github.com/KruxSYS/jungle-leopard-display";
    public const string WebsiteUrl = "https://kruxsys.com";

    /// <summary>"1.7.0" — the csproj &lt;Version&gt;, without the source-revision suffix.</summary>
    public static string Version
    {
        get
        {
            var asm = typeof(AppInfo).Assembly;
            string? info = asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
            if (!string.IsNullOrEmpty(info))
            {
                int plus = info.IndexOf('+');
                return plus >= 0 ? info[..plus] : info;
            }
            return asm.GetName().Version?.ToString(3) ?? "?";
        }
    }
}
