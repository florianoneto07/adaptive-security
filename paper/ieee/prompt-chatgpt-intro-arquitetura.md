# Prompt para o ChatGPT — escrever a Introdução e a Arquitetura

Copie **tudo** a partir da linha `--- INÍCIO DO PROMPT ---`. O repositório é
público, então os links abrem sem autenticação; ainda assim o prompt traz os
fatos por escrito, para o caso de o modelo não conseguir navegar.

Depois de colar, confira o retorno contra `paper/ieee/ACHADOS.md`: qualquer
número que não esteja lá ou no prompt é invenção e precisa sair.

--- INÍCIO DO PROMPT ---

Você vai escrever duas seções de um artigo científico de conferência IEEE
(5 a 6 páginas, formato IEEEtran, `conference`). Escreva em **inglês
técnico**, direto, sem adjetivos de venda. Entregue **LaTeX**, pronto para
colar num `main.tex` que já existe.

## O que eu preciso de você

1. **Section I — Introduction** (aprox. 450–600 palavras, 4 ou 5 parágrafos).
2. **Section II — Background and Profile Assignment** (aprox. 600–800
   palavras), com três subseções:
   - II.A *AKMA and the Ua\* Protocols*
   - II.B *UAV Application Classes*
   - II.C *One Fixed Profile per Class*

Não escreva resumo, método, resultados nem conclusão — essas partes já
existem.

## Regras que não podem ser quebradas

- **Não invente número nenhum.** Use apenas os valores deste prompt. Se
  precisar de um dado que não está aqui, escreva `\todo{falta: <o quê>}` no
  lugar.
- **Não invente citação nenhuma.** Cite somente com as chaves listadas em
  "Bibliografia disponível", no formato `\cite{chave}`. Se quiser apoiar uma
  afirmação e não houver chave adequada, escreva
  `\todo{citação necessária: <afirmação>}`.
- **Não prometa o que o trabalho não faz.** A seção "Fora de escopo" abaixo é
  limite rígido: nada no texto pode sugerir que essas coisas foram feitas.
- Use `\SI{}{}` do `siunitx` para grandezas com unidade.
- Trate "UE" e "AF" como os papéis do 3GPP, não como jargão solto.

## Contexto: o problema

Um veículo aéreo não tripulado (UAV) ligado a uma estação de solo não gera um
tráfego só. Ele gera classes de tráfego com requisitos incompatíveis entre
si: comando e controle exige baixa latência e tolera perda; telemetria é
pequena, periódica e pode atravessar intermediários; vídeo em tempo real é
volumoso e descartável; transferência de missão, firmware e logs exige
ordenação e confiabilidade, mas não urgência.

O 3GPP padronizou o AKMA (TS 33.535), que deriva uma chave por aplicação
(K_AF) a partir da autenticação primária de 5G e deixa em aberto qual
protocolo protege o ponto de referência Ua\* entre o UE e o Application
Function (AF). A Release 18 traz **três anexos normativos** com essa escolha:

- **Anexo B** — protocolos baseados em TLS. B.1.3.2.2 trata do TLS 1.3: a
  identidade PSK no ClientHello leva o prefixo `"3GPP-AKMA"` mais o A-KID, e
  UE e AF derivam a PSK externa do K_AF.
- **Anexo C** — DTLS 1.3. C.2.2 diz que os procedimentos de B.1.3.2.2 se
  aplicam também ao DTLS 1.3.
- **Anexo D** — OSCORE (RFC 8613). Define o estabelecimento por CoAP POST em
  `/akma`, e o contexto de segurança com
  `OMS = HKDF(K_AF, "AKMA-OSCORE")`.

A lacuna: o trabalho de referência mais próximo (Ko et al. 2024,
`\cite{ko2024}`) verificou formalmente e mediu **apenas a opção TLS**, com
cliente e servidor **na mesma máquina virtual** e contadores de CPU e memória
**do sistema inteiro**, sem baseline sem segurança e sem condições de enlace
descritas. Ninguém mediu os três perfis normativos lado a lado, num enlace
real, contra um controle em claro, e para classes de aplicação de UAV.

## Contexto: o que este trabalho faz

Atribui a cada classe de aplicação do UAV **um perfil de segurança fixo,
decidido em projeto e não negociado em execução**, e mede o custo dessa
atribuição num testbed de duas máquinas virtuais com emulação de enlace
simétrica.

| ID | Classe de aplicação | Perfil | Transporte | Porta | TS 33.535 |
|----|---|---|---|---|---|
| C1 | Comando e controle | DTLS 1.3 com PSK | UDP | 5001 | Anexo C |
| C2 | Telemetria periódica | OSCORE sobre CoAP | UDP | 5002 | Anexo D |
| C3 | Vídeo e áudio em tempo real | handshake DTLS 1.3 + SRTP na mídia | UDP | 5003 | fora dos Ua\* — proposta do autor |
| C4 | Missão, firmware, logs | TLS 1.3 com PSK | TCP | 5004 | Anexo B |

Justificativa de cada atribuição (use isto na II.C):

- **C1 / DTLS 1.3.** O requisito 3GPP para comando e controle de UAV é
  latência de \SI{50}{\milli\second} e taxa de erro de pacote de 10^-3
  `\cite{zeng2019}`. Isso exige UDP: o TCP retransmitiria e ordenaria
  mensagens de controle que já perderam a validade quando chegam. O DTLS 1.3
  tem sigilo futuro obrigatório e cabeçalho de registro de 11 bytes; o
  Connection ID cobre troca de endereço em mobilidade.
- **C2 / OSCORE.** Mensagens pequenas e periódicas que podem atravessar um
  proxy ou um sistema UTM. O OSCORE `\cite{rfc8613}` protege o objeto CoAP e
  mantém a segurança fim a fim **através** do intermediário; TLS e DTLS são
  terminados no proxy e a mensagem segue em claro do outro lado.
- **C3 / DTLS-SRTP.** Mídia em tempo real usa RTP. Aqui o DTLS serve
  **apenas para estabelecer as chaves**, pela extensão `use_srtp`
  `\cite{rfc5764}`; quem protege os pacotes é o SRTP. Não é a camada de
  registro do DTLS protegendo o vídeo. **Este perfil está fora dos Ua\* do
  3GPP e precisa ser marcado no texto como proposta dos autores.**
- **C4 / TLS 1.3.** Transferência volumosa e diferida, em que
  confiabilidade e ordenação importam mais que latência; TCP é adequado.

Cada canal usa **sua própria chave pré-compartilhada de 32 bytes, distinta
das demais**, o que simula a separação de chave por aplicação do AKMA: cada
classe corresponde a um AF com seu próprio K_AF, e uma aplicação não alcança
a chave de outra. **Nesta fase a chave é injetada, não derivada** — ver
"Fora de escopo".

## Contexto: a arquitetura do código

Repositório público: <https://github.com/florianoneto07/adaptive-security>
(branch `main`).

**Cada canal é um par de binários independente — um cliente e um servidor.**
O cliente roda na VM que representa o UAV; o servidor, na VM que representa a
estação de solo. Os pares sobem, rodam e são derrubados sem se afetar: a
falha de um canal não interrompe os outros. O perfil é **constante de
compilação**, escolhido numa tabela `const` em `common/as_profile.h`; não
existe caminho de código que troque o perfil de um canal pelo de outro.

Os quatro pares cliente/servidor:

| Canal | Cliente (UAV) | Servidor (estação de solo) |
|---|---|---|
| C1 | [`channels/c1_control/c1_client.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c1_control/c1_client.c) | [`c1_server.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c1_control/c1_server.c) |
| C2 | [`channels/c2_telemetry/c2_client.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c2_telemetry/c2_client.c) | [`c2_server.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c2_telemetry/c2_server.c) |
| C3 | [`channels/c3_media/c3_client.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c3_media/c3_client.c) | [`c3_server.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c3_media/c3_server.c) |
| C4 | [`channels/c4_bulk/c4_client.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c4_bulk/c4_client.c) | [`c4_server.c`](https://github.com/florianoneto07/adaptive-security/blob/main/channels/c4_bulk/c4_server.c) |

Código comum aos quatro:

- [`common/as_profile.h`](https://github.com/florianoneto07/adaptive-security/blob/main/common/as_profile.h)
  — a tabela `const` dos perfis: canal, classe, perfil, transporte, porta,
  arquivo de chave e identidade PSK.
- [`common/as_psk.c`](https://github.com/florianoneto07/adaptive-security/blob/main/common/as_psk.c)
  — instala a PSK no contexto TLS/DTLS e aplica as travas de perfil.
- [`common/as_keys.c`](https://github.com/florianoneto07/adaptive-security/blob/main/common/as_keys.c)
  — carrega a chave do canal (arquivo ou variável de ambiente) e calcula o
  fingerprint que vai ao manifesto (nunca a chave).
- [`common/as_metrics.c`](https://github.com/florianoneto07/adaptive-security/blob/main/common/as_metrics.c)
  — instrumentação: CSV por evento, percentis, bytes no fio, CPU, RSS.
- [`channels/baseline/`](https://github.com/florianoneto07/adaptive-security/tree/main/channels/baseline)
  — os pares **sem segurança** (UDP, CoAP, RTP e TCP puros), que carregam o
  mesmo tráfego e servem de controle.
- [`run.sh`](https://github.com/florianoneto07/adaptive-security/blob/main/run.sh),
  [`campaign.sh`](https://github.com/florianoneto07/adaptive-security/blob/main/campaign.sh),
  [`netem.sh`](https://github.com/florianoneto07/adaptive-security/blob/main/netem.sh)
  — orquestração das execuções, das campanhas entre as duas VMs e da emulação
  de enlace.
- [`README.md`](https://github.com/florianoneto07/adaptive-security/blob/main/README.md)
  — justificativa de cada perfil, com as fontes, e o formato dos dados.

## Contexto: os mecanismos de segurança, em detalhe

Descreva-os com esta precisão; não generalize para "TLS/DTLS genérico".

**C1 e C4 — (D)TLS 1.3 com PSK externa.** Cifra
`TLS_AES_256_GCM_SHA384`. O modo é **`psk_dhe_ke`**, imposto em código: a PSK
autentica os dois lados e o ECDHE efêmero garante sigilo futuro. Duas travas
tornam o perfil não negociável: uma exige PSK e desliga o downgrade de versão
(impedindo que o handshake recaia em autenticação por certificado), a outra
exige `psk_dhe_ke` (impedindo o modo `psk_ke`, que não tem sigilo futuro).
Não há certificados em nenhum dos canais. O servidor recusa identidade PSK
que não seja a do seu canal, de modo que um cliente apontado para a porta
errada é barrado antes da comparação de chave. Sobrecarga medida: 22 bytes
por registro (cabeçalho, tipo interno e tag de 16 bytes).

**C2 — OSCORE sobre CoAP.** O OSCORE não tem handshake: o contexto de
segurança é derivado direto do master secret (RFC 8613 §3.2). Parâmetros
usados: AEAD **AES-CCM-16-64-128** (o obrigatório da RFC 8613), derivação
**HKDF-SHA-256**, Sender/Recipient ID distintos para cliente e servidor. As
requisições são CoAP **confirmáveis**, e é daí que vêm as retransmissões
nativas. O número de sequência do remetente é persistido entre execuções,
porque reusar número de sequência com o mesmo par de chaves repetiria o nonce
do AEAD e quebraria a cifra. Sobrecarga medida: 11 bytes a mais por mensagem
que o CoAP em claro.

**C3 — DTLS 1.3 só para chaveamento, SRTP na mídia.** O handshake DTLS 1.3
negocia a extensão `use_srtp` com o perfil **`SRTP_AEAD_AES_256_GCM`** e
exporta 88 bytes de material de chaveamento (chave de 32 bytes mais salt de
12, para cada sentido). A partir daí **quem protege cada pacote é o SRTP**, e
não a camada de registro do DTLS. Sem a extensão negociada, a exportação
falha e o canal para: não existe caminho que envie mídia em claro.
Sobrecarga medida: 28 bytes por pacote (cabeçalho RTP de 12 mais tag de 16).

**Princípio comum (importante para a II.C):** se o perfil configurado falhar,
o canal **falha em voz alta e não degrada para outro**. Um fallback
silencioso é indistinguível, do lado de fora, de um ataque de rebaixamento
bem-sucedido.

## Contexto: o testbed (só o necessário para a Introdução)

Duas VMs Ubuntu 26.04 (2 vCPU, 5,2 GB) na mesma sub-rede, com `netem`
aplicado **nas duas pontas**, de modo que o atraso é simétrico e
RTT = 2 × atraso. Bibliotecas com commit fixado: wolfSSL 5.9.2 (TLS 1.3,
DTLS 1.3, PSK, `use_srtp`, AES-CCM), libcoap com OSCORE sobre o mesmo
wolfSSL, libsrtp2 2.7. Três condições base (enlace limpo; 50 ms com 0,1 % de
perda; 200±20 ms com 20 % de perda em rajada), mais uma varredura de perda e
uma de atraso. Cada execução grava um manifesto com commit, versões de
biblioteca, o netem efetivamente ativo e o fingerprint de cada chave.

## Os achados (a Introdução deve prometer exatamente estes)

1. O estabelecimento escala como um número **inteiro** de idas e voltas,
   fixado pelo perfil: 1 RTT para OSCORE, 2 para TLS 1.3 sobre TCP, 3 para
   DTLS 1.3 com cookie, mais um intercepto de 2 a 14 ms.
2. A sobrecarga por mensagem é constante e independe do enlace: 22 bytes
   (DTLS/TLS), 28 (SRTP) e 18 (OSCORE, incluindo o CoAP).
3. Sob perda, a entrega separa os perfis: DTLS e SRTP acompanham a perda
   (100 % → 80 % a 20 %) porque não retransmitem dados de aplicação;
   OSCORE e TCP mantêm 100 % e pagam em tempo — no OSCORE o atraso típico
   não muda (p50 de \SI{102}{\milli\second}, o RTT) e só a cauda cresce
   (p95 de \SI{103}{\milli\second} para \SI{30.6}{\second}); no TCP a vazão
   cai de 40,1 para 0,017 Mb/s.
4. **A segurança custa estabelecimento e bytes, não confiabilidade**: a
   entrega do canal seguro é indistinguível da do canal em claro em todas as
   condições.
5. A 20 % de perda **independente**, o handshake DTLS falhou em 3 de 5
   repetições (C1) e 2 de 5 (C3); sob 20 % **em rajada** completou 10 de 10,
   porque a rajada deixa janelas limpas.

## Fora de escopo — limite rígido

O texto **não pode** sugerir que qualquer destes foi feito:

- **Seleção adaptativa ou dinâmica de perfil.** Os perfis são fixos por
  decisão de projeto. (O repositório se chama "adaptive-security" por razões
  históricas; ignore o nome.)
- **Derivação de chave do AKMA.** Não há AAnF, não há A-KID, não há
  K_AKMA → K_AF. O K_AF é **emulado** por uma PSK de 32 bytes injetada por
  canal. Diga isso explicitamente na Introdução, como delimitação.
- **Verificação formal.** Não há ProVerif neste artigo; é trabalho futuro.
- **Hardware.** Nada de Pixhawk, Raspberry Pi, energia ou memória de
  microcontrolador. É VM.
- **Afirmar que a atribuição de perfis é ótima**, ou que outra escolha seria
  pior. O trabalho mede o custo dos perfis atribuídos; não compara cada
  classe com todas as alternativas.
- **MAVLink real ou codec de vídeo real.** O tráfego é sintético, com o
  perfil de cada classe.

## Contribuições (para o parágrafo final da Introdução)

1. A atribuição de um perfil fixo por classe de aplicação de UAV, derivada
   dos requisitos do 3GPP e das propriedades de cada protocolo.
2. Um testbed aberto de duas VMs, com emulação de enlace simétrica,
   instrumentação **por processo** e captura de pacotes, com baselines sem
   segurança para cada classe.
3. Uma avaliação dos três protocolos Ua\* normativos lado a lado, sob três
   condições e duas varreduras, isolando o custo da segurança.

## Bibliografia disponível

Use somente estas chaves:

`ko2024` (Ko et al., AKMA + TLS 1.3 PSK, ProVerif, Appl. Sci. 2024) ·
`ts33535` (3GPP TS 33.535 v18.5.0) · `ts22125` (3GPP TS 22.125, suporte a
UAS) · `rfc8446` (TLS 1.3) · `rfc9147` (DTLS 1.3) · `rfc9146` (Connection ID
do DTLS 1.2) · `rfc8613` (OSCORE) · `rfc5764` (DTLS-SRTP) · `rfc7252` (CoAP)
· `zeng2019` (UAV communications, requisitos por classe) · `fotouhi2020`
(levantamento de UAV móveis) · `wu2018` (taxas de vídeo) · `gundogan2020`
(OSCORE e NDN, medição) · `restuccia2020` (DTLS/TLS 1.3 em dispositivos de
baixa potência) · `gunnarsson2021` (desempenho do OSCORE) · `vucinic2015`
(DTLS em redes com ciclo de trabalho) · `hristozov2021` (custo de OSCORE e
EDHOC) · `gallenmuller2019` (custo de CPU do DTLS) · `chaari2020`
(MAV-DTLS, UAV) · `wolfssl` · `libcoap`

## Formato da entrega

- LaTeX puro, começando em `\section{Introduction}`.
- Sem `\begin{document}`, sem preâmbulo, sem bibliografia.
- Pode usar `itemize` na lista de contribuições; no resto, prosa.
- Ao final, fora do LaTeX, liste em português: (a) toda afirmação que você
  escreveu e não consegue apoiar no material acima, e (b) todo ponto em que
  achou o material insuficiente.

--- FIM DO PROMPT ---
