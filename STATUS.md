# Status do projeto

Última atualização: 2026-09-01

## Concluído

- Comunicação IP entre as duas VMs (`192.168.237.128` ↔ `192.168.237.129`).
- Cliente e servidor em C sobre wolfSSL.
- TLS 1.3 funcional sobre TCP/4433.
- Handshake TLS 1.3 validado E2E.
- Troca bidirecional de mensagens.
- Cipher observado no teste: `TLS_AES_256_GCM_SHA384`.
- Geração de certificados automatizada, parametrizável por IP e com SANs
  múltiplos.
- Build unificado via `Makefile`.
- **Verificação de identidade do peer** nos dois clientes: o endereço usado
  para conectar precisa constar do `subjectAltName` do certificado. Antes, a
  validação parava na cadeia, e qualquer certificado emitido pela mesma CA era
  aceito.
- **Suíte de teste E2E em loopback** (`scripts/run_local_test.sh`), incluindo
  casos negativos que exigem a rejeição de certificado com cadeia válida e
  identidade errada.

## Em andamento

- DTLS 1.3 sobre UDP/4444.
  - Descoberta do peer via `MSG_PEEK` + socket UDP conectado, corrigindo a
    causa provável do handshake não fechar (`wolfSSL_accept()` era chamado sem
    peer definido, então as respostas do servidor não tinham destino).
  - Cookie de `HelloRetryRequest` habilitado em DTLS 1.3.
  - Timeouts de retransmissão configurados (1s a 8s).
  - **Falta validar E2E entre as duas VMs.**

## Ainda não implementado

- CoAP.
- OSCORE.
- Motor de decisão adaptativo (seleção TLS / DTLS / OSCORE por contexto).
- Coleta automatizada de métricas (tempo de handshake, RTT, overhead de bytes).
- Integração com o projeto de AKMA para 5G.

## Débitos técnicos conhecidos

- Os servidores atendem a uma única sessão e encerram; não há loop de aceitação
  nem concorrência. Vai incomodar na etapa de coleta de métricas, que precisa
  de várias repetições.
- Sem autenticação mútua: o servidor não solicita certificado do cliente. Para
  AKMA isso muda, já que a identidade do UE precisa ser verificada.
- O servidor DTLS atende um peer por execução. Um servidor real precisa
  demultiplexar várias associações sobre o mesmo socket, ou usar um socket por
  peer.
- Sem revogação (CRL/OCSP).
- As mensagens trocadas são fixas; ainda não há payload configurável para medir
  overhead com tamanhos variados.
