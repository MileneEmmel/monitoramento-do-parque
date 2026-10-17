# Operação e diagnóstico

## Inicialização

Use alimentação e cabo estáveis, com o cartão inserido antes de ligar. Não retire o microSD nem movimente o flat da câmera com a placa energizada.

O firmware testa o núcleo, monta o cartão sem formatação automática, conecta à rede salva e inicia o detector. Se a câmera falhar, rede e envio do histórico existente podem continuar; isso não significa que novas passagens estejam sendo detectadas.

Confira última atualização e situação do armazenamento. Dados antigos no dashboard não comprovam atividade atual.

## Câmera e calibração

O proprietário abre a câmera em Equipamento. Uma função protegida do Supabase devolve endereço local e chave quando há heartbeat recente. Não existe túnel de vídeo pela nuvem.

Use celular ou computador na mesma rede da ESP. Redes de visitantes podem bloquear comunicação entre aparelhos. O dashboard HTTPS pode ser visto de qualquer lugar; a câmera HTTP permanece local.

Posicione a câmera para enxergar pessoas antes e depois da faixa A/B. Observe o centro da caixa, escolha o eixo e confira o sentido de entrada. Teste pessoas isoladas, duplas, retornos e velocidades diferentes. Aumentar a distância entre linhas não corrige uma pessoa que o detector não enxerga.

Pausar reduz conversão JPEG e tráfego da prévia. A IA continua enquanto o ciclo ativo não termina; na branch PIR a ESP pode dormir após inatividade, se todos os dados estiverem salvos. Outro navegador solicitando imagens mantém a prévia ativa e bloqueia o sono. Consulte [energia e PIR](ENERGIA.md).

## Troca de rede

Informe rede de 2,4 GHz e senha na interface local. Conecte o celular à nova rede após a troca. O IP pode mudar e aparecerá no dashboard quando a ESP sincronizar.

Se a senha estiver errada ou não houver internet para informar o IP, use USB:

```powershell
python .\firmware\object_detect\configure_wifi.py --port COM5
```

`--status` abre a rede salva sem alterá-la. Não há ponto de acesso Wi-Fi de recuperação. Uma reserva DHCP no roteador ajuda a manter o IP na instalação fixa.

## Histórico

CSVs são o diário local, incluindo passagens e registros técnicos. A nuvem deduplica linhas. O dashboard apresenta passagens, sem transformar referência inicial em nova entrada.

A gravação verifica alterações a cada dois segundos e mantém registros periódicos sem movimento. O checkpoint NVS ocorre a cada dez segundos quando há mudanças. Esses intervalos não garantem perda zero ao cortar a energia.

Sem horário válido, registros ficam em `undated-...csv` e não recebem data retroativa automaticamente. Verifique o relógio antes do ensaio.

Não há retenção automática. Cartão cheio, mau contato ou falha de gravação pode interromper persistência. Contagem em RAM não equivale a histórico durável; uma queda de energia nesse estado pode perder registros.

### Limpeza manual

`/api/history/cleanup` exige autenticação, prévia, plano e confirmação. Protege o dia atual e, com proteção cloud ativa, arquivos não confirmados pela nuvem. A interface atual não tem botão de limpeza.

`python dashboard/verify_cleanup.py` verifica somente prévia e rejeições; não confirma exclusão. Para manutenção do cartão, desligue a placa e faça backup de `/park-monitor-v1/` e `/park-cloud-v1/`. Não apague cursores isoladamente: isso pode causar reenvio.

## Diagnóstico USB

Com o monitor serial fechado e `pyserial` disponível:

```powershell
python .\firmware\object_detect\configure_cloud.py --port COM5 --status
python .\firmware\object_detect\configure_wifi.py --port COM5 --status --no-browser
```

`dashboard/verify_device.py` consulta estado, compara recursos web, solicita quadros, testa rejeição de entradas inválidas e lê o CSV do dia. Isso ativa temporariamente a prévia. Sincronização manual de horário e reinício exigem opções explícitas. Não execute dois processos na porta serial.

| Sintoma | Conferir |
| --- | --- |
| Sem novos registros online | Última atualização, internet, relógio, credencial, HTTP e erro do cartão. |
| HTTP 401 | ID e credencial da mesma placa. Não apagar histórico para corrigir autorização. |
| Câmera não abre | Mesma rede, IP recente, chave, isolamento e inicialização do sensor. |
| Imagem para, dados continuam | Pausa, aba oculta, rede lenta ou memória para JPEG/HTTP. |
| Caixa sem passagem | Lado inicial, cruzamento, ID estável e oclusão. |
| Cartão indisponível | Alimentação, encaixe com a placa desligada, FAT/FAT32 e diagnóstico de gravação. |

## Atualizações e acesso

Site, banco e firmware são atualizados separadamente. Após gravar, confira inicialização, câmera, cartão e envio real. Não use `erase-flash` em atualização normal: apaga configurações e checkpoints NVS.

O usuário do dashboard pertence ao Supabase Auth e é separado da conta administrativa. Criar login não concede acesso automaticamente; é necessária associação ao parque e papel adequado. Apenas o proprietário pode gerir equipamento e obter a chave da câmera.

Não compartilhe a URL completa da câmera em prints ou relatórios públicos. O fragmento contém uma credencial de controle local.
