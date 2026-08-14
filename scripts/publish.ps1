[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $root 'artifacts\publish'
$env:DOTNET_CLI_HOME = Join-Path $root '.dotnet-cli'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:NUGET_PACKAGES = Join-Path $root '.nuget\packages'
dotnet restore (Join-Path $root 'StarImageViewer.sln') --configfile (Join-Path $root 'NuGet.Config')
if ($LASTEXITCODE -ne 0) { throw "dotnet restore 失败 ($LASTEXITCODE)" }
if (Test-Path -LiteralPath $output) {
    $expectedParent = (Join-Path $root 'artifacts')
    if (-not $output.StartsWith($expectedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理非发布目录: $output"
    }
    Remove-Item -LiteralPath $output -Recurse -Force
}
New-Item $output -ItemType Directory -Force | Out-Null
dotnet publish (Join-Path $root 'src\StarImageViewer\StarImageViewer.csproj') -c $Configuration -p:Platform=x64 -o $output
if ($LASTEXITCODE -ne 0) { throw "主程序发布失败 ($LASTEXITCODE)" }
dotnet publish (Join-Path $root 'src\StarImageViewer.ThumbnailProvider\StarImageViewer.ThumbnailProvider.csproj') -c $Configuration -p:Platform=x64 -o $output
if ($LASTEXITCODE -ne 0) { throw "缩略图组件发布失败 ($LASTEXITCODE)" }

# PDFtoImage targets several operating systems and CPU architectures. AstraView is an x64 Windows
# application, and the required x64 DLLs are already copied to the publish root.
$unusedNativeDirectories = @(
    'arm', 'arm64', 'bionic-arm64', 'bionic-x64', 'loongarch64', 'musl-arm',
    'musl-arm64', 'musl-loongarch64', 'musl-riscv64', 'musl-x64', 'musl-x86',
    'riscv64', 'x64', 'x86'
)
foreach ($name in $unusedNativeDirectories) {
    $path = Join-Path $output $name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
Get-ChildItem -LiteralPath $output -File | Where-Object {
    $_.Extension -in @('.so', '.dylib', '.pdb')
} | Remove-Item -Force

Write-Host "发布目录: $output"
