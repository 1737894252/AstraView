$ErrorActionPreference = 'Stop'
try {
    $uninstaller = Join-Path $PSScriptRoot 'uninstall.ps1'
    if (-not (Test-Path -LiteralPath $uninstaller)) {
        throw "Uninstaller not found: $uninstaller"
    }
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $uninstaller
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -PassThru
    $process.WaitForExit()
    exit $process.ExitCode
}
catch {
    Write-Host ''
    Write-Host 'Unable to start the elevated uninstaller:' -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Yellow
    exit 1
}
