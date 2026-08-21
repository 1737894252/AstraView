[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $root 'artifacts\publish'
$appOutput = Join-Path $root 'artifacts\publish-app'
$shellOutput = Join-Path $output 'ShellExtension'
$workerProject = Join-Path $root 'src\StarImageViewer.ThumbnailWorker\StarImageViewer.ThumbnailWorker.csproj'
$nativeProject = Join-Path $root 'src\StarImageViewer.NativeThumbnailProvider\StarImageViewer.NativeThumbnailProvider.vcxproj'
$env:DOTNET_CLI_HOME = Join-Path $root '.dotnet-cli'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:NUGET_PACKAGES = Join-Path $root '.nuget\packages'
dotnet restore (Join-Path $root 'src\StarImageViewer\StarImageViewer.csproj') --configfile (Join-Path $root 'NuGet.Config')
if ($LASTEXITCODE -ne 0) { throw "dotnet restore 失败 ($LASTEXITCODE)" }
dotnet restore $workerProject -r win-x64 --configfile (Join-Path $root 'NuGet.Config')
if ($LASTEXITCODE -ne 0) { throw "缩略图工作进程还原失败 ($LASTEXITCODE)" }
foreach ($cleanPath in @($output, $appOutput)) {
if (Test-Path -LiteralPath $cleanPath) {
    $expectedParent = (Join-Path $root 'artifacts')
    if (-not $cleanPath.StartsWith($expectedParent + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理非发布目录: $cleanPath"
    }
    Remove-Item -LiteralPath $cleanPath -Recurse -Force
}
}
New-Item $appOutput -ItemType Directory -Force | Out-Null
New-Item $shellOutput -ItemType Directory -Force | Out-Null
dotnet publish (Join-Path $root 'src\StarImageViewer\StarImageViewer.csproj') -c $Configuration -p:Platform=x64 -r win-x64 --self-contained true -o $appOutput
if ($LASTEXITCODE -ne 0) { throw "主程序发布失败 ($LASTEXITCODE)" }
Copy-Item -Path (Join-Path $appOutput '*') -Destination $output -Recurse -Force
dotnet publish $workerProject -c $Configuration -p:Platform=x64 -r win-x64 --self-contained true -o $appOutput --no-restore
if ($LASTEXITCODE -ne 0) { throw "缩略图工作进程发布失败 ($LASTEXITCODE)" }
Copy-Item -Path (Join-Path $appOutput '*') -Destination $output -Recurse -Force

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw '找不到 Visual Studio Build Tools（vswhere.exe）' }
$msbuildRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $msbuildRoot) { throw '找不到带有 C++ x64 工具链的 Visual Studio Build Tools' }
$msbuild = Join-Path $msbuildRoot 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild $nativeProject /restore /m /p:Configuration=$Configuration /p:Platform=x64 "/p:OutDir=$shellOutput\" /p:TargetName=AstraView.ThumbnailProvider
if ($LASTEXITCODE -ne 0) { throw "原生缩略图组件构建失败 ($LASTEXITCODE)" }

# Explorer loads the COM thumbnail provider from ShellExtension, so its linked
# PDFium dependency must live beside the provider DLL (the application root is
# not part of regsvr32/dllhost's DLL search path).
$pdfium = Join-Path $output 'pdfium.dll'
if (-not (Test-Path -LiteralPath $pdfium)) { throw "找不到 PDFium 运行库: $pdfium" }
Copy-Item -LiteralPath $pdfium -Destination (Join-Path $shellOutput 'pdfium.dll') -Force

# PDFtoImage targets several operating systems and CPU architectures. AstraView is an x64 Windows
# application, and the required x64 DLLs are already copied to the publish root.
$unusedNativeDirectories = @(
    'arm', 'arm64', 'bionic-arm64', 'bionic-x64', 'loongarch64', 'musl-arm',
    'musl-arm64', 'musl-loongarch64', 'musl-riscv64', 'musl-x64', 'musl-x86',
    'riscv64', 'x64', 'x86'
)
foreach ($publishRoot in @($output, $shellOutput)) {
foreach ($name in $unusedNativeDirectories) {
    $path = Join-Path $publishRoot $name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
}
Get-ChildItem -LiteralPath $output -File -Recurse | Where-Object {
    $_.Extension -in @('.so', '.dylib', '.pdb', '.lib', '.exp', '.iobj', '.ipdb')
} | Remove-Item -Force

Write-Host "发布目录: $output"
