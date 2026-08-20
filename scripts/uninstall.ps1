#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\artifacts\publish'))

$ErrorActionPreference = 'Stop'
$provider = Join-Path ((Resolve-Path $BuildDirectory).Path) 'ShellExtension\AstraView.ThumbnailProvider.dll'
if (Test-Path $provider) { & (Join-Path $env:WINDIR 'System32\regsvr32.exe') /s /u $provider }
Remove-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\AstraView.Image' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\AstraView' -Recurse -Force -ErrorAction SilentlyContinue
Remove-ItemProperty 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RegisteredApplications' -Name 'AstraView' -Force -ErrorAction SilentlyContinue
Write-Host '卸载完成。'
