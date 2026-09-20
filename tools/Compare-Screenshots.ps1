param(
    [Parameter(Mandatory=$true, Position=0)][string]$Reference,
    [Parameter(Mandatory=$true, Position=1)][string]$Actual,
    [int]$Tolerance = 24,
    [string]$OutputDirectory = ".\pixel-diff"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$referencePath = (Resolve-Path -LiteralPath $Reference).Path
$actualPath = (Resolve-Path -LiteralPath $Actual).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null

$referenceBitmap = [System.Drawing.Bitmap]::new([string]$referencePath)
$actualBitmap = [System.Drawing.Bitmap]::new([string]$actualPath)
try {
    if ($referenceBitmap.Width -ne $actualBitmap.Width -or $referenceBitmap.Height -ne $actualBitmap.Height) {
        throw "Image dimensions differ: reference=$($referenceBitmap.Width)x$($referenceBitmap.Height), actual=$($actualBitmap.Width)x$($actualBitmap.Height)"
    }

    $width = $referenceBitmap.Width
    $height = $referenceBitmap.Height
    $rectangle = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
    $format = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    $refData = $referenceBitmap.LockBits($rectangle, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $format)
    $actualData = $actualBitmap.LockBits($rectangle, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $format)
    try {
        $length = [Math]::Abs($refData.Stride) * $height
        $refBytes = [byte[]]::new($length)
        $actualBytes = [byte[]]::new($length)
        [Runtime.InteropServices.Marshal]::Copy($refData.Scan0, $refBytes, 0, $length)
        [Runtime.InteropServices.Marshal]::Copy($actualData.Scan0, $actualBytes, 0, $length)
    } finally {
        $referenceBitmap.UnlockBits($refData)
        $actualBitmap.UnlockBits($actualData)
    }

    $diffBitmap = [System.Drawing.Bitmap]::new($width, $height, $format)
    $diffData = $diffBitmap.LockBits($rectangle, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $format)
    $diffBytes = [byte[]]::new([Math]::Abs($diffData.Stride) * $height)
    $different = 0L; $exactDifferent = 0L
    $minX = $width; $minY = $height; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $height; $y++) {
        for ($x = 0; $x -lt $width; $x++) {
            $offset = $y * [Math]::Abs($refData.Stride) + $x * 4
            $db = [Math]::Abs([int]$refBytes[$offset] - [int]$actualBytes[$offset])
            $dg = [Math]::Abs([int]$refBytes[$offset + 1] - [int]$actualBytes[$offset + 1])
            $dr = [Math]::Abs([int]$refBytes[$offset + 2] - [int]$actualBytes[$offset + 2])
            $delta = [Math]::Max($dr, [Math]::Max($dg, $db))
            if ($delta -gt 0) { $exactDifferent++ }
            if ($delta -gt $Tolerance) {
                $different++; $minX = [Math]::Min($minX, $x); $minY = [Math]::Min($minY, $y); $maxX = [Math]::Max($maxX, $x); $maxY = [Math]::Max($maxY, $y)
                $diffBytes[$offset] = 0; $diffBytes[$offset + 1] = 0; $diffBytes[$offset + 2] = 255; $diffBytes[$offset + 3] = 255
            } else {
                $gray = [byte]([Math]::Round(($refBytes[$offset] + $refBytes[$offset + 1] + $refBytes[$offset + 2]) / 6))
                $diffBytes[$offset] = $gray; $diffBytes[$offset + 1] = $gray; $diffBytes[$offset + 2] = $gray; $diffBytes[$offset + 3] = 255
            }
        }
    }
    [Runtime.InteropServices.Marshal]::Copy($diffBytes, 0, $diffData.Scan0, $diffBytes.Length)
    $diffBitmap.UnlockBits($diffData)
    $diffFile = Join-Path $outputPath "diff.png"
    $diffBitmap.Save($diffFile, [System.Drawing.Imaging.ImageFormat]::Png)
    $diffBitmap.Dispose()

    $pixels = [long]$width * $height
    $report = [ordered]@{
        reference = $referencePath; actual = $actualPath; width = $width; height = $height
        tolerance = $Tolerance; exactDifferentPixels = $exactDifferent; differentPixels = $different
        differentPercent = if ($pixels) { [Math]::Round(100.0 * $different / $pixels, 6) } else { 0 }
        boundingBox = if ($different) { @{ x=$minX; y=$minY; width=$maxX-$minX+1; height=$maxY-$minY+1 } } else { $null }
        diffImage = $diffFile
    }
    $reportFile = Join-Path $outputPath "report.json"
    $report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $reportFile -Encoding UTF8
    $report | ConvertTo-Json -Depth 4
} finally {
    $referenceBitmap.Dispose()
    $actualBitmap.Dispose()
}
