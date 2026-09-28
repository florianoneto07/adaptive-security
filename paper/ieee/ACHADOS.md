# Achados e figuras do artigo IEEE

Fechado em 2026-09-28. Este documento é a ponte entre os dados e o texto: para
cada figura, o que ela afirma, de onde vem o número e o que **não** se pode
dizer a partir dela. Quem escrever o artigo não precisa reabrir os CSVs.

Campanhas usadas (fixadas no cabeçalho do `scripts/paper_tables.py` e do
`scripts/plot_resultados.py`):

| Papel | Diretório | Cobertura |
|---|---|---|
| principal | `campaign-20260915T121452Z-ad89a84` | clean, 3gpp-c2, handover; 10 reps |
| baseline em claro | `campaign-20260916T124934Z-3717e9a` | as mesmas 3 condições; 5 reps |
| varredura de atraso | `campaign-20260916T112433Z-3717e9a` | delay-0 … delay-200; 5 reps |
| varredura de perda | `campaign-20260916T142149Z-3717e9a` + `campaign-20260916T182845Z-606a0af` | loss-0 … loss-20; 5 reps |

**Todas foram colhidas com o wolfSSL sem aceleração de hardware** (AES-256-GCM
a 138 MiB/s, ECDHE P-256 a 0,41 ms). A build acelerada entrou em `41c7c9d`,
depois destas medições — ver REPORT.md §2. O artigo declara isso; os números
de CPU são custo de software nesta plataforma.

---

## As figuras

Geradas por `./scripts/plot_resultados.py --out paper/ieee/figures --lang en`.
Nomes neutros de idioma; a versão em português fica em `docs/figuras/`.

O conjunto foi refeito em 2026-09-28. A primeira versão organizava as figuras
em torno dos achados (uma figura por conclusão) e não em torno do experimento.
Trocado pelo formato do trabalho de referência — **latência, CPU e memória,
sempre com os quatro perfis lado a lado sob os mesmos tratamentos** — que é o
que permite ao leitor comparar os perfis em vez de acreditar na conclusão.

### Fig. 1 — `fig1-establishment-latency`

Barras agrupadas: quatro perfis x três condições, tempo de estabelecimento em
ms, **eixo logarítmico** (vai de 1,1 ms a 4210 ms) com o valor escrito em cada
barra, porque área em eixo log não é proporcional.

| | enlace limpo | 3GPP C2 | handover |
|---|---|---|---|
| C1 DTLS 1.3 | 8,8 | 308,6 | 3754,3 |
| C2 OSCORE | 1,1 | 101,6 | 413,9 |
| C3 DTLS-SRTP | 6,9 | 308,7 | 4210,5 |
| C4 TLS 1.3 | 3,3 | 203,7 | 823,3 |

**Afirma:** a ordem entre os perfis se mantém nas três condições; o que muda
é a escala, por três ordens de grandeza.

### Fig. 2 — `fig2-establishment-cpu`

Mesmos eixos da Fig. 1, agora CPU do processo cliente
(`CLOCK_PROCESS_CPUTIME_ID`), escala linear.

| | enlace limpo | 3GPP C2 | handover |
|---|---|---|---|
| C1 DTLS 1.3 | 3,26 | 3,14 | 3,74 |
| C2 OSCORE | 0,44 | 0,30 | 0,32 |
| C3 DTLS-SRTP | 3,38 | 3,16 | 3,84 |
| C4 TLS 1.3 | 1,65 | 1,52 | 1,52 |

**Afirma:** dentro de cada perfil as três condições são indistinguíveis — o
mesmo estabelecimento que leva 8,8 ms e 3,8 s de relógio custa a mesma CPU. A
espera está na rede, não no nó. Entre perfis, uma ordem de grandeza: OSCORE
não faz handshake; o DTLS é dominado pelo ECDHE efêmero.

É o par da Fig. 1 e só funciona ao lado dela: sozinha, a figura parece dizer
que "nada acontece".

### Fig. 3 — `fig3-memory`

RSS de pico por canal, **seguro contra o baseline em claro**, em MB.

| | seguro | em claro | acréscimo |
|---|---|---|---|
| C1 DTLS 1.3 | 4,24 | 3,01 | +1,23 |
| C2 OSCORE | 4,15 | 4,01 | +0,14 |
| C3 DTLS-SRTP | 6,91 | 2,90 | +4,01 |
| C4 TLS 1.3 | 4,12 | 2,92 | +1,20 |

Entre as três condições o RSS varia menos de 0,07 MB, então mostrar as três
daria barras idênticas; o contraste que informa é com o canal sem segurança.
O acréscimo é a pilha que cada perfil linka: wolfSSL (C1, C4), wolfSSL mais
libsrtp2 (C3), e quase nada no C2, porque o libcoap já está nos dois lados e
o OSCORE acrescenta um contexto, não uma pilha.

**Não diz** nada comparável com a literatura de microcontrolador: é processo
Linux, dominado por libc e pelas bibliotecas.

### Fig. 4 — `fig4-delivery-loss`

Entrega (%) contra perda, seis pontos, 50 ms de atraso fixo. C1 e C3
acompanham a perda (100 % → 80,5 % / 79,9 %); C2 e C4 ficam em 100 % em todos
os pontos e por isso aparecem como uma série só. Eixo x ordinal de propósito:
0 e 0,1 % não existem num log e colariam num linear.

### Fig. 5 — `fig5-establishment-rtt` (reserva, fora do artigo)

Estabelecimento contra RTT, cinco pontos, eixo x **linear**: três retas de
inclinação exatamente 3, 2 e 1 RTT (medida entre os extremos:
(1213,1 − 13,6)/400 = 3,00; (805,7 − 6,3)/400 = 2,00; (402,4 − 2,0)/400 =
1,00), sobre um intercepto de 2 a 14 ms.

É a figura mais limpa do conjunto, mas mostra o mesmo fenômeno da Fig. 1 com
cinco pontos em vez de três, e não cabe num artigo de seis páginas junto com
as outras quatro. Fica gerada; a Tab. `tab_delay` carrega os mesmos números.
Se houver espaço, o melhor uso é **substituir** a Fig. 1 — ao custo de perder
a comparação sob as três condições nomeadas.

**Cuidado ao reaproveitar:** com eixo x ordinal (espaçamento igual entre 0,
50, 100, 200 e 400), esta relação exatamente linear aparece encurvada e a
figura passa a sugerir crescimento super-linear. Foi o defeito da primeira
versão.

### O que saiu do conjunto

- **Sobrecarga por mensagem em barras.** São quatro números constantes; vivem
  melhor na `tab_results` e na `tab_security_cost`.
- **Figura de dois painéis com p50/p95 do C2 e vazão do C4.** A informação
  (o custo do OSCORE cai na cauda, não na mensagem típica) continua no texto
  e na `tab_loss`.

## Os cinco achados

Em ordem de força. Os três primeiros sustentam o artigo sozinhos.

1. **O perfil fixa o número de RTTs; o enlace multiplica.** OSCORE 1 RTT,
   TLS 1.3/TCP 2 RTT, DTLS 1.3 com cookie 3 RTT, com intercepto de 2–14 ms.
   Cinco pontos de atraso, ajuste exato (Fig. 1). É a previsão da
   especificação confirmada em medição, e a figura mais limpa do conjunto.

2. **A sobrecarga por mensagem é constante e independe do enlace** (Fig. 4).
   O OSCORE é o mais barato por mensagem (+11 B contra +22 B do DTLS/TLS),
   coerente com Gunnarsson et al. e Gündoğan et al.

3. **A entrega separa os perfis sob perda, e o preço da confiabilidade é
   tempo** (Figs. 2 e 3). DTLS/SRTP não retransmitem dados de aplicação:
   perda é perda. CoAP/OSCORE e TCP entregam 100 % e pagam em cauda e em
   vazão. Para um canal de controle, o dado relevante é que no OSCORE o custo
   cai **inteiro na cauda** — a mensagem típica não é afetada.

4. **A segurança custa estabelecimento e bytes, não confiabilidade.** A
   entrega do canal seguro é indistinguível da do canal em claro em todas as
   condições — a perda vem do enlace, não do perfil. Custo de
   estabelecimento: 0 → 3–9 ms em enlace limpo, segundos sob handover. Este
   achado só existe porque há baseline em claro, e é o que o trabalho de
   referência (Ko et al. 2024) não tem.

5. **A 20 % de perda independente o handshake DTLS falha com frequência**
   (3 de 5 repetições no C1, 2 de 5 no C3) dentro do orçamento de
   retransmissão de 1 a 8 s. Sob 20 % **em rajada** (handover) passou 10/10,
   porque a rajada deixa janelas limpas. É resultado, não defeito, e está na
   coluna `reps_sem_dados` do `resumo.csv`.

---

## O que NÃO pode ser afirmado

Registro explícito, para não escorregar na escrita:

- **Que a atribuição de perfis é ótima.** O testbed mede o custo dos perfis
  atribuídos; não compara cada classe com todas as alternativas. Isso é o
  P2.1, e fica para o periódico.
- **Que seleção dinâmica seria pior.** Está fora de escopo por decisão de
  projeto.
- **Nada sobre energia ou memória de microcontrolador.** É VM.
- **Valores absolutos de atraso do C3.** Os relógios das VMs não são
  sincronizados; valem as diferenças entre percentis.
- **Comparação de CPU por mensagem entre condições.** Sob `clean` o canal em
  claro mede *mais* CPU que o seguro (290 contra 163 µs no C1), o que é
  impossível como custo de cifra — hipótese: sem atraso, a resposta é
  processada dentro da janela medida. Só a diferença seguro − claro **sob
  netem** é reportável.
- **Que os números de CPU valem para qualquer plataforma.** São AES em
  software nesta VM (ver topo).
