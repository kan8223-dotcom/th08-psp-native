param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class PpssppMenuDump
{
    [DllImport("user32.dll")]
    private static extern IntPtr GetMenu(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int GetMenuItemCount(IntPtr menu);

    [DllImport("user32.dll")]
    private static extern IntPtr GetSubMenu(IntPtr menu, int position);

    [DllImport("user32.dll")]
    private static extern uint GetMenuItemID(IntPtr menu, int position);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetMenuStringW(
        IntPtr menu, uint item, StringBuilder text, int capacity, uint flags);

    private const uint MF_BYPOSITION = 0x00000400;

    private static void AppendMenu(StringBuilder output, IntPtr menu,
                                   string prefix)
    {
        int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position)
        {
            StringBuilder text = new StringBuilder(512);
            GetMenuStringW(menu, (uint)position, text, text.Capacity,
                           MF_BYPOSITION);
            uint id = GetMenuItemID(menu, position);
            output.Append(prefix);
            output.Append("position=");
            output.Append(position);
            output.Append(" id=");
            output.Append(id == 0xFFFFFFFFU ? "submenu" : id.ToString());
            output.Append(" text=");
            output.AppendLine(text.ToString());

            IntPtr child = GetSubMenu(menu, position);
            if (child != IntPtr.Zero)
                AppendMenu(output, child, prefix + "  ");
        }
    }

    public static string Dump(IntPtr window)
    {
        IntPtr menu = GetMenu(window);
        if (menu == IntPtr.Zero)
            throw new InvalidOperationException("window has no menu");
        StringBuilder output = new StringBuilder();
        AppendMenu(output, menu, "");
        return output.ToString();
    }
}
'@

$process = Get-Process -Id $TargetProcessId -ErrorAction Stop
if ($process.ProcessName -ne "PPSSPPWindows64") {
    throw "process $TargetProcessId is $($process.ProcessName), not PPSSPPWindows64"
}
if ($process.MainWindowHandle -eq [IntPtr]::Zero) {
    throw "PPSSPP process $TargetProcessId has no main window"
}

[PpssppMenuDump]::Dump($process.MainWindowHandle)
