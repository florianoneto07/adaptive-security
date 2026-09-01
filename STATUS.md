# Status do projeto

Última atualização: 2026-09-01

## Concluído

- Comunicação IP entre as duas VMs (`192.168.237.128` ↔ `192.168.237.129`).
- Cliente e servidor em C sobre wolfSSL.
- TLS 1.3 funcional sobre TCP/4433.
- Handshake TLS 1.3 validado E2E.
- Certificado do servidor validado pelo cliente contra a CA de teste.
- Troca bidirecional de mensagens.
- Cipher observado no teste: `TLS_AES_256_GCM_SHA384`.
- Geração de certificados automatizada e parametrizável por IP.
- Build unificado via `Makefile`.

## Em andamento

- Migração da arquitetura para UDP/DTLS 1.3.
- Cliente DTLS 1.3 (`client/dtls_client.c`) — baseline escrito.
- Servidor DTLS 1.3 (`server/dtls_server.c`) — baseline escrito.
- Teste E2E DTLS 1.3.

### Pontos abertos no DTLS

- Definir a estratégia de peer no servidor (socket UDP conectado vs.
  `wolfSSL_dtls_set_peer`).
- Avaliar o uso de cookie / `HelloVerifyRequest` contra amplificação.
- Ajustar timeouts de retransmissão do handshake.

## Ainda não implementado

- CoAP.
- OSCORE.
- Motor de decisão adaptativo (seleção TLS / DTLS / OSCORE por contexto).
- Coleta automatizada de métricas (tempo de handshake, RTT, overhead de bytes).
- Integração com o projeto de AKMA para 5G.

## Débitos técnicos conhecidos

- Os clientes não fazem verificação explícita de nome/IP do peer
  (`wolfSSL_check_domain_name`); hoje dependem apenas da validação de cadeia.
- Os servidores atendem a uma única conexão e encerram; não há loop de
  aceitação nem concorrência.
- Não há suíte de testes automatizada.
