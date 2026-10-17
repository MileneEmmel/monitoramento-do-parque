# Firmware

`object_detect/` contém as adaptações sobre ESP-WHO, não uma cópia completa do framework. `preparar.ps1` baixa a revisão fixada, seus submódulos e aplica esses arquivos. Consulte a sequência de compilação no README da raiz.

## Ciclo de desenvolvimento

Edite os arquivos de `firmware/object_detect/` e reaplique:

```powershell
.\atualizar-firmware.ps1
```

Por padrão, o checkout é `esp-who/` dentro do repositório. Para um checkout externo existente:

```powershell
.\atualizar-firmware.ps1 -Checkout 'C:\caminho\esp-who'
```

O script confere revisão e estrutura antes de sobrepor arquivos. Não apaga o checkout. Ao compilar um checkout externo, informe o caminho absoluto do dashboard:

```powershell
$idfArgs = @(
  '-DSDKCONFIG_DEFAULTS=sdkconfig.freenove;sdkconfig.bsp.esp32_s3_eye_noglib',
  '-DDETECT_MODEL=pedestrian_detect',
  '-DPARK_DASHBOARD_DIR=C:/caminho/monitoramento-do-parque/dashboard'
)
idf.py @idfArgs reconfigure
idf.py @idfArgs build
```

Mantenha no terminal as configurações do exemplo: `IDF_TARGET=esp32s3`, `IDF_EXTRA_ACTIONS_PATH` apontando para `esp-who/tools`, `SDKCONFIG_DEFAULTS=sdkconfig.freenove;sdkconfig.bsp.esp32_s3_eye_noglib` e `DETECT_MODEL=pedestrian_detect`.

Para gravar na placa compatível, após conferir porta, pinagem e alimentação:

```powershell
idf.py @idfArgs -p COM5 flash
```

Não use `erase-flash` numa atualização normal. A compilação gera binários que não pertencem ao Git. Atualizar fontes não altera a placa até uma nova gravação.

## Pinagem usada

| Interface | Sinais/GPIO |
| --- | --- |
| Câmera SCCB | SDA 4, SCL 5 |
| Câmera D0–D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| Câmera sincronização | VSYNC 6, HREF 7, PCLK 13, XCLK 15 |
| microSD, 1 bit | CMD 38, CLK 39, D0 40 |

A GC0308 usa XCLK de 20 MHz. A configuração inclui PSRAM octal e flash de 8 MB. O PIR HC-SR501 usa GPIO1 como entrada de diagnóstico, VCC em 5V e GND comum. Não reutilize pinos da câmera ou do cartão.

## Diagnóstico do PIR

Após gravar o firmware atualizado, execute `python firmware/object_detect/check_pir.py --port COM5 --seconds 60` na raiz. O script consulta apenas `@PIR_STATUS`, sem reiniciar a placa nem registrar outras mensagens USB. Aguarde a estabilização do sensor antes de comparar movimento e repouso. Um sinal constante é inconclusivo, e transições durante a estabilização não comprovam detecção correta.

Nesta branch o PIR controla a ativação da câmera e o despertar do deep sleep. O diagnóstico apenas lê o GPIO1; não o reconfigura durante a preparação do sono. Um sinal constante não identifica sozinho sensor ausente ou fio solto. Consulte [energia e PIR](../docs/ENERGIA.md), incluindo o modo contínuo de recuperação por USB.

## Configuradores USB

`configure_wifi.py` configura ou consulta a rede; `configure_cloud.py` configura ou consulta o envio. Ambos usam `pyserial`, UART a 115200 e validação da identidade USB da placa deste projeto. Feche monitores antes de executá-los.

O wrapper `configure_wifi.ps1` aceita `-Python`, `-Port` e `-OpenSaved`; não exige uma instalação Python em caminho específico.

## Testes

`test_configure_wifi.py` e `test_configure_cloud.py` usam mocks, sem abrir a ESP. `main/counting_core_tests.cpp` exercita o núcleo de rastreamento e é chamado durante a inicialização. Testes sintéticos não substituem medição de precisão em campo.

O [guia do código](../docs/CODIGO.md) descreve cada módulo e suas relações.
