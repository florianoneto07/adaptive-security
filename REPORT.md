# Relatório de implementação

O que mudei no código que já existia, o que não consegui verificar, e todo
parâmetro que precisei escolher por conta própria.

---

## 1. Inventário: o estado real ao começar

Confirmado executando, não só lendo.

| Peça | Estado encontrado |
|---|---|
| TLS 1.3 / TCP 4433 | funcional, compila sem advertências |
| DTLS 1.3 / UDP 4444 | funcional |
| `scripts/generate_certs.sh` | funcional |
| `scripts/run_local_test.sh` | **6/6 passaram** |
| `scripts/run_oscore_test.sh` | **4/4 passaram** |

### Três correções ao enunciado

**O defeito do `dtls_server.c` já estava corrigido.** O enunciado pedia para
verificar se o socket UDP recebia `bind()` sem nunca ser associado a um peer
antes de `wolfSSL_accept()`. Não está mais presente:
[`server/dtls_server.c:87`](server/dtls_server.c) faz `MSG_PEEK`, filtra o tipo
de registro 22, chama `connect()` e depois `wolfSSL_dtls_set_peer()`. Foi
resolvido no commit `30033c9`. **Não mexi nesse arquivo.**

**Não existia código OSCORE.** O enunciado supunha que OSCORE já funcionava. O
que funciona são os binários `coap-client`/`coap-server` **do próprio libcoap**,
dirigidos por dois shell scripts. Não havia uma linha de C nossa. O canal C2 é
código inteiramente novo.

**O IP desta VM é `192.168.218.128`, não `192.168.237.128`.** A outra VM
responde em `192.168.218.129`; `192.168.237.129` não responde. O repositório já
havia migrado para essa sub-rede no commit `f0d0f5a`.

---

## 2. O que mudei no código existente

### `scripts/setup_wolfssl.sh`

**Acrescentei `--enable-srtp`.** Sem ela o canal C3 não existe: os protótipos
`wolfSSL_CTX_set_tlsext_use_srtp()` e
`wolfSSL_export_dtls_srtp_keying_material()` estão em `ssl.h`, mas sob
`#ifdef WOLFSSL_SRTP`, e nenhum símbolo SRTP era compilado na biblioteca — o
erro apareceria só no link. Confirmei com `nm -D` antes e depois.

**Fixei o commit do wolfSSL** (`88c766b6b`). O script fazia
`git reset --hard origin/HEAD` num clone raso do branch `master`. Quem clonasse
este repositório amanhã compilaria contra outra revisão, e as métricas
deixariam de ser comparáveis — o que contraria diretamente o requisito de
reprodutibilidade. Sobrescrevível por `WOLFSSL_COMMIT`.

Acrescentei também um `make distclean` antes do build: trocar de flags de
`configure` sem limpar deixa objetos compilados sob as opções antigas, e o
defeito aparece como símbolo ausente no link de outro projeto.

### `scripts/setup_libcoap.sh`

**Fixei o commit do libcoap** (`dbeedd59b`), pelo mesmo motivo. O script seguia
o branch `develop`.

### `Makefile`

Reescrito para delegar aos quatro canais, preservando **inalterados** os alvos
dos binários legados. Passou a rodar `scripts/check_wolfssl.sh` antes de
compilar.

### `.gitignore`

Acrescentei `keys/` e `results/`. Nenhuma chave de canal é versionada.

### `README.md`

Reescrito para o testbed. O anterior descrevia um "motor de segurança
adaptativa" com seleção dinâmica, que este enunciado coloca explicitamente fora
de escopo.

### Não toquei

`server/tls_server.c`, `server/dtls_server.c`, `client/tls_client.c`,
`client/dtls_client.c`, `common/adaptive_security.h`,
`scripts/generate_certs.sh`, `scripts/generate_oscore_conf.sh`,
`scripts/run_local_test.sh`, `scripts/run_oscore_test.sh`,
`scripts/run_remote_test.sh`, `docs/RUNBOOK.md`, `testes/README.md`,
`STATUS.md`. Todos continuam passando.

---

## 3. Problemas de ambiente encontrados

### Duas instalações divergentes de wolfSSL, e o `make` usava a errada

| | `/usr/local` | `~/.local` |
|---|---|---|
| TLS13 / DTLS13 / CID / IP_ALT_NAME | sim | sim |
| `HAVE_AESCCM` | **não** | sim |
| PSK | **`NO_PSK`** | sim |
| `OPENSSL_ALL` | **não** | sim |

O `make` sem argumento resolvia por `pkg-config` e pegava `/usr/local`. Como os
canais C1, C3 e C4 autenticam por PSK, o build silenciosamente inutilizável era
o padrão.

Resolvido com [`scripts/check_wolfssl.sh`](scripts/check_wolfssl.sh), que lê o
`options.h` do prefixo que será efetivamente linkado e aborta listando o que
falta. **Não removi nem sobrescrevi a instalação de `/usr/local`.**

### `libsrtp2` ausente

Instalado do repositório Ubuntu (`libsrtp2-dev` 2.7.0) com sua autorização.

---

## 4. Decisão que reduz uma garantia de segurança

**Desliguei o modo B.1.2 da RFC 8613 no canal C2** (`rfc8613_b_1_2,bool,false`).

**O que é.** O Apêndice B.1.2 manda o servidor desafiar com a opção Echo
(RFC 9175) a primeira requisição de um cliente cujo número de sequência ele
ainda não conhece, para barrar repetição de mensagens antigas depois de o
servidor reiniciar.

**Por que desliguei.** Com o desafio ligado, o libcoap desta versão marca
`session->doing_first` na primeira requisição OSCORE (`coap_net.c`, trecho de
`oscore_encryption`) para segurar os envios seguintes até saber se o peer
responde ao Echo. O reenvio que **carrega** o Echo também cai nessa fila e
nunca é transmitido. O resultado é um deadlock: qualquer cliente que envie mais
de uma requisição fica travado. Confirmei lendo a fonte e reproduzindo com o
`coap-client` oficial.

**O que se perde.** Exatamente essa proteção: replay de mensagens capturadas
antes de um reinício do servidor. A janela anti-replay de 32 continua valendo
dentro da sessão, que é a defesa que importa durante uma execução.

**Por que é aceitável aqui.** As chaves são estáticas e injetadas, e o servidor
não reinicia no meio de uma execução, então o cenário que o B.1.2 cobre não
ocorre no testbed.

**Como reverter.** Uma linha em
[`channels/c2_telemetry/c2_oscore.h`](channels/c2_telemetry/c2_oscore.h), ciente
de que o C2 passa a atender uma requisição por execução.

Se preferir manter o B.1.2 ligado e viver com uma requisição por execução, é
uma troca legítima — me avise que eu inverto.

---

## 5. O que não consegui verificar

### A captura pcap não foi exercitada

O `setcap` do `tcpdump` ainda não foi aplicado, então a opção `--capture` do
`run.sh` nunca gravou um pcap. O código degrada com aviso quando o tcpdump não
sobe, mas o caminho de sucesso não foi percorrido. Consequência prática: para o
canal C2, que não tem contadores internos de bytes no fio, **ainda não há
medida de bytes no fio nenhuma**.

Para destravar:

```bash
sudo setcap cap_net_raw,cap_net_admin+eip /usr/bin/tcpdump
```

### Defeito no modo `--role server`, encontrado na primeira campanha real

Na primeira execução entre as duas VMs, `telemetry` passou 10/10 enquanto
`media` e `bulk` falharam em todas as repetições e `control` só funcionou na
primeira.

A causa era estrutural no orquestrador: no papel de servidor, os canais subiam
**em sequência**, esperando cada servidor terminar antes de subir o próximo. O
servidor de comando e controle atende uma associação e sai — daí `control`
funcionar só na repetição 1. Em seguida subia o de telemetria, que atende em
laço contínuo e nunca termina, e a fila parava ali: os servidores de mídia e
volumoso jamais chegaram a escutar. No cliente isso apareceu como
`error state on socket` no UDP e `connection refused` no TCP.

Servidores de canais independentes precisam coexistir, não se revezar. O modo
`--role server` passou a subir todos em paralelo, persistentes (`-k`), com as
métricas indo para `<run>/server/` em vez de por repetição — o processo é um só
para toda a campanha. Validado em loopback: 4 canais × 3 repetições, 12/12.

Junto disso, o Ctrl+C passou a abortar a campanha em vez de apenas interromper
o canal em andamento e seguir para o próximo.

### Só loopback, nunca entre as duas VMs

O par `--role server` / `--role client` foi exercitado entre as duas VMs uma
vez, o que revelou o defeito de serialização descrito acima; depois da correção,
só foi revalidado em loopback. **A campanha completa entre máquinas ainda não
rodou até o fim.**
Duas coisas que só aparecem lá: MTU real e o comportamento do C3 quando o
pacote SRTP de 1228 B encontra a MTU de 1500 com cabeçalhos.

### Emulação de enlace: exercitada e validada

Com `cap_net_admin` no `tc`, os perfis foram aplicados e medidos. Como o netem
age na interface de saída e o tráfego de loopback não passa por ela, apliquei os
perfis na própria `lo` para exercitar a instrumentação; a campanha de verdade
exige as duas VMs.

**Canal C1 sob `3gpp-c2` (50 ms, perda 0,1%):**

| | linha de base | com netem |
|---|---|---|
| RTT p50 | 0,31 ms | **100,6 ms** |
| RTT p99 | 0,67 ms | 215,8 ms |
| handshake | 13,7 ms | 309,6 ms |
| entregues/enviados | 512/512 | **510/512** |

Os 100,6 ms de RTT são os 50 ms aplicados em cada sentido, como esperado, e a
perda induzida aparece corretamente na contagem de lacunas de sequência.

**Canal C2 sob 30% de perda — a comparação central do experimento:**

| rep | leituras | retransmissões | CPU envio inicial | CPU retransmissão |
|---|---|---|---|---|
| 01 | 4 | 4 | 804.365 ns | **0** |
| 02 | 3 | 5 | 1.444.480 ns | **0** |
| 03 | 1 | 4 | 671.195 ns | **0** |

É o resultado que o enunciado buscava: o CoAP confirmável reenvia um objeto que
o OSCORE já protegeu, **sem cifrar de novo**, a custo de CPU zero — contra cerca
de 200 µs por mensagem na proteção inicial. DTLS e TLS não têm célula
correspondente porque não retransmitem dados de aplicação: no DTLS, perda é
perda; no TCP, a retransmissão acontece abaixo da aplicação e não refaz a
cifragem.

### Nove de dez repetições do C2 perdidas na primeira campanha com netem

A campanha reportou `telemetry 10 ok, 0 falhas`, mas ao agregar os resultados
apenas a repetição 1 tinha amostras de latência: as outras nove enviaram 30
leituras cada e não receberam nenhuma resposta.

A causa foi o número de sequência do OSCORE. Cada execução do cliente criava um
contexto novo e recomeçava a numeração do zero; o servidor, persistente para
toda a campanha, recusava como repetição e respondia `4.01`. O servidor estava
certo: reusar número de sequência com o mesmo par de chaves repete o nonce do
AEAD.

Duas correções. O cliente passou a persistir o Sender Sequence Number em
`keys/c2.seq` e a retomar dele, que é o mecanismo do Apêndice B.1.1 da RFC 8613
e é suportado pelo libcoap via `coap_new_oscore_conf()`. E o cliente passou a
falhar quando enviou tudo sem receber nada — antes retornava sucesso, e foi por
isso que uma campanha inteira passou por boa.

Vale registrar o que este episódio custou: o defeito não apareceu no canal, nem
no log do orquestrador, apenas na tabela agregada. Sem o agregador ele teria ido
para a tese.

### O overhead medido não bate com o citado no enunciado

| Canal | Enunciado | Medido |
|---|---|---|
| C1 / DTLS 1.3 | 11 B | **22,1 B** por registro |
| C4 / TLS 1.3 | 14 B | **22,0 B** por registro |
| C3 / SRTP | — | **28,0 B** por pacote |

Os 28 B do C3 decompõem-se exatamente: 12 B de cabeçalho RTP + 16 B de tag do
AES-GCM.

Para C1 e C4, 22 B é consistente com cabeçalho de registro + 1 B de tipo
interno + 16 B de tag AEAD. **Não confirmei** que os valores de 11 e 14 do
enunciado se referem ao cabeçalho de registro sem a tag — é a explicação
plausível, mas não a verifiquei contra as fontes originais, e o número que
reporto é o medido.

### Não verifiquei

- comportamento sob mais de um cliente por canal simultaneamente;
- vazamento de memória em execuções longas (rodei no máximo 25 s por canal);
- se o `wolfSSL_send_hrr_cookie()` do C1 e C3 realmente rejeita um ClientHello
  com endereço forjado — o caminho existe, mas não o exercitei.

---

## 6. Parâmetros que escolhi por conta própria

Todos expostos como opção de linha de comando e marcados no README como
"padrão do testbed, não vem da literatura".

| Onde | Valor | Justificativa |
|---|---|---|
| C2 recurso CoAP | `/telemetry` | nome arbitrário |
| C2 espera por resposta | 30 s | cobre as retransmissões do CoAP confirmável (RFC 7252 §4.8) sem prender o gerador |
| C2 semente do jitter | 1, fixa | torna a sequência de períodos reproduzível entre execuções |
| C3 bitrate | 4 Mbps | dentro de "alguns Mbps" para FHD (Wu et al.) |
| C3 payload RTP | 1200 B | cabe na MTU de 1500 com IP+UDP+RTP e a tag do AES-GCM |
| C3 perfil SRTP | `SRTP_AEAD_AES_256_GCM` | sua escolha; alinha com o `TLS_AES_256_GCM_SHA384` dos demais |
| C3 fim de fluxo | 5 s sem pacote | SRTP não tem `close_notify` |
| C3 SSRC e payload type | `0x0C3ACAFE`, PT 96 | PT 96 é dinâmico (RFC 3551); o SSRC é arbitrário |
| C4 tamanho do arquivo | 32 MiB | a literatura dá a taxa (50 Mbps), não o tamanho |
| C4 bloco | 16 KiB | casa com o registro máximo do TLS |
| PSK | 32 bytes | casa com a força do AES-256-GCM usado nos quatro canais |
| Master Salt do OSCORE | SHA-256("AS-OSCORE-SALT" ‖ chave), 8 B | as duas VMs precisam do mesmo valor sem trocar um segundo segredo; o rótulo separa o salt do fingerprint publicado no log |
| Sender/Recipient ID do C2 | `01` servidor, `02` cliente | arbitrário, precisa apenas ser simétrico |
| Identidades PSK | `uav-c1-control` etc. | rótulos legíveis; o servidor recusa identidade de outro canal |
| netem `handover` correlação | 25% | o netem sem correlação distribuiria a perda uniformemente, e um handover perde em rajada |
| netem `handover` jitter | ±20 ms sobre 200 ms | você fixou perda e atraso, não o jitter |
| Janela anti-replay SRTP | 128 | padrão usual do libsrtp2 |
| Timeout de retransmissão DTLS | 1 s a 8 s | mesmos valores já usados nos binários legados |

### Você fixou, eu apenas implementei

128 B a 64 Hz no C1; 60 s × 10 repetições; perfis netem `3gpp-c2` e `handover`;
`SRTP_AEAD_AES_256_GCM`; PSK pura com `psk_dhe_ke`; medir só retransmissão
nativa; padronizar o wolfSSL em `~/.local` com Makefile validando.

---

## 7. Defeitos que encontrei durante a implementação

Registrados porque cada um custou tempo e voltaria a morder.

1. **`wolfSSL_CTX_require_psk()` e `only_dhe_psk()` retornam `0` em sucesso**,
   não `WOLFSSL_SUCCESS` (que vale 1). Conferido em `src/tls13.c`.

2. **`wolfSSL_new()` copia os callbacks de I/O do contexto.** Instalá-los no
   `WOLFSSL_CTX` depois de criar a sessão não tem efeito, e o sintoma é
   silencioso: contadores zerados no fim da execução.

3. **O contexto dos callbacks de I/O tem tipos diferentes em TLS e DTLS.**
   `EmbedSend` faz `*(int*)ctx`; `EmbedSendTo` faz `(WOLFSSL_DTLS_CTX*)ctx`.
   Fabricar um contexto próprio funciona em TLS e causa segmentation fault em
   DTLS. A correção é preservar e repassar o contexto que o wolfSSL montou.

4. **`signal()` na glibc liga `SA_RESTART`.** Um servidor parado em
   `recvfrom()` nunca observava o pedido de parada e exigia `SIGKILL`, o que
   descartava as métricas do lado servidor — elas são gravadas no caminho
   normal de saída. Resolvido com `sigaction()` e `sa_flags = 0`
   ([`common/as_signal.h`](common/as_signal.h)).

5. **O exporter de material de chave exige `wolfSSL_KeepArrays()`** antes do
   handshake. Sem ele, o handshake fecha normalmente e a exportação falha — o
   motivo só aparece em build de debug.

6. **O `channel.mk` não declarava os headers do próprio canal como
   dependência.** Editar `c2_oscore.h` não recompilava nada, e servidor e
   cliente ficavam com versões diferentes da mesma lógica. Apareceu como falha
   de decifragem OSCORE, bem longe da causa.

7. **Um PDU de resposta não significa sucesso.** O cliente C2 contava como
   leitura bem-sucedida uma resposta `4.00 Decryption failed`. Passou a checar a
   classe do código antes de contar.

---

## 8. Estado dos requisitos

| Req. | Estado |
|---|---|
| R1 subida independente | atendido; `./run.sh control`, `control telemetry`, `all` testados |
| R2 perfil fixo, sem fallback | atendido; ver §"Perfil fixo" no README |
| R3 geradores por classe | atendido nos quatro canais |
| R4 instrumentação | atendido, com as quatro ressalvas do README |
| R5 emulação de enlace | atendido e validado; ver §5. Falta só o pcap (`--capture`) |
| R6 reprodutibilidade | atendido; manifesto por execução, commits das dependências fixados |
