# Interface local da ESP

`local-public/` contém a câmera e os ajustes de calibração e Wi-Fi. `public/` contém apenas CSS, fonte e ilustração compartilhados com o dashboard online. Esses recursos são incorporados ao firmware pelo CMake.

Não há servidor de dashboard no notebook. Para abrir a ESP na rede salva:

```powershell
python .\firmware\object_detect\configure_wifi.py --port COM5 --status
```

Execute na raiz do repositório. O configurador obtém a URL completa por USB e abre o navegador sem imprimir a chave. O proprietário também pode abrir a câmera em Equipamento no dashboard online, com heartbeat recente e na mesma rede local.

`app.js` consulta `/api/local` para status e `/api/frame` para imagens. As requisições usam a chave local. Pausar ou ocultar a página interrompe a atualização da prévia, sem suspender o detector.

## Arquivos de apoio

- `test_local.cjs`: regressões de autenticação, atualização, prévia e calibração com ambiente de navegador simulado.
- `verify_device.py`: diagnóstico de hardware por USB e HTTP, sem exibir chaves ou logs brutos.
- `verify_cleanup.py`: prévia de limpeza e validação das proteções, sem exclusão.
- `DESIGN.md`: referência dos componentes e estilos compartilhados.
- `FONT-SOURCE.md` e `FONT-LICENSE.txt`: procedência e licença da fonte.

Teste sem placa: `node dashboard/test_local.cjs`. Procedimentos de instalação e diagnóstico estão em [docs/OPERACAO.md](../docs/OPERACAO.md).
