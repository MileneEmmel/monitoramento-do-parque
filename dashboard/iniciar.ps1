$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' '..\esp-who\examples\object_detect\configure_wifi.py' --status
} finally {
    Pop-Location
}
