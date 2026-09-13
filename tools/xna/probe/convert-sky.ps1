param(
  [string]$Source = (Join-Path $PSScriptRoot "sky.jpg"),
  [string]$Target = (Join-Path $PSScriptRoot "sky.bgra"),
  [int]$Width = 1024,
  [int]$Height = 576
)

Add-Type -AssemblyName System.Drawing

$image = [System.Drawing.Image]::FromFile($Source)
$bitmap = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.DrawImage($image, 0, 0, $Width, $Height)
$graphics.Dispose()
$image.Dispose()

$rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
$data = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($Width * 4 * $Height)
for ($y = 0; $y -lt $Height; $y++) {
  [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $bytes, $y * $Width * 4, $Width * 4)
}
$bitmap.UnlockBits($data)
$bitmap.Dispose()

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([int32]$Width)
$writer.Write([int32]$Height)
$writer.Write($bytes)
$writer.Flush()
[System.IO.File]::WriteAllBytes($Target, $stream.ToArray())
"sky: {0}x{1} BGRA -> {2} ({3} bytes)" -f $Width, $Height, $Target, $stream.Length
