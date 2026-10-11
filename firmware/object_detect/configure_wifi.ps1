param([string]$Port = 'COM5', [switch]$OpenSaved)
$ErrorActionPreference = 'Stop'
$wifiArgs = @((Join-Path $PSScriptRoot 'configure_wifi.py'), '--port', $Port)
if ($OpenSaved) { $wifiArgs += '--status' }
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' @wifiArgs
