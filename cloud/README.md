# Dashboard online

Frontend estático com login Supabase, gráfico de entradas/saídas, histórico, CSV e gestão restrita ao proprietário. Não recebe vídeo da câmera. A identificação do parque é editável; a interface está vinculada a uma placa, sem seletor de parques.

## Desenvolvimento

Requer Node.js 22 ou superior, sem dependências npm. Na raiz:

```powershell
node cloud/test_cloud.cjs
node cloud/test_build.mjs
node cloud/build.mjs
```

Para uma prévia local, sirva `cloud/dist/` com um servidor HTTP estático de desenvolvimento. Ele não é necessário após a publicação e não guarda histórico.

`public/config.js` contém URL do Supabase, chave publishable e ID público da placa. As permissões vêm do login e das políticas do banco. Nunca coloque token da ESP ou chave administrativa nesse arquivo.

`build.mjs` copia os recursos públicos e os recursos compartilhados de `dashboard/public/`. A saída é `dist/`; arquivos inesperados são rejeitados e recursos antigos de build são removidos. Credenciais e metadados de publicação devem ficar fora da saída.

`test_cloud.cjs` verifica vínculos DOM, precisão de inteiros, gráfico e acesso do proprietário à câmera. `test_build.mjs` verifica a saída e a rejeição de arquivos privados.

Consulte [PUBLICACAO.md](PUBLICACAO.md) para publicar e [../supabase/README.md](../supabase/README.md) para configurar banco e permissões.
