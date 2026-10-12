param([string]$Checkout = (Join-Path $PSScriptRoot 'esp-who'))
$ErrorActionPreference = 'Stop'
$revision = '1abda05e1c0782237fcb9e8d33a2fa7e105f06b2'
$checkoutPath = (Resolve-Path -LiteralPath $Checkout).Path
$actual = & git -C $checkoutPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -ne $revision) {
    throw 'O checkout não está na revisão ESP-WHO fixada pelo projeto. Nada foi copiado.'
}
$destination = Join-Path $checkoutPath 'examples/object_detect'
if (-not (Test-Path -LiteralPath (Join-Path $destination 'CMakeLists.txt'))) {
    throw 'Estrutura object_detect não encontrada. Nada foi copiado.'
}
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'firmware/object_detect') | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Recurse -Force
}
Write-Host 'Adaptações aplicadas. Compile antes de gravar; a placa não foi alterada.'
Write-Host "Dashboard: $(Join-Path $PSScriptRoot 'dashboard')"
