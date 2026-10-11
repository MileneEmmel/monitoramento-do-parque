# Monitoramento do parque

Contagem aproximada de entradas e saídas para apoiar relatórios de visitação.
Detecção de pessoas e rastreamento executados em uma Freenove ESP32-S3 N8R8,
com câmera GC0308 e histórico no microSD. Não identifica visitantes.

## Estado do projeto

- Contagem, calibração, câmera local, histórico e exportação funcionam no firmware.
- A prévia JPEG é preparada sob demanda de um espectador autenticado.
- Migração em andamento: gráficos e histórico online; câmera e ajustes na ESP.
- Supabase Free com banco, RLS, ingestão restrita e testes de isolamento/duplicação aplicados.
- Dashboard online publicado em Sites, privado para a proprietária; requer login ChatGPT da hospedagem e login Supabase da aplicação.
- Firmware com sincronização HTTPS gravado na ESP; primeiro envio real e recuperação offline ainda aguardam validação.
- PIR, deep sleep, bateria e precisão em campo ainda precisam de integração/testes.

O site atual continua na ESP até o substituto online estar verificado.
Nenhuma imagem de visitante ou credencial deve entrar neste repositório.

## Organização

- `dashboard/public/`: interface atual e seus recursos locais.
- `cloud/public/`: dashboard online sem câmera, com login, gráficos, histórico e exportação.
- `cloud/test_cloud.cjs`: regressões do painel online.
- `dashboard/test_frontend.cjs`: regressões de interface e precisão numérica.
- `dashboard/NUVEM.md`: arquitetura aprovada para a migração.
- `firmware/object_detect/`: arquivos adaptados, sobrepostos ao exemplo ESP-WHO.
- `preparar.ps1`: baixa a base fixada e aplica essas adaptações.
- `supabase/README.md`: projeto cloud e cuidados de implantação.

## Preparar o firmware

Requer Git e ESP-IDF 5.5.5 instalado. Na raiz deste repositório:

```powershell
.\preparar.ps1
```

Em um terminal com o ambiente ESP-IDF ativo:

```powershell
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path .\esp-who\tools).Path
$env:IDF_TARGET = 'esp32s3'
cd .\esp-who\examples\object_detect
idf.py '-DSDKCONFIG_DEFAULTS=sdkconfig.freenove;sdkconfig.bsp.esp32_s3_eye_noglib' '-DDETECT_MODEL=pedestrian_detect' set-target esp32s3
idf.py '-DSDKCONFIG_DEFAULTS=sdkconfig.freenove;sdkconfig.bsp.esp32_s3_eye_noglib' '-DDETECT_MODEL=pedestrian_detect' build
```

Só grave em uma placa com pinagem compatível, após conferir as configurações.
O roteiro de clonagem limpa ainda precisa ser testado fora do ambiente original.
Não formate o microSD nem apague a NVS durante uma atualização normal.

## Teste da interface

```powershell
cd dashboard
node test_frontend.cjs
```

## Base e licenças

Painel publicado: https://monitoramento-parque.vercel.app/.
O histórico exige login e RLS do Supabase. O painel original na ESP foi preservado.
O endereço anterior em chatgpt.site continua privado, mas não é o endereço principal.
Prepare/teste com `node cloud/build.mjs` e `node cloud/test_cloud.cjs` na raiz.
Não é necessário manter um servidor no notebook para a versão publicada.

Base: [Espressif ESP-WHO](https://github.com/espressif/esp-who), revisão
`1abda05e1c0782237fcb9e8d33a2fa7e105f06b2`.
As adaptações do exemplo preservam a licença da base em `firmware/LICENSE-ESP-WHO`.
A licença da fonte está em `dashboard/FONT-LICENSE.txt`.
Não concedemos aqui uma nova licença geral aos demais recursos do projeto.

## Segurança e limitações

Não expor portas da ESP à internet. A interface HTTP local é para redes confiáveis.
Links com chave da câmera, senhas de Wi-Fi, tokens de dispositivo, `.env`, cópias
do microSD e logs de diagnóstico não devem ser enviados ao GitHub.
Dados da nuvem devem ter RLS e login; chaves administrativas não vão no firmware.
Não há promessa de contagem de visitantes únicos, precisão validada ou ocupação.

