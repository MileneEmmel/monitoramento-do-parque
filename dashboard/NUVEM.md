# Separação entre painel online e ajuste local

Decisão de 11/10/2026: duas interfaces, sem notebook como servidor.

## Site online

Preservar o visual do dashboard atual. Gráficos, histórico, resumo por dia e
exportação ficam online, com login e acesso apenas aos parques autorizados.
Dados no Supabase Free; hospedagem estática deve ter plano gratuito confirmado
antes da publicação. Nenhum serviço pago ou domínio comprado automaticamente.
Mostrar última sincronização, inclusive quando a ESP estiver offline.
Não publicar chaves administrativas nem credenciais de Wi-Fi.

## Site na ESP

Página de câmera para instalação e manutenção: imagem ao vivo, linhas A/B e
calibração. Wi-Fi e acesso local ficam disponíveis dentro dos ajustes dessa tela.
Não carregar gráficos nem histórico na versão local final.
O endereço local não é acessível de fora da rede. Por solicitação da usuária,
o dashboard online não tem menu nem atalho da câmera. O vídeo fica somente na ESP.
Não incorporar stream HTTP em HTTPS nem expor portas da ESP na internet.

## Sincronização

A contagem continua embarcada. O microSD permanece como histórico local e fila
de recuperação durante falta de internet. Enviar lotes por HTTPS, sem imagens,
com identificação única por registro e confirmação do servidor para permitir
reenvio sem duplicação. Não usar apenas o horário como identificador.
Não enviar pedidos de rede dentro do callback de inferência nem bloquear o
mutex do microSD durante HTTPS. Preservar totais grandes sem perda de precisão.

Autorização da placa deve ser limitada ao envio de dados do próprio dispositivo;
nunca colocar a chave service_role/secret do Supabase no firmware.
Políticas RLS limitam leitura/alteração aos responsáveis autorizados.
Definir como a limpeza manual do cartão trata registros ainda não sincronizados
antes de ativar a fila: impedir exclusão silenciosa de dados pendentes.

## Vídeo e carga

O código anterior preparava JPEG/base64 mesmo sem espectadores. A otimização
local suspende cópia/conversão quando não há consulta autenticada de imagem
há três segundos. Não desliga câmera nem IA, que continuam necessárias à contagem.
Não há promessa de redução específica de temperatura ou ganho de FPS sem medição.

## Transição

1. Criar projeto Supabase Free e registrar URL pública do projeto.
2. Implementar banco, políticas, ingestão e testes de permissões e duplicação.
3. Implementar fila/sincronização na ESP e testar reconexão e reinício.
4. Publicar o dashboard online com login e dados reais.
5. Conferir acesso remoto e só então substituir a interface local pela câmera.

Estado em 11/10/2026: Supabase configurado e testado; dashboard publicado em Sites
owner-private; sincronizador compilado e gravado. Login da aplicação verificado.
Primeiro envio real e recuperação offline ainda precisam de validação. O painel
local existente permanece até essa confirmação, antes da troca por câmera/ajustes.
Sites exige login ChatGPT além do login Supabase. Acesso de outros responsáveis
à hospedagem ainda precisa de definição; não presumir que o link é público.
