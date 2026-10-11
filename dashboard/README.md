# Monitoramento do parque — painel na ESP

A Freenove hospeda a interface, processa a câmera, conta passagens e grava o histórico no microSD.
O notebook não é servidor nem armazenamento. Celular e notebook são apenas navegadores.

## Abrir

Com a placa no USB, feche o monitor serial e execute:

```powershell
cd C:\Users\Milene\SistemasEmbarcados\dashboard
.\iniciar.ps1
```

O comando consulta somente a Freenove verificada (CH343 série 5C93054388) e abre o link direto da ESP.
Depois que o painel estiver aberto, copie o link em **Wi-Fi e acesso** e abra no celular na mesma rede.
O notebook pode ser desligado. A ESP precisa continuar alimentada por fonte USB ou outra alimentação adequada.
O endereço IP pode mudar com o DHCP: consulte novamente pelo USB ou peça uma reserva de IP ao administrador do roteador.

## Rede fixa do parque

A ESP usa somente modo estação (STA). Não cria rede própria nem portal de configuração.
A rede salva é usada ao ligar; não precisa configurá-la a cada visita.

- **Configuração manual pelo USB:** execute ` .\configurar-wifi.ps1 `, informe SSID e senha oculta. Sem salvar senha no PC.
- **Troca pelo painel:** em **Alterar a rede salva**, informe a nova rede. O comando é enviado diretamente à ESP pelo Wi-Fi. A resposta confirma apenas que a troca foi solicitada, não que a nova conexão funcionou. O painel pode ficar inacessível até o aparelho entrar na nova rede e usar o novo endereço.
- **Recuperação:** se o SSID ou senha estiver errado, use o USB. Não há fallback SoftAP.
- Suporte configurado para 2,4 GHz, WPA2 ou WPA2/WPA3 misto. Captive portal, WPA3 exclusivo e redes corporativas não fazem parte desta configuração.

Estar no mesmo SSID não garante comunicação: redes de visitantes podem ter isolamento de clientes.
Peça uma rede autorizada sem isolamento ou use um roteador portátil. Hotspot em 2,4 GHz é outra opção,
mas o acesso do próprio celular à ESP e a comunicação entre clientes dependem do aparelho; teste antes.

Não há acesso remoto pela internet nesta versão. Não encaminhe portas da ESP.
HTTP local não criptografa senha, imagem ou chave em trânsito: use apenas Wi-Fi confiável; para a primeira configuração ou ambientes não confiáveis, prefira o USB.
O link completo inclui uma chave de acesso que permite visualizar e alterar a placa. Compartilhe somente com responsáveis.
O fragmento é removido da URL visível e a chave fica na sessão da aba. Arquivos públicos da interface não contêm a chave.
APIs exigem autenticação; exportação CSV também. Não são usadas fontes, imagens ou serviços externos no painel.

## microSD e histórico

O firmware monta o cartão sem formatar e cria somente a pasta `park-monitor-v1`.
Não remove arquivos existentes. Cartões FAT/FAT32 são o caminho previsto; exFAT não está habilitado.
Se o cartão estiver incompatível ou falhar, o painel mostra o erro. Não formate sem copiar antes os seus arquivos.

A cada 2 segundos, uma tarefa na ESP verifica as contagens. Variações e reinícios geram registros;
sem variação, grava uma confirmação a cada 30 segundos. O histórico não depende de navegador, USB ou Wi-Fi disponível.
Cada gravação é sincronizada com o cartão. Não remova o cartão com a placa ligada.
Arquivos diários CSV têm checksum por linha; linhas incompletas ou danificadas são ignoradas, nunca reescritas automaticamente.
O relatório é lido do microSD, com cache de até 10 segundos, resumo por hora/dia e até 100 eventos recentes.
Exportar CSV baixa um resumo diário, não imagens. Dias sem registros são identificados como sem coleta.

Os totais antigos não são lançados como visitas de hoje: cada inicialização cria uma referência.
Os acumulados continuam na NVS, com o mecanismo de gravação anterior. Uma queda abrupta de energia pode perder
as últimas passagens ainda não gravadas (até o intervalo de leitura do cartão e o checkpoint dos acumulados).
Isso não é um banco transacional, e o microSD não substitui cópias de segurança.
Se reiniciar, um novo trecho começa sem inventar passagens ocorridas durante o desligamento.
O primeiro registro mostrado se refere ao período selecionado, não ao primeiro dia de toda a instalação.

### Horário

Com internet, a ESP sincroniza o relógio por SNTP. Se ainda estiver sem horário válido,
abrir o painel permite sincronizar com o relógio do celular/notebook.
Depois disso, a placa continua registrando enquanto alimentada, mesmo sem internet.
Após desligamento total, sem internet e sem navegador para sincronizar, registros novos ficam em arquivos
`undated-<boot>.csv`, com tempo desde o boot. Esses registros são preservados, mas não entram nos relatórios por data.
Não há relógio RTC com bateria nesta versão; isso precisa ser resolvido antes de uma instalação permanentemente offline.

### Operação

**Zerar totais** exige ZERAR, preserva linhas e histórico e registra a fronteira de reinício.
**Salvar linhas** reinicia somente trajetos em andamento. Contagens são aproximações de passagens, não visitantes únicos.
PIR, deep sleep, bateria e precisão em campo continuam pendentes; esta mudança não altera o detector ou o rastreamento.

## Migração e recuperação

O servidor anterior foi encerrado. `server.py` agora é apenas um atalho para abrir o painel da ESP.
Nenhum servidor persistente ou coletor SQLite é iniciado.
O banco legado em `data/park.sqlite3` foi preservado, mas não é usado para os novos relatórios nem importado como visitas reais.
O código e a aplicação anteriores estão em `../backups/pre-autonomous-sd-20261010.zip`, sem credenciais.
Para voltar o firmware, grave somente o binário da aplicação em 0x10000; não apague NVS ou cartão.

## Desenvolvimento e verificação

```powershell
node test_frontend.cjs
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' -m unittest -v test_server.py
& 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe' verify_device.py --watch 15
```

O último comando usa a chave apenas em memória, confere autenticação, recusa de entradas inválidas,
avanço dos quadros reais e CSV. Não gera passagens artificiais, não zera os totais e não troca a rede.
O diagnóstico opcional `verify_device.py --reboot` aplica um reset normal pela UART e
verifica a persistência do histórico. Não use comandos de identificação em modo de gravação
como substituto de um reinício normal para esse teste.

Os arquivos em `public/` são embutidos no firmware por CMake.
O menu pode alternar entre a versão completa e a faixa de ícones pelo botão de três traços.
Na página da câmera, entradas e saídas vêm do histórico de hoje no microSD, mesmo se outro período estiver selecionado nos relatórios. Falhas do cartão mostram dados indisponíveis, não zero. A opção de zerar foi removida da interface; os acumulados internos da ESP continuam preservados como referência para o registro de passagens.
Interrupções, tempo de coleta e totais reiniciados não são exibidos como indicadores nem como aviso de rotina. Seus campos continuam no diário/CSV técnico; avisos reais de armazenamento, conexão e relógio permanecem.

O histórico não possui limpeza automática nem rotação de arquivos. Em Histórico → Limpar histórico antigo, escolha uma data, consulte a quantidade/tamanho dos arquivos e digite APAGAR para confirmar a exclusão permanente. Só arquivos diários reconhecidos da pasta park-monitor-v1, anteriores à data e anteriores a hoje, podem ser apagados. Outros arquivos do cartão, o dia atual e a NVS não são alterados. A seleção é revalidada ao confirmar; erro pode interromper uma limpeza parcialmente concluída e será informado. Exporte os relatórios necessários antes de excluir.
Se faltar espaço, a tentativa de gravar falha e o estado de armazenamento indica erro; não há exclusão automática. O painel ainda não mede espaço livre nem diferencia cartão cheio de outras falhas de gravação.
Os detalhes por horário do gráfico aparecem durante a interação, sem permanecer abertos após sair.
Fonte e ilustração são servidas pela própria ESP; a interface não depende de um serviço externo de fontes.
Depois de editar a interface, compile e grave novamente **somente a aplicação**; reiniciar um servidor do PC não atualiza o painel.
Pinagem SDMMC 1 bit: CMD 38, CLK 39, D0 40, conforme o [tutorial oficial Freenove](https://docs.freenove.com/projects/fnk0083/en/latest/fnk0083/codes/C/28_Read_and_Write_the_SDcard.html).
Montagem e sincronização seguem a [API FatFS do ESP-IDF 5.5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/storage/fatfs.html).
