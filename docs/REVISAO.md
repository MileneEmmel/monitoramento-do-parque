# Revisão técnica

Revisão de organização e leitura do código em 11/10/2026. Os pontos abaixo distinguem comportamento implementado de testes ainda necessários.

## Organização

A aplicação local tem uma fonte em `dashboard/local-public/`. `dashboard/public/` contém recursos compartilhados, e o dashboard online fica em `cloud/public/`. A interface local antiga e seus testes exclusivos não fazem parte da aplicação atual.

O firmware versionado fica em `firmware/object_detect/`. ESP-WHO é uma dependência de desenvolvimento fora do Git deste projeto. Scripts tornam explícitos a atualização das adaptações e o caminho dos recursos web. Rotas antigas `/frame.json`, `/counter.json` e os endpoints de zerar contagem foram removidos, assim como o comando temporário USB `@CLOUD_DEVICE`. A configuração normal usa `@CLOUD` pelos configuradores versionados.

Imagens de verificação e pacotes antigos não são necessários para compilar. Dados de operação, documentos acadêmicos e backups não são código descartável.

## Pontos de atenção

| Área | Evidência | Consequência e próximo teste |
| --- | --- | --- |
| Memória interna | TLS, Wi-Fi, HTTP, FAT e JPEG competem por memória. Houve leituras com bloco interno contíguo muito pequeno. | PSRAM livre não garante alocação interna/DMA. Medir mínimo de heap e latência com prévia ativa/pausada e envio simultâneo. |
| Prévia | JPEG sob demanda, base64 em JSON e consultas sequenciais. | Base64 aumenta o payload em cerca de um terço. Medir custo antes de migrar para JPEG binário. |
| Cartão | Envio lista arquivos e consulta cursores; relatórios locais percorrem o diário sob mutex. | Custo cresce com dias armazenados. A UI usa `/api/local`, sem relatório completo a cada atualização. Testar meses de dados antes de implementar índice. |
| Backlog | Lotes de oito linhas e conexões HTTPS por envio. | Acúmulo offline pode levar tempo para escoar. Medir taxa de drenagem e memória antes de aumentar lote. |
| microSD intermitente | Falha de abertura seguida de recuperação, sem causa única determinada. | Validar alimentação, contato, escrita, reinício e cartão cheio. A nuvem não elimina a dependência do cartão para dados pendentes. |
| Energia | Diário assíncrono e checkpoints periódicos. | Sem garantia de perda zero. Testar cortes controlados e comparar diário, NVS e banco. |
| Relógio | Envio exige horário válido; arquivos sem data não são datados retroativamente. | Testar partida sem internet e recuperação do relógio. |
| Contagem | Até 32 trajetórias, retenção curta e dependência do detector. | Validar grupos, corrida, sombras, oclusão e ângulo real. Não há percentual de precisão medido. |
| Segurança local | HTTP e NVS sem criptografia de flash. | Exigir rede confiável e proteção física. Não expor a ESP à internet. |

O heartbeat pode informar status e endereço da câmera quando um lote do cartão falha. Isso preserva o diagnóstico, mas não confirma registros não enviados. O cursor não avança nesse caso.

## Critérios de validação em campo

- Comparar entradas e saídas com contagem manual documentada.
- Verificar detecção sem abrir a câmera e com prévia pausada.
- Interromper internet, gerar passagens e conferir recuperação sem duplicação.
- Reiniciar preservando cartão e credenciais e comparar totais por dia.
- Testar indisponibilidade do cartão sem apagar dados reais.
- Conferir proprietário, leitor e usuário sem associação.
- Reproduzir preparação e compilação em outra instalação limpa.

PIR, deep sleep e bateria permanecem fora da implementação. Deep sleep exige rever latência de inicialização da câmera/modelo e passagens perdidas durante o despertar.

## Verificações desta revisão

Passaram os testes de interface local, dashboard online e build público, além de sete testes Python dos configuradores USB. Os scripts PowerShell passaram pela análise de sintaxe e os links locais da documentação foram conferidos. O firmware foi recompilado no checkout de desenvolvimento com os recursos da pasta organizada.

Não houve nova gravação na ESP nesta revisão. Os testes C++ foram compilados, mas não executados novamente na placa. Os testes SQL já executados na implantação não foram reaplicados ao banco nesta etapa. Precisão em campo, estresse do cartão e reprodução em outra instalação continuam pendentes.
