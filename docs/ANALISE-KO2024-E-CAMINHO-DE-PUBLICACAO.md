# Análise de Ko et al. (2024) e caminho de publicação

Escrito em 2026-09-18. Responde a quatro perguntas, nesta ordem:

1. O que o paper de referência faz, como faz, e onde é fraco.
2. O TLS dele é o mesmo que o meu? Dá para reproduzir o experimento dele nas
   duas VMs?
3. O que falta para um artigo IEEE de 5–6 páginas com resultados preliminares,
   usando o que já está colhido.
4. O que falta para um periódico "igual ao dele", mas com os três protocolos.

Fecha com o passo a passo do ProVerif e com a lista de decisões que são suas.

Fonte de referência: Ko, Y.; Pawana, I.W.A.J.; Won, T.; Astillo, P.V.; You, I.
*Toward an Era of Secure 5G Convergence Applications: Formal Security
Verification of 3GPP AKMA with TLS 1.3 PSK Option.* **Appl. Sci. 2024, 14(23),
11152** (MDPI, acesso aberto CC BY, 28 páginas). DOI 10.3390/app142311152.
Modelos ProVerif em https://github.com/yonghoko/ProVerif (pasta com o título do
artigo; três arquivos: `psk-only.pv`, `psk-dh-ecdhe.pv`, `0rtt.pv`).

---

## 1. Análise do paper

### 1.1 O que ele afirma

Três contribuições declaradas (§1):

1. Descrever como os três modos PSK do TLS 1.3 (PSK-only, PSK-(EC)DHE, 0-RTT)
   se encaixam como protocolo Ua\* do AKMA, com K_AF como PSK externa.
2. Verificar formalmente cada modo em ProVerif, com sessões ilimitadas.
3. Comparar os modos experimentalmente (latência, CPU, memória, perda).

### 1.2 Estrutura, seção a seção

| § | Conteúdo | Observação |
|---|---|---|
| 1 | Introdução, 5G cApps, contribuições | genérica |
| 2 | "Materials and Methods": **declara que o texto foi rascunhado com ChatGPT** | é só isso; não é metodologia |
| 3 | AKMA: arquitetura (Fig. 2), hierarquia de chaves (Fig. 3), fase inicial e fase de autenticação (Figs. 4–5), TLS como Ua\*, notação (Tab. 1), os três modos PSK | paráfrase do TS 33.535 |
| 4 | Integração: Fig. 6 (MSC com os cinco passos + key schedule) e descrição passo a passo | é o coração da parte "protocolo" |
| 5 | ProVerif: Algoritmos 1–6 (declarações, proc_UE, proc_AF, proc_AAnF, main sem FS, main com FS), Tab. 2 (requisito ↔ query), resultados (Tab. 3), Apêndice A (Figs. A1–A5) | modelos públicos |
| 6 | Experimento: OpenSSL 3.2.1, **uma VM** VirtualBox (Ubuntu 20.04, i5-13600K × 5 núcleos, 8 GB), cenário CCTV, Figs. 8–11 | ver fraquezas |
| 7 | Discussão: trade-offs por cenário IoT; Tab. 5 (contagem de operações AKMA × EAP) | contagem manual |
| 8 | Conclusão e trabalho futuro (AVISPA, Scyther, BAN/SVO; 0-RTT melhorado) | |

### 1.3 O protocolo (Fig. 6)

Cinco mensagens: (1) Application Session Establishment Request = ClientHello
com `pre_shared_key` = A-KID e binder; (2) `Naanf_AKMA_ApplicationKey_Get_Request`
(AF → AAnF, canal privado); (3) resposta com K_AF, SUPI, validade; (4)
Application Session Establishment Response = ServerHello + {EncryptedExtensions}
+ {Finished} + [App Data2]; (5) Application Session Communication = {Finished}
do cliente + [App Data3]. K_AF = KDF(K_AKMA, 0x82 ‖ AF_ID ‖ L(AF_ID)), como no
TS 33.535 Anexo A.4. Os três modos diferem só no ClientHello: `psk_ke` (1a);
`psk_dhe_ke` + `key_share` (1b); idem + `early_data` + [App Data1] cifrado com
`tk_cets` (1c).

### 1.4 Modelo ProVerif (o que está de fato nos `.pv`)

Estrutura dos três arquivos (~220 linhas cada):

- **Primitivas**: `senc/sdec`, DH com `exp(exp(g,x),y) = exp(exp(g,y),x)`,
  `hmac`, `hash`, conversores de tipo, `aead_enc/aead_dec`, `kdf`.
- **Key schedule** como `letfun`: `hkdf_extract`, `hkdf_expand_label`,
  `derive_secret`, geração de ClientHello/ServerHello e `key_gen_psk_dhe_ke`
  (HS, tk_chs, tk_shs, fk_c, fk_s, MS, tk_capp, tk_sapp).
- **Eventos**: `S_STEP1_C_to_S` / `E_STEP1_C_to_S` (ClientHello),
  `S_STEP2_S_to_C` / `E_STEP2_S_to_C` (Finished do servidor),
  `S_STEP3_C_to_S` / `E_STEP3_C_to_S` (Finished do cliente).
- **Queries**: Q1–Q3 `inj-event(E) ==> inj-event(S)`; Q4–Q6
  `attacker(app_dataN)`.
- **Processos**: `proc_UE`, `proc_AF` (consulta `aanf_key_db` via canal privado
  `sp`), `proc_AAnF`. Main: `!proc_UE | !proc_AF | !proc_AAnF`, e a variante
  com `phase 1; out(c, (k_akma, af_id))` para sigilo futuro.

Simplificações a registrar (importam para o que você vai escrever):

- O binder é calculado sobre `length(client_hello)`, não sobre o transcript
  parcial como no RFC 8446 §4.2.11.2.
- O AD do AEAD de dados de aplicação é `(crand, srand, dhe)`, não o cabeçalho
  do registro TLS.
- Não há EncryptedExtensions, HRR, `psk_key_exchange_modes` como negociação,
  nem múltiplas identidades. O canal AF↔AAnF é privado por hipótese; NEF não
  aparece.
- Não citam os modelos de referência do TLS 1.3 (Bhargavan et al., ProVerif
  e CryptoVerif, IEEE S&P 2017; Cremers et al., Tamarin, CCS 2017). Num
  periódico mais exigente isso seria cobrado — e é onde você pode se
  posicionar melhor do que eles.

### 1.5 Resultados de segurança (Tab. 3)

| Requisito | PSK-only | PSK-(EC)DHE | 0-RTT |
|---|---|---|---|
| Confidencialidade, integridade, autenticação mútua, troca de chave | ○ | ○ | ○ |
| Sigilo futuro perfeito | × | ○ | △ (early data exposto) |
| Defesa contra replay | × (sinalização) | × (sinalização) | × (sinalização + dados) |

**Leitura crítica do "replay de sinalização".** Q1 dá `false` nos três modos
porque o atacante reenvia o ClientHello e o `!proc_AF` replicado o aceita de
novo — dois eventos `E_STEP1` para um `S_STEP1`. Isso não quebra nenhuma
garantia do TLS 1.3 (Q2 e Q3 dão `true`: as chaves da sessão só fecham com
quem tem K_AF). É custo de processamento no servidor (DoS), e é exatamente o
que o cookie/HRR do **DTLS 1.3** foi desenhado para mitigar. Num modelo bem
feito, a distinção "frescor do ClientHello" (não é objetivo do TLS) versus
"acordo injetivo sobre as chaves" (é) precisa ser explícita. Esse é um ponto
em que o seu trabalho pode corrigir o deles, não só estendê-los.

### 1.6 Experimento (§6) e suas fraquezas

Montagem: **cliente e servidor na mesma VM**, 10 processos cliente por shell,
servidor com pthreads (1 e 10 threads), 8 KB de early data + 8 KB depois
(0-RTT) ou 16 KB depois (outros), 120 s com 12 amostras a cada 10 s.

Figuras a reproduzir e o que cada uma mede:

| Fig. | Eixos | O que é de fato |
|---|---|---|
| 8 | latência (ms) × "número de conexões" 0–120 | tempo de conclusão em loopback; o eixo x conflita com o texto (12 amostras no tempo) |
| 9 | uso de memória (%) × tempo | **memória do sistema inteiro** (23–24,5 % de 8 GB); não é o processo |
| 10 | uso de CPU (%) × tempo | idem, CPU do sistema |
| 11 | "latência (s)" 80–200 × perda | tempo total da carga sob perda; o texto diz {0, 3, 5, 7, 10} %, a figura mostra {5; 7,5; 10; 12,5; 15} % |
| A1–A3 | — | caixas com o "Verification summary" do ProVerif |
| A4–A5 | — | grafos de ataque gerados com `proverif -graph` |

Fraquezas objetivas:

1. **Sem enlace real**: uma VM, sem descrever como a perda foi injetada.
2. **Métricas de sistema, não de processo**: % de CPU e memória globais.
3. **Sem estatística**: 12 amostras, sem repetições, IC ou IQR; as linhas
   tracejadas são médias.
4. **Inconsistências internas** (eixos e valores de perda acima).
5. **Só TLS**, embora o TS 33.535 R18 tenha três anexos normativos de Ua\*:
   **B (TLS 1.2/1.3 PSK), C (DTLS 1.3), D (OSCORE)**. É o espaço exato do
   seu trabalho.
6. Tab. 5 (operações) é contagem manual sem método declarado.
7. A recomendação final ("0-RTT tem que ser aprovado/melhorado") contradiz
   os próprios achados de segurança.

O que vale copiar da forma: a Fig. 6 (MSC + key schedule numa figura só), a
Tab. 2 (requisito ↔ query), a Tab. 3 (○/△/×), a Tab. 4 (setup) e o apêndice
com as saídas do ProVerif.

---

## 2. O TLS dele é o meu?

### 2.1 Comparação direta

| Aspecto | Ko et al. | C4 deste testbed |
|---|---|---|
| Biblioteca | OpenSSL 3.2.1 | wolfSSL 5.9.2 (commit `88c766b`) |
| Versão | TLS 1.3 | TLS 1.3 (`wolfTLSv1_3_client_method`) |
| PSK | externa = K_AF (no modelo, derivada de K_AKMA) | externa, 32 B injetada de `keys/c4.key` (K_AF simulada) |
| Identidade PSK | "3GPP-AKMA" + A-KID (TS 33.535 B.1.3.2.2) | `uav-c4-bulk` |
| Modos | `psk_ke`, `psk_dhe_ke`, 0-RTT | **só `psk_dhe_ke`**, forçado por `wolfSSL_CTX_only_dhe_psk()` (R2) |
| Fallback para certificado | não dito | desligado (`wolfSSL_CTX_require_psk()`) |
| Cifra | não dita | `TLS_AES_256_GCM_SHA384` |
| Transporte | TCP em loopback, uma VM | TCP entre duas VMs, netem nas duas pontas |
| Carga | 16 KB por conexão, 10 clientes concorrentes, 120 s | 32 MiB (8/1 MiB nas varreduras), um cliente, 10/5 repetições |
| Métricas | tempo total, CPU % e mem % do sistema | handshake (parede e CPU), bytes de handshake, sobrecarga B/msg, vazão, entrega, CPU/msg, RSS do processo, pcap |
| Perda | 5–15 % (figura) | 0–20 %, varredura de atraso, handover em rajada, baseline em claro |

**Resposta curta: o meu C4 é o modo PSK-(EC)DHE deles** — o mesmo handshake
(ClientHello com `pre_shared_key` + `key_share` + `psk_key_exchange_modes =
psk_dhe_ke`, binder, 1-RTT), o modo que eles concluem ser o mais seguro. O
que falta para reproduzir o paper inteiro são os outros dois modos.

### 2.2 O que é preciso para reproduzir o experimento dele nas duas VMs

Tudo verificado contra `~/.local/include/wolfssl/ssl.h` e `configure.ac`:

| Item | Como | Custo |
|---|---|---|
| PSK-only (`psk_ke`) | trocar `wolfSSL_CTX_only_dhe_psk()` por `wolfSSL_CTX_no_dhe_psk()` (ssl.h:1516) numa variante de compilação do C4 | horas |
| 0-RTT | recompilar o wolfSSL com `--enable-earlydata` (hoje `WOLFSSL_EARLY_DATA` **não** está definido em `options.h`); usar `wolfSSL_CTX_set_max_early_data()`, `wolfSSL_write_early_data()`, `wolfSSL_read_early_data()` (ssl.h:1539–1546); fixar o novo commit/flags em `setup_wolfssl.sh` e `check_wolfssl.sh` | 1–2 dias; confirmar que o wolfSSL aceita early data com PSK **externa** (não só com ticket) |
| N conexões concorrentes | o servidor atende um peer por vez (limitação declarada). Duas saídas: (a) N instâncias com `--port-offset` (já existe no `run.sh`); (b) thread por conexão no `c4_server` | (a) horas; (b) 1–2 dias |
| Séries temporais de CPU/mem | amostrar `/proc/<pid>/stat` e `VmRSS` a cada 10 s no `run.sh` (hoje só RSS de pico e CPU por mensagem) | horas |
| Tamanho de carga em KiB | `--bulk-size` aceita MiB; aceitar KiB | minutos |
| Perda | varredura já existe (`loss-<X>`) | — |

Manter os modos como **variantes de compilação** (três binários), não como
opção negociada — preserva o R2 (perfil fixo, sem fallback).

### 2.3 O diferencial que as duas VMs dão

1. Enlace real com netem simétrico, e RTT como variável controlada — a
   figura "estabelecimento = k × RTT" (P1.2) não existe no paper deles.
2. Métricas **por processo** e no fio (pcap), não % do sistema.
3. Baseline em claro por classe (P1.3): isola o custo do perfil.
4. Dispersão (IQR) em tudo, `reps_sem_dados` como taxa de falha de handshake.
5. Os **três** Ua\* normativos (Anexos B, C e D do TS 33.535), não um.
6. Classes de aplicação de UAV com requisitos do 3GPP, em vez de CCTV.

Cuidado ao escrever: as suas VMs (2 vCPU, 5,2 GB) são mais fracas que a
deles; declarar numa tabela como a Tab. 4 e não comparar valores absolutos.

---

## 3. Artigo IEEE de 5–6 páginas com resultados preliminares

### 3.1 O que já existe e basta

- Campanha completa `ad89a84`: 3 condições × 4 canais × 10 repetições.
- P1.1 (perda, 6 pontos), P1.2 (atraso, 5 pontos), P1.3 (baseline em claro).
- Quatro figuras em SVG (`docs/figuras/`), o esqueleto na
  `PRONTIDAO-PARA-PUBLICACAO.md` §7, e as cinco observações da §2.4.

### 3.2 O que falta (por ordem)

1. **Enquadramento AKMA.** Hoje o README diz que AKMA está fora de escopo. Para
   estar "no caminho" de Ko et al., o artigo precisa dizer, desde o resumo,
   que os perfis avaliados são os três Ua\* normativos do TS 33.535 R18 — TLS
   1.3 PSK (Anexo B.1.3.2.2), DTLS 1.3 PSK (Anexo C.2.2) e OSCORE (Anexo D) —
   com K_AF **simulada** como PSK por aplicação. Isso é honesto e não exige
   código novo. O C3 (DTLS-SRTP) fica fora dos Ua\* e precisa de decisão (§6).
2. **Figura do testbed** (duas VMs, netem nas duas pontas, quatro canais,
   AAnF ausente/simulada). Não existe ainda. Um dia.
3. **Trabalhos relacionados** em meia coluna: Ko et al. (só TLS, uma VM),
   Restuccia/Gunnarsson/Gündoğan (já levantados na PRONTIDAO §5), Chaari
   (MAV-DTLS). Meio dia.
4. **Tabelas**: I — perfis × classe × anexo do TS 33.535; II — parâmetros
   (literatura vs. padrão do testbed); III — resumo por condição (mediana,
   IQR). Saem do `resumo.csv`.
5. **CPU: não precisa refazer testes.** Ver §3.4 — dois números já
   existentes são publicáveis (CPU do handshake; CPU por mensagem como
   diferença seguro − claro sob netem), e um não é (CPU por mensagem em
   `clean`).
6. **ProVerif, opcional nesta versão.** Se entrar, só o que é reutilização
   direta: rodar o `psk-dh-ecdhe.pv` deles (é o C4) e a variante com cookie
   para o C1, e reportar uma tabela ○/× "preliminar". Custo: 2–3 dias depois
   da instalação. Se não entrar, declarar como trabalho em curso.
7. **Escrita** no template IEEEtran (conference), ~4 figuras e 3 tabelas.
   Seções: I Introdução; II AKMA Ua\* e classes de UAV; III Testbed;
   IV Resultados; V Trabalhos relacionados; VI Conclusão e trabalho futuro
   (verificação formal, derivação de K_AF, hardware).

Estimativa: 2–3 semanas de escrita com os dados prontos, mais os itens 2–3.
Nenhuma medição nova é obrigatória.

### 3.3 Local

Decidido em 2026-09-18: conferência IEEE já escolhida pelo autor, prazo em
três semanas (≈ 2026-10-09).

### 3.4 A CPU que já existe (verificado em 2026-09-18)

Campanha segura `ad89a84` contra os baselines em claro `3717e9a`, mesma
condição, mediana das repetições:

| Cond. | Canal | CPU handshake | CPU/msg seguro | CPU/msg claro | seguro − claro | RSS seguro − claro |
|---|---|---|---|---|---|---|
| clean | C1 DTLS | 3,26 ms | 163 µs | **290 µs** | **−127 µs** | +1,26 MB |
| clean | C2 OSCORE | 0,44 ms | 576 µs | 408 µs | +168 µs | +0,14 MB |
| clean | C3 SRTP | 3,38 ms | 144 µs | **248 µs** | **−104 µs** | +4,09 MB |
| clean | C4 TLS | 1,65 ms | 173 µs | **298 µs** | **−126 µs** | +1,21 MB |
| 3gpp-c2 | C1 DTLS | 3,14 ms | 31 µs | 20 µs | +11 µs | +1,26 MB |
| 3gpp-c2 | C2 OSCORE | 0,30 ms | 378 µs | 131 µs | +246 µs | +0,14 MB |
| 3gpp-c2 | C3 SRTP | 3,16 ms | 44 µs | 41 µs | +3 µs | +4,11 MB |
| 3gpp-c2 | C4 TLS | 1,52 ms | 142 µs | 25 µs | +117 µs | +1,27 MB |
| handover | C1 DTLS | 3,74 ms | 32 µs | 23 µs | +9 µs | +1,21 MB |
| handover | C2 OSCORE | 0,32 ms | 390 µs | 149 µs | +241 µs | +0,14 MB |
| handover | C3 SRTP | 3,84 ms | 41 µs | 31 µs | +10 µs | +4,10 MB |
| handover | C4 TLS | 1,52 ms | 140 µs | 22 µs | +117 µs | +1,21 MB |

Três conclusões:

1. **A CPU por mensagem em `clean` não serve.** O canal em claro custa
   *mais* que o seguro (290 contra 163 µs no C1), o que é impossível como
   custo de cifra. Hipótese (não verificada): sem atraso, a resposta chega
   enquanto o processo ainda está na janela medida, e o processamento de
   recepção (softirq) é contabilizado ao processo; com 50–200 ms de atraso a
   resposta chega com o processo dormindo. Explica também o "clean 163 µs vs.
   netem 31 µs" da PRONTIDAO §3.3.
2. **Sob netem, seguro − claro é estável (IQR 0,2–3 µs) e fisicamente
   plausível**: DTLS +9–11 µs por mensagem de 128 B; SRTP +3–10 µs por pacote
   de 1200 B; TLS +117 µs por bloco de 16 KiB — coerente com AES-GCM em
   software, porque o wolfSSL foi compilado **sem** `--enable-aesni` /
   `--enable-intelasm` (a vCPU expõe `aes`, `pclmulqdq`, `avx2`); OSCORE
   +241–246 µs por leitura (proteção **e** verificação, CBOR/COSE do
   libcoap). É o "custo de CPU da segurança" do artigo, e sai sem medir nada.
3. **CPU do handshake e RSS são estáveis entre condições** e vão direto:
   DTLS 3,1–3,8 ms, TLS 1,5–1,7 ms, OSCORE (primeira troca) 0,3–0,4 ms;
   memória +1,2 MB (wolfSSL) em C1/C4, +4,1 MB (wolfSSL + libsrtp2) no C3,
   +0,14 MB no C2 (libcoap está nos dois lados).

`perf stat` em ciclos não acrescentaria nada aqui: num VMware, `cycles` só
existe com "Virtualize CPU performance counters" ligado na VM; sem isso o
perf devolve `task-clock`, que é o que já está medido. Declarar no artigo:
CPU de processo (`CLOCK_PROCESS_CPUTIME_ID`), AES em software, e o método da
diferença. Recompilar com AES-NI fica para o periódico — invalidaria a
comparabilidade com as campanhas feitas.

---

## 4. Periódico "igual ao dele", com os três protocolos

Título de trabalho: *Formal Security Verification and Experimental Evaluation
of 3GPP AKMA Ua\* Protocols — TLS 1.3, DTLS 1.3 and OSCORE — for UAV
Application Classes.*

### 4.1 Integração (equivalente ao §4 / Fig. 6 deles, uma por protocolo)

Aqui o escopo muda: **passa a ter AKMA**. É a maior decisão (§6).

| Protocolo | O que o TS 33.535 exige | O que muda no código |
|---|---|---|
| TLS 1.3 (B.1.3.2.2) | identidade PSK = "3GPP-AKMA" + A-KID; PSK externa derivada de K_AF | identidade em `as_profile.h`; K_AF = KDF(K_AKMA, 0x82 ‖ AF_ID ‖ L) com o KDF do TS 33.220 B.2 (HMAC-SHA-256) em `as_keys.c`, a partir de um K_AKMA e A-KID injetados |
| DTLS 1.3 (C.2.2) | "os procedimentos de B.1.3.2.2 se aplicam"; identidade via mensagem DTLS | mesmo que acima, no C1 |
| OSCORE (D.3) | POST `/akma` com {id do protocolo, A-KID, N1, AF-SID}; resposta `Created` com {N2, UE-SID}; OMS = HKDF(K_AF, "AKMA-OSCORE"); Master Salt = payload req ‖ payload resp; Sender IDs trocados | **novo** no C2: a troca de estabelecimento e a criação do contexto OSCORE em tempo de execução a partir do OMS derivado (hoje o contexto é estático, de arquivo) |
| AAnF | fornece K_AF ao AF por canal seguro | tabela local no processo servidor ou um terceiro processo na VM servidora; o paper também assume o canal AF↔AAnF seguro |

Duas VMs bastam: UE na cliente, AF + AAnF simulada na servidora.

### 4.2 Verificação formal (equivalente ao §5)

Modelos, em ordem de esforço:

1. **TLS 1.3 PSK-(EC)DHE** — adaptar o `psk-dh-ecdhe.pv` deles (identidade,
   sentido dos dados, cifra). Dias.
2. **DTLS 1.3 PSK-(EC)DHE com HRR/cookie** — acrescentar o par
   ClientHello → HelloRetryRequest(cookie = HMAC(k_srv, hash(CH))) → ClientHello
   com cookie. Hipótese a verificar: o cookie mitiga o "replay de sinalização"
   como custo (o servidor não cria estado nem faz DH antes do cookie), mesmo
   que Q1 siga `false` symbolicamente. Uma semana.
3. **OSCORE Ua\* (Anexo D)** — modelo novo: N1/N2, Sender IDs, OMS por HKDF,
   AEAD com Partial IV, janela anti-replay como `table` (`get ... in ... else
   insert`). Propriedades: sigilo, acordo injetivo no estabelecimento (N1/N2
   dão frescor — hipótese: aqui o equivalente da Q1 pode dar `true`), replay
   de dados (janela), sigilo futuro (sem DH: esperado `false` ao vazar K_AKMA
   na `phase 1`). Duas semanas.
4. **Separação de chaves entre canais** — query própria do seu desenho:
   vazar K_AF de um canal e verificar o sigilo dos outros. Dias.
5. Opcional: rodar `psk-only.pv` e `0rtt.pv` deles para reproduzir a Tab. 3
   e confirmar (com crédito).

Saídas no formato deles: Tab. requisito ↔ query; Tab. ○/△/× por protocolo;
apêndice com os "Verification summary" e grafos de ataque.

### 4.3 Experimento (equivalente ao §6)

- Os três modos do TLS no C4 (§2.2) para reproduzir as Figs. 8–11 deles, nas
  duas VMs.
- Concorrência (N clientes), séries temporais de CPU/RSS por processo.
- CPU em ciclos (`perf stat`), como Gallenmüller et al.
- Tudo que já existe: três condições, varreduras, baseline em claro, pcap.
- P2.1 (cada classe com a alternativa óbvia) — é o que sustenta "por classe".
- P2.2 (handover com troca de endereço e CID no C1) — opcional, mas é o único
  ponto em que o DTLS 1.3 justifica a escolha de forma experimental.
- Tabela como a Tab. 5 deles: operações criptográficas por estabelecimento
  nos três Ua\*, contadas a partir dos RFCs 8446/9147/8613.

### 4.4 Escrita

25–30 páginas, mesma espinha dorsal: 1 Introdução; 2 AKMA e Ua\*; 3 Integração
(três MSCs); 4 Verificação formal; 5 Experimentos (tabela de setup, figura de
cenário com UAV no lugar do CCTV, resultados); 6 Discussão (trade-offs por
classe de UAV); 7 Conclusão. Publicar modelos `.pv` e testbed no GitHub, como
eles.

Periódicos candidatos: MDPI Applied Sciences / Sensors / Drones / Electronics
(a família do paper deles); IEEE Access (Xplore); Computer Networks. Decisão
sua.

### 4.5 Sequência sugerida (só estimativa)

1. Artigo IEEE (semanas 1–3) com o que existe.
2. ProVerif: instalação, reprodução dos três modelos deles, adaptação
   TLS/DTLS (semanas 4–6).
3. Integração AKMA no código: K_AF, identidade 3GPP, Anexo D no C2
   (semanas 6–10). Commits por tema.
4. Modelo OSCORE e separação de chaves (semanas 8–11).
5. Experimentos estendidos: modos do TLS, concorrência, P2.1 (semanas 10–14).
6. Escrita do periódico (semanas 14–19).

---

## 5. ProVerif passo a passo

### 5.1 Instalar

```bash
sudo apt update && sudo apt install -y proverif graphviz
```

O pacote `proverif` está no *universe* do Ubuntu (não consegui confirmar
daqui: o cache do apt estava vazio e a VM não alcança packages.ubuntu.com).
Se não estiver disponível, ou para a versão mais recente:

```bash
sudo apt install -y opam && opam init -y && eval "$(opam env)" && opam install -y proverif
```

`graphviz` é o que permite `-graph` (grafos de ataque, Figs. A4–A5).

### 5.2 Reproduzir as Figs. A1–A3 do paper

```bash
git clone --depth 1 https://github.com/yonghoko/ProVerif.git ~/ko-proverif
```

```bash
cd ~/ko-proverif/Toward\ an\ Era\ of\ Secure\ 5G\ Convergence\ Applications*/ && proverif psk-dh-ecdhe.pv
```

Esperado (Fig. A2): Q1 `false`, Q2 `true`, Q3 `true`, `not attacker(app_data1)`
e `app_data2` `true` (com `phase 1`, isto é, sigilo futuro). Depois
`psk-only.pv` (Fig. A1: sigilo `false` — sem sigilo futuro) e `0rtt.pv`
(Fig. A3: `app_data1` `false`). Para os grafos:

```bash
mkdir -p out && proverif -graph out psk-only.pv
```

Gera um PDF por ataque em `out/`. `-html out` produz a versão navegável.

### 5.3 Anatomia de um modelo (para adaptar)

1. Cabeçalho: `set ignoreTypes = false.` e primitivas (DH, HMAC, AEAD).
2. `letfun` do key schedule — copiar sem mudar; é o RFC 8446 §7.1.
3. Canais: `c` público, `sp` privado (AF↔AAnF).
4. Eventos S_/E_ por passo, queries `inj-event` e `attacker`.
5. `proc_UE`, `proc_AF`, `proc_AAnF`.
6. Main: replicação `!` e a `phase 1` que vaza K_AKMA para testar sigilo
   futuro.

### 5.4 Adaptar aos meus três canais

- **C4 / TLS 1.3**: `psk-dh-ecdhe.pv` quase intacto. Se K_AF continuar
  injetada, substituir a derivação por `new k_af: key` e remover `proc_AAnF`;
  se a derivação entrar (§4.1), manter.
- **C1 / DTLS 1.3**: partir do mesmo arquivo; acrescentar em `proc_AF` a
  resposta HRR com cookie quando o ClientHello vem sem cookie, e em `proc_UE`
  o reenvio. Chave do cookie: `new k_cookie: key` no main, só do AF.
- **C2 / OSCORE**: modelo novo (§4.2, item 3). Começar pelo contexto estático
  (como o C2 é hoje), depois acrescentar a troca N1/N2 do Anexo D.
- **Separação entre canais**: um main com os três processos e `phase 1;
  out(c, k_af_c1)`, com queries de sigilo sobre os dados do C2 e do C4.

Regra prática: cada query que der `false` precisa do grafo (`-graph`) lido
até o fim antes de virar "vulnerabilidade" no texto — o caso da Q1 deles
(§1.5) é o exemplo do que acontece quando não se faz isso.

---

## 6. Decisões tomadas (2026-09-18)

| # | Decisão | Consequência |
|---|---|---|
| 1 | **AKMA entra no código** | o escopo registrado em 2026-09-09 muda: K_AF derivada de K_AKMA (TS 33.535 A.4, KDF do TS 33.220 B.2), identidade "3GPP-AKMA"+A-KID, Anexo D no C2, AAnF simulada. Só para o periódico |
| 2 | **C3 (DTLS-SRTP) fica, como proposição** | precisa de um canal de mídia com o DTLS 1.3 da norma (Anexo C, camada de registro) para comparar na mesma classe — é o P2.1 da mídia. **Pendente: para o artigo IEEE ou para o periódico?** |
| 3 | ProVerif **só no periódico** | o artigo IEEE é puramente experimental; declara a verificação formal como trabalho em curso |
| 4 | CPU no artigo IEEE **sem refazer testes** | usar §3.4: CPU do handshake, seguro − claro sob netem, RSS; omitir CPU/msg em `clean` |
| 5 | PSK-only e 0-RTT **só no periódico** | o C4 segue `psk_dhe_ke`; o wolfSSL não é recompilado antes do artigo |
| 6 | Conferência IEEE escolhida, **prazo ≈ 2026-10-09**; periódico a decidir | o plano abaixo cabe nas três semanas |

Continua fora de escopo: seleção adaptativa de perfil.

## 7. Plano das três semanas (artigo IEEE)

Sem medição nova obrigatória. Se a decisão 2 for "no artigo IEEE", o canal
de mídia sobre DTLS entra na semana 1 (1–2 dias de código reaproveitando o
handshake do C1 e o gerador do C3, mais ~1 h de campanha nas três condições
com 5 repetições) e desloca o resto em dois dias.

**Semana 1 — material**

1. Figura do testbed (duas VMs, netem nas duas pontas, quatro canais, K_AF
   simulada como PSK por aplicação).
2. Tabelas a partir dos `resumo.csv`: I perfis × classe × anexo do TS
   33.535; II parâmetros (literatura vs. padrão do testbed); III resumo por
   condição com IQR; IV custo da segurança (§3.4 + bytes + estabelecimento).
3. Figuras finais (as quatro de `docs/figuras/`, revistas para uma coluna
   IEEE: fonte, legenda, unidades).
4. Esqueleto LaTeX IEEEtran (conference) com as seções da §3.2.

**Semana 2 — texto**

5. Introdução e enquadramento AKMA Ua\* (Anexos B, C, D) + classes de UAV.
6. Testbed e método (reprodutibilidade: commits fixos, manifesto,
   `campaign.sh`; limitações: VM, relógios, tráfego sintético, AES em
   software).
7. Resultados: as cinco observações da PRONTIDAO §2.4 + varreduras + custo
   da segurança.
8. Trabalhos relacionados (Ko et al.; Restuccia; Gunnarsson; Gündoğan;
   Vučinić; Chaari).

**Semana 3 — fechamento**

9. Conclusão e trabalho futuro (ProVerif, AKMA no código, modos PSK, CID,
   hardware).
10. Revisão de página (5–6), figuras em vetor, referências, checagem de
    todos os números contra os `resumo.csv`.
11. Submissão com margem de dois dias.
