# Relatório de prontidão dos resultados para publicação

Primeira versão: 2026-09-12. **Atualizado em 2026-09-15** com a campanha
completa `results/campaign-20260915T121452Z-ad89a84` (as três condições num
commit só, já com a correção do C2), que passa a ser a fonte autoritativa. As
campanhas antigas (`b929392`, `ccc8e7c`) ficam só como histórico.

A pergunta que este documento responde: **os dados colhidos bastam para um
artigo?** A resposta curta é: bastam para um artigo *mínimo* de avaliação
experimental — a campanha completa está feita e os três itens de baixo esforço
sobre ela (dispersão, bytes de fio do C2, disclaimer da CPU) foram resolvidos;
não bastam para um artigo *forte*, que exige as duas séries de medições da §6
(controle sem segurança e varredura de perda/atraso).

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

Campanha `campaign-20260915T121452Z-ad89a84`, 10 repetições por célula. Entre
parênteses, o IQR entre repetições (dispersão), quando relevante.

| Cond. | Canal | Estab. ms | Estab. B | Msgs | Sobr. B/msg | p50 ms | p95 ms | p99 ms | Entrega | CPU/msg µs (♦) | CPU/verif µs | Mbps | RSS kB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clean | C1 DTLS 1.3 | 8,8 (3,1) | 4588 | 38400 | 22,0 | 0,78 | 0,93 | 1,31 | 100 % | 163 | 65 | — | 4340 |
| clean | C2 OSCORE | 1,1 | 57/46 (‡) | 300 | 18 (‡) | 1,12 | 1,42 | 1,57 | 100 % | 576 | — | — | 4264 |
| clean | C3 DTLS-SRTP | 6,9 | 4611 | 250000 | 28,0 | (†)0,00 | 0,00 | 0,00 | 100 % | 144 | 21 | — | 7062 |
| clean | C4 TLS 1.3 | 3,3 | 2941 | 20490 | 22,0 | 578,8 (‡‡) | — | — | 100 % | 173 | 276 | **460,6** | 4200 |
| 3gpp-c2 | C1 DTLS 1.3 | 308,6 | 4588 | 38400 | 22,0 | 101,2 | 101,3 | 101,5 | 99,9 % | 31 | 60 | — | 4340 |
| 3gpp-c2 | C2 OSCORE | 101,6 | 57/46 (‡) | 300 | 18 (‡) | 101,6 | 101,8 | 102,0 | 100 % | 378 | — | — | 4246 |
| 3gpp-c2 | C3 DTLS-SRTP | 308,7 | 4611 | 250000 | 28,0 | (†)0,00 | 0,00 | 0,00 | 99,9 % | 44 | 21 | — | 7072 |
| 3gpp-c2 | C4 TLS 1.3 | 203,7 | 2941 | 20490 | 22,0 | 2148,0 (‡‡) | — | — | 100 % | 142 | 238 | **109,5** | 4262 |
| handover | C1 DTLS 1.3 | 3754 (1563) | 7474 | 38400 | 22,0 | 401,2 | 428,9 | 436,2 | **89,7 %** | 33 | 63 | — | 4304 |
| handover | C2 OSCORE | 413,9 | 57/46 (‡) | 251 | 18 (‡) | 405,0 | 3232 | 6827 | **100 %** | 390 | — | — | 4240 |
| handover | C3 DTLS-SRTP | 4210 (1833) | 6652 | 250000 | 28,0 | (†)62,7 | 80,7 | 83,1 | **89,8 %** | 41 | 19 | — | 7076 |
| handover | C4 TLS 1.3 | 823 | 2941 | 20490 | 22,0 | 2123008 (‡‡) | — | — | **100 %** | 140 | 438 | **0,13** | 4214 |

(♦) **CPU/msg tem ressalva** — ver §3.3: mede o caminho de envio numa VM, não
o custo isolado da cifra; a diferença entre condições é ambiental. Fica no
paper com disclaimer, não como custo do protocolo. IQR de cada célula sai no
`resumo.csv` (colunas `*_iqr`); no console, no bloco "Dispersão".
(†) C3 é unidirecional sem relógio comum: valem as DIFERENÇAS p95−p50 e
p99−p50; em clean/3gpp o atraso ficou sub-ms (0,00), sob handover o spread
p99−p50 ≈ 20 ms é o número útil.
(‡) C2 pelo pcap (`pcap_bytes.py --split-port 5002`): requisição 57 B,
resposta 46 B (IP+). A resposta carrega os 2 B de leitura em 18 B de
CoAP+OSCORE — **abaixo** dos 22 B por registro do DTLS/TLS. Constante nas três
condições.
(‡‡) C4: tempo da transferência COMPLETA de 32 MiB, uma amostra por repetição
(não é latência por mensagem). Vazão mediana por repetição na coluna Mbps.

### 2.4 O que os números já contam

Os dados apoiam cinco observações que seriam parágrafos do artigo:

1. **O estabelecimento escala em RTTs, como a especificação prevê.** A 100 ms
   de RTT: OSCORE 1 RTT (102 ms, o desafio Echo), TLS 1.3 sobre TCP 2 RTT
   (204 ms: SYN + handshake de 1-RTT), DTLS 1.3 com cookie 3 RTT (309 ms).
   Sob handover, o DTLS 1.3 sobe para 3,8 s (IQR 1,6 s — cauda das
   retransmissões de handshake), e os bytes de estabelecimento vão de 4588
   para 7474 B (C1) e de 4611 para 6652 B (C3): são as retransmissões.
2. **A sobrecarga por mensagem é constante e independe do enlace**: 22 B
   (DTLS/TLS: cabeçalho de registro + tipo interno + tag de 16 B) e 28 B
   (RTP 12 B + tag 16 B), estáveis nas três condições. O C2/OSCORE, pelo pcap,
   entrega 2 B de leitura em 18 B de CoAP+OSCORE na resposta — **abaixo** dos
   22 B do DTLS por registro (mas com o custo de uma requisição de 57 B, que
   os canais unidirecionais não têm).
3. **A entrega separa os perfis sob perda** — o resultado mais forte. Com 20 %
   de perda em rajada: DTLS (C1) 89,7 % e SRTP (C3) 89,8 % (não retransmitem
   dados de aplicação — perda é perda); CoAP/OSCORE (C2) **100 %** pelas
   retransmissões confirmáveis; TCP (C4) **100 %**. A correção do C2 se
   confirma na prática: 100 % em todas as 10 reps do handover (o 56 % anterior
   era artefato). O trade-off do C2 aparece na cauda: p50 405 ms mas p99
   6,8 s, pelo backoff do CoAP.
4. **A vazão do C4 despenca com a condição**: 460,6 → 109,5 → 0,13 Mbps
   (clean → 3gpp-c2 → handover). TCP sobre 20 % de perda em rajada colapsa —
   35 min para 32 MiB por repetição.
5. **Memória residente é indiferente ao perfil** na escala de um processo
   Linux (4,2–7,1 MB, dominada por libc e wolfSSL). Isso é resultado, não
   lacuna, mas não se compara com a literatura de microcontroladores (§5).

---

## 3. Defeitos — todos resolvidos ou contornados

Em ordem de gravidade. Todos foram tratados; esta seção fica como registro do
que era e de como foi resolvido.

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

### 3.3 CPU por mensagem incoerente entre condições — mantida com disclaimer

A CPU/msg difere de forma **sistemática** entre condições, não aleatória:
clean ~163 µs, 3gpp-c2 ~31 µs, handover ~33 µs para o mesmo `wolfSSL_write` de
128 B do C1 (medições apertadas dentro de cada condição: IQR de 0,4–15 µs).
Cifrar 128 B com AES-GCM custa décimos de microssegundo; o que a janela mede é
o caminho de envio inteiro numa VM (chamada de sistema + contabilidade do
escalonador), e o fator ambiental que separa clean das condições com netem não
é o custo do protocolo.

**Decisão (2026-09-15): a coluna fica no resumo, com disclaimer.** O
`summarize.py` imprime uma nota fixa e o relatório/paper carregam o mesmo aviso:
os valores servem de ordem de grandeza do custo por mensagem no caminho
completo, **não** para comparar protocolos entre condições. As três opções mais
rigorosas (afinidade com `taskset`; ciclos via `perf stat`, como Gallenmüller
et al.; ou isolar a cifra com o `benchmark` do wolfSSL) ficam para uma medição
dedicada de CPU, fora do caminho crítico da campanha.

### 3.4 Itens menores — resolvidos ou registrados

- **Dispersão** (IQR): resolvido (55a3716). O `summarize.py` reporta o IQR
  entre repetições para estabelecimento, p50, CPU/msg e vazão, no console e em
  colunas `*_iqr` do CSV.
- **Bytes de fio do C2**: resolvido (63210d7). `pcap_bytes.py --split-port`
  separa requisição (57 B) de resposta (46 B); a resposta leva 2 B em 18 B de
  CoAP+OSCORE.
- **Vazão do C4 inflada 10×**: era bug do `summarize.py` (numerador somava as
  10 reps, denominador era a mediana de uma). Resolvido (422df0f); os valores
  corretos são 460,6 / 109,5 / 0,13 Mbps.
- **Relógios não sincronizados** limitam o C3 a jitter. Aceito como limitação
  declarada; sob handover o spread p99−p50 ≈ 20 ms é o número útil. Se quiser
  o absoluto, `chrony` entre as VMs antes da campanha (custo: minutos).
- **Tamanho da amostra de RTT do C2**: ~25–30 leituras por repetição (a
  cadência de 2 s em 60 s), 250–300 no total da condição — p99 por repetição é
  grosseiro. Reportar p50/p95 para o C2, ou alongar a duração só desse canal.
- **C4 sob perda alta é lento**: 32 MiB levou ~35 min/rep no handover. Não é
  defeito, é o resultado do TCP sob 20 % de perda — mas inviabiliza varreduras
  (§6, P1). Decisão pendente: reduzir o `--bulk-size` nas condições
  degradadas e declarar no texto.

---

## 4. O que a campanha entregou (feito)

`campaign-20260915T121452Z-ad89a84`, rodada em 2026-09-15 com
`./campaign.sh --client-ssh lab-client --host 192.168.218.130 --capture`, num
commit só (`ad89a84`) já com a correção do C2:

- as três condições completas e comparáveis, **3 × 4 × 10 = 120 execuções**;
- pcap dos quatro canais nas três condições (sobrecarga real no fio, incluindo
  o C2, extraída com `pcap_bytes.py --split-port`);
- 7 métricas por célula, com dispersão (IQR) entre repetições.

O handover levou ~6 h por causa do C4 (35 min/rep a 0,13 Mbps). **É o conjunto
mínimo publicável** para um artigo de avaliação experimental em workshop ou
conferência de comunicações, com o argumento centrado nas cinco observações da
§2.4. Os itens de tratamento (§3) estão todos fechados.

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

**P0 — necessário para o artigo mínimo: TUDO FEITO**

| # | Teste | Estado |
|---|---|---|
| P0.1 | Corrigir o C2 e rodar as três condições num commit só | **feito** (9703c07; campanha ad89a84) |
| P0.2 | Dispersão (IQR) no `summarize.py` | **feito** (55a3716) |
| P0.3 | Bytes de fio do C2 pelo pcap | **feito** (63210d7: `--split-port`) |
| P0.4 | Tratar a CPU (§3.3) | **feito**: mantida com disclaimer (55a3716) |

Com o P0 fechado, **o material do artigo mínimo está pronto para escrever**. O
que segue (P1/P2) é o que eleva de "mínimo" a "forte".

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

1. ~~Corrigir o C2 antes da campanha~~ — feito (9703c07) e validado.
2. ~~Tratamento da CPU~~ — decidido: fica no resumo com disclaimer (§3.3). Uma
   medição de CPU dedicada (perf/taskset) só se for virar contribuição própria.
3. Sincronizar os relógios das VMs para o C3, ou manter só jitter? (Recomendo
   manter jitter e declarar a limitação — o spread sob handover já é útil.)
4. **C4/bulk sob perda alta**: 32 MiB deu ~35 min/rep no handover. Para as
   varreduras (P1) isso é proibitivo. Reduzir o `--bulk-size` (p.ex. 4–8 MiB)
   nas condições degradadas e declarar no texto, ou manter 32 MiB e aceitar as
   horas? Precisa da sua decisão antes de qualquer varredura.
5. P1.3 (controle sem segurança) entra? É a adição de maior retorno por
   esforço para separar "custo do perfil" de "custo de rodar o tráfego".
6. P2.1 (alternativas por classe) é para este artigo ou para o seguinte?
7. Alvo de publicação: workshop/conferência (mínimo, já pronto) ou periódico
   (exige P1 + P2)? Isso define quanto da §6 é obrigatório.
