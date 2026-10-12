# Banco, autenticação e ingestão

Supabase fornece PostgreSQL, autenticação de usuários e funções REST/RPC. O frontend usa chave publishable; a ESP usa credencial de envio exclusiva, sem acesso administrativo.

## Migrações

| Arquivo | Conteúdo |
| --- | --- |
| `202610110001_monitoramento.sql` | Estrutura, membros, dispositivos, ingestão e relatórios com RLS. |
| `202610110002_limite_periodo.sql` | Limites de período dos relatórios. |
| `202610110003_rotacao_credencial.sql` | Rotação da credencial de envio. |
| `202610110004_identificacao.sql` | Edição de identificação do parque. |
| `202610110005_camera_local.sql` | Metadados privados e acesso do proprietário à câmera. |

As cinco migrações foram aplicadas no projeto existente pelo SQL Editor. Não reaplique scripts de criação sobre as mesmas tabelas. Uma integração futura de migrations precisa registrar essa linha de base. Em outro projeto, revise os scripts e aplique-os na ordem em um banco vazio.

## Acesso

Leitura exige usuário autenticado associado ao parque. Proprietário pode editar identificação, gerir dispositivo e obter o acesso à câmera; leitor consulta relatórios. Criar usuário em Auth não cria automaticamente a associação ao parque.

O dashboard usa o `deviceId` público de `cloud/public/config.js`. O ID não ignora RLS. A interface não cria nem seleciona vários parques.

A credencial da ESP é validada contra seu SHA-256 em esquema privado e só permite ingestão da placa correspondente. Gere-a em Equipamento e grave por `firmware/object_detect/configure_cloud.py`. Rotacionar invalida a chave anterior; mantenha ID e token do mesmo cadastro.

## Integridade

Registros têm identificadores derivados das linhas do diário e são deduplicados. O cursor do microSD avança somente após confirmação HTTPS. Lote incompleto/corrompido não deve ser confirmado. Registros sem horário conhecido não são atribuídos a uma data fictícia.

## Câmera local

`park_private.camera_access` guarda endereço IPv4 privado e chave local enviados pela ESP em HTTPS. Não recebe senha de Wi-Fi, SSID ou vídeo. Administradores do banco podem acessar esse esquema.

`park_camera_access` exige proprietário e dispositivo ativo. A consulta comum retorna metadados; a URL com chave exige ação explícita com `p_include_key=true` e atualização de menos de dois minutos. Readers não recebem o acesso. Isso descobre o endereço local, sem torná-lo acessível fora da rede.

## Testes

`tests.sql` verifica ingestão, deduplicação, precisão, períodos, fuso, permissões, rotação e identificação. `tests_camera.sql` verifica privacidade, endereços permitidos, atualização, expiração e compatibilidade do ACK.

Execute os testes completos em um ambiente controlado. Os scripts usam dados sintéticos em transação e terminam com `ROLLBACK`; não substituem backup ou ensaio de hardware. Eles foram executados no banco durante a implantação; uma revisão local de arquivos não equivale a nova execução remota.

Nunca versionar senha do banco, `service_role`, secret API key ou credencial da ESP. A NVS da placa não tem criptografia configurada. Não resolva autorização desabilitando RLS ou apagando o cartão.
