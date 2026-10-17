# PIR e deep sleep — branch experimental

Implementação em `feature/pir-deep-sleep`. O ponto de retorno é `backup/antes-pir-deep-sleep`, com a operação contínua e o diagnóstico USB do sensor. Nenhuma dessas branches é mesclada automaticamente em `main`.

## Ligações

HC-SR501: VCC em 5V, GND comum e OUT em GPIO1 da Freenove. OUT deve ser de 3,3V, nunca de 5V. GPIO1 é RTC IO e não é usado pela câmera nem pelo microSD deste projeto. Mantenha o PIR alimentado enquanto a ESP dorme. Faça alterações de fios somente com a alimentação desligada.

## Ciclo de operação

1. A placa inicia contagem persistente, diário, rede e sincronização antes de aguardar presença. Aguarda até 10 segundos pela referência inicial do diário; se o cartão falhar, a operação pode continuar em RAM, mas o sono fica bloqueado e não há garantia de histórico durável.
2. Em modo PIR, a captura e a IA só iniciam com nível alto no sensor ou solicitação autenticada de vídeo. O modo contínuo inicia a câmera sem esperar pelo PIR.
3. Cada nível alto do PIR ou pessoa detectada pela IA renova a janela de atividade. Quando iniciada, a câmera permanece ativa até o próximo deep sleep.
4. Após 30 segundos sem atividade, a ESP pode preparar o sono. Prévia ativa, sensor ainda alto ou inferência sem responder bloqueiam essa transição. Não há limite que force a dormir enquanto há pessoas.
5. A contagem é congelada, o checkpoint NVS é confirmado e o diário microSD é gravado com `fflush`, `fsync` e fechamento. Há uma janela de até 8 segundos para envio. O ciclo HTTPS deve terminar antes do sono; se estiver ocupado além do limite, o sono é cancelado. O envio não é requisito para dormir: dados não confirmados permanecem no cartão.
6. O diário é bloqueado após uma última gravação, o Wi-Fi é parado e o despertar é armado por GPIO1 alto ou temporizador de 300 segundos. O sono nunca apaga credenciais, calibração, contagens ou arquivos.
7. O despertar reinicia o programa. Um despertar periódico sem presença permite rede e sincronização, sem ligar a câmera. Novos registros são deduplicados pelo fluxo já existente.

## Proteções e prazos

- Primeiro boot/RESET: pelo menos 120 segundos acordada para configuração e estabilização. Antes de permitir sono, deve ser observado nível baixo e nível alto do PIR. Essa validação é mantida em RTC entre ciclos de deep sleep, mas precisa ser refeita após um boot comum.
- Despertar pelo PIR: pelo menos 60 segundos acordada.
- Despertar periódico: pelo menos 20 segundos acordada, respeitando também a janela de 30 segundos sem atividade.
- Sem cartão saudável, referência datada, relógio válido ou checkpoint íntegro: sono cancelado. A placa permanece disponível para diagnóstico.
- Movimento durante o salvamento cancela o sono. Movimento na transição final pode provocar despertar imediato por nível alto. Não existe garantia de perda zero durante inicialização ou falha elétrica.
- Quando a câmera foi iniciada, a ausência de callbacks recentes bloqueia o sono em vez de assumir que não existem pessoas.

## Configuração USB

Feche outros monitores da COM5 e use o Python do projeto. Execute na raiz:

```powershell
# Consultar estado, sem mudar o modo nem resetar a placa
python firmware/object_detect/configure_power.py --port COM5

# Desabilitar economia para calibração e comparação com operação contínua
python firmware/object_detect/configure_power.py --port COM5 --disable

# Habilitar novamente o modo PIR
python firmware/object_detect/configure_power.py --port COM5 --enable

# Acompanhar somente os campos de energia por até 120 segundos
python firmware/object_detect/configure_power.py --port COM5 --watch-seconds 120
```

O modo fica salvo na NVS. Se a placa estiver dormindo, acione o PIR ou pressione RESET antes de configurar. USB não é fonte de despertar UART neste deep sleep. RESET inicia a janela de manutenção, mas não apaga dados. Os comandos correspondentes são `@POWER_STATUS` e `@POWER {"enabled":false}`/`true`.

Os padrões de compilação ficam em `sdkconfig.freenove`: `APP_PIR_SLEEP`, `APP_PIR_IDLE_SECONDS` e `APP_PIR_SYNC_SECONDS`. Uma configuração USB salva prevalece sobre o padrão `APP_PIR_SLEEP`.

## Câmera, Wi-Fi e consumo

O link da câmera fica inacessível durante o sono. Para ajustes, acorde a ESP e mantenha a prévia ativa ou use modo contínuo. Histórico já sincronizado continua acessível no dashboard da nuvem. A indicação de atividade na nuvem pode envelhecer durante o sono: indisponibilidade do vídeo não comprova defeito.

Deep sleep desliga CPU, inferência e rádio, mas não desliga fisicamente todas as cargas da Freenove. PIR, reguladores, LEDs, interface USB e câmera podem continuar consumindo. A placa não tem PWDN/reset controlável da câmera nesta ligação. Não atribua à montagem completa o consumo em microampères especificado apenas para o chip ESP32-S3. Autonomia e aquecimento precisam de medição elétrica.

O PIR deve detectar a aproximação antes das linhas A/B, com margem para boot, montagem do cartão e inicialização do modelo. Essa latência ainda precisa ser medida em campo; uma pessoa rápida pode cruzar antes de a IA estar pronta. Pessoas paradas e eventos não humanos também limitam o PIR. Teste com e sem economia e não apresente a contagem como equivalente sem comparação manual.

## Verificação

Os autotestes da política C++ rodam no início do gerenciador e produzem `Policy self-tests PASS`. Os testes Python cobrem comandos USB e contratos de ordenação/abortos, não substituem ensaio de hardware.

Antes da instalação, confirme vários ciclos reais: presença inicia a câmera; repouso permite sono; PIR acorda; temporizador acorda sem presença; câmera volta a produzir quadros; NVS e totais por dia permanecem consistentes; falta de Wi-Fi não descarta pendências; erro do cartão bloqueia sono. Meça o tempo entre movimento e primeira detecção e registre falhas.

Referência: [Sleep Modes — ESP-IDF 5.5.5 / ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/sleep_modes.html).
