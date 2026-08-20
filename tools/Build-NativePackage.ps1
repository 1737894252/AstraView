[CmdletBinding()]
param(
    [switch]$Installer
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$configuration = 'Release'
$stage = Join-Path $root 'artifacts\native-package-2.0.0-preview.3'
$nativeProject = Join-Path $root 'src\AstraView.Native\AstraView.Native.vcxproj'
$providerProject = Join-Path $root 'src\StarImageViewer.NativeThumbnailProvider\StarImageViewer.NativeThumbnailProvider.vcxproj'
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$pdfium = Join-Path $root '.nuget\packages\bblanchon.pdfium.win32\152.0.7961\runtimes\win-x64\native\pdfium.dll'
$magick = Join-Path $root 'tools\vendor\ImageMagick-Q8'

if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild was not found: $msbuild" }
if (-not (Test-Path -LiteralPath $magick)) { throw 'ImageMagick-Q8 runtime is missing. Restore tools\vendor\ImageMagick-Q8 before packaging.' }

& $msbuild $nativeProject /m "/p:Configuration=$configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Native viewer build failed.' }
& $msbuild $providerProject /m "/p:Configuration=$configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Native thumbnail provider build failed.' }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage, (Join-Path $stage 'ShellExtension'), (Join-Path $stage 'modules\coders') | Out-Null

$nativeOutput = Join-Path $root 'artifacts\native\Release\AstraView.Native.exe'
$providerOutput = Join-Path $root 'src\StarImageViewer.NativeThumbnailProvider\x64\Release\AstraView.ThumbnailProvider.dll'
Copy-Item -LiteralPath $nativeOutput -Destination (Join-Path $stage 'AstraView.exe')
Copy-Item -LiteralPath $pdfium -Destination $stage
Copy-Item -LiteralPath $providerOutput -Destination (Join-Path $stage 'ShellExtension\AstraView.ThumbnailProvider.dll')
Copy-Item -LiteralPath $pdfium -Destination (Join-Path $stage 'ShellExtension\pdfium.dll')

$runtimeFiles = @(
    'CORE_RL_MagickWand_.dll', 'CORE_RL_MagickCore_.dll', 'CORE_RL_bzip2_.dll',
    'CORE_RL_freetype_.dll', 'CORE_RL_lcms_.dll', 'CORE_RL_lqr_.dll',
    'CORE_RL_raqm_.dll', 'CORE_RL_xml_.dll', 'CORE_RL_zlib_.dll',
    'CORE_RL_glib_.dll', 'CORE_RL_fribidi_.dll', 'CORE_RL_harfbuzz_.dll',
    'vcomp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'
)
foreach ($name in $runtimeFiles) {
    $source = Join-Path $magick $name
    if (-not (Test-Path -LiteralPath $source)) { throw "ImageMagick runtime file is missing: $name" }
    Copy-Item -LiteralPath $source -Destination $stage
}
foreach ($name in @('IM_MOD_RL_psd_.dll', 'IM_MOD_RL_raw_.dll')) {
    $source = Join-Path $magick (Join-Path 'modules\coders' $name)
    if (-not (Test-Path -LiteralPath $source)) { throw "ImageMagick coder module is missing: $name" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage 'modules\coders')
}

Write-Host "Native package stage created: $stage"
Get-ChildItem -LiteralPath $stage -Recurse -File | Measure-Object -Property Length -Sum | ForEach-Object {
    Write-Host ('Payload size: {0:N1} MiB' -f ($_.Sum / 1MB))
}

if ($Installer) {
    $iscc = @('C:\Program Files (x86)\Inno Setup 6\ISCC.exe', 'C:\Program Files\Inno Setup 6\ISCC.exe') | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 was not found. The staged native payload is ready, but the installer was not created.' }
    & $iscc (Join-Path $root 'installer\AstraView.Native.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }
}
