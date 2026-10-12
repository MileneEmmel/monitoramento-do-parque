# Guia do código

## Por onde começar

Leia `firmware/object_detect/main/app_main.cpp` para acompanhar a inicialização. Siga `visitor_counter.cpp` e `counting_core.hpp` para entender como uma detecção vira passagem. `park_history.cpp` e `park_cloud.cpp` cuidam da persistência e do envio.

Para trabalhar no site, use `cloud/public/` para relatórios online e `dashboard/local-public/` para câmera e ajustes. Os dois usam recursos de `dashboard/public/`. Nenhum servidor Python é necessário à operação.

## Firmware

| Arquivo em `firmware/object_detect/main/` | Responsabilidade |
| --- | --- |
| `app_main.cpp` | Inicializa serviços e conecta o callback do detector à contagem e à prévia. |
| `frame_cap_pipeline.cpp` | Sensor, clock, pinagem e captura. |
| `counting_core.hpp` | Associação de detecções às trajetórias e contagem pelas linhas. Sem dependência do ESP-IDF. |
| `counting_core_tests.cpp` | Casos sintéticos: sentidos, múltiplas pessoas, perda de trajetória e capacidade. |
| `visitor_counter.cpp` | Adapta caixas ao núcleo, protege o estado com mutex e salva checkpoints NVS. |
| `camera_preview.cpp` | Prepara JPEG e metadados sob demanda. |
| `wifi_preview.cpp` | Wi-Fi, HTTP local, autenticação, ajustes e comandos USB. |
| `park_history.cpp` | Diário microSD, checksums, relatórios, limpeza e cursores. |
| `park_cloud.cpp` | HTTPS com certificado verificado, lotes, retentativas e status. |
| `Kconfig.projbuild` | Opções da aplicação. |
| `idf_component.yml` | Dependências do componente. |
| `CMakeLists.txt` | Compilação e inclusão dos recursos web no firmware. |

Os `.hpp` expõem a interface de cada módulo. `sdkconfig.freenove` configura a placa; o lockfile fixa dependências do exemplo. Trechos de suporte a outros modelos/placas pertencem à base ESP-WHO e são selecionados por configuração na compilação.

### Detecção e contagem

O modelo fornece caixas e confiança. `visitor_counter_update()` filtra observações e chama `park::Counter::update()`. O núcleo usa atribuição global para associar observações às trajetórias, com capacidade para 32 trajetórias.

As coordenadas são normalizadas entre 0 e 1. O centro da caixa precisa estabelecer um lado inicial e atravessar a faixa entre as linhas. A ordem A/B determina o sentido; a configuração permite inverter entrada/saída e usar linhas horizontais. Há tolerância na transição entre zonas e retenção temporária de trajetórias perdidas.

Uma caixa pode aparecer sem haver trajeto suficiente para contar. Alterar calibração limpa trajetórias em andamento, preservando totais. IDs são temporários e não identificam visitantes entre visitas.

### Concorrência

Captura/inferência, gravação, envio e HTTP executam em tarefas diferentes. Núcleo e diário têm mutexes próprios. Não coloque HTTP ou leitura longa de cartão no callback do detector.

Solicitar `/api/frame` mantém ativa por poucos segundos a preparação JPEG. Sem novas solicitações, essa conversão para; a captura necessária à IA permanece ativa. Pausar a imagem não é suspender a placa.

### Persistência

`park_history_capture()` registra diferenças entre totais anteriores e atuais. A primeira leitura estabelece referência, sem somar totais antigos ao dia atual. Registros de referência e manutenção não são passagens novas.

Cada linha tem checksum. `park_history_cloud_batch()` monta até oito linhas datadas; `park_history_cloud_ack()` persiste o avanço após confirmação do servidor. Ignorar uma falha para avançar o cursor descartaria histórico ainda não enviado.

## Interfaces web

| Local | Conteúdo |
| --- | --- |
| `cloud/public/index.html` | Dashboard, autenticação e gestão. |
| `cloud/public/cloud.js` | Sessão, REST/RPC, gráfico SVG, histórico, CSV e ações do proprietário. |
| `cloud/public/cloud.css`, `refinements.css` | Estilos específicos online. |
| `cloud/public/config.js` | URL pública do Supabase, chave publishable e ID da placa. |
| `dashboard/local-public/index.html` | Câmera, calibração e Wi-Fi. |
| `dashboard/local-public/app.js` | Autenticação local, consultas leves, caixas e ajustes. |
| `dashboard/public/` | CSS base, fonte e ilustração compartilhados; não contém outra aplicação. |

Contadores de 64 bits trafegam como strings para preservar precisão em JavaScript. Mantenha isso nos relatórios e exportações. Leitura indisponível não deve virar zero.

## Banco

As migrações definem tabelas, RLS e funções. A ingestão autentica a placa, valida registros e deduplica envios. Consultas dependem do login e da associação ao parque. O acesso à câmera está em esquema privado e exige papel de proprietário.

Não use a chave administrativa no navegador para contornar políticas. Corrija política ou associação e execute testes de isolamento.

## Onde alterar

| Alteração | Ponto de partida | Verificação |
| --- | --- | --- |
| Linhas e sentido | Interface local | Passagens manuais nos dois sentidos |
| Rastreamento | `counting_core.hpp` | Casos C++ e validação em campo |
| Sensor/pinagem | `frame_cap_pipeline.cpp`, `sdkconfig.freenove` | Compilação e inicialização real |
| Gravação | `park_history.cpp` | Integridade, reinício e cartão indisponível |
| Reenvio | `park_cloud.cpp` | ACK, duplicação e recuperação offline |
| Gráficos online | `cloud/public/` | `node cloud/test_cloud.cjs` |
| Câmera web | `dashboard/local-public/` | `node dashboard/test_local.cjs` |
| Permissões | `supabase/migrations/` | Ambos os testes SQL |

Edite a fonte versionada e reaplique o firmware com `atualizar-firmware.ps1`. Não copie binários, registros reais ou credenciais para o Git.

## Formatação

`.editorconfig` define codificação, quebras de linha e indentação. `.prettierrc.json` configura HTML, CSS e JavaScript. Para manter a formatação desses arquivos, use Prettier 3.6.2, a versão utilizada nesta revisão:

```powershell
npm exec --yes --package prettier@3.6.2 -- prettier --write "cloud/public/*.{js,css,html}" "cloud/*.{cjs,mjs}" "dashboard/local-public/*.{js,html}" dashboard/public/style.css dashboard/test_local.cjs
```

Depois da formatação, execute os testes e o build. Recursos web locais alterados precisam de nova compilação/gravação para aparecer na ESP.
