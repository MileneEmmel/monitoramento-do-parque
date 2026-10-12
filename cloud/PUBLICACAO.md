# Publicação na Vercel

Endereço principal: https://monitoramento-parque.vercel.app/.

O projeto usa hospedagem estática no plano Hobby. A publicação atual é manual, sem integração Git automática. Push no GitHub não atualiza o site nem a ESP.

## Build

Na raiz do repositório:

```powershell
node cloud/test_cloud.cjs
node cloud/test_build.mjs
node cloud/build.mjs
```

Somente os arquivos de `cloud/dist/` são públicos. Não envie `.env`, `.vercel`, logs, cartão, tokens de placa ou arquivos do firmware. O frontend usa somente a chave publishable; ela não concede leitura sem login/RLS.

## Configuração do projeto

Na importação Git pela Vercel, use Root Directory `cloud`, permita incluir os arquivos compartilhados fora dessa pasta e mantenha `vercel.json`: build `node build.mjs`, saída `dist`, framework sem detecção. Essa integração precisa ser configurada antes de depender de publicação automática.

Na publicação manual pela CLI, com o projeto correto vinculado e conta autenticada, a partir da raiz:

```powershell
vercel deploy cloud/dist --prod
```

Confira o projeto de destino antes de confirmar. Metadados locais de vínculo não devem ser copiados para o diretório público nem versionados.

## Verificação

Após publicar, teste login, gráfico do dia, histórico e CSV. Com proprietário, confira identificação e Equipamento. Com leitor, confira ausência das ações restritas. Uma sessão de outro domínio não é transferida automaticamente.

Abrir a câmera consulta uma função protegida do Supabase, mas o endereço retornado é HTTP local. Exige a mesma rede da ESP. Nenhum vídeo é publicado pela Vercel.

Migrações do Supabase e atualização do firmware são etapas separadas; não estão automatizadas por este deploy.
