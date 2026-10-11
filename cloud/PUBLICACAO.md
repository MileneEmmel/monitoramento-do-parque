# Publicação do painel

O frontend reutiliza `dashboard/public/style.css`, a fonte e a ilustração do
painel original da ESP. O gráfico SVG conserva eixos, barras, foco por teclado
e valores apenas durante interação. A câmera não faz parte do site online.

Publicado em 11/10/2026 na conta Vercel da usuária, plano Hobby, projeto
`monitoramento-parque`: https://monitoramento-parque.vercel.app/.
Foi enviado somente `cloud/dist` (oito arquivos públicos), usando Drop to Deploy.
Não há integração Git automática configurada nesta publicação. Atualizações
precisam ser publicadas novamente ou ter essa integração autorizada.
Não foi registrado domínio pago nem ampliado o acesso ao banco.

Na importação do repositório, usar Root Directory `cloud`, permitir incluir arquivos
fora dessa pasta (fonte, CSS e ilustração em `dashboard/public`), e as opções de
`vercel.json`: build `node build.mjs`, saída `dist`. Sem chave secreta de ambiente.

O login e as regras RLS do Supabase continuam obrigatórios. O bundle contém apenas
a chave publishable e um UUID público da placa. Não publicar token de envio,
senha do banco, arquivos do microSD, vídeo ou logs. O endereço antigo em
chatgpt.site permanece privado e não foi excluído. No novo endereço, a tela de
login é pública; o histórico continua protegido pelo login e pelas regras RLS.
Não houve transferência de sessão entre domínios.
