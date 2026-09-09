# Testbed de perfis de segurança fixos por classe de aplicação UAV

Simula o enlace entre um UAV e uma estação de controle em solo, sobre duas VMs,
antes de levar para hardware real.

A proposta é: **cada classe de aplicação do UAV tem um perfil de segurança
fixo, atribuído em projeto e não negociado em tempo de execução.** Isso é
deliberado. Seleção dinâmica ou adaptativa de perfil está explicitamente **fora
de escopo** nesta fase.

## Os quatro canais

| ID | Classe de aplicação | Perfil de segurança | Transporte | Porta |
|----|--------------------|--------------------|-----------|-------|
| C1 | Comando e controle | DTLS 1.3 | UDP | 5001 |
| C2 | Telemetria periódica | OSCORE sobre CoAP | UDP | 5002 |
| C3 | Vídeo e áudio em tempo real | handshake DTLS 1.3 + SRTP na mídia | UDP | 5003 |
| C4 | Missão, firmware, logs | TLS 1.3 | TCP | 5004 |

A atribuição vive em [`common/as_profile.h`](common/as_profile.h), numa tabela
`const`. Cada binário escolhe sua entrada em tempo de compilação, e não há
caminho de código que troque o perfil de um canal pelo de outro.

### Por que cada perfil foi atribuído àquela classe

**C1 / DTLS 1.3.** O requisito 3GPP para comando e controle de UAV é latência
de 50 ms e taxa de erro de pacote de 10⁻³ [Zeng et al., arXiv 1903.05289,
Tab. I; Fotouhi et al., arXiv 2005.00781, Tab. 1], o que exige UDP — o TCP
retransmitiria e ordenaria mensagens de controle que já perderam a validade
quando chegam atrasadas. DTLS 1.3 tem sigilo futuro obrigatório e overhead de
registro de 11 bytes. O Connection ID (RFC 9146) cobre troca de endereço em
mobilidade.

**C2 / OSCORE.** Mensagens pequenas e periódicas, tolerantes a atraso, que
podem atravessar proxy ou sistema UTM. OSCORE (RFC 8613) mantém a segurança
fim a fim através de intermediários; TLS e DTLS não mantêm, porque são
terminados no proxy e a mensagem segue em claro do outro lado.

**C3 / DTLS-SRTP.** Mídia em tempo real usa RTP. O DTLS aqui serve **apenas
para estabelecer as chaves**, pela extensão `use_srtp` (RFC 5764); quem protege
os pacotes de mídia é o SRTP. Não é a camada de registro do DTLS protegendo o
vídeo.

> **Este perfil está fora dos perfis Ua\* do 3GPP.** É proposta do autor, não
> uma escolha derivada de norma.

**C4 / TLS 1.3.** Transferência volumosa e diferida, em que confiabilidade e
ordenação importam mais que latência — TCP é adequado. Overhead de registro de
14 bytes.

## Chaves

Cada canal usa **sua própria chave pré-compartilhada, distinta das demais**.
Isso simula a separação de chave por aplicação do AKMA, em que cada classe de
aplicação corresponde a um Application Function com seu próprio K_AF, e uma
aplicação não alcança a chave de outra.

As chaves são **injetadas**, por arquivo ou variável de ambiente. Derivação
AKMA está fora de escopo nesta fase.

```bash
./scripts/generate_keys.sh
```

Gera `keys/c1.key` … `keys/c4.key`, com 32 bytes cada, independentes entre si,
e imprime um fingerprint por chave. Leve à VM cliente as que ela precisa:

```bash
scp keys/c1.key [USUARIO]@[IP_DO_CLIENTE]:~/adaptive-security-prototype/keys/
```

Confira que os fingerprints batem nas duas VMs. Chaves divergentes se
manifestam como falha de handshake genérica, e é o defeito mais penoso de
diagnosticar nesta montagem.

Ordem de precedência na carga: variável do canal (`AS_C1_KEY`… em hex),
depois `AS_KEY_FILE`, depois `$AS_KEY_DIR/<canal>.key` (padrão `keys/`).

C1, C3 e C4 usam a chave como PSK de handshake (TLS/DTLS 1.3, modo
`psk_dhe_ke`, sem certificado). C2 a usa como Master Secret do contexto OSCORE.

## Dependências

- **wolfSSL** com TLS 1.3, DTLS 1.3, PSK, AES-CCM e **SRTP**
- **libcoap** com OSCORE, sobre o mesmo wolfSSL
- **libsrtp2** (pacote `libsrtp2-dev`)

```bash
sudo apt install -y build-essential autoconf automake libtool pkg-config git libsrtp2-dev
./scripts/setup_wolfssl.sh "$HOME/.local"
./scripts/setup_libcoap.sh "$HOME/.local"
```

Os dois scripts fixam o commit exato do wolfSSL e do libcoap. Isso é
deliberado: com `origin/HEAD`, cada clone deste repositório compilaria contra
uma revisão diferente e as métricas deixariam de ser comparáveis. Para testar
outra revisão, use `WOLFSSL_COMMIT=` ou `LIBCOAP_COMMIT=`.

Com o wolfSSL fora de `/usr/local`, **toda execução precisa de
`LD_LIBRARY_PATH`**:

```bash
echo 'export LD_LIBRARY_PATH=$HOME/.local/lib:${LD_LIBRARY_PATH:-}' >> ~/.bashrc
```

O `run.sh` resolve isso sozinho quando encontra o wolfSSL em `~/.local`.

## Compilação

```bash
make WOLFSSL_DIR="$HOME/.local"
```

Antes de compilar, o `make` roda [`scripts/check_wolfssl.sh`](scripts/check_wolfssl.sh),
que confere as opções da build do wolfSSL e aborta com a lista do que falta.
Sem essa checagem, uma build sem PSK compila sem erro e falha só em execução,
com sintoma idêntico ao de chave divergente entre as VMs.

Outros alvos: `make c1` (idem `c2`, `c3`, `c4`), `make channels`,
`make legacy`, `make keys`, `make clean`.

## Como rodar

```bash
./run.sh control
./run.sh control telemetry
./run.sh all
```

Cada canal é um par de binários independente: sobe, roda e é derrubado sem
afetar os outros. A falha de um canal não interrompe os demais — o relatório
final diz quais passaram.

Opções principais:

| Opção | Padrão | O que faz |
|---|---|---|
| `--role both\|server\|client` | `both` | papel desta máquina; `both` roda em loopback |
| `--host ENDERECO` | `127.0.0.1` | servidor a contatar quando `--role client` |
| `--duration SEG` | `60` | duração por execução |
| `--repeat N` | `10` | repetições por canal |
| `--bulk-size MiB` | `32` | tamanho da transferência do C4 |
| `--netem PERFIL` | — | aplica o perfil antes e remove depois |
| `--capture` | — | grava pcap por canal (exige `CAP_NET_RAW`) |
| `--port-offset N` | `0` | desloca as portas, para execuções em paralelo |
| `--out DIR` | `results` | raiz dos resultados |

### Campanha completa, as três condições

O caminho recomendado para colher dados de publicação. Roda na VM servidora e
comanda a cliente por SSH:

```bash
./campaign.sh --client-ssh lab-client --host [IP_DO_SERVIDOR] --capture
```

Percorre `clean`, `3gpp-c2` e `handover` em sequência, aplicando e removendo o
netem **nas duas VMs** a cada condição, e agrega tudo em um único
`resumo.csv` com uma linha por condição e canal — o formato direto para gráfico.

Antes de começar, confere que as duas VMs estão no mesmo commit e recusa a
campanha se não estiverem: mais de uma hora de medição atribuída a uma versão
que só vale para um dos lados não serve para nada. `--dry-run` mostra o plano e
a estimativa de tempo sem executar.

```
results/campaign-<timestamp>-<commit>/
  clean/       servidor/ e cliente/ com CSVs brutos, logs e manifestos
  3gpp-c2/
  handover/
  resumo.csv   uma linha por (condição, canal)
  campaign.log
```

Uma campanha completa de dez repetições leva por volta de duas horas.

### Entre as duas VMs

Na VM servidora (estação de solo):

```bash
./run.sh --role server --duration 60 --repeat 10 all
```

Na VM cliente (UAV):

```bash
./run.sh --role client --host [IP_DO_SERVIDOR] --duration 60 --repeat 10 all
```

### Individualmente, sem o orquestrador

```bash
./build/c1_server -p 5001 -o results/manual
```

```bash
./build/c1_client -H [IP_DO_SERVIDOR] -p 5001 -d 60 -o results/manual
```

Todo binário aceita `-h`.

## Emulação de enlace

```bash
./netem.sh 3gpp-c2      # 50 ms unidirecional, perda 0,1%
./netem.sh handover     # 200 ms ± 20 ms, perda 20% com correlação de 25%
./netem.sh off          # remove
./netem.sh status
./netem.sh custom "delay 30ms 5ms loss 1%"
```

Dois cuidados que mudam a leitura dos resultados:

1. **O netem age só na saída da interface.** Para atraso simétrico, aplique o
   mesmo perfil nas **duas** VMs. Aplicado só numa, o pacote atrasa num sentido
   e volta sem atraso.
2. **Tráfego de loopback não passa pela interface física.** Testes em
   `127.0.0.1` ignoram o netem silenciosamente. Use as duas VMs.

A linha de base é rodar sem netem: sem ela não há como atribuir uma degradação
ao enlace em vez de ao perfil de segurança.

`tc` exige `CAP_NET_ADMIN`. Uma vez, permanentemente:

```bash
sudo setcap cap_net_admin+eip /usr/sbin/tc
```

Para o `--capture`, o mesmo vale para o tcpdump:

```bash
sudo setcap cap_net_raw,cap_net_admin+eip /usr/bin/tcpdump
```

## Como ler os CSVs

Cada execução produz `results/<run_id>/rep<NN>/<canal>_<papel>.csv` e
`.summary`, mais um `manifest.json` na raiz da execução.

### O CSV: uma linha por evento

```
event,seq,t_rel_ns,rtt_ns,app_bytes,cpu_ns
handshake,0,13880564,13701822,0,7239162
msg,0,13983914,0,128,32645
rtt,0,14122479,223978,0,0
```

| `event` | Significado |
|---|---|
| `handshake` | estabelecimento concluído; `rtt_ns` é o tempo de parede, `cpu_ns` o de CPU |
| `msg` | mensagem de aplicação enviada; `cpu_ns` é o custo de protegê-la |
| `rtt` | ida e volta de uma mensagem já contada em `msg` |
| `recv` | mensagem recebida (lado servidor); `cpu_ns` é o custo de verificá-la |
| `retx` | retransmissão **nativa** do protocolo |
| `lost` | mensagem enviada que não chegou (lacuna de número de sequência) |

Tempos em nanossegundos; volumes em bytes. A conversão para as unidades da
literatura (kbps, ms) fica na leitura, não na coleta — o CSV é bruto de
propósito, para que percentis e razões possam ser recalculados com outro
critério sem repetir o experimento.

### O `.summary`: agregados

Campos em `chave=valor`. Os que respondem diretamente às métricas pedidas:

| Métrica | Campos |
|---|---|
| bytes no fio vs. payload | `wire_tx_bytes`, `app_bytes_tx`, `ip_udp_tcp_overhead_bytes` |
| estabelecimento | `handshake_wall_ns`, `handshake_cpu_ns`, `handshake_wire_tx_bytes`, `handshake_wire_rx_bytes` |
| distribuição de atraso | `rtt_p50_ns`, `rtt_p95_ns`, `rtt_p99_ns`, `rtt_samples` |
| entregues / enviados | `msgs_recv` (servidor) sobre `msgs_sent` (cliente); `msgs_lost` |
| CPU por mensagem | `cpu_first_send_ns` e `cpu_retransmit_ns`, sobre `msgs_sent` e `msgs_retx` |
| CPU de verificação | `cpu_verify_ns` sobre `msgs_recv` |
| memória residente | `rss_peak_kb` (`VmHWM`) |

O overhead por mensagem sai de:

```
(wire_tx_bytes - handshake_wire_tx_bytes - app_bytes_tx) / msgs_sent
```

`ip_udp_tcp_overhead_bytes` é **calculado**, não medido: 28 B por datagrama UDP
ou 40 B por segmento TCP, que o processo não enxerga. A captura em pcap
(`--capture`) é a medida direta.

### Quatro ressalvas que mudam a interpretação

**1. `wire_instrumented=0` no C2.** O libcoap gerencia os próprios sockets e
não expõe estatística de bytes, então os campos `wire_*` do canal C2 ficam
zerados. Para esse canal, os bytes no fio saem **só** do pcap (`--capture`).

**2. A janela de CPU do C2 é mais larga.** Em C1, C3 e C4 o `cpu_ns` de `msg`
cobre apenas a proteção do envio. No C2 cobre proteger o pedido **e** verificar
a resposta, porque a API síncrona do libcoap não expõe ponto de medição entre
as duas metades. Não tome isso por um custo maior do OSCORE.

**3. Retransmissão só existe no C2.** Com a decisão de medir apenas o que cada
protocolo faz nativamente, `msgs_retx` e `cpu_retransmit_ns` são preenchidos
somente pelo CoAP confirmável, que reenvia um objeto que o OSCORE já protegeu,
sem cifrar de novo. C1 e C3 ficam vazios porque **o DTLS não retransmite dados
de aplicação** — perda é perda. C4 fica vazio porque a retransmissão do TCP
acontece abaixo da aplicação e não refaz a cifragem. Essas células vazias são o
resultado, não uma lacuna de instrumentação.

**4. O atraso do C3 é unidirecional e não tem relógio comum.** A mídia é
unidirecional, então não há ida e volta. O receptor calcula a diferença entre a
marca de tempo do emissor e sua própria chegada; como os relógios das duas VMs
não são sincronizados, o valor carrega um deslocamento constante desconhecido.
**Use as diferenças entre percentis (p99 − p50), não os valores absolutos.**

### Bytes no fio pela captura

O canal C2 não tem contadores internos, então a captura é a única fonte de bytes
no fio para o OSCORE. Rode a campanha com `--capture` e depois:

```bash
./scripts/pcap_bytes.py results/<run_id>/server/*.pcap
```

```
ARQUIVO                 ENLACE           PACOTES    BYTES IP+  MÉDIA/PKT
c2_telemetry.pcap       Ethernet             420        21420       51.0
```

`BYTES IP+` desconta o cabeçalho de enlace, ficando comparável com os contadores
internos dos outros canais, que enxergam a partir do IP. Some 28 B por datagrama
UDP (ou 40 B por segmento TCP) aos contadores internos para comparar com a
captura:

```
contadores + 28 × pacotes  ≈  bytes do pcap
```

Numa campanha real de 6,5 MB no C1, as duas medições ficaram a 0,036% uma da
outra — a diferença são os pacotes que o tcpdump perde ao iniciar e ao encerrar.
Vale como verificação independente da instrumentação.

Uma observação sobre volume: uma campanha de dez repetições com `--capture`
produz centenas de megabytes por canal de mídia e de transferência volumosa.
Apague os pcaps que já foram processados, ou capture só nos canais que precisa.

### Agregando as repetições

Comparar 40 arquivos `.summary` a olho não escala. O agregador produz uma tabela
única com os quatro perfis lado a lado:

```bash
./scripts/summarize.py results/<run_id>
```

Numa campanha entre as duas VMs, os resultados do cliente e os do servidor ficam
em máquinas diferentes. Copie um para junto do outro e passe os dois:

```bash
scp -r [USUARIO]@[IP_DO_SERVIDOR]:~/adaptive-security-prototype/results/<run_servidor> /tmp/
./scripts/summarize.py results/<run_cliente> /tmp/<run_servidor>
```

`--csv arquivo.csv` grava a tabela agregada para levar a uma planilha ou gráfico.

A agregação entre repetições usa **mediana**, não média: latência tem cauda
longa, e uma repetição em que o escalonador atrapalhou deslocaria a média sem
dizer nada sobre o perfil. Os percentis vêm calculados de dentro de cada
execução, sobre todas as amostras daquela repetição.

O relatório anota sozinho as três armadilhas de leitura: que o C3 mede atraso
unidirecional sem relógio comum, que as colunas de percentil do C4 trazem o
tempo da transferência completa e não latência por mensagem, e que células
vazias em `CPU/retx` são o resultado, não instrumentação faltando.

### O manifesto

`results/<run_id>/manifest.json` traz commit git (com marca de árvore suja),
versões de wolfSSL, libcoap, libsrtp2 e kernel, perfil netem aplicado, todos os
parâmetros dos geradores, timestamp e o fingerprint de cada chave — nunca a
chave.

## Parâmetros dos geradores

### Vindos da literatura

| Canal | Parâmetro | Fonte |
|---|---|---|
| C1 | 60–100 kbps, PER 10⁻³, latência 50 ms | Zeng et al., arXiv 1903.05289, Tab. I; Fotouhi et al., arXiv 2005.00781, Tab. 1 |
| C1 | mensagem MAVLink bem menor que 256 B | arXiv 2404.07557 |
| C2 | leitura de 2 B a cada 2 ± 0,5 s | Gündoğan et al., arXiv 2001.08023, §V-A |
| C3 | FHD exige alguns Mbps; 4K acima de 30 Mbps | Wu et al., arXiv 1804.02217 |
| C4 | dados de aplicação de UAV chegam a 50 Mbps | 3GPP, via arXiv 1903.05289 |

### Padrão do testbed, **não vem da literatura**

Todos são opção de linha de comando.

| Onde | Valor | Por quê |
|---|---|---|
| C1 tamanho × taxa | 128 B a 64 Hz (≈65,5 kbps) | par dentro da faixa 3GPP de 60–100 kbps |
| C2 recurso CoAP | `/telemetry` | nome arbitrário |
| C2 espera por resposta | 30 s | cobre as retransmissões do CoAP confirmável (RFC 7252 §4.8) |
| C2 semente do jitter | 1, fixa | execuções reproduzíveis; `-S` muda |
| C3 bitrate | 4 Mbps | dentro de "alguns Mbps" para FHD |
| C3 payload RTP | 1200 B | cabe na MTU de 1500 com IP+UDP+RTP e a tag do AES-GCM |
| C3 perfil SRTP | `SRTP_AEAD_AES_256_GCM` | alinha com o `TLS_AES_256_GCM_SHA384` dos demais canais |
| C3 fim de fluxo | 5 s sem pacote | SRTP não tem `close_notify` |
| C4 tamanho do arquivo | 32 MiB | a literatura dá a taxa, não o tamanho |
| C4 bloco | 16 KiB | casa com o registro máximo do TLS |
| Duração × repetições | 60 s × 10 | amostra suficiente para p99 em C1 e C3 |
| netem `handover` | correlação de 25% | aproxima perda em rajada; o netem sem correlação distribuiria a perda uniformemente |
| PSK | 32 bytes | casa com a força do AES-256-GCM usado nos quatro canais |

## Número de sequência do C2 entre execuções

O canal C2 grava `keys/c2.seq` com o Sender Sequence Number do OSCORE e retoma
dele na execução seguinte (RFC 8613, Apêndice B.1.1).

Sem isso, cada execução do cliente recomeçaria a numeração do zero e o servidor
— que guarda a última sequência vista daquele Sender ID — recusaria tudo com
`4.01`. Não é rigor excessivo do protocolo: reusar número de sequência com o
mesmo par de chaves repete o nonce do AEAD, o que quebra a cifra.

Consequências práticas:

- **Não apague `keys/c2.seq` sem trocar a chave do canal.** Apagar só o arquivo
  faz o cliente recomeçar do zero contra um servidor que ainda lembra das
  sequências antigas, e o canal volta a ser recusado.
- Ao gerar chaves novas, apague o arquivo junto — ele perde o sentido.
- Se o C2 falhar com `4.01 — contexto OSCORE recusou a sequência`, é isto.

## Perfil fixo, sem fallback (R2)

Se o perfil configurado falhar, o canal falha em voz alta e **não** degrada
para outro. Em concreto:

- o perfil é constante de compilação, não parâmetro;
- `wolfSSL_CTX_require_psk()` impede o handshake de recair em autenticação por
  certificado, e desliga o downgrade de versão;
- `wolfSSL_CTX_only_dhe_psk()` exige `psk_dhe_ke`, preservando o sigilo futuro;
- o servidor recusa identidade PSK que não seja a do seu canal, de modo que um
  cliente apontado para a porta errada é barrado antes da comparação de chave;
- no C3, sem a extensão `use_srtp` negociada a exportação de chave falha e o
  canal para — não há caminho que envie mídia em claro;
- no C2, sem contexto OSCORE o canal não sobe, e resposta que não seja de
  classe 2.xx é contada como perda, com o motivo no log.

## Fora de escopo nesta fase

- AKMA, derivação de K_AF, AAnF, qualquer coisa de 5G
- seleção adaptativa ou dinâmica de perfil
- parsing real de MAVLink e codec de vídeo real
- hardware, Pixhawk, Raspberry Pi

## Limitações conhecidas

- **Modo B.1.2 da RFC 8613 desligado no C2.** Ver
  [`channels/c2_telemetry/c2_oscore.h`](channels/c2_telemetry/c2_oscore.h) para
  o motivo e o que se perde. Detalhado no [REPORT.md](REPORT.md).
- Os servidores atendem um peer por vez; não há demultiplexação de várias
  associações sobre o mesmo socket.
- Sem autenticação mútua por certificado nos canais — a PSK autentica os dois
  lados, mas não há identidade de UE verificável como o AKMA exigiria.
- Sem revogação.
- O C3 não usa codec real; o tráfego é sintético com o perfil da classe.

## Binários legados

Os quatro binários originais (`tls_server`, `tls_client`, `dtls_server`,
`dtls_client`, portas 4433 e 4444) continuam no repositório, **intactos**. Eles
autenticam por certificado X.509 e validam identidade de peer por
`subjectAltName`; é para eles que existe `scripts/generate_certs.sh` e o
diretório `certs/`. Ver [testes/README.md](testes/README.md) e
[docs/RUNBOOK.md](docs/RUNBOOK.md).

```bash
make legacy
./scripts/run_local_test.sh    # TLS e DTLS com certificado, em loopback
./scripts/run_oscore_test.sh   # OSCORE via ferramentas do libcoap
```

## Estrutura

```
adaptive-security-prototype/
├── channels/
│   ├── channel.mk           regras comuns aos quatro canais
│   ├── c1_control/          DTLS 1.3            UDP/5001
│   ├── c2_telemetry/        OSCORE sobre CoAP   UDP/5002
│   ├── c3_media/            DTLS + SRTP         UDP/5003
│   └── c4_bulk/             TLS 1.3             TCP/5004
├── common/
│   ├── as_profile.h         tabela dos perfis fixos
│   ├── as_keys.[ch]         carga da chave do canal
│   ├── as_psk.[ch]          PSK em TLS 1.3 e DTLS 1.3
│   ├── as_metrics.[ch]      CSV, percentis, bytes no fio, CPU, RSS
│   ├── as_pack.h            serialização em ordem de rede
│   ├── as_signal.h          parada limpa dos servidores
│   └── adaptive_security.h  helpers dos binários legados
├── scripts/
│   ├── setup_wolfssl.sh     wolfSSL com as flags exigidas, commit fixo
│   ├── setup_libcoap.sh     libcoap com OSCORE, commit fixo
│   ├── check_wolfssl.sh     valida a build antes de compilar
│   ├── generate_keys.sh     uma chave por canal
│   └── ...                  scripts dos binários legados
├── run.sh                   orquestrador
├── netem.sh                 emulação de enlace
├── REPORT.md                o que mudou, o que não foi verificado, o que escolhi
└── README.md
```
