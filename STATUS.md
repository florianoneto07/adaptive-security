# Status do projeto

Última atualização: 2026-09-01

## Concluído

- Comunicação IP entre as duas VMs (`192.168.218.128` ↔ `192.168.218.129`).
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

- **DTLS 1.3 fechando handshake**, verificado em loopback com wolfSSL 5.9.2
  (`DTLSv1.3`, `TLS_AES_256_GCM_SHA384`). A causa do handshake não fechar era
  `wolfSSL_accept()` num socket UDP sem peer definido: como UDP não tem
  `accept()`, as respostas do servidor não tinham destino. O servidor passou a
  descobrir a origem com `MSG_PEEK`, conectar o socket e informar o wolfSSL.
- Cookie de `HelloRetryRequest` habilitado em DTLS 1.3.
- Timeouts de retransmissão configurados (1s a 8s).
- Clientes aceitam hostname além de literal IP, via `getaddrinfo()`.

## Em andamento

- **Validação do DTLS 1.3 entre as duas VMs.** Em loopback funciona; falta
  confirmar sobre a rede real, onde MTU, fragmentação e perda de datagramas
  entram em jogo.

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
