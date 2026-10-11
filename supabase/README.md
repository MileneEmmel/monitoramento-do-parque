# Supabase

Projeto `monitoramento-do-parque`, Free, São Paulo (sa-east-1).
URL pública de identificação: https://pcxeestzvcpbfkcbuqpa.supabase.co

As quatro migrações em `migrations/` foram aplicadas pelo SQL Editor em 11/10/2026.
Não reaplicar sobre as mesmas tabelas. Uma futura integração de migrações precisa
registrar essa linha de base. Ver `../dashboard/NUVEM.md` para a arquitetura.

RLS limita leitura aos membros do parque. Só proprietários cadastram placas.
Cada token de ESP permite apenas enviar dados do dispositivo correspondente;
o banco armazena apenas seu SHA-256, em esquema privado.
`tests.sql` passou: token incorreto e leitura anônima negados, isolamento entre
usuários, deduplicação, inteiros exatos, fuso de Brasília, limite de período,
rotação de credencial e edição da identificação somente pelo proprietário.
Os dados sintéticos foram revertidos por ROLLBACK.

O painel fica vinculado à ESP definida pelo identificador público `deviceId` em
`cloud/public/config.js`; o identificador não é uma chave e não contorna RLS.
Não há seleção nem criação de parques na interface. O proprietário pode editar
nome e ponto de contagem em “Editar identificação”, sem criar outro cadastro.
Ativação: entre no painel, gere a credencial em Equipamento
e execute `firmware/object_detect/configure_cloud.py` no terminal com pyserial.
Informe ID e token interativamente. O token não aparece nem é salvo no notebook;
ele vai pelo USB apenas para a Freenove identificada pelo número de série.
`--status` consulta a situação sem imprimir credenciais.

A chave publishable no frontend é pública e não substitui login/RLS. O token
da placa fica na NVS, sem criptografia de flash configurada: proteja acesso físico.
O cursor do microSD só avança após confirmação HTTPS. Linhas corrompidas bloqueiam
o lote; registros ainda não enviados impedem a limpeza manual. Não há envio de
imagens nem exclusão automática. Testes reais de reinício e recuperação offline
ainda precisam ser realizados; testes SQL não substituem testes de hardware.

Integração GitHub ainda não autorizada/configurada. A conexão pode automatizar
migrações em `supabase/migrations`, mas não hospeda automaticamente o frontend
nem grava firmware na placa. Não ativar branching pago ou upgrade.

Nunca adicionar senha do banco, service_role, secret API key ou token de dispositivo
ao repositório. Políticas devem restringir leitura por responsável e ingestão
ao dispositivo correspondente. Testar duplicação, reinício e recuperação offline.
