# Referência da interface

Os estilos base ficam em `public/style.css`. A versão online acrescenta `cloud.css` e `refinements.css`; a estrutura local fica em `local-public/index.html`.

## Cores e fonte

| Uso | Cor |
| --- | --- |
| Fundo | `#edf4e3` |
| Superfície do gráfico | `#e1edcf` |
| Texto principal | `#203d2e` |
| Texto de apoio | `#50634c` |
| Entradas | `#176b42` |
| Saídas | `#175b91` |
| Ação principal | `#205b3c` |
| Menu lateral | `#d6e7ba` |
| Separador | `#c8d8b9` |

Source Sans 3 é carregada localmente, com fallback Segoe UI/sans-serif. Corpo de 18 px e apoio de pelo menos 16 px na interface. As anotações do canvas têm escala própria, vinculada aos 320 × 240 pixels da imagem.

## Navegação e componentes

A lateral ocupa toda a altura, sem arredondamento externo. Expandida mede 260 px; minimizada conserva ícones em 68 px. O mesmo botão alterna os estados e mantém rótulo acessível. A folhagem é decorativa, abaixo dos links, e desaparece na faixa estreita.

O gráfico abre no período Hoje. Entradas e saídas têm rótulos além das cores. Valores por intervalo aparecem na interação por mouse, toque ou teclado, sem permanecer abertos após atualização. Totais de 64 bits não devem perder precisão na apresentação.

Controles têm foco visível, áreas de toque amplas e cantos arredondados. Campos, interruptores e expansores usam o mesmo sistema visual. Em telas estreitas, formulários empilham e tabelas mantêm rolagem própria.

A ilustração não pode cobrir dados ou controles. Erros devem descrever uma falha real; ausência de leitura não equivale a zero. Impressão oculta navegação e controles, preservando relatório e gráfico.

## Recursos

`folhagem-v2.webp` é a ilustração do menu. A fonte e sua licença ficam ao lado deste documento. Recursos compartilhados não dependem de CDN em operação local. Não incluir fotos de visitantes nos assets do projeto.
