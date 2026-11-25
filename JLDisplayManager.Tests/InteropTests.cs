using System.Runtime.InteropServices;
using JLDisplayManager.Interop;
using Xunit;

namespace JLDisplayManager.Tests;

/// <summary>
/// The managed mirrors of jl_api.h. The native side asserts the same sizes at
/// compile time (audit M8), so the two can only change together.
/// </summary>
public class InteropTests
{
    [Fact]
    public void Verify_layout_passes() => NativeMethods.VerifyLayout();

    [Fact]
    public void Struct_sizes_match_the_header()
    {
        Assert.Equal(100, Marshal.SizeOf<NativeMethods.JlRenderOpts>());
        Assert.Equal(1184, Marshal.SizeOf<NativeMethods.JlStatus>());
        Assert.Equal(1832, Marshal.SizeOf<NativeMethods.JlMediaStatus>());
    }

    [Fact]
    public void Status_fields_sit_where_the_header_puts_them()
    {
        // state, connected, port[32] wchar, then the int64s on an 8-byte boundary.
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeMethods.JlStatus>(nameof(NativeMethods.JlStatus.State)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeMethods.JlStatus>(nameof(NativeMethods.JlStatus.Port)));
        Assert.Equal(72, (int)Marshal.OffsetOf<NativeMethods.JlStatus>(nameof(NativeMethods.JlStatus.FramesSent)));
        Assert.Equal(36, (int)Marshal.OffsetOf<NativeMethods.JlRenderOpts>(nameof(NativeMethods.JlRenderOpts.Hwaccel)));
    }
}
