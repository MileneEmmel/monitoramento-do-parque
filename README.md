# Monitoramento do parque

Sistema de contagem aproximada de entradas e saídas para apoiar relatórios de visitação de parques e unidades de conservação. A detecção e o rastreamento de pessoas acontecem em uma ESP32-S3. O microSD guarda os registros, e o dashboard permite consultar o histórico pela internet.

[Dashboard](https://monitoramento-parque.vercel.app/) · [Guia do código](docs/CODIGO.md) · [Operação e diagnóstico](docs/OPERACAO.md) · [Revisão técnica](docs/REVISAO.md)

O sistema conta passagens. Uma pessoa que sai e retorna pode gerar outra entrada; os resultados não representam visitantes únicos nem ocupação garantida. O objetivo é estimar o fluxo, sem reconhecimento facial.

## Funcionamento

```text
Câmera → detector de pessoas → rastreamento → cruzamento das linhas A/B
                                               │
                                               ▼
                                        microSD da ESP
                                               │ HTTPS, com reenvio
                                               ▼
                                      Supabase → dashboard

Celular na mesma rede → ESP → imagem e ajustes de instalação
```

A ESP inicia a contagem ao ligar, desde que a câmera e o detector inicializem. Não é necessário abrir um site ou manter um notebook ligado. Pausar a imagem interrompe as solicitações de prévia; a detecção continua em uma tarefa independente.

O dashboard online contém gráficos, histórico, exportação CSV e identificação editável do parque. A interface local da ESP contém câmera, calibração e Wi-Fi. O vídeo não passa pela nuvem e não é gravado no microSD.

## Hardware e software

| Componente | Configuração do projeto |
| --- | --- |
| Placa | Freenove ESP32-S3-WROOM N8R8, 8 MB de flash e 8 MB de PSRAM |
| Câmera instalada | GC0308, captura RGB565 de 320 × 240 pixels |
| Armazenamento | microSD em FAT/FAT32, SDMMC de 1 bit |
| Rede | Wi-Fi de 2,4 GHz, modo estação; internet para sincronização |
| Firmware | C++ com ESP-IDF 5.5.5 e ESP-WHO |
| Modelo | `pedestrian_detect`, da Espressif |
| Dashboard | HTML, CSS e JavaScript |
| Banco e autenticação | Supabase/PostgreSQL, Auth e políticas RLS |
| Hospedagem | Vercel |

A base ESP-WHO está fixada na revisão `1abda05e1c0782237fcb9e8d33a2fa7e105f06b2`. O firmware é uma adaptação do exemplo `object_detect`, aplicada sobre essa base.

O PIR possui uma leitura de diagnóstico no GPIO1; ainda não controla a detecção ou o consumo. Deep sleep e operação por bateria não estão implementados. Precisão em campo e autonomia energética precisam ser medidas. A falha intermitente observada no microSD requer teste prolongado; não é tratada como definitivamente resolvida.

## Organização

```text
monitoramento-do-parque/
├── docs/                       mapa do código, operação e revisão técnica
├── firmware/object_detect/     firmware adaptado e configuração USB
├── dashboard/local-public/     câmera e ajustes servidos pela ESP
├── dashboard/public/           CSS, fonte e ilustração compartilhados
├── cloud/public/               dashboard online
├── supabase/                   migrações SQL e testes do banco
├── preparar.ps1                obtém a base e aplica o firmware
└── atualizar-firmware.ps1       reaplica alterações ao checkout de trabalho
```

`esp-who/`, `cloud/dist/`, binários e caches são gerados localmente e não entram no Git. Edite as adaptações em `firmware/`, não apenas a cópia dentro de `esp-who/`.

Comece pelo [guia do código](docs/CODIGO.md). Os guias de [firmware](firmware/README.md), [interface local](dashboard/README.md), [dashboard online](cloud/README.md) e [banco](supabase/README.md) detalham cada parte.

## Preparar e compilar

Requer Git com submódulos, ESP-IDF 5.5.5 com ferramentas ESP32-S3 e terminal com o ambiente IDF ativo. Node.js 22 ou superior é usado no dashboard. Os configuradores USB precisam de Python com `pyserial`.

Em uma cópia limpa do repositório:

```powershell
.\preparar.ps1
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path .\esp-who\tools).Path
$env:IDF_TARGET = 'esp32s3'
cd .\esp-who\examples\object_detect
idf.py '-DSDKCONFIG_DEFAULTS=sdkconfig.freenove;sdkconfig.bsp.esp32_s3_eye_noglib' '-DDETECT_MODEL=pedestrian_detect' set-target esp32s3
idf.py '-DSDKCONFIG_DEFAULTS=sdkconfig.freenove;sdkconfig.bsp.esp32_s3_eye_noglib' '-DDETECT_MODEL=pedestrian_detect' build
```

A preparação recusa sobrescrever um checkout existente. Após editar `firmware/`, execute `atualizar-firmware.ps1` na raiz e compile novamente. Consulte o [guia do firmware](firmware/README.md) antes de gravar.

A compilação no ambiente de desenvolvimento não substitui a validação de uma instalação limpa em outra máquina.

## Configuração inicial

Conecte a ESP por USB e feche o monitor serial. No ambiente Python com `pyserial`:

```powershell
python .\firmware\object_detect\configure_wifi.py --port COM5
python .\firmware\object_detect\configure_cloud.py --port COM5
```

O primeiro configurador solicita a rede e abre a interface local. O segundo solicita o ID e a credencial exclusiva de envio gerada pelo proprietário no dashboard. Senhas e tokens são solicitados de forma oculta e não são gravados em arquivos no computador.

Os configuradores verificam a identificação USB da Freenove deste projeto. Outra placa exige revisar essa identificação e a pinagem, não apenas o número da COM.

Para abrir a câmera sem alterar a rede salva:

```powershell
python .\firmware\object_detect\configure_wifi.py --port COM5 --status
```

O IP pode mudar entre redes. O proprietário também pode abrir a câmera pela página Equipamento do dashboard, quando houver atualização recente da ESP. O celular precisa estar na mesma rede local, sem isolamento entre dispositivos. Descobrir o endereço não transmite vídeo pela internet.

## Histórico

O cartão mantém arquivos diários em `/park-monitor-v1/` e cursores de sincronização em `/park-cloud-v1/`. A ESP persiste o cursor depois da confirmação HTTPS do lote. Reenvios de uma linha já recebida são deduplicados no banco.

Os registros incluem horário, deltas de entradas e saídas e informações de integridade. Totais acumulados e calibração também têm checkpoints na NVS; eles não substituem o histórico diário.

Sem internet, a ESP pode continuar registrando no cartão. O envio exige Wi-Fi, credencial e horário válidos. Eventos anteriores à sincronização do relógio ficam em arquivos sem data e não são atribuídos automaticamente a um dia. Desligamentos abruptos podem perder alterações ainda não persistidas.

Não há exclusão automática. Se o cartão encher ou falhar, novas gravações podem falhar. Existe uma API local de limpeza com prévia e confirmação; a interface atual não oferece botão de limpeza. Consulte [operação](docs/OPERACAO.md).

## Testes

Na raiz do repositório:

```powershell
node dashboard/test_local.cjs
node cloud/test_cloud.cjs
node cloud/test_build.mjs
node cloud/build.mjs
python -m unittest discover -s firmware/object_detect -p 'test_*.py'
```

O núcleo de rastreamento tem testes C++ executados na inicialização do firmware. `supabase/tests.sql` e `supabase/tests_camera.sql` criam dados sintéticos em uma transação e fazem `ROLLBACK`.

Esses testes não medem a precisão da câmera. Antes da instalação definitiva, compare contagem manual e automática nos dois sentidos, incluindo grupos, corridas, oclusões e condições de luz diferentes.

## Publicação

`node cloud/build.mjs` prepara os arquivos públicos em `cloud/dist/`. O procedimento da Vercel está em [cloud/PUBLICACAO.md](cloud/PUBLICACAO.md).

Um push não grava firmware na ESP. A publicação atual também não tem integração Git automática configurada: atualizar o GitHub, sozinho, não atualiza o site. As migrações do Supabase são uma etapa separada.

## Segurança

- Histórico protegido por login e RLS; proprietário e leitor têm permissões distintas.
- Credencial de envio limitada à placa. Chaves administrativas não pertencem ao firmware nem ao frontend.
- Chave publishable e ID da placa são públicos e não substituem autenticação.
- Câmera local com chave de acesso e HTTP: usar em rede confiável, sem encaminhar portas para a internet.
- Endereço e chave da câmera ficam em esquema privado do banco, acessível ao proprietário pela função protegida. Administradores do banco também podem acessá-los.
- A NVS não tem criptografia de flash configurada. Proteja fisicamente placa e cartão.
- Não versionar senhas, tokens, `.env`, logs com credenciais, imagens de visitantes ou cópias do microSD.

## Licenças

A licença da base está em [firmware/LICENSE-ESP-WHO](firmware/LICENSE-ESP-WHO). Source Sans 3 usa SIL OFL, incluída em [dashboard/FONT-LICENSE.txt](dashboard/FONT-LICENSE.txt). Este repositório não concede uma licença geral adicional aos demais recursos.
