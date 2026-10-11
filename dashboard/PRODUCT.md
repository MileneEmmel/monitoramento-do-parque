# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

Responsáveis pelo parque consultam a visitação para relatórios e editais.
A equipe do projeto usa a câmera e a calibração para testar a contagem em campo.

## Product Purpose

Registrar contagens aproximadas de entradas e saídas, sem identificar visitantes.
O gráfico é a informação principal ao abrir o painel; o período inicial é hoje.

## Operating Context

Freenove ESP32-S3 com câmera e microSD. ESP, celular e notebook na mesma rede local.
O painel é servido pela ESP; o notebook não é servidor nem armazenamento do histórico.

## Capabilities and Constraints

Gráfico por hora/dia, resumo diário, registros, exportação CSV, câmera ao vivo,
calibração, identificação, totais acumulados e configuração da rede salva.
Não alterar detector, rastreamento, autenticação, Wi-Fi salvo ou histórico nesta revisão.
PIR, deep sleep, bateria e acesso remoto continuam pendentes.
Nunca inventar passagens, precisão, ocupação ou visitantes únicos.

## Brand Commitments

Nome: Monitoramento do parque. Textos simples, sem slogans.
Cantos arredondados. Natureza ilustrada conforme os prints fornecidos pela usuária:
folhagens e árvores, verdes marcantes e tipografia legível sem peso exagerado.
Sem caixas brancas repetidas. Gráfico evidente e legível.
Manter a organização do menu lateral. A lateral externa ocupa toda a altura,
sem cantos arredondados; uma versão estreita mantém somente os ícones.
Um único hamburger alterna as versões, sem X e sem botão com o texto Menu.
Corpo de 18 px e apoio de pelo menos 16 px; Nunito Sans foi rejeitada.

## Evidence on Hand

Firmware real em esp-who/examples/object_detect; painel em dashboard/public.
Dados do microSD e câmera são as únicas fontes de dados de operação.
As referências visuais não são fotografias do parque nem evidência de visitação.

## Product Principles

- Gráfico primeiro, informações auxiliares depois.
- Natureza nas bordas, leitura clara na área de dados.
- Interface independente do notebook, com arquivos locais na placa.
- Distinguir passagens do período de totais acumulados.
