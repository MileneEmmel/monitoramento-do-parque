param([string]$Port = 'COM5', [switch]$OpenSaved, [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$wifiArgs = @((Join-Path $PSScriptRoot 'configure_wifi.py'), '--port', $Port)
if ($OpenSaved) { $wifiArgs += '--status' }
& $Python @wifiArgs
if ($LASTEXITCODE -ne 0) { throw 'O configurador não concluiu. Consulte a mensagem acima.' }
