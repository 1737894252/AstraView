#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\artifacts\publish'))

$ErrorActionPreference = 'Stop'
$logPath = Join-Path $PSScriptRoot 'install.log'
Start-Transcript -Path $logPath -Force | Out-Null
$resolved = (Resolve-Path $BuildDirectory).Path
$viewer = Join-Path $resolved 'AstraView.exe'
$provider = Join-Path $resolved 'StarImageViewer.ThumbnailProvider.dll'
$regasm = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\RegAsm.exe'

if (-not (Test-Path $viewer)) { throw "找不到 $viewer，请先运行 scripts\publish.ps1" }
if (-not (Test-Path $provider)) { throw "找不到 $provider，请先运行 scripts\publish.ps1" }

& $regasm $provider /codebase /nologo
if ($LASTEXITCODE -ne 0) { throw "缩略图处理器注册失败 ($LASTEXITCODE)" }

$extensions = @('.3fr','.arw','.avif','.bmp','.cr2','.cr3','.crw','.dcr','.dds','.dng','.emf','.erf','.exr','.gif','.heic','.heif','.ico','.jfif','.jpe','.jpeg','.jpg','.jxl','.kdc','.miff','.mos','.mrw','.nef','.nrw','.orf','.pbm','.pcx','.pef','.pgm','.png','.pnm','.ppm','.psb','.psd','.raf','.raw','.rw2','.rwl','.sgi','.sr2','.srf','.svg','.svgz','.tga','.tif','.tiff','.webp','.wmf','.x3f','.xbm','.xpm')
$progId = 'AstraView.Image'
New-Item "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\$progId\shell\open\command" -Force | Out-Null
Set-ItemProperty "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\$progId" -Name '(default)' -Value '图片文件'
Set-ItemProperty "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\$progId\shell\open\command" -Name '(default)' -Value ('"{0}" "%1"' -f $viewer)
$capabilities = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\AstraView\Capabilities'
New-Item "$capabilities\FileAssociations" -Force | Out-Null
Set-ItemProperty $capabilities -Name 'ApplicationName' -Value 'AstraView'
Set-ItemProperty $capabilities -Name 'ApplicationDescription' -Value '支持 PSD、RAW、HEIC、WebP 等格式的图片查看器'
New-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RegisteredApplications' -Force | Out-Null
Set-ItemProperty 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RegisteredApplications' -Name 'AstraView' -Value 'SOFTWARE\AstraView\Capabilities'
foreach ($extension in $extensions) {
    New-Item "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\$extension\OpenWithProgids" -Force | Out-Null
    New-ItemProperty "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\$extension\OpenWithProgids" -Name $progId -Value ([byte[]]@()) -PropertyType Binary -Force | Out-Null
    New-ItemProperty "$capabilities\FileAssociations" -Name $extension -Value $progId -PropertyType String -Force | Out-Null
}

Write-Host '安装完成：Explorer 缩略图已注册。'
Write-Host '首次双击图片时，请在“打开方式”中选择“AstraView”并勾选“始终使用”。'
Stop-Transcript | Out-Null
Read-Host '按 Enter 关闭'
