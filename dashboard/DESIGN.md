---
name: Monitoramento do parque
description: Painel local de contagem aproximada, histórico e câmera do parque.
colors:
  canvas: "#edf4e3"
  ground: "#e1edcf"
  ink: "#203d2e"
  muted: "#50634c"
  signal: "#176b42"
  secondary: "#175b91"
  line: "#c8d8b9"
  soft: "#d8e9c2"
  rail: "#d6e7ba"
  warning: "#714b16"
  warning-bg: "#f5e6c5"
  danger: "#913c30"
  heading: "#205538"
  action: "#205b3c"
  action-text: "#f2f8e9"
  action-hover: "#174a30"
  action-active: "#103e25"
  button: "#d4e5bd"
  button-text: "#214c30"
  field: "#f0f6e8"
  field-border: "#9ab184"
  calibration: "#dceacb"
  disclosure: "#dce9cb"
typography:
  display:
    fontFamily: '"Source Sans 3", "Segoe UI", sans-serif'
    fontSize: "36px"
    fontWeight: 700
    lineHeight: 1.15
    letterSpacing: "-.03em"
  title:
    fontFamily: '"Source Sans 3", "Segoe UI", sans-serif'
    fontSize: "20px"
    fontWeight: 650
    lineHeight: 1.3
    letterSpacing: "-.015em"
  body:
    fontFamily: '"Source Sans 3", "Segoe UI", sans-serif'
    fontSize: "18px"
    lineHeight: 1.55
  label:
    fontFamily: '"Source Sans 3", "Segoe UI", sans-serif'
    fontSize: "17px"
    fontWeight: 600
  support:
    fontFamily: '"Source Sans 3", "Segoe UI", sans-serif'
    fontSize: "16px"
    lineHeight: 1.6
  metric:
    fontFamily: '"Source Sans 3", "Segoe UI", sans-serif'
    fontSize: "34px"
    fontWeight: 700
    lineHeight: 1.2
    letterSpacing: "-.03em"
rounded:
  sidebar: "0"
  menu-toggle: "12px"
  period: "14px"
  input: "16px"
  button: "17px"
  period-group: "19px"
  notice: "22px"
  camera: "28px"
  dialog: "30px"
  plot: "32px"
spacing:
  xs: "4px"
  sm: "8px"
  control-gap: "12px"
  md: "16px"
  section-gap: "20px"
  lg: "24px"
  xl: "28px"
  page: "40px"
components:
  button:
    backgroundColor: "{colors.button}"
    textColor: "{colors.button-text}"
    typography: "{typography.label}"
    rounded: "{rounded.button}"
    padding: "11px 21px"
  button-primary:
    backgroundColor: "{colors.action}"
    textColor: "{colors.action-text}"
    typography: "{typography.label}"
    rounded: "{rounded.button}"
    padding: "11px 21px"
  button-primary-hover:
    backgroundColor: "{colors.action-hover}"
  button-primary-active:
    backgroundColor: "{colors.action-active}"
  button-danger:
    backgroundColor: "{colors.danger}"
    textColor: "#fff8ee"
    rounded: "{rounded.button}"
    padding: "11px 21px"
  field:
    backgroundColor: "{colors.field}"
    textColor: "{colors.ink}"
    rounded: "{rounded.input}"
    padding: "11px 13px"
  nav-link-active:
    backgroundColor: "{colors.action}"
    textColor: "{colors.action-text}"
    rounded: "{rounded.button}"
    padding: "14px 13px"
  period-selected:
    backgroundColor: "{colors.action}"
    textColor: "{colors.action-text}"
    rounded: "{rounded.period}"
    padding: "10px 16px"
  plot:
    backgroundColor: "{colors.ground}"
    rounded: "{rounded.plot}"
    padding: "20px 24px 14px"
  calibration:
    backgroundColor: "{colors.calibration}"
    rounded: "{rounded.camera}"
    padding: "24px"
  disclosure:
    backgroundColor: "{colors.disclosure}"
    textColor: "{colors.ink}"
    rounded: "{rounded.input}"
    padding: "13px 16px"
---

# Design System: Monitoramento do parque

## Overview

Interface de consulta e operação com natureza ilustrada, superfícies verdes e texto simples. A vegetação ocupa o espaço livre abaixo dos links da navegação, sem cobrir dados ou controles. O nome é Monitoramento do parque, sem slogan ou marca inventada.

A leitura usa Source Sans 3, pesos moderados, corpo de 18px e apoio de pelo menos 16px na tela. O gráfico abre em Hoje, com entradas e saídas identificadas por texto e cor. Espaçamento e alinhamento organizam as informações auxiliares.

Características principais:

- Natureza ilustrada abaixo da navegação.
- Superfícies verdes e gráfico evidente.
- Texto legível e controles com foco visível.

Direção confirmada pela usuária em 10/10/2026 e extraída de public/style.css, public/index.html e public/app.js. Este documento substitui o sistema anterior, preservado em backups/pre-botanical-ui-20261010.zip.

## Colors

### Primary

Verde-floresta signal identifica entradas; action identifica seleção e ações principais. Heading mantém títulos e marca em verde. Os estados hover e active escurecem a ação. Action-text mantém texto claro nos controles selecionados.

### Secondary

Azul secondary identifica saídas e o foco de teclado. Entradas e saídas também têm rótulos: a cor nunca é a única informação.

### Neutral

Canvas é o fundo da página; ground é a superfície do gráfico e de informações de rede; rail é a lateral. Ink e muted distinguem texto principal e apoio. Line separa linhas e informações de coleta; soft responde a interação e aparece em avisos comuns. Button e button-text compõem a ação secundária. Field e field-border definem campos. Calibration e disclosure agrupam controles específicos sem criar uma sequência de caixas brancas.

Warning e warning-bg aparecem em avisos de coleta, gravação ou conexão reais. Danger identifica ações destrutivas. As cores no frontmatter são normativas; detalhes de estado extraídos estão nos snippets do sidecar.

## Typography

Fonte local Source Sans 3, arquivo source-sans-3-v1.woff2, com fallback Segoe UI e sans-serif. O arquivo atende pesos de 400 a 700; não depende de serviço de fontes externo.

Os papéis do frontmatter refletem os estilos de tela: display para o título da página, title para seções, body para leitura, label para controles, support para explicações e metric para totais do gráfico. Títulos de gráfico e diálogos têm ajustes específicos de 22px e 24px. A marca usa 24px e peso 650. Números usam algarismos tabulares em strong, output e células.

O corpo usa 18px; nenhum apoio da interface HTML ou do gráfico SVG desce de 16px na tela. Os títulos de página passam a 32px até 700px. Notas limitam a leitura a 65–75ch quando previsto no componente.

Os rótulos desenhados sobre os pixels da câmera conservam 12px/13px em Segoe UI no canvas original; são anotações da imagem, não a escala de texto da interface. Impressão usa corpo de 15px e título de 26px.

## Layout

A lateral é fixa, encostada à borda esquerda e ocupa toda a altura, com largura expandida de 260px e externa sem arredondamento. Minimizada, mantém uma faixa de ícones de 68px. O mesmo hamburger alterna os estados e informa aria-expanded; links preservam nome acessível, title e aria-current.

A área de trabalho acompanha a largura da lateral. O cabeçalho tem altura mínima de 92px; o conteúdo tem largura máxima de 1580px e padding lateral de 40px. Período, título do gráfico, legenda com totais e superfície do gráfico formam a leitura inicial. Tempo e contexto de coleta ficam abaixo; registros vêm depois.

O gráfico mede 340px de altura, passa a 310px até 1020px e a 290px até 700px. Câmera e calibração usam duas colunas, com 255px na calibração; rede usa duas colunas e máximo de 1150px. Ambas empilham até 1020px. Tabelas têm rolagem interna.

Os breakpoints de 1240px, 1020px, 900px, 700px e 380px ajustam espaçamento, colunas e disposição dos totais. Até 900px a página mantém margem de 68px; o menu começa minimizado e, expandido, sobrepõe o conteúdo com largura min(300px, calc(100% - 68px)). Selecionar uma página recolhe o menu nesse intervalo. Até 700px o padding lateral é de 18px.

Na impressão, lateral, hamburger, cabeçalho e controles de interação ficam ocultos; a margem da área de trabalho é zero nos dois estados do menu. Gráfico e resumos de histórico compõem a saída, com fundo branco de impressão.

## Elevation & Depth

Superfícies verdes e separadores sutis organizam a profundidade. Painéis de leitura permanecem planos. Sombras existem apenas no detalhe flutuante do gráfico (0 8px 22px #234c3520), no diálogo (0 20px 70px #20482e38) e no menu expandido até 900px (0 12px 42px #20482e38). O backdrop do diálogo usa #1b3e2c70.

Interações mudam cor de fundo, texto e borda em .18s. Não há animação decorativa; prefers-reduced-motion remove transições.

## Shapes

A lateral externa tem cantos retos. Controles e superfícies internas têm cantos suaves conforme os raios do frontmatter. O gráfico usa raio de 32px, reduzido a 26px até 700px; câmera usa 28px, reduzido a 24px. A calibração usa 28px, reduzido a 26px. Tabelas recortam o contorno com 18px.

Os pequenos marcadores de legenda têm raio de 3px e tamanho de 10px. Interruptores têm trilho de 46 × 28px, raio de 16px e círculo de 18px. SVGs de navegação usam traço simples e tamanho de 22px.

## Components

### Buttons

Botões comuns e principais usam raio de 17px, padding de 11px 21px e altura mínima de 48px. A variante small usa 16px, padding de 10px 16px e altura mínima de 46px. Ações secundárias têm fundo verde claro; ações principais usam verde escuro com texto claro. Danger usa vermelho-terra apenas para a confirmação destrutiva. Disabled reduz opacidade a .5 e altera o cursor.

Todos os controles focáveis recebem outline de 3px em secondary, afastado 4px. Botões de texto têm sublinhado e área mínima de 44px.

### Period controls

O grupo tem fundo ground, padding de 5px e raio de 19px. Cada opção tem altura mínima de 46px e seleção em verde escuro. Hoje é o estado inicial. Escolher datas revela campos de início/fim e Aplicar; aria-pressed e aria-expanded acompanham os estados.

### Fields and switches

Campos têm fundo claro esverdeado, borda de 1px, raio de 16px e altura mínima de 44px. Hover escurece a borda; foco usa signal e fundo #f5f9ed. Readonly tem fundo #dce8cc; campo inválido em foco usa danger. Labels ficam acima dos campos, com espaço de 9px.

Checkboxes são interruptores, com círculo deslocado para a direita quando selecionados. Select usa seta SVG desenhada em seu fundo e espaço de 44px à direita.

### Navigation

Cinco áreas conservam sua ordem: Visão geral, Histórico, Câmera e contagem, Equipamento, Wi-Fi e acesso. Links usam raio de 17px, altura mínima de 52px, fonte de 17px e ícone de 22px. A seleção usa action/action-text. A lateral não contém status ESP online.

O hamburger é único, fixo e acessível. A imagem folhagem-v2.webp ocupa o espaço flexível abaixo dos links, com altura máxima de 390px; é decorativa, sem interação e com alt vazio. Ela desaparece na faixa minimizada.

### Graph

A superfície integra barras, eixos e estado vazio. Verde e azul distinguem entradas e saídas, acompanhados de texto. SVG usa rótulos de 16px. Totais ficam junto à legenda, sem cartões individuais. Horários futuros, coleta sem passagens e ausência de coleta têm marcas e explicações próprias.

Mouse mostra os valores do horário enquanto permanece no gráfico; sair oculta o detalhe, inclusive após atualização automática. Toque mantém o detalhe até tocar fora. Teclado mostra durante foco; setas, Home/End, Enter/espaço e Escape permitem consulta. Atualizações preservam o foco sem reabrir um detalhe oculto.

### Containers, records and disclosures

As seções de conteúdo usam alinhamento e espaço, sem fundo de cartão por padrão. Calibração é um agrupamento verde com padding de 24px. Cabeçalhos de tabela usam #dce9cc; linhas usam separadores line e hover #e2edd4.

Expansores têm resumo verde, altura mínima de 52px e ícone mais/menos em SVG. O ícone vertical desaparece no estado aberto. Avisos aparecem depois do conteúdo da página e descrevem condições reais.

### Dialogs and camera

Diálogos usam fundo canvas, raio de 30px, padding de 30px e largura min(460px, calc(100% - 32px)). A identificação tem campos e Salvar. Zerar exige confirmação digitada ZERAR e bloqueia fechamento durante envio. O X pertence ao fechamento do diálogo; não substitui o hamburger da navegação.

A câmera usa fundo #183e2c, canvas 4:3 e estado de espera textual. Pausar imagem mantém a contagem ativa. A calibração permanece ao lado da imagem quando a largura permite.

## Do's and Don'ts

- Do usar Source Sans 3 local, corpo de 18px e apoio de pelo menos 16px na tela.
- Do manter o gráfico em destaque e Hoje como período inicial.
- Do manter a lateral externa reta, de altura completa, e a faixa de ícones de 68px.
- Do usar um único hamburger para expandir e minimizar a navegação.
- Do manter folhagem ilustrada fora dos links e da área de dados.
- Do identificar entradas e saídas com texto além da cor.
- Do separar passagens do período e totais acumulados da ESP.
- Do preservar foco visível, estados vazios e mensagens de falha baseadas nos dados reais.

- Don't repetir caixas brancas nas telas.
- Don't usar Nunito Sans, slogans, metáforas ou frases publicitárias.
- Don't transformar a navegação em menu horizontal no celular.
- Don't substituir o hamburger por X ou botão com o texto Menu.
- Don't adicionar animações decorativas.
- Don't inventar registros, precisão, ocupação ou visitantes únicos.
- Don't exibir senhas ou chaves por padrão.
