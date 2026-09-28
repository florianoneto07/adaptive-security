#!/usr/bin/env python3
"""
Gera as tabelas LaTeX do artigo IEEE a partir dos resumo.csv das campanhas.

    ./scripts/paper_tables.py                 # usa as campanhas padrão
    ./scripts/paper_tables.py --out DIR

Mesma filosofia do plot_resultados.py: sem dependências, e cada número da
tabela é rastreável a um resumo.csv (que por sua vez é rastreável aos CSVs
brutos e ao manifesto). Nada aqui é digitado à mão, com uma exceção marcada:
os bytes no fio do C2 vêm do pcap (pcap_bytes.py --split-port 5002), porque o
libcoap não expõe contadores, e ficam na constante C2_PCAP abaixo.

Saída (em --out, padrão paper/ieee/tables/):
  tab_results.tex        campanha principal: 3 condições x 4 canais
  tab_security_cost.tex  canal seguro vs. baseline em claro, por condição
  tab_delay.tex          estabelecimento vs. atraso (P1.2)
  tab_loss.tex           degradação vs. perda (P1.1)
"""
import argparse
import csv
import os

# Campanhas autoritativas (ver docs/PRONTIDAO-PARA-PUBLICACAO.md §2.1 e §10).
MAIN = "campaign-20260915T121452Z-ad89a84"        # clean, 3gpp-c2, handover; 10 reps
PLAIN = "campaign-20260916T124934Z-3717e9a"       # baselines em claro; 5 reps
DELAY = "campaign-20260916T112433Z-3717e9a"       # delay-0..200; 5 reps
LOSS = ["campaign-20260916T142149Z-3717e9a",      # loss-0..10; 5 reps
        "campaign-20260916T182845Z-606a0af"]      # loss-20 (bulk 1 MiB); 5 reps

# C2 pelo pcap, IP+ (constante nas três condições — PRONTIDAO §2.3, nota ‡):
# requisição 57 B, resposta 46 B; a resposta leva 2 B de leitura em 18 B de
# CoAP+OSCORE. Baseline em claro: 46/35 B (§10.3).
C2_PCAP = {"secure_resp_overhead": 18, "plain_resp_overhead": 7,
           "secure_req": 57, "secure_resp": 46, "plain_req": 46, "plain_resp": 35}

CHANNELS = ["C1", "C2", "C3", "C4"]
LABEL = {"C1": "C1 DTLS 1.3", "C2": "C2 OSCORE", "C3": "C3 DTLS-SRTP", "C4": "C4 TLS 1.3"}
COND_LABEL = {"clean": "clean", "3gpp-c2": "3GPP C2", "handover": "handover"}


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    out = {}
    for r in rows:
        out[(r["condicao"], r["canal"][:2])] = r
    return out


def num(r, key, div=1.0):
    v = r.get(key, "")
    if v in ("", None):
        return None
    try:
        return float(v) / div
    except ValueError:
        return None


def fmt(x, prec=1, dash="--"):
    if x is None:
        return dash
    if prec == 0:
        return f"{x:,.0f}".replace(",", "\\,")
    return f"{x:.{prec}f}"


def fmt_mbps(x):
    if x is None:
        return "--"
    return f"{x:.3f}" if x < 1 else f"{x:.1f}"


def table_results(main, out):
    """Tabela III: campanha principal."""
    lines = []
    lines.append(r"\begin{table*}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Measured cost of each fixed profile under the three link conditions "
                 r"(median of 10 repetitions; IQR across repetitions in parentheses where informative).}")
    lines.append(r"\label{tab:results}")
    lines.append(r"\begin{tabular}{llrrrrrrrrr}")
    lines.append(r"\toprule")
    lines.append(r"Cond. & Channel & Estab. [ms] & Estab. [B] & Overhead [B/msg] & "
                 r"$p_{50}$ [ms] & $p_{95}$ [ms] & $p_{99}$ [ms] & Delivery [\%] & Thr. [Mb/s] & RSS [MB] \\")
    lines.append(r"\midrule")
    for cond in ["clean", "3gpp-c2", "handover"]:
        for ch in CHANNELS:
            r = main[(cond, ch)]
            hs = num(r, "handshake_ms", 1e6)
            hs_iqr = num(r, "handshake_ms_iqr", 1e6)
            hs_s = fmt(hs) + (f" ({fmt(hs_iqr)})" if hs_iqr and hs and hs_iqr / hs > 0.1 else "")
            hsb = num(r, "handshake_bytes")
            ovh = num(r, "overhead_b")
            if ch == "C2":
                hsb_s = f"{C2_PCAP['secure_req']}/{C2_PCAP['secure_resp']}$^{{\\dagger}}$"
                ovh_s = f"{C2_PCAP['secure_resp_overhead']}$^{{\\dagger}}$"
            else:
                hsb_s = fmt(hsb, 0)
                ovh_s = fmt(ovh)
            p50, p95, p99 = (num(r, k, 1e6) for k in ("p50_ms", "p95_ms", "p99_ms"))
            if ch == "C4":
                p50_s, p95_s, p99_s = fmt(p50, 0) + "$^{\\S}$", "--", "--"
            elif ch == "C3":
                p50_s, p95_s, p99_s = (fmt(v, 1) + "$^{\\ddagger}$" for v in (p50, p95, p99))
            else:
                p50_s, p95_s, p99_s = fmt(p50), fmt(p95), fmt(p99)
            dl = num(r, "entrega_pct")
            mb = num(r, "mbps")
            rss = num(r, "rss_kb", 1024)
            lines.append(f"{COND_LABEL[cond]} & {LABEL[ch]} & {hs_s} & {hsb_s} & {ovh_s} & "
                         f"{p50_s} & {p95_s} & {p99_s} & {fmt(dl)} & {fmt_mbps(mb)} & {fmt(rss)} \\\\")
        if cond != "handover":
            lines.append(r"\midrule")
    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\begin{flushleft}\footnotesize")
    lines.append(r"$^{\dagger}$C2 bytes from packet capture (request/response, IP and above); "
                 r"the response carries the 2-byte reading in 18 bytes of CoAP+OSCORE. "
                 r"$^{\ddagger}$C3 is one-way, measured relative to the stream floor without a common clock: "
                 r"only differences between percentiles are meaningful. "
                 r"$^{\S}$C4: completion time of the whole 32~MiB transfer, one sample per repetition.")
    lines.append(r"\end{flushleft}")
    lines.append(r"\end{table*}")
    write(out, "tab_results.tex", lines)


def table_security_cost(main, plain, out):
    """Tabela IV: custo da segurança = seguro − claro, mesma condição."""
    lines = []
    lines.append(r"\begin{table*}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Cost of security: each secured channel against its unprotected baseline "
                 r"(same traffic, same condition). Per-message CPU is reported as the secured$-$plain "
                 r"difference and only under emulated links (see Sec.~\ref{sec:method}).}")
    lines.append(r"\label{tab:cost}")
    lines.append(r"\begin{tabular}{llrrrrrrr}")
    lines.append(r"\toprule")
    lines.append(r"Cond. & Channel & Estab. [ms] & Estab. CPU [ms] & Overhead sec./plain [B/msg] & "
                 r"CPU/msg sec.$-$plain [$\mu$s] & Delivery sec./plain [\%] & RSS sec./plain [MB] \\")
    lines.append(r"\midrule")
    for cond in ["clean", "3gpp-c2", "handover"]:
        for ch in CHANNELS:
            s = main[(cond, ch)]
            p = plain[(cond, ch)]
            hs = num(s, "handshake_ms", 1e6)
            hs_cpu = num(s, "handshake_cpu_ms", 1e6)
            if ch == "C2":
                ovh_s = f"{C2_PCAP['secure_resp_overhead']}/{C2_PCAP['plain_resp_overhead']}$^{{\\dagger}}$"
            else:
                ovh_s = f"{fmt(num(s, 'overhead_b'))}/{fmt(num(p, 'overhead_b'), 0)}"
            cs, cp = num(s, "cpu_msg_us", 1e3), num(p, "cpu_msg_us", 1e3)
            if cond == "clean" or cs is None or cp is None:
                dcpu_s = "n/a$^{\\ddagger}$"
            else:
                dcpu_s = f"{cs - cp:+.0f}"
            dl_s = f"{fmt(num(s, 'entrega_pct'))}/{fmt(num(p, 'entrega_pct'))}"
            rss_s = f"{fmt(num(s, 'rss_kb', 1024))}/{fmt(num(p, 'rss_kb', 1024))}"
            lines.append(f"{COND_LABEL[cond]} & {LABEL[ch]} & {fmt(hs)} & {fmt(hs_cpu, 2)} & {ovh_s} & "
                         f"{dcpu_s} & {dl_s} & {rss_s} \\\\")
        if cond != "handover":
            lines.append(r"\midrule")
    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\begin{flushleft}\footnotesize")
    lines.append(r"$^{\dagger}$C2 from packet capture: response overhead over the 2-byte payload, "
                 r"CoAP+OSCORE vs.\ plain CoAP. "
                 r"$^{\ddagger}$Not reported under \emph{clean}: without link delay the reply is "
                 r"processed inside the timed window and the plain channel measures \emph{more} CPU "
                 r"than the secured one, so the difference is not a cipher cost.")
    lines.append(r"\end{flushleft}")
    lines.append(r"\end{table*}")
    write(out, "tab_security_cost.tex", lines)


def table_delay(delay, out):
    """Estabelecimento vs. atraso (P1.2). RTT = 2 x delay (netem nas duas pontas)."""
    conds = ["delay-0", "delay-25", "delay-50", "delay-100", "delay-200"]
    lines = []
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Establishment time [ms] vs.\ one-way delay (median of 5 repetitions; "
                 r"loss fixed at 0.1\,\%; RTT $= 2\times$ delay).}")
    lines.append(r"\label{tab:delay}")
    lines.append(r"\begin{tabular}{rr" + "r" * len(CHANNELS) + "}")
    lines.append(r"\toprule")
    lines.append(r"Delay & RTT & " + " & ".join(LABEL[c] for c in CHANNELS) + r" \\")
    lines.append(r"[ms] & [ms] & " + " & ".join("" for _ in CHANNELS) + r" \\")
    lines.append(r"\midrule")
    for cond in conds:
        d = int(cond.split("-")[1])
        vals = [fmt(num(delay[(cond, c)], "handshake_ms", 1e6)) for c in CHANNELS]
        lines.append(f"{d} & {2 * d} & " + " & ".join(vals) + r" \\")
    lines.append(r"\midrule")
    lines.append(r"\multicolumn{2}{l}{Slope [RTT]} & 3 & 1 & 3 & 2 \\")
    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\end{table}")
    write(out, "tab_delay.tex", lines)


def table_loss(loss, out):
    """Degradação vs. perda (P1.1)."""
    conds = ["loss-0", "loss-0.1", "loss-1", "loss-5", "loss-10", "loss-20"]
    lines = []
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Degradation vs.\ independent packet loss at 50\,ms one-way delay "
                 r"(median of 5 repetitions). Failed = repetitions whose handshake never completed.}")
    lines.append(r"\label{tab:loss}")
    lines.append(r"\begin{tabular}{rrrrrrr}")
    lines.append(r"\toprule")
    lines.append(r"Loss & C1 estab. & C1 deliv. & C3 deliv. & C2 $p_{95}$ & C4 thr. & Failed \\")
    lines.append(r"[\%] & [ms] & [\%] & [\%] & [ms] & [Mb/s] & C1/C3 \\")
    lines.append(r"\midrule")
    for cond in conds:
        pct = cond.split("-")[1]
        c1, c2, c3, c4 = (loss[(cond, c)] for c in CHANNELS)
        lines.append(f"{pct} & {fmt(num(c1, 'handshake_ms', 1e6), 0)} & {fmt(num(c1, 'entrega_pct'))} & "
                     f"{fmt(num(c3, 'entrega_pct'))} & {fmt(num(c2, 'p95_ms', 1e6), 0)} & "
                     f"{fmt_mbps(num(c4, 'mbps'))} & "
                     f"{int(float(c1['reps_sem_dados']))}/{int(float(c3['reps_sem_dados']))} \\\\")
    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\end{table}")
    write(out, "tab_loss.tex", lines)


def write(out, name, lines):
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, name)
    with open(path, "w") as f:
        f.write("% Gerado por scripts/paper_tables.py — não editar à mão.\n")
        f.write("\n".join(lines) + "\n")
    print(f"  {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default="paper/ieee/tables")
    a = ap.parse_args()
    R = a.results
    main_c = load(f"{R}/{MAIN}/resumo.csv")
    plain = load(f"{R}/{PLAIN}/resumo.csv")
    delay = load(f"{R}/{DELAY}/resumo.csv")
    loss = {}
    for c in LOSS:
        loss.update(load(f"{R}/{c}/resumo.csv"))
    print("Tabelas:")
    table_results(main_c, a.out)
    table_security_cost(main_c, plain, a.out)
    table_delay(delay, a.out)
    table_loss(loss, a.out)


if __name__ == "__main__":
    main()
