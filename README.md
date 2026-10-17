# Monitoramento do parque

Sistema de estimativa de entradas e saídas de visitantes com câmera e inteligência artificial na ESP32-S3. A placa processa as imagens, acompanha as pessoas que cruzam duas linhas virtuais, registra as passagens no microSD e sincroniza o histórico com o Supabase. O painel web permite consultar os dados de qualquer lugar.

Não há reconhecimento facial. As imagens não são enviadas à nuvem: a visualização local serve para posicionar a câmera e ajustar a contagem.

## Organização do código

| Pasta | Conteúdo |
| --- | --- |
| `firmware/main/` | Aplicação embarcada: câmera, contagem, armazenamento, rede, nuvem e gerenciamento de energia. |
| `firmware/components/` | Componentes originais do ESP-WHO necessários para esta aplicação. |
| `dashboard/local-public/` | Página de câmera e configurações servida pela ESP. |
| `dashboard/public/` | Estilos, fonte e ilustração incorporados ao firmware. |
| `cloud/public/` | Painel estático publicado na Vercel, incluindo seus próprios recursos visuais. |
| `supabase/migrations/` | Estrutura do banco, funções de envio e regras de acesso. |

O projeto ESP-IDF começa no `CMakeLists.txt` da raiz. Não depende da pasta externa `esp-who` nem de ferramentas auxiliares para copiar código antes de compilar. Os pacotes de terceiros são obtidos pelo gerenciador de componentes do ESP-IDF; as versões usadas pelo firmware estão fixadas em `firmware/main/idf_component.yml`.

### Módulos principais

- `app_main.cpp` e `frame_cap_pipeline.cpp`: inicialização e processamento dos quadros.
- `counting_core.hpp` e `visitor_counter.cpp`: rastreamento, cruzamento das linhas e contagem por sentido.
- `park_history.cpp`: diário persistente no microSD.
- `park_cloud.cpp`: sincronização autenticada, com retomada dos registros pendentes.
- `wifi_preview.cpp` e `camera_preview.cpp`: rede e acesso local à câmera.
- `pir_sensor.cpp`, `park_power.cpp` e `power_policy.hpp`: presença, condições de repouso e despertar.
- `counting_core_checks.cpp`: verificações internas executadas na inicialização, preservadas do firmware. Não é uma suíte externa de desenvolvimento.

As adaptações desta aplicação estão em `firmware/main/`. Os componentes em `firmware/components/` vieram do [ESP-WHO](https://github.com/espressif/esp-who), revisão `1abda05e1c0782237fcb9e8d33a2fa7e105f06b2`, e mantêm o código original e a licença em `firmware/LICENSE-ESP-WHO`.

## Hardware

- Freenove ESP32-S3 com 8 MB de flash e 8 MB de PSRAM.
- Câmera GC0308, configurada para captura RGB565 em 320 × 240.
- Cartão microSD em FAT32.
- Sensor PIR HC-SR501 na versão com economia de energia.
- Alimentação USB de 5 V e acesso a uma rede Wi-Fi de 2,4 GHz.

| Ligação | Pino da ESP32-S3 |
| --- | --- |
| PIR OUT | GPIO1 |
| PIR VCC / GND | 5V / GND |
| microSD CMD / CLK / D0 | GPIO38 / GPIO39 / GPIO40 |
| Câmera SDA / SCL | GPIO4 / GPIO5 |
| Câmera D0 a D7 | GPIO11, 9, 8, 10, 12, 18, 17, 16 |
| Câmera VSYNC / HREF / PCLK / XCLK | GPIO6 / GPIO7 / GPIO13 / GPIO15 |

O sinal de saída do PIR deve ser compatível com 3,3 V. Faça qualquer alteração de fios com a alimentação desligada; nunca inverta VCC e GND por tentativa.

## Compilação

Utilize ESP-IDF 5.5.5. No VS Code, abra a **raiz deste repositório**, selecione a instalação do ESP-IDF e o alvo `esp32s3`. No terminal do ESP-IDF, execute `idf.py build`. O resultado é `build/monitoramento_parque.bin`.

As configurações iniciais estão em `firmware/sdkconfig.freenove` e `firmware/sdkconfig.defaults`. A tabela de partições está em `partitions.csv`. O diretório `build/`, os componentes baixados, o `sdkconfig` e o `dependencies.lock` gerados são locais e não fazem parte do repositório. O arquivo de dependências gerado contém caminhos específicos do computador; por isso, as versões são fixadas no manifesto da aplicação.

A gravação pode ser feita pela extensão ESP-IDF ou com `idf.py -p COM5 flash`, ajustando a porta ao computador. Não use apagamento completo da flash nem formatação do cartão para atualizar normalmente: essas ações podem remover configurações e registros.

## Funcionamento

No modo contínuo, a placa inicializa a câmera e a detecção após ligar, independentemente de o navegador estar aberto. No modo com PIR, a captura aguarda presença ou solicitação da prévia local. O PIR sinaliza movimento; a contagem de pessoas é feita pela câmera e pelo cruzamento das linhas, não pelo número de pulsos do sensor.

Na versão experimental de baixo consumo, a placa respeita a estabilização inicial do sensor, a janela mínima de funcionamento e o período sem atividade antes de preparar o deep sleep. Antes de dormir, precisa concluir a gravação do diário. O despertar pode ocorrer pelo PIR ou pelo temporizador de sincronização, configurado para cinco minutos. A visualização da imagem não deve impedir o repouso. As regras e os tempos exatos estão em `park_power.cpp`, `power_policy.hpp` e `Kconfig.projbuild`.

O histórico é gravado no microSD em `park-monitor-v1`; os marcadores de sincronização ficam em `park-cloud-v1`. Se a internet cair, os registros persistidos podem ser enviados quando a conexão voltar. Uma falha no cartão não deve ser interpretada como histórico salvo: o sistema informa o erro e pode cancelar a entrada em sono para não perder registros.

## Painel e nuvem

O painel da nuvem usa os arquivos de `cloud/public/`, sem etapa de montagem. A configuração de hospedagem está em `cloud/vercel.json`. O endereço publicado é [monitoramento-parque.vercel.app](https://monitoramento-parque.vercel.app/).

As migrações do Supabase definem o armazenamento e as permissões. O envio utiliza uma credencial exclusiva da placa; usuários do painel têm acesso conforme suas permissões. Senhas de Wi-Fi, credenciais de envio e chaves administrativas não devem entrar no Git. A configuração pública do painel contém somente os dados necessários à conexão do navegador, protegida pelas regras do banco.

A câmera continua na rede local. O celular precisa ter acesso à mesma rede da ESP para abrir o vídeo; o histórico na nuvem não tem essa restrição. No deep sleep, a página local fica indisponível até a placa despertar.

## Limites da contagem

O resultado é uma estimativa de passagens, não de visitantes únicos. Ângulo, iluminação, velocidade, oclusões e pessoas muito próximas podem causar erros. É necessário validar o posicionamento e comparar a contagem automática com observações no local antes de utilizar os números em relatórios.
