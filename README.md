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
| `supabase/schema.sql` | Definição final do banco, funções de envio e regras de acesso para um projeto Supabase novo. |
| `docs/diagramas/` | Diagramas D2, figuras SVG/PNG e guia de uso no relatório. |

O projeto ESP-IDF começa no `CMakeLists.txt` da raiz. Não depende da pasta externa `esp-who` nem de ferramentas auxiliares para copiar código antes de compilar. Os pacotes de terceiros são obtidos pelo gerenciador de componentes do ESP-IDF; as versões usadas pelo firmware estão fixadas em `firmware/main/idf_component.yml`.

### Módulos principais

- `app_main.cpp` e `frame_cap_pipeline.cpp`: inicialização e processamento dos quadros.
- `counting_core.hpp` e `visitor_counter.cpp`: rastreamento, cruzamento das linhas e contagem por sentido.
- `park_history.cpp`: diário persistente no microSD.
- `park_cloud.cpp`: sincronização autenticada, com retomada dos registros pendentes.
- `wifi_preview.cpp` e `camera_preview.cpp`: rede e acesso local à câmera.
- `pir_sensor.cpp`, `park_power.cpp` e `power_policy.hpp`: presença, condições de repouso e despertar.
- `power_monitor.cpp`: telemetria do PIR e da IA, eventos recentes e confirmação de ciclos de sono pela memória RTC e pelo motivo real do reinício.
- `app_settings.hpp` e `cloud_policy.hpp`: tempos da aplicação e agendamento dos envios, com verificações internas de coleta e repetição.
- `counting_core_checks.cpp`: verificações internas executadas na inicialização, preservadas do firmware. Não é uma suíte externa de desenvolvimento.

As adaptações desta aplicação estão em `firmware/main/`. Os componentes em `firmware/components/` vieram do [ESP-WHO](https://github.com/espressif/esp-who), revisão `1abda05e1c0782237fcb9e8d33a2fa7e105f06b2`, e mantêm o código original e a licença em `firmware/LICENSE-ESP-WHO`.

## Diagramas

Os modelos de casos de uso, classes, sequência, estados e hardware estão em [docs/diagramas](docs/diagramas/README.md), junto da visão geral, da organização de software e do fluxo de contagem. O guia relaciona cada figura às seções do relatório e explica como editar e exportar os arquivos com D2.

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

Na versão experimental de baixo consumo, a placa respeita a estabilização inicial e o período sem atividade antes de preparar o deep sleep. Não há mínimo de um minuto após despertar pelo PIR. O despertar ocorre pelo PIR ou após seis horas de sono. A visualização não impede o repouso. Os tempos estão centralizados em `app_settings.hpp`; inatividade e temporizador também são configuráveis em `Kconfig.projbuild`.

O histórico é gravado no microSD em `park-monitor-v1`; os marcadores de sincronização ficam em `park-cloud-v1`. Se a internet cair, os registros não confirmados permanecem para reenvio. Falha no cartão é informada, mas não impede o sono. Sem cartão e sem internet, os totais internos não substituem o histórico detalhado por horário.

## Painel e nuvem

O painel da nuvem usa os arquivos de `cloud/public/`, sem etapa de montagem. A configuração de hospedagem está em `cloud/vercel.json`. O endereço publicado é [monitoramento-parque.vercel.app](https://monitoramento-parque.vercel.app/).

O arquivo `supabase/schema.sql` reúne a definição final das tabelas, índices, funções, políticas de acesso e permissões. O envio utiliza uma credencial exclusiva da placa; usuários do painel têm acesso conforme suas permissões. Senhas de Wi-Fi, credenciais de envio e chaves administrativas não devem entrar no Git. A configuração pública do painel contém somente os dados necessários à conexão do navegador, protegida pelas regras do banco.

### Banco de dados

O esquema é destinado à instalação inicial em um **projeto Supabase novo**, com os recursos de autenticação padrão já disponíveis. Execute o arquivo inteiro uma única vez pelo SQL Editor, como proprietário do banco. A criação ocorre em uma transação e é recusada se as tabelas da aplicação já existirem. O arquivo não é uma atualização de banco existente e não contém usuários, registros de visitação ou credenciais.

O projeto Supabase que já está em funcionamento não precisa receber esse SQL novamente. A consolidação dos arquivos locais não altera o banco remoto, as contas ou os dados existentes. Não utilize reset, recriação de tabelas ou `supabase db push` para aplicar esta reorganização ao ambiente atual. O repositório passa a documentar o esquema final, sem manter uma sequência de migrações para atualização automática; mudanças futuras no banco existente exigem SQL incremental próprio e backup prévio.

Em uma instalação nova, cadastre os usuários no Supabase Auth e configure o parque, as permissões dos responsáveis e a credencial do dispositivo antes de usar o painel e o firmware. Informe o endereço e a chave pública do novo projeto em `cloud/public/config.js` e configure o acesso de envio na ESP sem publicar sua credencial no repositório. O esquema não é um backup dos dados nem substitui a configuração desses acessos.

A câmera continua na rede local. O celular precisa ter acesso à mesma rede da ESP para abrir o vídeo; o histórico na nuvem não tem essa restrição. No deep sleep, a página local fica indisponível até a placa despertar.

## Monitor de campo e deep sleep

Na página local, **Monitor de campo** acompanha o PIR, a atividade da IA, o motivo que impede dormir e a origem do último despertar. A imagem começa pausada: consultar o monitor não solicita quadros nem inicia a câmera. **Mostrar imagem** pode iniciar a câmera para ajustes mesmo sem acionamento do PIR.

O modo **Contínuo para ajustes** impede o sono. O modo **Sensor e deep sleep** segue as regras abaixo. A seleção é salva na ESP e permanece após reiniciar; trocar o modo não altera os tempos.

### Tempos atuais, ainda sujeitos à validação no parque

| Condição | Comportamento configurado |
| --- | --- |
| Ligamento ou reinício comum | Estabilização de 120 segundos desde o boot, sem exigir uma primeira ativação do PIR. A contagem pode começar durante esse período. |
| Despertar pelo PIR | Inicia câmera/IA sem aguardar rede ou montagem do cartão; não exige mínimo de 60 segundos. |
| Despertar pelo temporizador | Sincroniza sem iniciar a câmera e volta a dormir. Movimento transfere para operação normal. Não há piso de 20 segundos. |
| Sem atividade | Aguarda 30 segundos após o último PIR ativo ou pessoa detectada pela IA. |
| Coleta para envio | Primeiro registro pendente inicia uma janela de 10 segundos; novos registros não adiam a janela. |
| Lotes | Até 8 registros, com 2 segundos entre lotes confirmados. O limite conserva memória e tamanho da mensagem na ESP; o banco aceita até 32. |
| Tentativa de comunicação | Prazo monotônico de até 10 segundos, incluindo resolução de endereço e comunicação HTTPS. |
| Falha temporária no fechamento | Uma repetição, com pausa de 2 segundos; erros de autorização/dados não são repetidos cegamente. |
| Falha durante operação normal | Aguarda 60 segundos para tentar novamente enquanto houver atividade. |
| Preparação do sono | Tenta escoar a fila, com até 120 segundos de orçamento total de rede; termina antes se concluir ou falhar de modo persistente. Nova atividade cancela o preparo. |
| Durante o sono | Desperta com GPIO1 alto ou após 21600 segundos (6 horas), o que ocorrer primeiro. |
| Câmera sem quadros | Após 5 segundos sem quadros, uma recuperação cooperativa com prazo de 10 segundos por boot; falha informada não veta o sono. |
| Gravação | microSD verifica mudanças a cada 2 segundos; resumo interno verifica alterações a cada 10 segundos; fechamento força persistência. |

Esses tempos não são somados obrigatoriamente. Para dormir, todas as condições precisam estar atendidas. PIR alto renova a atividade enquanto permanecer alto; a ausência de pessoas na IA não anula o sinal do PIR. A prévia e as consultas do monitor não renovam o prazo de inatividade.

Na preparação, a contagem permanece ativa durante o envio. O fechamento pausa o envio e congela a contagem apenas para o checkpoint final. Uma nova passagem cancela o preparo. Dados novos durante o fechamento recebem nova oportunidade sob o mesmo prazo, sem reiniciar o orçamento. Se a rede falhar ou o teto terminar, a fila não confirmada permanece no cartão e não impede o sono. O checkpoint interno continua sendo uma proteção: falhar ao salvar os totais pode cancelar o fechamento.

O prazo de rede não inclui a estabilização inicial nem o intervalo de inatividade. Os dois valores de 120 segundos têm funções diferentes: um é estabilização ao ligar; o outro é um teto de comunicação durante o fechamento. Esperas internas de segurança para concluir uma operação de armazenamento não devem ser confundidas com esse teto de rede.

Falhas de microSD, relógio inválido e câmera não mantêm a placa acordada indefinidamente. Há uma montagem do cartão por boot, sem formatação automática; se não montar, a próxima tentativa ocorre no próximo despertar. A recuperação da câmera só reinicia a captura depois de pausar cooperativamente seus consumidores, sem matar tarefas segurando recursos. Caso não consiga recuperar, a contagem fica indisponível e o aviso permanece no monitor.

### Persistência e sincronização

Os totais e a calibração permanecem na NVS, atualizando o mesmo resumo, sem acumular um histórico interno. Após montar o cartão, a aplicação verifica as caudas dos arquivos reconhecidos e recupera totais mais recentes que o checkpoint interno, preservando passagens já detectadas neste boot. O histórico por horário continua no cartão; um resumo interno mais recente não reconstrói horários de registros que nunca chegaram ao microSD.

O cursor do cartão avança somente após a nuvem confirmar a quantidade exata recebida. Reenvios mantêm os identificadores e o banco não duplica passagens. Falha ao gravar o cursor mantém os registros para reenvio, mesmo que já tenham sido recebidos. Amostras auxiliares sem movimento não iniciam, sozinhas, requisições periódicas.

Estado do cartão, validade do relógio, endereço privado da câmera e operação são enviados no anúncio inicial e quando mudam. O controle de mudanças avança somente após confirmação. O banco preserva campos ausentes nos envios parciais. O anúncio de preparo indica intenção de dormir e previsão de despertar, nunca comprova o estado atual. A nuvem não trata dois minutos de silêncio como defeito e permite ao proprietário abrir o último link conhecido, avisando que ele exige a mesma rede e uma ESP acordada.

O esquema final inclui os campos de operação e as funções de sincronização usados pelo firmware atual. Esses objetos já fazem parte do projeto existente; a consolidação local não exige reaplicar SQL, publicar o painel ou gravar novamente a ESP.

O monitor só chama um ciclo de **confirmado** quando o próximo boot informa reinício por deep sleep e encontra o marcador anterior na memória RTC. Perda de conexão, sozinha, nunca comprova sono. Os ciclos da sessão são zerados em um reinício comum ou desligamento. Isso confirma o estado do chip, não mede a corrente da placa completa, câmera, PIR ou regulador.

### Validação no local

1. Instale PIR e câmera na posição pretendida. O PIR precisa alcançar a pessoa antes de ela chegar às linhas de contagem.
2. Abra o site no celular, na mesma rede. Em **Testar sem vídeo**, acompanhe os sinais sem transmitir imagens.
3. No modo com sensor, deixe a área sem movimento e observe o motivo de espera. Ao perder a resposta, aguarde; ainda não é uma prova de sono.
4. Passe diante do PIR sem pressionar RESET. Após reconectar, confira a confirmação de deep sleep, a origem do despertar e o tempo do boot ao primeiro quadro.
5. Compare passagens reais e registros, incluindo a primeira pessoa após o repouso, as duas direções e pessoas lado a lado.
6. Baixe o registro antes de fechar a aba. A ESP mantém 32 eventos recentes por boot e a tela mantém 200; esse registro de teste não vai para o histórico de visitação.

### Validações antes da instalação definitiva

- Confirmar no parque se os 30 segundos de inatividade e seis horas entre despertares sem movimento atendem ao uso. Se falhar um envio e não houver novas passagens, a atualização pode demorar seis horas ou mais, caso a rede continue indisponível.
- Medir se o acionamento antecipado do PIR deixa tempo suficiente para detectar a primeira passagem. O tempo até obter rede no celular não é o mesmo que o tempo para iniciar a IA.
- Exercitar falha de rede, rejeição de credencial, cartão indisponível, recuperação da câmera e orçamento de envio sem interromper o rastreamento. Não induzir falhas removendo cartão ou câmera com alimentação ligada.
- Validar alimentação, estabilidade do cartão, consumo real e autonomia. Não retirar cartão nem trocar fios com a placa ligada.
- Após perda total de alimentação, é necessário recuperar um horário válido. Registros sem data ficam separados no cartão e não são enviados automaticamente como registros datados.
- Durante o repouso não há vídeo nem IA. O PIR continua sendo a fonte externa de acionamento. A ausência de comunicação nunca distingue, sozinha, sono, falta de energia e perda de rede.

## Limites da contagem

O resultado é uma estimativa de passagens, não de visitantes únicos. Ângulo, iluminação, velocidade, oclusões e pessoas muito próximas podem causar erros. É necessário validar o posicionamento e comparar a contagem automática com observações no local antes de utilizar os números em relatórios.



C:\Users\Milene>& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' 'C:\Users\Milene\SistemasEmbarcados\backups\reorganizacao-20261017-codigo\object_detect-anterior\configure_wifi.py' --port COM5
& foi inesperado neste momento.
