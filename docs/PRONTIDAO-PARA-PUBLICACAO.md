# Relatório de prontidão dos resultados para publicação

Data deste levantamento: 2026-09-12. Dados de `results/campaign-20260909T211230Z-b929392`
(linha de base) e `results/campaign-20260910T212140Z-ccc8e7c` (3gpp-c2 e handover).

A pergunta que este documento responde: **os dados colhidos até aqui, mais a
campanha que vai rodar em seguida, bastam para um artigo?** A resposta curta
é: bastam para um artigo *mínimo* de avaliação experimental, desde que três
defeitos sejam sanados antes da campanha; não bastam para um artigo *forte*,
que exige mais duas séries de medições descritas na §6.

---

## 1. O que o artigo afirma

O testbed sustenta uma afirmação empírica, e só uma:

> Atribuir a cada classe de aplicação do UAV um perfil de segurança fixo —
> DTLS 1.3 ao controle, OSCORE à telemetria, DTLS-SRTP à mídia, TLS 1.3 ao
> volume — tem custo mensurável de estabelecimento, sobrecarga por mensagem,
> atraso, entrega, CPU e memória, e esse custo se comporta de modo distinto sob
> enlace limpo, sob o enlace de C2 do 3GPP e sob handover.

O que o artigo **não** afirma (e o texto precisa dizer isso): que a atribuição
é ótima, ou que seleção dinâmica seria pior. Isso está fora de escopo por
decisão de projeto (README, "Fora de escopo").

---

## 2. O que existe hoje

### 2.1 Condições e cobertura

| Condição | netem (nas duas VMs) | Reps | Commit | Estado |
|---|---|---|---|---|
| clean | nenhum | 10 × 4 canais | b929392 | completo |
| 3gpp-c2 | delay 50 ms, loss 0,1 % | 10 × 4 canais | ccc8e7c | completo |
| handover | delay 200 ms ± 20 ms, loss 20 % corr. 25 % | **4** × 4 canais | ccc8e7c | **incompleto** (ssh caiu) |

Entre b929392 e ccc8e7c só o `campaign.sh` mudou (`git diff --stat`), então
as condições são comparáveis. Ainda assim, a próxima campanha deve colher as
três condições num commit só, para que o manifesto de todas aponte a mesma
versão.

### 2.2 Métricas que o testbed já produz

Por (condição, canal), a partir de `resumo.csv` e dos CSVs brutos por
repetição:

| Métrica | Como sai | Canais |
|---|---|---|
| tempo de estabelecimento (parede e CPU) | `handshake` no CSV; mediana das reps no resumo | C1, C3, C4 (handshake); C2 (primeira transação com Echo) |
| bytes do estabelecimento | contadores de fio | C1, C3, C4; C2 só por pcap |
| sobrecarga por mensagem (B) | `(wire_tx − hs_wire_tx − app_bytes) / msgs` | C1, C3, C4; C2 só por pcap |
| atraso p50/p95/p99 | RTT no cliente (C1, C2), unidirecional no receptor (C3), tempo total (C4) | todos, com as ressalvas do README |
| razão de entrega | `msgs_recv / msgs_sent` | todos |
| CPU por mensagem enviada / verificada | `CLOCK_PROCESS_CPUTIME_ID` em volta do `write`/`read` | todos (C2 com janela mais larga) |
| retransmissões nativas | eventos `retx` | só C2 (CoAP CON) |
| memória residente de pico | `VmHWM` | todos |

### 2.3 Os números (mediana das repetições; tempos em ms, bytes em B)

| Cond. | Canal | Reps | Estab. | Estab. B | Msgs | Sobrec. B/msg | p50 | p95 | p99 | Entrega | CPU/msg µs | CPU/verif µs | RSS kB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clean | C1 DTLS 1.3 | 10 | 14,1 | 4588 | 38400 | 22,0 | 1,32 | 1,56 | 2,25 | 100 % | 308 | 105 | 4264 |
| clean | C2 OSCORE | 10 | 2,0 | pcap | 300 | pcap: 23,4 | 1,89 | 2,70 | 2,89 | 100 % | 1029 | — | 4200 |
| clean | C3 DTLS-SRTP | 10 | 11,9 | 4611 | 250000 | 28,0 | (†) | +0,24 | +0,35 | 100 % | 294 | 30 | 6980 |
| clean | C4 TLS 1.3 | 10 | 6,5 | 2941 | 20490 | 22,0 | 2335 (‡) | — | — | 100 % | 554 | 1035 | 4194 |
| 3gpp-c2 | C1 DTLS 1.3 | 10 | 313,1 | 4588 | 38400 | 22,0 | 101,6 | 101,9 | 102,2 | 99,4 % | 54 | 94 | 4276 |
| 3gpp-c2 | C2 OSCORE | 10 | 102,4 | pcap | 300 | pcap: 23,5 | 102,2 | 103,2 | 103,5 | 100 % | 692 | — | 4186 |
| 3gpp-c2 | C3 DTLS-SRTP | 10 | 313,5 | 4611 | 250000 | 28,0 | (†) | +0,38 | +0,82 | 97,6 % | 84 | 30 | 7028 |
| 3gpp-c2 | C4 TLS 1.3 | 10 | 205,6 | 2941 | 20490 | 22,0 | 2219 (‡) | — | — | 100 % | 953 | 520 | 4170 |
| handover | C1 DTLS 1.3 | 4 | 3460,6 | 6835 | 11520 | 22,1 | 401,7 | 429,5 | 437,1 | 89,6 % | 65 | 112 | 4240 |
| handover | C2 OSCORE | 4 | 391,1 | pcap | 107 | pcap: 23,7 | 405,7 | 3131,8 | 5852,1 | 56,1 % (§) | 537 | — | 4240 |
| handover | C3 DTLS-SRTP | 4 | 3528,5 | 7576 | 75000 | 28,0 | (†) | +18,0 | +20,3 | 89,6 % | 61 | 26 | 7126 |
| handover | C4 TLS 1.3 | 4 | 1304,0 | 2941 | 4288 | 22,1 | 1046491 (‡) | — | — | 99,6 % | 280 | 810 | 4286 |

(†) C3 é unidirecional sem relógio comum: só as diferenças p95−p50 e p99−p50
valem, e são as que estão na tabela.
(‡) C4: tempo da transferência completa de 32 MiB, uma amostra por repetição
(1145, 1062 e 0,34 Mbps, respectivamente).
(§) Artefato de implementação, não do OSCORE — corrigido em 9703c07 (§3.2);
a próxima campanha traz o número real.
Sobrecarga do C2 por pcap: 51,4 B médios por pacote IP+UDP+CoAP+OSCORE para
2 B de leitura, menos 28 B de IP+UDP = 23,4 B de CoAP+OSCORE. Comparável aos
22 B de DTLS/TLS por registro **só depois** de descontar o cabeçalho CoAP, que
existiria mesmo sem segurança (a decomposição exata está por fazer, §6, P0).

### 2.4 O que os números já contam

Mesmo incompletos, os dados apoiam quatro observações que seriam parágrafos do
artigo:

1. **O estabelecimento escala em RTTs, como a especificação prevê.** A 100 ms
   de RTT: OSCORE 1 RTT (102 ms, o desafio Echo), TLS 1.3 sobre TCP 2 RTT
   (206 ms: SYN + handshake de 1-RTT), DTLS 1.3 com cookie 3 RTT (313 ms).
   Sob handover, o DTLS 1.3 sobe para 3,5 s, e os bytes de estabelecimento vão
   de 4588 para 6835–7576 B: são as retransmissões de handshake.
2. **A sobrecarga por mensagem é constante e independe do enlace**: 22 B
   (DTLS/TLS: cabeçalho de registro + tipo interno + tag de 16 B) e 28 B
   (RTP 12 B + tag 16 B), estáveis nas três condições.
3. **A entrega separa os perfis sob perda.** Com 20 % de perda em rajada, DTLS
   e SRTP entregam ~90 % (não retransmitem: perda é perda), TCP entrega 99,6 %
   ao custo de 1046 s para 32 MiB, e o C2 deve entregar >99 % pelas
   retransmissões do CoAP (o número real virá da próxima campanha, já com a
   correção da §3.2; a entrega de 56 % da campanha anterior era artefato).
4. **Memória residente é indiferente ao perfil** na escala de um processo
   Linux (4,2–7,1 MB, dominada por libc e wolfSSL). Isso é resultado, não
   lacuna, mas não se compara com a literatura de microcontroladores (§5).

---

## 3. Defeitos a sanar ANTES da próxima campanha

Em ordem de gravidade. Os dois primeiros invalidam parte dos dados; o terceiro
compromete uma coluna inteira.

### 3.1 `handover` incompleto — resolvido no `campaign.sh`

A sessão SSH que dirigia o cliente atravessava o enlace com 20 % de perda e
caiu na terceira repetição. Corrigido em 4d36592: o cliente roda em sessão
própria na VM cliente e o orquestrador só sonda. Basta rodar de novo.

### 3.2 C2 não se recuperava depois de um NACK — resolvido (9703c07)

Sintoma original: em `handover/rep03`, a leitura 10 recebeu NACK aos 28,0 s
(retransmissões do CoAP esgotadas) e, a partir daí, **todas** as leituras
seguintes falhavam em ~10 µs com `coap_send_recv=-2` ("Failed to transmit
PDU"), até o fim da repetição; em `rep04` nenhuma das 30 chegou. A sessão do
libcoap 4.3.5 ficava inutilizável e esta versão não a reconecta sozinha. A
entrega de 56,1 % e as 12 retransmissões do C2 sob handover eram artefato
disso, não custo do OSCORE.

Correção em `channels/c2_telemetry/c2_client.c`: ao ver `-2` (falha de
transmissão) ou `-4` (erro de I/O), o cliente recria contexto + sessão OSCORE
(`c2_open_session()`) e segue. A leitura que falhou continua contando como
perda; as seguintes voltam a valer. O número de sequência do OSCORE é
persistido em arquivo a cada envio (`ssn_freq=1`), então o contexto
reconstruído retoma de onde parou, sem reusar nonce e sem ser recusado pelo
servidor pela janela anti-replay. O resumo do cliente passa a informar quantas
sessões foram recriadas.

Validado em loopback derrubando e reerguendo o servidor por ~42 s: 100
leituras, 42 perdas e 21 recriações **na janela de queda**; assim que o
servidor voltou, a entrega retornou a 100 % até o fim (58 respostas no total).
Com o código antigo, ficariam ~5 respostas. A perda do C2 agora reflete só a
indisponibilidade real do enlace.

### 3.3 CPU por mensagem inconsistente entre condições

O mesmo `wolfSSL_write` de 128 B custa 308 µs em `clean` e 54 µs em
`3gpp-c2`; o C4 vai de 554 para 953 e 280 µs; o C2 de 1029 para 692 e 537 µs.
Cifrar 128 B com AES-GCM custa décimos de microssegundo: o que a janela mede é
sobretudo a chamada de sistema e a contabilidade de CPU numa VM, com ruído de
escalonamento que varia com o que mais estava rodando. Nesse estado a coluna
não sustenta afirmação nenhuma.

Opções, da mais barata à mais rigorosa:
- reportar mediana e IQR por repetição (os CSVs já têm o dado por mensagem) e
  fixar a afinidade de CPU do processo (`taskset`), com a VM ociosa;
- medir ciclos com `perf stat -e cycles` por processo, como Gallenmüller et
  al. (§5), em vez de tempo de CPU;
- separar o custo criptográfico com o `benchmark` do wolfSSL (mesma cifra,
  mesma máquina) e apresentar as duas coisas: custo do algoritmo e custo do
  caminho completo.

### 3.4 Menores, mas que um revisor apontaria

- **Dispersão não é reportada.** O `resumo.csv` traz a mediana das
  repetições; falta IQR ou intervalo de confiança. Os dados por repetição
  existem (p.ex. estabelecimento C1 em clean: 12,9–16,0 ms; em 3gpp-c2:
  312,3–362,4 ms). É só somar ao `summarize.py`.
- **Relógios não sincronizados** limitam o C3 a jitter. Ou se aceita como
  limitação declarada, ou se sincroniza as VMs com `chrony` apontando uma
  para a outra antes da campanha (custo: minutos).
- **Bytes de estabelecimento do C2** só saem do pcap e ainda não foram
  extraídos (a primeira transação, com o Echo, tem tamanho diferente das
  seguintes).
- **Tamanho da amostra de RTT do C2**: 300 leituras em 10 repetições — p99
  de 300 amostras é a 3.ª pior. Ou se alonga a duração para o C2, ou se
  reporta só p50/p95 para ele.

---

## 4. O que a próxima campanha entrega

Rodando `./campaign.sh --client-ssh lab-client --host 192.168.218.130 --capture`
(as três condições, 60 s × 10) num commit só, com a correção da §3.2:

- as três condições completas e comparáveis;
- pcap dos quatro canais nas três condições (sobrecarga real no fio, incluindo
  o C2 e o estabelecimento);
- tempo estimado: ~80 min para clean e 3gpp-c2, mais **~3,5 h para handover**
  — o C4 leva ~17 min por repetição a 0,34 Mbps. Deixe rodando; o script não
  precisa mais de operador.

Com isso, e com as correções da §3, o conjunto é: 3 condições × 4 canais ×
10 repetições, 7 métricas. **É o mínimo publicável** para um artigo de
avaliação experimental em workshop ou conferência de comunicações, com o
argumento centrado nas quatro observações da §2.4.

---

## 5. O que os trabalhos semelhantes mediram

Levantamento de 2026-09-12. A coluna "o que replicar" diz o que cada um
faz que o testbed ainda não faz.

| Trabalho | Plataforma | Protocolos | Métricas | Condições de rede | Estatística | O que replicar |
|---|---|---|---|---|---|---|
| Restuccia, Tschofenig, Baccelli, *Low-Power IoT Communication Security: On the Performance of DTLS and TLS 1.3*, PEMWN 2020 ([arXiv:2011.12035](https://arxiv.org/abs/2011.12035)) | nRF52840, STM32F407, RIOT; wolfSSL 4.4.0, Mbed TLS 2.16 | (D)TLS 1.2 vs 1.3; PSK vs ECDHE-ECDSA; AES-128-CCM vs AES-256-GCM | bytes no ar, Flash, RAM, heap, **energia do handshake** | nenhuma (enlace local) | médias | comparar **modos de autenticação** (PSK vs ECDHE) e **cifras**; energia fica fora (VM) |
| Gunnarsson et al., *Evaluating the performance of the OSCORE security protocol in constrained IoT environments*, Internet of Things 13, 2021 ([DOI](https://www.sciencedirect.com/science/article/pii/S2542660520301645)) | Contiki-NG, dispositivos reais | OSCORE vs TinyDTLS (DTLS 1.2) | **sobrecarga por mensagem, memória, RTT, energia** | enlace local | — | **OSCORE lado a lado com DTLS na mesma classe** — é a comparação que falta ao C2 |
| Gündoğan et al., *IoT Content Object Security with OSCORE and NDN*, IFIP Networking 2020 ([arXiv:2001.08023](https://arxiv.org/abs/2001.08023)) | FIT IoT-Lab, RIOT, 802.15.4 | CoAP/DTLS, OSCORE, NDN | sobrecarga (B), **tempo de conclusão (CDF)**, tempo de criação do pedido (µs), retransmissões | rádio real, 1 e multi-hop, 10 min por experimento, 1000 pedidos a 2 ± 0,5 s | distribuições / CDF | **CDF** em vez de três percentis; é a fonte do gerador do C2 |
| Vučinić et al., *DTLS Performance in Duty-Cycled Networks*, PIMRC 2015 ([arXiv:1507.05810](https://arxiv.org/abs/1507.05810)) | Contiki, tinyDTLS, MSP430 (emulado + real) | DTLS 1.2 PSK AES-CCM-8 | **duração do handshake** e energia | **varredura de PDR** (100 → 90 %), n.º de saltos, intervalo de wake-up | **IC de 95 %** | **varrer a perda**, não só três pontos; reportar IC |
| Hristozov et al., *The Cost of OSCORE and EDHOC for Constrained Devices*, CODASPY 2021 ([arXiv:2103.13832](https://arxiv.org/abs/2103.13832)) | MCU Cortex-M, TrustZone-M | OSCORE + EDHOC | Flash, RAM, **tempo de estabelecimento por modo de autenticação** | — | — | estabelecimento de chave do OSCORE (hoje o C2 usa contexto estático) |
| Gallenmüller et al., *DTLS Performance – How Expensive is Security?*, 2019 ([arXiv:1904.11423](https://arxiv.org/abs/1904.11423)) | x86, DPDK, OpenSSL | DTLS 1.2, várias cifras | **ciclos de CPU** por pacote e por conexão; vazão | gerador de carga | modelo com sMAPE | **ciclos** em vez de tempo de CPU (§3.3) |
| wolfSSL, *DTLS 1.3 Benchmarks* ([blog](https://www.wolfssl.com/dtls-1-3-benchmarks/)) | uma máquina | DTLS 1.2 vs 1.3, AES-256-GCM | tempo de handshake, vazão (15 MB), largura de banda sob perda | 100 ms de latência; perda | 10 amostras, média ± desvio | referência direta para o C1: handshake em 1 RTT, banda sob perda |
| Chaari, Chahbani, Rezgui, *MAV-DTLS toward Security Enhancement of the UAV-GCS Communication*, VTC2020-Fall ([IEEE](https://ieeexplore.ieee.org/document/9348584/)) | UAV + GCS, MAVLink | DTLS sobre MAVLink | latência, energia | — | — | o único trabalho de UAV com DTLS; conclui "efeito desprezível" sem varrer o enlace — é o espaço que o testbed ocupa |
| Dixit et al., *A Novel Cipher for Enhancing MAVLink Security*, 2025 ([arXiv:2504.20626](https://arxiv.org/abs/2504.20626)) | drone real | cifras leves sobre MAVLink | memória, CPU, bateria | — | — | avaliação em hardware de UAV; fora de escopo nesta fase |

O que se repete em quase todos e o testbed **não** tem:

1. **Um controle sem segurança** (mesmo tráfego, sem proteção). É assim que a
   literatura isola "o custo da segurança". Sem ele, o artigo mostra custos
   absolutos, mas não o quanto deles é do perfil.
2. **Varredura de um parâmetro do enlace** (perda ou atraso), com curva, e não
   três pontos discretos.
3. **Comparação dentro da mesma classe** (o perfil atribuído contra a
   alternativa óbvia). Sem isso, "perfil por classe" é uma premissa, não um
   resultado.
4. **Energia e memória de microcontrolador.** Não é possível em VM; declarar
   como limitação e trabalho futuro (hardware está fora de escopo).

---

## 6. Testes propostos, por prioridade

**P0 — necessário para o artigo mínimo** (tudo cabe numa campanha noturna)

| # | Teste | Motivo | Custo |
|---|---|---|---|
| P0.1 | ~~Corrigir §3.2~~ (feito, 9703c07) e rodar as três condições num commit só | dados comparáveis e completos | ~5 h de campanha |
| P0.2 | Dispersão (IQR ou IC 95 %) no `summarize.py` | todo trabalho da tabela reporta | 1 h de script |
| P0.3 | Sobrecarga e estabelecimento **pelo pcap** nos quatro canais | valida os contadores internos e cobre o C2; decompõe os 23,4 B do C2 em CoAP vs OSCORE | script sobre pcaps já colhidos |
| P0.4 | Tratar §3.3 (CPU) por uma das três opções | a coluna não sustenta afirmação hoje | 2–4 h |

**P1 — o que separa "mínimo" de "forte"**

| # | Teste | Motivo | Custo |
|---|---|---|---|
| P1.1 | Varredura de perda: {0; 0,1; 1; 5; 10; 20} % a 50 ms | curva de estabelecimento e entrega vs perda, como Vučinić; é o resultado que dá figura | 6 condições ≈ 5 h + o C4 sob perda alta (pode passar de 3 h a 20 %) |
| P1.2 | Varredura de atraso: {0; 25; 50; 100; 200} ms a 0,1 % | confirma "estabelecimento = k × RTT" com k = 1, 2, 3 por perfil | 5 condições ≈ 4 h |
| P1.3 | Controle sem segurança por classe (UDP puro, CoAP sem OSCORE, RTP sem SRTP, TCP puro) | isola o custo do perfil; padrão da literatura | 4 binários pequenos (só medição; não é fallback em runtime, R2 preservado) + 1 campanha |

**P2 — sustenta a tese "por classe", não só "por perfil"**

| # | Teste | Motivo | Custo |
|---|---|---|---|
| P2.1 | Cada classe com a alternativa óbvia: C1 com TLS 1.3/TCP, C2 com CoAP/DTLS 1.3, C3 com registro DTLS na mídia, C4 com DTLS 1.3 | mostra o que se perde ao escolher errado; é o argumento central | binários adicionais por par (classe, perfil); maior esforço |
| P2.2 | Handover com **troca de endereço** e Connection ID (RFC 9146) no C1 | o README justifica o DTLS 1.3 no C1 pelo CID, e o testbed nunca o exercita | implementar CID no C1 + cenário de renumeração |
| P2.3 | Modo B.1.2 do OSCORE (hoje desligado) | recuperação de contexto após reinício — relevante ao handover | ver REPORT.md §4 |

**P3 — fora do alcance desta fase; declarar como trabalho futuro**

energia, Flash/RAM em microcontrolador, hardware de UAV, AKMA.

---

## 7. Esqueleto do artigo que os dados sustentam

1. Introdução: classes de aplicação do UAV e requisitos do 3GPP (TS 22.125,
   via Zeng et al.); por que um perfil por classe.
2. Perfis e sua justificativa (README §"Por que cada perfil"); C3 marcado como
   proposta do autor, fora dos Ua\* do 3GPP.
3. Testbed: duas VMs, netem nas duas pontas, geradores por classe com
   parâmetros da literatura, instrumentação, reprodutibilidade (manifesto,
   commits fixados, `campaign.sh`).
4. Resultados:
   - Fig. 1 — estabelecimento (ms) por canal × condição, com IC; anotação de
     "k RTT".
   - Fig. 2 — sobrecarga por mensagem (B) por canal, contadores vs pcap.
   - Fig. 3 — CDF do atraso por canal, uma curva por condição (Gündoğan).
   - Fig. 4 — entrega (%) por canal × condição.
   - Tab. — CPU e RSS, com o método da §3.3 explicitado.
   - (com P1) Fig. 5 — estabelecimento e entrega vs perda; Fig. 6 — vs atraso.
5. Limitações: VM (sem energia), relógios, tráfego sintético, um peer por
   servidor, contexto OSCORE estático.
6. Conclusão e trabalho futuro (P2, P3, AKMA).

---

## 8. Decisões que são suas

Nada abaixo foi assumido; o documento só enumera.

1. ~~Corrigir o C2 (§3.2) antes da campanha~~ — feito (9703c07) e validado.
2. Qual tratamento para a CPU (§3.3): mediana+afinidade, `perf`, ou
   microbenchmark do wolfSSL?
3. Sincronizar os relógios das VMs para o C3, ou manter só jitter?
4. Fazer P1.1/P1.2 (varreduras) nesta rodada? Se sim, o C4 sob perda alta
   precisa de decisão: manter 32 MiB (horas) ou reduzir só para as varreduras
   (e dizer isso no texto).
5. P1.3 (controle sem segurança) entra? É a adição de maior retorno por
   esforço.
6. P2.1 (alternativas por classe) é para este artigo ou para o seguinte?
7. Alvo de publicação: workshop/conferência (mínimo + P1) ou periódico
   (P1 + P2)? Isso define quanto da §6 é obrigatório.
