param(
    [string]$Executable = ".\x64\Debug\FA50DataGUI.exe",
    [string]$OutputDirectory = ".\capture\current",
    [int]$ClientWidth = 1920,
    [int]$ClientHeight = 1009,
    [int]$FocusX = -1,
    [int]$FocusY = -1,
    [string]$FocusText = "",
    [string]$BlockFilePath = ""
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class TWebFrameCaptureNative {
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int width, int height, bool repaint);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageText(IntPtr h, uint message, IntPtr wParam, string lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder value, int count);
    public delegate bool EnumProc(IntPtr h, IntPtr data);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

# Keep capture and resize coordinates in physical pixels. Without an explicit
# thread context Windows virtualizes cross-process HWND coordinates at display
# scales such as 150%, producing a misleading double-scaled reference image.
[TWebFrameCaptureNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($outputPath) | Out-Null
$process = Start-Process -FilePath $executablePath -PassThru

function Save-ChildWindow([IntPtr]$Handle, [string]$Path) {
    $rect = New-Object TWebFrameCaptureNative+RECT
    [TWebFrameCaptureNative]::GetClientRect($Handle, [ref]$rect) | Out-Null
    $bitmap = [Drawing.Bitmap]::new($rect.Right, $rect.Bottom, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $dc = $graphics.GetHdc()
            try { [TWebFrameCaptureNative]::PrintWindow($Handle, $dc, 2) | Out-Null }
            finally { $graphics.ReleaseHdc($dc) }
        } finally { $graphics.Dispose() }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}

function Click-Child([IntPtr]$Handle, [int]$X, [int]$Y) {
    $position = [IntPtr](($Y -shl 16) -bor ($X -band 0xffff))
    [TWebFrameCaptureNative]::SendMessage($Handle, 0x0201, [IntPtr]1, $position) | Out-Null
    [TWebFrameCaptureNative]::SendMessage($Handle, 0x0202, [IntPtr]0, $position) | Out-Null
    Start-Sleep -Milliseconds 150
}

try {
    for ($attempt = 0; $attempt -lt 50 -and $process.MainWindowHandle -eq 0; $attempt++) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
    }
    Start-Sleep -Milliseconds 500
    $process.Refresh()
    $window = $process.MainWindowHandle
    if ($window -eq 0) { throw 'FA50DataGUI main window was not created.' }

    $outer = New-Object TWebFrameCaptureNative+RECT
    $client = New-Object TWebFrameCaptureNative+RECT
    [TWebFrameCaptureNative]::GetWindowRect($window, [ref]$outer) | Out-Null
    [TWebFrameCaptureNative]::GetClientRect($window, [ref]$client) | Out-Null
    $frameWidth = ($outer.Right - $outer.Left) - ($client.Right - $client.Left)
    $frameHeight = ($outer.Bottom - $outer.Top) - ($client.Bottom - $client.Top)
    [TWebFrameCaptureNative]::MoveWindow($window, -8, 0, $ClientWidth + $frameWidth, $ClientHeight + $frameHeight, $true) | Out-Null
    Start-Sleep -Milliseconds 500

    $script:tWebFrame = [IntPtr]::Zero
    $callback = [TWebFrameCaptureNative+EnumProc]{
        param([IntPtr]$child, [IntPtr]$data)
        $className = [Text.StringBuilder]::new(128)
        [TWebFrameCaptureNative]::GetClassName($child, $className, 128) | Out-Null
        if ($className.ToString() -eq 'TWebFrame.View.1') { $script:tWebFrame = $child; return $false }
        return $true
    }
    [TWebFrameCaptureNative]::EnumChildWindows($window, $callback, [IntPtr]::Zero) | Out-Null
    if ($script:tWebFrame -eq [IntPtr]::Zero) { throw 'TWebFrame child window was not found.' }

    Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'twebframe1.png')
    if ($FocusX -ge 0 -and $FocusY -ge 0) {
        Click-Child $script:tWebFrame $FocusX $FocusY
        if ($FocusText) {
            $script:nativeEdit = [IntPtr]::Zero
            $editCallback = [TWebFrameCaptureNative+EnumProc]{
                param([IntPtr]$child, [IntPtr]$data)
                $className = [Text.StringBuilder]::new(128)
                [TWebFrameCaptureNative]::GetClassName($child, $className, 128) | Out-Null
                if ($className.ToString() -eq 'Edit') { $script:nativeEdit = $child; return $false }
                return $true
            }
            [TWebFrameCaptureNative]::EnumChildWindows($script:tWebFrame, $editCallback, [IntPtr]::Zero) | Out-Null
            if ($script:nativeEdit -eq [IntPtr]::Zero) { throw 'Native edit control was not found.' }
            [TWebFrameCaptureNative]::SendMessageText($script:nativeEdit, 0x000c, [IntPtr]::Zero, $FocusText) | Out-Null
            Click-Child $script:tWebFrame 600 600
            Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'twebframe-text-before.png')
            Click-Child $script:tWebFrame $FocusX $FocusY
        }
        Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'twebframe-focused.png')
    }
    Click-Child $script:tWebFrame 198 75
    if ($BlockFilePath) {
        Click-Child $script:tWebFrame 300 143
        $script:blockEdit = [IntPtr]::Zero
        $blockEditCallback = [TWebFrameCaptureNative+EnumProc]{
            param([IntPtr]$child, [IntPtr]$data)
            $className = [Text.StringBuilder]::new(128)
            [TWebFrameCaptureNative]::GetClassName($child, $className, 128) | Out-Null
            if ($className.ToString() -eq 'Edit') { $script:blockEdit = $child; return $false }
            return $true
        }
        [TWebFrameCaptureNative]::EnumChildWindows($script:tWebFrame, $blockEditCallback, [IntPtr]::Zero) | Out-Null
        if ($script:blockEdit -eq [IntPtr]::Zero) { throw 'Block viewer native edit control was not found.' }
        [TWebFrameCaptureNative]::SendMessageText($script:blockEdit, 0x000c, [IntPtr]::Zero, $BlockFilePath) | Out-Null
        Click-Child $script:tWebFrame 600 600
        Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'block-text-before.png')
        Click-Child $script:tWebFrame 300 143
        Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'block-text-focused.png')
    }
    Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'twebframe2.png')
    Click-Child $script:tWebFrame 324 75
    Save-ChildWindow $script:tWebFrame (Join-Path $outputPath 'twebframe3.png')
    Get-ChildItem -LiteralPath $outputPath -Filter 'twebframe*.png' | Select-Object FullName, Length
} finally {
    if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
}
