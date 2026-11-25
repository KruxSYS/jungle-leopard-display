using System.IO;
using System.Text.RegularExpressions;
using JLDisplayManager.Services;
using Xunit;

namespace JLDisplayManager.Tests;

/// <summary>
/// Things that must agree for a release: the managed and native versions,
/// and the names existing users' data depends on.
/// </summary>
public class ReleaseTests
{
    private static string Root => TestSupport.RepoRoot;

    private static string CsprojVersion()
    {
        string csproj = File.ReadAllText(Path.Combine(Root, "JLDisplayManager", "JLDisplayManager.csproj"));
        return Regex.Match(csproj, "<Version>([^<]+)</Version>").Groups[1].Value;
    }

    [Fact]
    public void Assembly_version_is_the_csproj_version() =>
        Assert.Equal(CsprojVersion(), AppInfo.Version);

    [Fact]
    public void Native_version_header_agrees()
    {
        string h = File.ReadAllText(Path.Combine(Root, "JLDisplayCore", "jl_version.h"));
        int Part(string name) => int.Parse(Regex.Match(h, $@"#define {name}\s+(\d+)").Groups[1].Value);
        Assert.Equal(CsprojVersion(), $"{Part("JL_VERSION_MAJOR")}.{Part("JL_VERSION_MINOR")}.{Part("JL_VERSION_PATCH")}");
    }

    [Fact]
    public void Data_paths_are_unchanged()
    {
        // Existing users' library and settings live here; renaming it would
        // quietly start them from nothing.
        Assert.EndsWith(@"\JungleLeopardDisplay", Models.Storage.Directory);
    }

    [Fact]
    public void Mutex_and_task_names_are_unchanged()
    {
        string app = File.ReadAllText(Path.Combine(Root, "JLDisplayManager", "App.xaml.cs"));
        Assert.Contains(@"Local\JungleLeopardDisplayManager.Instance", app);
        string task = File.ReadAllText(Path.Combine(Root, "JLDisplayManager", "Services", "StartupTask.cs"));
        Assert.Contains("TaskName = \"JungleLeopardDisplayManager\"", task);
    }
}
