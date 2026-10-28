# Diagramas do monitoramento do parque

Fontes editáveis em D2 e figuras exportadas para documentar a implementação atual. O conjunto usa TALA, fundo branco, verde-escuro para os elementos principais e verde-claro para os demais blocos. As cores não substituem os nomes, as condições ou os tipos de seta.

Os diagramas estruturais usam composição em paisagem. As duas sequências usam formato vertical, com o tempo avançando de cima para baixo. A visão geral ilustrada usa linhas e colunas de equipamentos, sem obrigação de uma faixa horizontal única. Nenhuma figura contém notas miúdas no rodapé: detalhes, ressalvas e referências ficam neste guia. O posicionamento dos blocos foi definido explicitamente onde necessário para preservar o alinhamento.

Os diagramas não alteram o firmware. A base de implementação desta revisão é o commit `a47a881`, na branch `feature/pir-deep-sleep`.

## Arquivos e posição no relatório

| Fonte D2 | Conteúdo | Exercício | Seção sugerida do modelo |
| --- | --- | --- | --- |
| [00_visao_geral.d2](00_visao_geral.d2) | Integração ilustrada do parque, PIR, placa, microSD, rede e acesso remoto | Complementar | 2.1 Visão geral |
| [01_casos_de_uso.d2](01_casos_de_uso.d2) | Objetivos dos atores, fronteira do sistema e especialização de papel | 3.1 | 2.5 Arquitetura de software |
| [02_classes.d2](02_classes.d2) | Classes e estruturas reais, composição, herança e dependências | 3.2 | 2.5 Arquitetura de software |
| [03_sequencia_contagem.d2](03_sequencia_contagem.d2) | Captura, inferência, rastreamento e confirmação da passagem | 3.3 | 2.5 Arquitetura de software |
| [04_sequencia_sono_e_nuvem.d2](04_sequencia_sono_e_nuvem.d2) | Fechamento, lotes, confirmação e sono | 3.3, cenário adicional | 2.5 Arquitetura de software |
| [05_maquina_de_estados.d2](05_maquina_de_estados.d2) | Inicialização, monitoramento, recuperação e baixo consumo | 4 | 2.5 Arquitetura de software |
| [06_arquitetura_hardware.d2](06_arquitetura_hardware.d2) | Componentes, alimentação, interfaces e pinos | 5 | 2.4 Arquitetura de hardware |
| [07_componentes_software.d2](07_componentes_software.d2) | Módulos da aplicação, ESP-WHO e interfaces web | Complementar | 2.5 Arquitetura de software |
| [08_fluxograma_contagem.d2](08_fluxograma_contagem.d2) | Decisões usadas para confirmar uma passagem | Complementar | 2.5 ou 3.1 Implementação |

Cada fonte numerada possui uma figura de mesmo nome em `svg/` e outra em `png/`. `_tema.d2` contém o estilo compartilhado: mantenha esse arquivo junto das fontes numeradas. Os atores do diagrama de casos de uso usam a silhueta nativa `person` do D2, com dimensões fixas e nomes externos para evitar distorção. Não há imagens externas de atores.

### Visão geral ilustrada

A visão geral usa ilustrações vetoriais originais em `assets/visao-geral/`, organizadas e conectadas no arquivo D2. Essa pasta deve acompanhar a fonte na edição. Os arquivos exportados incorporam os desenhos e não dependem da internet para exibi-los.

A referência de composição é a Figura 1, na página 3 do exemplo de ar-condicionado do professor: equipamentos reconhecíveis e conexões diretas. Aqui a composição foi adaptada ao parque com alinhamento por colunas. Parque e telas mantêm os verdes; os demais equipamentos usam predominantemente cinza, branco e grafite. As ilustrações são esquemáticas, não fotografias do protótipo, desenho de instalação em escala nem um mapa de ligação dos pinos. As barras nas telas são apenas uma representação de gráficos, sem valores de medição.

As linhas mostram o movimento que aciona o PIR, a imagem capturada pela câmera, o processamento na ESP e o histórico de entradas e saídas. Uma linha comum se ramifica para a gravação no microSD e o envio por Wi-Fi, sem ligar diretamente os dois equipamentos. A ramificação representa o mesmo histórico, não duas fontes independentes: no código, a ESP primeiro grava no cartão, depois lê e envia esses registros à nuvem. O arquivo local contém campos técnicos adicionais, e o cartão não transmite dados sozinho. O quadro de trajetória A/B representa processamento dentro da própria ESP, não outro equipamento. O “+1” ilustra uma passagem confirmada entre os lados externos das linhas: não significa que cada pessoa detectada, cada pulso do PIR ou cada quadro gere uma contagem. A direção configurada determina se ela será entrada ou saída. O cartão pode agrupar várias passagens em um registro.

O caminho tracejado representa acesso local à câmera e aos ajustes, que exige conectividade até a ESP acordada. O caminho pela nuvem representa o dashboard e o histórico; não transmite vídeo. Os detalhes elétricos e os protocolos ficam no diagrama de hardware e neste guia.

O exercício também pede requisitos, particionamento HW/SW, casos de teste e matriz de rastreabilidade. Essas são entregas textuais ou tabulares; não são substituídas por estes diagramas. Este conjunto não preenche nem modifica o relatório DOCX original.

## Abrir no VS Code

1. Abra um dos arquivos numerados, por exemplo `00_visao_geral.d2`.
2. Pressione `Ctrl+Shift+P` e escolha **d2: Show D2 Preview**. Também existe o atalho `Ctrl+Shift+D` quando o editor ativo é um arquivo D2.
3. Edite os nomes, mensagens ou conexões e salve. A prévia pode acompanhar as alterações automaticamente.
4. Para manter a composição utilizada nas figuras entregues, escolha **d2: Pick Preview Layout → tala**. A extensão instalada passa a opção de layout explicitamente; ela pode prevalecer sobre a configuração do arquivo.
5. Para gerar uma figura vetorial pela extensão, use **d2: Compile D2 to SVG**.

A extensão precisa do executável D2. Foi usada uma cópia portátil oficial, versão 0.9.0, com TALA incluído. Neste notebook ela está em:

```text
C:\Users\Milene\SistemasEmbarcados\.tools\d2-v0.9.0\bin\d2.exe
```

Se a extensão avisar que não encontrou o programa, abra as configurações do VS Code, procure **D2: Exec Path** e informe esse caminho. Não é necessário mudar a configuração do ESP-IDF. Em outro computador, instale o D2 ou ajuste o caminho para a instalação correspondente. O executável não faz parte do repositório.

Para gerar as duas versões pelo PowerShell neste notebook:

```powershell
cd 'C:\Users\Milene\SistemasEmbarcados\monitoramento-do-parque\docs\diagramas'
& 'C:\Users\Milene\SistemasEmbarcados\.tools\d2-v0.9.0\bin\d2.exe' 00_visao_geral.d2 svg/00_visao_geral.svg
& 'C:\Users\Milene\SistemasEmbarcados\.tools\d2-v0.9.0\bin\d2.exe' --scale 0.65 00_visao_geral.d2 png/00_visao_geral.png
```

Execute os comandos na pasta dos diagramas, para que a importação de `_tema.d2` seja resolvida corretamente. Troque o nome do arquivo para exportar outro diagrama. Os comandos sobrescrevem apenas a figura de saída indicada, não a fonte D2.

## Editar o D2

```d2
sensor: "Sensor PIR" {class: bloco}
esp: "ESP32-S3" {class: destaque}
sensor -> esp: "Movimento / GPIO1"
```

O texto antes de `:` é o identificador; o texto entre aspas é o rótulo. `->` indica uma relação dirigida, `<->` indica comunicação nos dois sentidos e `--` uma associação sem direção. `{ ... }` agrupa propriedades ou componentes.

Use `\n` dentro das aspas para quebrar uma linha. Rótulos contendo `;`, `:` ou condições devem ficar entre aspas: fora delas esses caracteres podem ser interpretados como código e criar elementos indesejados. Em diagramas de sequência, a ordem das mensagens na fonte é a ordem vertical da interação.

Para mudar a paleta, edite `_tema.d2`. Para mudar o título de uma figura, altere a linha `titulo:` da fonte correspondente. A classe `classe` tem cores específicas para os compartimentos de UML; não troque suas cores pelo estilo de um bloco comum sem verificar o resultado.

## Usar as figuras no Word

Prefira SVG quando o Word aceitar imagens vetoriais. Caso contrário, use o PNG correspondente, sem distorcer a proporção. Não use capturas da tela de prévia: os arquivos exportados têm melhor resolução.

Insira as figuras com **Inserir → Imagens** e use **Referências → Inserir legenda** para numerá-las. Exemplo de legenda: `Figura X – Arquitetura de hardware do sistema de monitoramento do parque`. Fonte sugerida: `Elaborado pelos autores`.

Use seções em paisagem para os desenhos estruturais e páginas em retrato para as sequências. Preserve a proporção das exportações e não force todos os desenhos ao mesmo tamanho. Nas sequências, os participantes ficam lado a lado e o tempo avança para baixo. O fechamento voltou a ser um único cenário vertical, com a repetição dos lotes inserida no ponto em que é executada, em vez de um quadro separado ao lado.

## Correspondência com o código

### Casos de uso: escopo e papéis

O sujeito do diagrama é o sistema completo: dispositivo, página local, dashboard e serviços de nuvem. A fronteira não representa só o firmware. Portanto, ESP32-S3, câmera, PIR e Supabase são partes internas desse sujeito, não atores externos. Se o escopo fosse apenas o firmware, o conjunto de atores seria outro.

| Caso | Papel participante | Resultado observável / implementação |
| --- | --- | --- |
| UC01 – Consultar visitação e histórico | Usuário autorizado, incluindo proprietário | Visualizar gráficos e registros produzidos pela contagem automática. `cloud/public/cloud.js` e políticas de leitura de `park_members`. |
| UC02 – Exportar relatório CSV | Usuário autorizado, incluindo proprietário | Obter os dados do período. Exportação no dashboard. |
| UC03 – Gerenciar credencial da placa | Proprietário | Criar ou substituir a credencial restrita de envio. Funções do banco verificam `role='owner'`. |
| UC04 – Editar identificação do parque | Proprietário | Alterar a identificação apresentada no painel. `park_update_identification` verifica o papel do usuário. |
| UC05 – Ajustar câmera e linhas A/B | Responsável pela instalação | Visualizar o enquadramento e salvar a calibração pela página local. |
| UC06 – Validar PIR e ciclos de sono | Responsável pela instalação | Acompanhar o monitor de campo e conferir o ciclo registrado ao despertar. Não significa consultar a ESP enquanto dorme. |
| UC07 – Configurar rede e modo de operação | Responsável pela instalação | Alterar a rede e escolher operação PIR ou contínua pela interface local. |

“Usuário autorizado” é o papel geral de consulta. A especialização com triângulo vazio aponta do proprietário para esse papel: o proprietário também consulta e exporta. “Responsável pela instalação” é um papel operacional, não um novo perfil de autenticação no Supabase; pressupõe acesso autorizado à página local. A mesma pessoa pode desempenhar mais de um papel.

O visitante foi retirado deste diagrama. Não é uma proibição geral da UML: um ator pode representar um papel humano, hardware ou sistema externo, e não precisa necessariamente usar uma tela. Em um modelo de interação física seria defensável representar o visitante como o papel cuja passagem fornece um estímulo ao sistema. Nesta visão dos objetivos de operação, porém, esse símbolo confundia o alvo observado com quem solicita os serviços. Os atores escolhidos são os papéis de consulta, administração e ajuste. O visitante continua na visão geral ilustrada, como parte do ambiente observado.

A contagem de passagens permanece uma função central e autônoma do sistema, descrita nas sequências, estados e fluxograma. Consultar UC01 não inicia nem mantém a contagem ligada; apenas lê os resultados disponíveis. A retirada do visitante não muda o funcionamento do projeto nem transforma a contagem em uma ação manual do gestor.

Detecção, rastreamento, persistência, sincronização e deep sleep são descritos nos diagramas de sequência, componentes e estados. Não foram mantidos como elipses soltas nem usados para transformar o diagrama de casos de uso em fluxograma. Também não foram forçados `«include»` ou `«extend»`: não há, nesta visão resumida, um comportamento comum extraído ou uma extensão que precise dessas relações. Autenticação é uma precondição das operações protegidas, não uma obrigação de repetir o login em cada consulta.

A composição é horizontal, com duas colunas de objetivos, papéis fora da fronteira e associações sem setas. Os identificadores UC01–UC07 pertencem a esta revisão; substituem a numeração anterior, que continha uma elipse de contagem associada ao visitante. Alinhe-os com o texto e a rastreabilidade do relatório antes de entregar. A figura é uma visão dos objetivos principais, não um inventário de cada botão do site.

### Leitura do diagrama de software

O diagrama de software apresenta primeiro o caminho funcional dos dados: câmera e IA localizam pessoas, o contador confirma as passagens, o diário grava as mudanças no microSD e o serviço de envio sincroniza os registros com o Supabase. O dashboard consulta os dados autorizados no banco. As setas desse caminho representam os dados que passam de uma responsabilidade para a seguinte, não chamadas síncronas entre todos os arquivos.

Na parte inferior aparecem dois serviços da ESP: a página local, que recebe a prévia e oferece ajustes, e o controle de energia, que combina PIR e presença informada pela IA para decidir o sono. A ligação tracejada indica informação de presença, não contagem. Inicialização, calibração, consulta do monitor, confirmação dos lotes e bloqueios de fechamento continuam no código, mas não aparecem como uma seta para cada função. Essa separação permite ler o fluxo principal sem confundir o desenho com um grafo de dependências de arquivos.

Os nomes e as funções individuais dos arquivos são detalhados no mapa do relatório, inclusive cabeçalhos, arquivos de configuração e componentes opcionais. A tabela abaixo é apenas um resumo dos módulos centrais.

### Tipos e módulos

| Elemento do modelo | Implementação |
| --- | --- |
| Adaptação da aplicação de detecção | `firmware/main/app_main.cpp`, classe `FreenoveDetectApp`, derivada de `WhoDetectAppTerm` |
| Câmera e orientação | `firmware/main/frame_cap_pipeline.cpp` |
| Associação e cruzamento | `firmware/main/counting_core.hpp`, tipos `Counter`, `Track`, `Observation` e `Config` |
| Totais, calibração e NVS | `firmware/main/visitor_counter.cpp` |
| Presença e sono | `firmware/main/pir_sensor.cpp`, `park_power.cpp` e `power_policy.hpp` |
| Histórico e confirmação local | `firmware/main/park_history.cpp` |
| Requisições e agendamento | `firmware/main/park_cloud.cpp` e `cloud_policy.hpp` |
| Imagens e página local | `firmware/main/camera_preview.cpp`, `wifi_preview.cpp` e `dashboard/local-public/` |
| Monitor de campo | `firmware/main/power_monitor.cpp` e `dashboard/local-public/monitor.js` |
| Dashboard remoto | `cloud/public/` |
| Autenticação e banco | `supabase/schema.sql` |

O diagrama de classes não transforma módulos funcionais em classes que não existem. `PowerPolicy` e `CloudSchedule` são estruturas C++ com métodos; os serviços que as utilizam aparecem no diagrama de componentes. Nomes abreviados de tipos, como `atomic_uint32` e `const_char_ptr`, servem à leitura do desenho; correspondem a `std::atomic<uint32_t>` e `const char *`, não a novos tipos declarados no firmware.

As dependências tracejadas de `FreenoveDetectApp` passam pelas funções indicadas nas setas; a aplicação não contém diretamente um objeto `Counter` ou `PowerPolicy`. Losangos preenchidos indicam composição; triângulo vazio indica herança. A assinatura `update(...)` foi abreviada no desenho, sem representar uma função C++ variádica. `CloudSchedule` aparece como um tipo de política usado pelo módulo de envio; não foi acrescentada uma associação fictícia com os tipos de contagem. O diagrama de componentes é uma visão arquitetural de módulos, não uma declaração de classes ou um diagrama UML de implantação.

## Adaptações do exercício para o parque

O sistema-problema do exercício é um monitor de temperatura. Aqui os mesmos tipos de modelo foram aplicados à estimativa de visitação. Não foram acrescentados display físico, sirene, medição de temperatura ou um estado de alarme de temperatura.

Na máquina de estados, `FALHA DE CÂMERA` e `RECUPERANDO CÂMERA` representam o tratamento de erro relevante ao dispositivo. A configuração pela página ocorre junto dos serviços ativos e não suspende toda a aplicação; por isso não foi desenhada como um estado exclusivo. Caso o professor exija os nomes literais `ALARME`, `CONFIGURAÇÃO` e `ERRO SENSOR`, confirme com ele a adaptação ao tema antes de renomear estados e atribuir ao código um comportamento que não possui.

`OPERAÇÃO ATIVA` reúne duas situações: aguardar a presença com a câmera ainda não iniciada, ou capturar e processar quadros. PIR, modo contínuo ou solicitação autenticada de prévia podem iniciar a captura. Essa agregação mantém o ciclo principal legível da esquerda para a direita; não afirma que a câmera esteja sempre ligada. A transição de recuperação pressupõe uma câmera solicitada e uma tentativa ainda disponível no boot. Uma nova atividade ou uma condição insegura de fechamento pode cancelar o preparo; falha de rede ou de cartão, isoladamente, não equivale a esse bloqueio.

Também não há diagnóstico elétrico capaz de distinguir, só pelo nível GPIO, uma pessoa ausente de um PIR desconectado ou travado em zero. Os diagramas não afirmam que essa falha é detectada automaticamente. A recuperação de câmera, por outro lado, observa ausência de quadros, tenta recuperação uma vez por boot e informa o resultado.

As sequências mostram cenários específicos. Elas não exigem que tarefas independentes executem sempre na ordem desenhada. O quadro de contagem resume as decisões do rastreador; o desenho de estados é uma abstração comportamental, não a reprodução de um único `switch` do firmware.

### Leitura dos diagramas de sequência

As caixas no topo identificam os participantes, não etapas sucessivas. As mensagens são lidas de cima para baixo. Uma seta que retorna à mesma linha indica trabalho feito pelo próprio participante; setas tracejadas representam respostas. `loop` indica repetição e `opt` indica uma etapa executada somente quando a condição escrita for atendida. As setas entre módulos da ESP não representam transmissões por Wi-Fi; o envio de rede está identificado explicitamente.

Os nomes exibidos priorizam a responsabilidade de cada participante. A correspondência com o código é:

| Participante no desenho | Implementação |
| --- | --- |
| Câmera e inteligência artificial | Pipeline de captura, componentes ESP-WHO e modelo `pedestrian_detect` |
| Controle do sistema | `FreenoveDetectApp`, em `app_main.cpp` |
| Contagem de pessoas | `visitor_counter.cpp` e `counting_core.hpp` |
| Controle do sono | `park_power.cpp` e `power_policy.hpp`, com leitura de `pir_sensor.cpp` |
| Totais na ESP | Persistência em NVS, em `visitor_counter.cpp` |
| Histórico no microSD | `park_history.cpp` |
| Envio de dados | `park_cloud.cpp` e `cloud_policy.hpp` |
| Nuvem / Supabase | API `park_ingest` e banco definidos em `supabase/schema.sql` |

A sequência de contagem começa com a câmera já ativa: captura e inferência produzem um resultado para o callback da aplicação, que informa a presença à política de energia e atualiza o contador. O laço expira trajetórias antigas, associa as observações, atualiza posições e confirma o cruzamento quando seus critérios são satisfeitos. A prévia é disponibilizada sob demanda, depois da contagem. O despertar pelo PIR está representado na máquina de estados. A gravação periódica do diário é uma tarefa independente e não aparece depois do laço da câmera; assim não se sugere que o histórico só grava quando a captura termina.

A sequência de envio e sono mostra um caminho de sucesso, com as pendências enviadas dentro do prazo, sem nova atividade nem mudança da revisão na gravação final. Ela começa quando as condições para preparar o sono já foram atendidas; a carência inicial, o modo PIR, o nível do sensor e os 30 s sem atividade estão detalhados na máquina de estados e na tabela de tempos. Durante a janela de rede a contagem permanece ativa. Só depois são pausados os envios, congeladas as atualizações e persistido o estado final sob os bloqueios necessários. A confirmação exige HTTP 200 e `accepted` igual à quantidade de registros, além da gravação do cursor quando houver registros. No desenho, “salvar até onde os registros foram confirmados” é a gravação desse cursor; “salvar totais na memória interna” é o checkpoint em NVS; e “bloquear acesso até dormir” representa manter o mutex do histórico durante o fechamento final. Se a revisão mudar após uma drenagem bem-sucedida, o código pode liberar os bloqueios e oferecer mais uma oportunidade de envio sob o mesmo prazo de 120 s; essa variação não é desenhada como uma janela nova. Falhas, cancelamento por atividade e reenvios são tratados no código e nas regras deste guia, não apresentados como sucesso garantido na figura. Se o prazo acabar antes de esvaziar a fila, os registros restantes ficam no cartão; nesse caso não se aplica a resposta “Sem pendências de envio” mostrada no cenário.

No fluxograma, o bloco de preparação reúne validação de confiança e dimensões, normalização de coordenadas, associação um a um e atualização de posição e velocidade. Cada trajetória é avaliada contra a faixa A/B. Há margem de 1,5% nos extremos e expiração após mais de 2 s sem observação. O desenho não implica reconhecimento facial, visitantes únicos ou contagem por pulso do PIR.

## Tempos representados

| Regra | Valor na implementação de referência |
| --- | --- |
| Estabilização após ligar ou RESET comum | 120 s desde o boot; não bloqueia a contagem |
| Inatividade para preparar sono no modo PIR | 30 s desde a última atividade; GPIO deve estar baixo |
| Despertar periódico | 6 h de sono ou PIR antes disso |
| Janela normal de coleta para envio | 10 s desde a primeira mudança pendente, sem adiar a cada nova mudança |
| Máximo por lote | 8 registros; um registro pode reunir várias passagens |
| Intervalo entre lotes confirmados | 2 s |
| Prazo de uma tentativa de comunicação | Até 10 s |
| Orçamento total de rede no fechamento | Até 120 s; pode terminar antes |
| Falha temporária durante fechamento | Uma repetição após 2 s, quando aplicável |
| Falha temporária durante operação normal | Nova tentativa após 60 s |
| Verificação de mudanças para o diário | A cada 2 s; operações de I/O podem acrescentar latência |
| Verificação de checkpoint interno | A cada 10 s quando há mudanças; fechamento força gravação |
| Câmera sem quadros / recuperação | 5 s / até 10 s para uma tentativa por boot |
| Expiração de uma trajetória | Mais de 2 s sem observação |

Os valores são configurações, não resultados de medição de consumo, precisão ou latência. O PIR e pessoas detectadas renovam a atividade; vídeo e consultas do monitor não renovam o prazo de inatividade. Modo contínuo impede o sono.

O microSD guarda os registros detalhados. A NVS mantém um resumo de totais e calibração. O cursor só avança depois de confirmação válida da nuvem; reenvios são deduplicados. Falha de rede não obriga a permanecer acordada indefinidamente. Falha de cartão é avisada e, por si só, não veta o sono, mas falha no checkpoint interno e impossibilidade de obter um bloqueio necessário podem cancelar o fechamento.

Não há vídeo nem IA durante o deep sleep. O PIR permanece alimentado. O marcador RTC e o motivo do boot seguinte permitem confirmar um ciclo; a simples indisponibilidade da página não prova que a ESP dormiu.

## Interfaces de hardware

- **PIR por GPIO1:** sinal digital compatível com 3,3 V e pino RTC usado como fonte EXT0. O módulo é alimentado em 5 V e compartilha o GND da placa. O PIR não identifica exclusivamente seres humanos nem mede quantas pessoas passaram.
- **Câmera por DVP:** barramento paralelo de 8 bits para os pixels, sinais de sincronismo e clock de sensor de 20 MHz. O barramento SCCB/I²C separado configura o sensor; não transporta o vídeo. Mapeamento desta Freenove: D0…D7 → GPIO11, 9, 8, 10, 12, 18, 17 e 16; VSYNC → GPIO6; HREF → GPIO7; PCLK → GPIO13; SDA → GPIO4; SCL → GPIO5; XCLK → GPIO15. Os oito pinos de dados ficam neste guia para não sobrecarregar a figura.
- **microSD por SDMMC:** modo de 1 bit, clock configurado em 20 MHz, pinos CMD38, CLK39 e D0=40. O driver utiliza DMA. O cartão armazena histórico e posição confirmada de envio; não é necessário gravar imagens.
- **Wi-Fi:** transmite lotes por HTTPS para a nuvem. A prévia local usa HTTP com chave de acesso e exige conectividade até a rede da placa. O rádio não funciona durante o deep sleep.
- **Alimentação:** entrada de 5 V na placa e alimentação do PIR com GND comum. A arquitetura omite o tipo de conector e os circuitos internos de regulação; não representa uma ligação de 5 V diretamente ao chip ESP32-S3. O desenho não dimensiona bateria, autonomia ou proteção elétrica. Deep sleep do microcontrolador não corta automaticamente a alimentação de todos os componentes.

O diagrama de blocos não substitui um esquemático de circuito, se esse for solicitado. Ele distingue os periféricos físicos do software mostrado no diagrama de componentes.

## Fontes e revisão

### Conferência de consistência dos nove diagramas

| Diagrama | Conferência com o projeto |
| --- | --- |
| Visão geral | PIR sinaliza movimento, a IA e o rastreador confirmam passagens na ESP, microSD registra o histórico e só os dados seguem à nuvem. O quadro A/B é uma ilustração de processamento, não um processador externo. |
| Casos de uso | Três papéis operacionais e sete objetivos. O proprietário herda a consulta; o papel de instalação exige autorização local e não é um perfil extra criado no Supabase. Visitantes ficam no contexto físico. |
| Classes | Herança real de `FreenoveDetectApp`, composição de `Counter` com `Config` e as 32 posições de `Track`. Tipos e métodos conferidos nos cabeçalhos; dependências por funções intermediárias são identificadas como tais. |
| Sequência de contagem | Captura, inferência, callback, atualização de energia, rastreamento e contagem. Um quadro vazio não renova a atividade; uma caixa detectada não equivale automaticamente a uma passagem. |
| Sequência de sono | Checkpoint, captura inicial, lotes com ACK, cursor confirmado, congelamento final, bloqueios, rádio desligado e fontes de despertar. É um cenário de sucesso delimitado, não a representação de todos os erros possíveis. |
| Estados | Modo PIR é condição para dormir. O despertar só por timer pode seguir à sincronização sem iniciar a câmera. Boot comum tem carência de 120 s; recuperação de câmera tem tentativa limitada. |
| Hardware | Pinos de câmera conferidos em `frame_cap_pipeline.cpp`; SDMMC de 1 bit a 20 MHz em `park_history.cpp`; GPIO1 e despertar EXT0 no PIR. Não há barramento SPI desenhado para o cartão. |
| Componentes | Originais ESP-WHO separados das adaptações da aplicação. Contagem inclui calibração e resumo em NVS; diário detalhado fica no microSD. Serviços locais e dashboard remoto são separados. |
| Fluxograma | Origem fora da faixa, observação no lado oposto e ao menos dois pontos na trajetória. Não é obrigatório observar um quadro no meio da faixa. Ausência de associação não inventa entrada ou saída. |

Esta é uma revisão de documentação e coerência com o código, não uma nova medição de precisão ou um ensaio de campo. Nenhum firmware foi alterado ou gravado por esta revisão.

### Referências

- `Exercícios Engenharia de Software e Hardware.pdf`: página física 6 (seção 4, modelagem UML); página física 7 (seção 5, estados); página física 8 (seção 6, arquitetura); páginas físicas 9 a 11 (testes, rastreabilidade e produto final).
- `Modelo_de_relatorio_-_v.1.6_(Completo) (2).docx`: seções 2.1, 2.4 e 2.5. A ordem e os números de página devem ser atualizados ao preencher o relatório.
- `Especificação - Controlador de Ar Condicionado - Exemplo não revisado (1).pdf`: página física 3, usada como referência da visão geral ilustrada, e páginas físicas 8 a 14, usadas como referência de organização, sem copiar funcionalidades ou inconsistências do exemplo para o parque.
- [Exemplos oficiais do TALA](https://d2lang.com/examples/tala/), [classes UML](https://d2lang.com/tour/uml-classes/) e [sequências](https://d2lang.com/tour/sequence-diagrams/).
- [OMG UML 2.5.1 – especificação normativa](https://www.omg.org/spec/UML/2.5.1/PDF): capítulo 18, seções 18.1.3 e 18.1.4, páginas impressas 639–645 (páginas físicas 681–687). A figura 18.10 foi inspecionada como referência da fronteira, dos atores e das associações; a seção 18.1.3.3 orientou a retirada de inclusões que apenas reproduziam etapas internas.
- [PlantUML – documentação oficial de casos de uso](https://plantuml.com/use-case-diagram): referências práticas de atores UML, associações, fronteira retangular e organização da esquerda para a direita. A notação foi consultada sem trocar as fontes editáveis D2 por PlantUML.

As fontes foram compiladas com D2 0.9.0 e TALA; as exportações foram inspecionadas visualmente. A revisão de conteúdo confrontou tempos, interfaces, formatos e responsabilidades com o firmware de referência. Na revisão específica de casos de uso, também foram conferidos o exemplo normativo da OMG, a proporção dos atores, os rótulos dentro das elipses, as ligações sem sobreposição de texto e a direção da generalização. Isso verifica a documentação, não substitui medições nem executa novos ensaios no parque.

Foram priorizados a leitura pelo professor, a distinção entre hardware e software e a fidelidade ao código. Não há chaves, senhas, endereços privados de câmera ou fotografias de visitantes nos arquivos. Antes da entrega acadêmica, alinhe estes identificadores UC com os requisitos e testes do relatório final e confirme as adaptações de estados com o professor.
