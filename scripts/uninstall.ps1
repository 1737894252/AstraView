#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\artifacts\publish'))

$ErrorActionPreference = 'Stop'
$provider = Join-Path ((Resolve-Path $BuildDirectory).Path) 'StarImageViewer.ThumbnailProvider.dll'
$regasm = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\RegAsm.exe'
if (Test-Path $provider) { & $regasm $provider /unregister /nologo }
Remove-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\AstraView.Image' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\AstraView' -Recurse -Force -ErrorAction SilentlyContinue
Remove-ItemProperty 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RegisteredApplications' -Name 'AstraView' -Force -ErrorAction SilentlyContinue
Write-Host '卸载完成。'
