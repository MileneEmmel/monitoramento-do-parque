$ErrorActionPreference = 'Stop'
$revision = '1abda05e1c0782237fcb9e8d33a2fa7e105f06b2'
$checkout = Join-Path $PSScriptRoot 'esp-who'
if (Test-Path -LiteralPath $checkout) {
    throw 'A pasta esp-who já existe. Não vou sobrescrever seu trabalho; use uma cópia limpa deste repositório.'
}
git clone --no-checkout https://github.com/espressif/esp-who.git $checkout
if ($LASTEXITCODE -ne 0) { throw 'Falha ao clonar ESP-WHO.' }
git -C $checkout checkout --detach $revision
if ($LASTEXITCODE -ne 0) { throw 'Falha ao selecionar a revisão fixada.' }
git -C $checkout submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'Falha ao carregar submódulos.' }
$destination = Join-Path $checkout 'examples/object_detect'
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'firmware/object_detect') | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Recurse -Force
}
Write-Host 'Base preparada. Abra um terminal ESP-IDF e siga o README para compilar.'

