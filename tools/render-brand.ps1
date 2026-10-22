# Render the editable PulseIt mark into Windows icon/setup formats. No extra packages.
param([string]$Root = (Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$brand = Join-Path $Root 'assets\brand'
$setup = Join-Path $Root 'assets\installer'
New-Item -ItemType Directory $brand,$setup -Force | Out-Null
function Draw-Mark($Graphics, [float]$X, [float]$Y, [float]$Size) {
    $state = $Graphics.Save()
    $Graphics.TranslateTransform($X,$Y); $Graphics.ScaleTransform($Size/256,$Size/256)
    $Graphics.SmoothingMode = 'AntiAlias'
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    foreach ($arc in @(@(0,0,0,180),@(192,0,0,270),@(192,192,0,0),@(0,192,0,90))) {
        $path.AddArc([float]$arc[0],[float]$arc[1],64,64,[float]$arc[3],90)
    }
    $path.CloseFigure()
    $gradient = [Drawing.Drawing2D.LinearGradientBrush]::new([Drawing.Rectangle]::new(0,0,256,256),[Drawing.Color]::FromArgb(90,180,255),[Drawing.Color]::FromArgb(10,96,235),60)
    $Graphics.FillPath($gradient,$path)
    $pen = [Drawing.Pen]::new([Drawing.Color]::White,16)
    $pen.StartCap='Round'; $pen.EndCap='Round'; $pen.LineJoin='Round'
    $points = [Drawing.PointF[]]@([Drawing.PointF]::new(52,136),[Drawing.PointF]::new(88,136),[Drawing.PointF]::new(108,88),[Drawing.PointF]::new(140,172),[Drawing.PointF]::new(164,120),[Drawing.PointF]::new(204,120))
    $Graphics.DrawLines($pen,$points)
    $pen.Dispose(); $gradient.Dispose(); $path.Dispose(); $Graphics.Restore($state)
}
function Mark-Bitmap([int]$Size) {
    $bitmap = [Drawing.Bitmap]::new($Size,$Size)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    Draw-Mark $graphics 0 0 $Size
    $graphics.Dispose()
    return ,$bitmap
}
$logo = Mark-Bitmap 1024
$logo.Save((Join-Path $brand 'pulseit.png'),[Drawing.Imaging.ImageFormat]::Png); $logo.Dispose()
$sizes = @(16,24,32,48,64,128,256)
$streams = @()
foreach ($size in $sizes) {
    $bitmap = Mark-Bitmap $size
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $streams += ,$stream.ToArray(); $stream.Dispose(); $bitmap.Dispose()
}
$file = [IO.File]::Create((Join-Path $brand 'PulseIt.ico'))
$writer = [IO.BinaryWriter]::new($file)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($index=0; $index -lt $sizes.Count; $index++) {
    $dimension = if ($sizes[$index] -eq 256) { 0 } else { $sizes[$index] }
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$streams[$index].Length); $writer.Write([uint32]$offset)
    $offset += $streams[$index].Length
}
foreach ($bytes in $streams) { $writer.Write([byte[]]$bytes) }
$writer.Dispose()
# Twice the standard wizard panel dimensions for a sharp high-DPI setup.
$panel = [Drawing.Bitmap]::new(328,628)
$g = [Drawing.Graphics]::FromImage($panel); $g.ScaleTransform(2,2)
$g.Clear([Drawing.Color]::FromArgb(13,20,36)); $g.TextRenderingHint='AntiAliasGridFit'
Draw-Mark $g 24 28 64
$white = [Drawing.SolidBrush]::new([Drawing.Color]::White)
$muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(160,180,205))
$title = [Drawing.Font]::new('Segoe UI',17,[Drawing.FontStyle]::Bold,[Drawing.GraphicsUnit]::Pixel)
$body = [Drawing.Font]::new('Segoe UI',13,[Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
$small = [Drawing.Font]::new('Segoe UI',10,[Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
$g.DrawString('PulseIt',$title,$white,22,110)
$g.DrawString("From phone.`nTo PC.`nAnd back.",$body,$muted,22,146)
$g.DrawString("Windows x64`nDeveloper preview",$small,$muted,22,266)
$panel.Save((Join-Path $setup 'wizard.bmp'),[Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $panel.Dispose(); $white.Dispose(); $muted.Dispose(); $title.Dispose(); $body.Dispose(); $small.Dispose()
$smallMark = [Drawing.Bitmap]::new(110,110)
$g = [Drawing.Graphics]::FromImage($smallMark); $g.Clear([Drawing.Color]::White)
Draw-Mark $g 10 10 90
$smallMark.Save((Join-Path $setup 'wizard-small.bmp'),[Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $smallMark.Dispose()
Write-Output 'Brand icons and setup panels rendered.'
