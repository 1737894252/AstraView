$ErrorActionPreference = 'Stop'
try {
    $installer = Join-Path $PSScriptRoot 'install.ps1'
    if (-not (Test-Path -LiteralPath $installer)) {
        throw "Installer not found: $installer"
    }

    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $installer
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -PassThru
    $process.WaitForExit()
    exit $process.ExitCode
}
catch {
    Write-Host ''
    Write-Host 'Unable to start the elevated installer:' -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Yellow
    exit 1
}
