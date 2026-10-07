# Sends one virtual key to the foreground (menus close on Escape; SendKeys needs a foreground
# window and steals focus, keybd_event does not).
param([int]$Vk = 0x1B, [int]$Scan = 0x01)
$code = @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class Kd
{
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);

    public static string Go(int vkCode, int scanCode)
    {
        keybd_event((byte)vkCode, (byte)scanCode, 0, IntPtr.Zero);
        Thread.Sleep(60);
        keybd_event((byte)vkCode, (byte)scanCode, 2, IntPtr.Zero); // KEYUP
        return "sent vk=" + vkCode;
    }
}
'@
Add-Type -TypeDefinition $code
Write-Output ([Kd]::Go($Vk, $Scan))
