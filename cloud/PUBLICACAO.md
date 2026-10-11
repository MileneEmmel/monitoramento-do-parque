# Publicação do painel

O frontend reutiliza `dashboard/public/style.css`, a fonte e a ilustração do
painel original da ESP. O gráfico SVG conserva eixos, barras, foco por teclado
e valores apenas durante interação. A câmera não faz parte do site online.

O projeto para a próxima hospedagem deve se chamar `monitoramento-parque`.
O endereço `monitoramento-parque.vercel.app` é pretendido, não confirmado.
Depende de disponibilidade e de uma conta Vercel autorizada. Não registrar domínio
pago nem ampliar acesso ao banco para publicar o frontend.

Na importação do repositório, usar Root Directory `cloud`, permitir incluir arquivos
fora dessa pasta (fonte, CSS e ilustração em `dashboard/public`), e as opções de
`vercel.json`: build `node build.mjs`, saída `dist`. Sem chave secreta de ambiente.

O login e as regras RLS do Supabase continuam obrigatórios. O bundle contém apenas
a chave publishable e um UUID público da placa. Não publicar token de envio,
senha do banco, arquivos do microSD, vídeo ou logs. O site atual permanece privado
até a nova hospedagem e seu controle de acesso serem confirmados.
