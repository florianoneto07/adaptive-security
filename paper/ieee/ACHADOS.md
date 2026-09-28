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

## As quatro figuras

Geradas por `./scripts/plot_resultados.py --out paper/ieee/figures --lang en`.
Nomes neutros de idioma; a versão em português fica em `docs/figuras/`.

### Fig. 1 — `fig1-establishment-rtt`

**Afirma:** o perfil determina o número de idas e voltas do estabelecimento; o
enlace apenas multiplica.

Eixo x **linear em RTT** (não ordinal). Isso não é detalhe de estilo: com o
eixo ordinal que a primeira versão usava, uma relação exatamente linear
aparecia encurvada e a figura sugeria crescimento super-linear — o oposto da
afirmação do título. Três retas, inclinações 3, 2 e 1:

| RTT (ms) | DTLS 1.3 (C1, C3) | TLS 1.3/TCP (C4) | OSCORE (C2) |
|---|---|---|---|
| 0 | 13,6 / 12,5 | 6,3 | 2,0 |
| 50 | 163,0 | 105,7 | 52,6 |
| 100 | 313,3 | 205,5 | 102,2 |
| 200 | 613,3 | 405,9 | 202,5 |
| 400 | 1213,1 | 805,7 | 402,4 |

Inclinação medida entre os extremos: (1213,1 − 13,6)/400 = **3,00**;
(805,7 − 6,3)/400 = **2,00**; (402,4 − 2,0)/400 = **1,00**. Intercepto de
2 a 14 ms: é o processamento local.

C1 e C3 são fundidos numa série só. Usam o mesmo handshake DTLS 1.3 e ficam a
≤ 1,2 ms um do outro — duas linhas sobrepostas não informariam nada e
gastariam um slot da paleta.

**Não diz:** nada sobre perda; a varredura de atraso fixa 0,1 %.

### Fig. 2 — `fig2-delivery-loss`

**Afirma:** só os perfis que retransmitem seguram a entrega.

C1 (DTLS) e C3 (SRTP) acompanham a perda: 100 % → 80,5 % / 79,9 % a 20 %.
C2 (OSCORE sobre CoAP confirmável) e C4 (TCP) ficam em 100 % em todos os
pontos. Eixo x ordinal de propósito: 0 e 0,1 % não existem num log e colariam
num linear.

**Não diz:** que C2 e C4 são "melhores". O preço está na Fig. 3.

### Fig. 3 — `fig3-price-of-delivery`

**Afirma:** a entrega de 100 % se paga em tempo, não em perda. Dois painéis,
eixos logarítmicos, mesmo eixo x da Fig. 2.

(a) **C2/OSCORE:** o atraso **típico não muda** — p50 fica em 102 ms (o RTT)
em todos os níveis de perda — e só a cauda cresce: p95 de 103 ms a **30,6 s**.
É o backoff do CoAP confirmável.

| perda | 0 | 0,1 | 1 | 5 | 10 | 20 |
|---|---|---|---|---|---|---|
| p50 (ms) | 102 | 102 | 102 | 102 | 102 | 102 |
| p95 (ms) | 103 | 103 | 103 | 2978 | 6853 | 30569 |

(b) **C4/TLS sobre TCP:** vazão de 40,1 a **0,017 Mbps**, quatro ordens de
grandeza.

Esta figura substituiu uma que mostrava só a vazão. A distinção p50/p95 é o
que importa para um canal de controle e estava invisível.

**Ressalva a declarar:** a amostra de RTT do C2 é de ~25–30 leituras por
repetição (cadência de 2 s em 60 s), então o p95 por repetição é grosseiro; a
mediana entre 5 repetições atenua, mas não elimina.

### Fig. 4 — `fig4-overhead-bytes`

**Afirma:** o que cada perfil acrescenta por mensagem, constante nas três
condições: DTLS +22 B, SRTP +16 B, TLS +22 B, OSCORE **+11 B**.

Origem: contadores de fio para C1/C3/C4; **pcap** para o C2, que não tem
contadores (`pcap_bytes.py --split-port 5002`): requisição 46 → 57 B, resposta
35 → 46 B.

**Não diz:** que o OSCORE é mais barato no total. Ele é o mais barato **por
mensagem**, com a contrapartida de exigir uma requisição de 46 B que os canais
unidirecionais não têm.

---

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
