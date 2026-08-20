[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
& (Join-Path $PSScriptRoot 'publish.ps1')
if ($LASTEXITCODE -ne 0) { throw "应用发布失败 ($LASTEXITCODE)" }

$candidates = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
)
$compiler = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $compiler) { throw '未找到 Inno Setup 6，请先安装后重试。' }

& $compiler (Join-Path $root 'installer\StarImageViewer.iss')
if ($LASTEXITCODE -ne 0) { throw "安装包编译失败 ($LASTEXITCODE)" }
Write-Host "安装包: $(Join-Path $root 'artifacts\installer\AstraView-Setup-1.3.2-x64.exe')"
