#!/usr/bin/env python3
"""
Gera as figuras do artigo a partir dos resumo.csv das campanhas, em SVG.

    ./scripts/plot_resultados.py            # usa as campanhas padrão
    ./scripts/plot_resultados.py --out DIR

SVG e sem dependências de propósito: a VM não tem matplotlib nem pip, o SVG é
vetorial (o que um artigo precisa) e o script continua rodando em qualquer
clone do repositório, que é o requisito de reprodutibilidade.

Decisões de visualização, para quem for mexer:

- No máximo TRÊS séries por figura. Não é limitação de espaço: a paleta de
  referência valida todos os pares só até o terceiro slot — o quarto põe
  amarelo ao lado de laranja e reprova o piso de visão normal. Os dados
  cooperam: C1 e C3 têm estabelecimento idêntico (diferença <= 1,2 ms) e
  C2/C4 ficam ambos em 100% de entrega, então a fusão é honesta e não
  esconde nada.
- Cor nunca é o único canal: cada série tem marcador e traço próprios, para
  o artigo sobreviver a impressão em tons de cinza e a daltonismo.
- Eixo de perda é ORDINAL (espaçamento igual). Os pontos 0 e 0,1% não cabem
  num log, e num eixo linear os cinco primeiros colariam. Os rótulos trazem
  o valor real.
"""
import argparse
import csv
import os

# Tokens da paleta de referência (instância validada), modo claro. Figuras de
# artigo são impressas: só o modo claro é gerado.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e6e5e1"
AXIS = "#b8b7b2"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]   # slots 1, 2, 3 — ordem fixa
DASH = ["", "7 4", "2 3"]                    # codificação secundária
MARK = ["circle", "square", "triangle"]

FONT = ("-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, "
        "'Helvetica Neue', Arial, sans-serif")

W, H = 700, 400
PAD_L, PAD_R, PAD_T, PAD_B = 68, 200, 34, 54


# Idioma dos rótulos. O artigo IEEE é em inglês; o REPORT e a PRONTIDAO, em
# português. As figuras são as MESMAS, só os rótulos mudam — nunca regerar um
# conjunto com dados diferentes do outro.
L = {
    "pt": {
        "f1_t": "Estabelecimento cresce com o RTT, e a inclinação identifica o perfil",
        "f1_s": "Mediana de 5 repetições. O netem age nos dois sentidos, então RTT = 2 x atraso.",
        "f1_y": "Estabelecimento (ms)", "f1_x": "RTT do enlace (ms)",
        "f1_a": "DTLS 1.3 (C1, C3) · 3 RTT", "f1_b": "TLS 1.3/TCP (C4) · 2 RTT",
        "f1_c": "OSCORE (C2) · 1 RTT",
        "f2_t": "Só os perfis que retransmitem seguram a entrega",
        "f2_s": "Mediana de 5 repetições. C2 e C4 ficam em 100% em todos os pontos.",
        "f2_y": "Entrega (%)", "f2_x": "Perda no enlace (%), cada sentido",
        "f2_a": "DTLS 1.3 (C1)", "f2_b": "DTLS-SRTP (C3)", "f2_c": "OSCORE e TLS (C2, C4)",
        "f3_t": "Entrega de 100% se paga em tempo, não em perda",
        "f3_s": "Os dois perfis que retransmitem, sob a mesma varredura. Eixos logarítmicos.",
        "f3_pa": "(a) OSCORE sobre CoAP (C2): o atraso típico não muda; a cauda explode",
        "f3_pb": "(b) TLS 1.3 sobre TCP (C4): a vazão cai quatro ordens de grandeza",
        "f3_ya": "Atraso (ms, log)", "f3_yb": "Vazão (Mbps, log)",
        "f3_x": "Perda no enlace (%), cada sentido",
        "f3_a": "p95 (cauda)", "f3_b": "p50 (típico)",
        "f4_t": "O que cada perfil acrescenta a cada mensagem",
        "f4_s": "Diferença entre o canal seguro e seu baseline em claro. Constante nas três condições.",
        "f4_y": "Bytes acrescentados por mensagem",
        "f4_n": "C2 medido no pcap (requisição 46->57 B, resposta 35->46 B); os demais pelos contadores de fio.",
        "f4_l": ["C1\nDTLS 1.3", "C2\nOSCORE", "C3\nSRTP", "C4\nTLS 1.3"],
        "dec": ",",
    },
    "en": {
        "f1_t": "Establishment grows with RTT, and the slope identifies the profile",
        "f1_s": "Median of 5 repetitions. netem runs at both ends, so RTT = 2 x delay.",
        "f1_y": "Establishment (ms)", "f1_x": "Link RTT (ms)",
        "f1_a": "DTLS 1.3 (C1, C3) · 3 RTT", "f1_b": "TLS 1.3/TCP (C4) · 2 RTT",
        "f1_c": "OSCORE (C2) · 1 RTT",
        "f2_t": "Only the profiles that retransmit hold delivery up",
        "f2_s": "Median of 5 repetitions. C2 and C4 stay at 100% at every point.",
        "f2_y": "Delivery (%)", "f2_x": "Link loss (%), each direction",
        "f2_a": "DTLS 1.3 (C1)", "f2_b": "DTLS-SRTP (C3)", "f2_c": "OSCORE and TLS (C2, C4)",
        "f3_t": "100% delivery is paid for in time, not in loss",
        "f3_s": "The two retransmitting profiles, same sweep. Logarithmic axes.",
        "f3_pa": "(a) OSCORE over CoAP (C2): typical delay is flat; the tail explodes",
        "f3_pb": "(b) TLS 1.3 over TCP (C4): throughput falls four orders of magnitude",
        "f3_ya": "Delay (ms, log)", "f3_yb": "Throughput (Mb/s, log)",
        "f3_x": "Link loss (%), each direction",
        "f3_a": "p95 (tail)", "f3_b": "p50 (typical)",
        "f4_t": "What each profile adds to every message",
        "f4_s": "Secured channel minus its unprotected baseline. Constant across the three conditions.",
        "f4_y": "Bytes added per message",
        "f4_n": "C2 from packet capture (request 46->57 B, response 35->46 B); the others from wire counters.",
        "f4_l": ["C1\nDTLS 1.3", "C2\nOSCORE", "C3\nSRTP", "C4\nTLS 1.3"],
        "dec": ".",
    },
}


def dec(txt, lang):
    """Separador decimal do idioma. SVG não tem locale; o número é texto."""
    return txt.replace(".", L[lang]["dec"])


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def marker(kind, x, y, color, r=4.5):
    """Marcador com anel da superfície, para não sumir quando duas séries cruzam."""
    if kind == "circle":
        return (f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{color}" '
                f'stroke="{SURFACE}" stroke-width="2"/>')
    if kind == "square":
        s = r * 1.8
        return (f'<rect x="{x-s/2:.1f}" y="{y-s/2:.1f}" width="{s:.1f}" height="{s:.1f}" '
                f'fill="{color}" stroke="{SURFACE}" stroke-width="2"/>')
    s = r * 1.15
    pts = f"{x:.1f},{y-s*1.2:.1f} {x-s:.1f},{y+s*0.8:.1f} {x+s:.1f},{y+s*0.8:.1f}"
    return (f'<polygon points="{pts}" fill="{color}" stroke="{SURFACE}" '
            f'stroke-width="2"/>')


def header(title, subtitle):
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" font-family="{FONT}">',
        f'<rect width="{W}" height="{H}" fill="{SURFACE}"/>',
        f'<text x="{PAD_L-44}" y="20" font-size="14" font-weight="600" '
        f'fill="{INK}">{esc(title)}</text>',
        f'<text x="{PAD_L-44}" y="36" font-size="11.5" fill="{INK2}">'
        f'{esc(subtitle)}</text>',
    ]


def axes(parts, x0, x1, y0, y1, xticks, yticks, xlab, ylab, ylabels=None):
    """Grade recessiva, eixos discretos, rótulos em tinta (nunca cor de série)."""
    for i, (yv, ypix) in enumerate(yticks):
        parts.append(f'<line x1="{x0}" y1="{ypix:.1f}" x2="{x1}" y2="{ypix:.1f}" '
                     f'stroke="{GRID}" stroke-width="1"/>')
        lab = ylabels[i] if ylabels else f"{yv:g}"
        parts.append(f'<text x="{x0-9}" y="{ypix+4:.1f}" font-size="11" '
                     f'text-anchor="end" fill="{INK2}">{esc(lab)}</text>')
    parts.append(f'<line x1="{x0}" y1="{y0}" x2="{x0}" y2="{y1}" '
                 f'stroke="{AXIS}" stroke-width="1"/>')
    parts.append(f'<line x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}" '
                 f'stroke="{AXIS}" stroke-width="1"/>')
    for lab, xpix in xticks:
        parts.append(f'<text x="{xpix:.1f}" y="{y1+18}" font-size="11" '
                     f'text-anchor="middle" fill="{INK2}">{esc(lab)}</text>')
    parts.append(f'<text x="{(x0+x1)/2:.1f}" y="{y1+40}" font-size="11.5" '
                 f'text-anchor="middle" fill="{INK2}">{esc(xlab)}</text>')
    parts.append(f'<text x="16" y="{(y0+y1)/2:.1f}" font-size="11.5" fill="{INK2}" '
                 f'text-anchor="middle" transform="rotate(-90 16 {(y0+y1)/2:.1f})">'
                 f'{esc(ylab)}</text>')


def largura_aprox(txt, px=11):
    """Largura aproximada de um texto: 0,52 em por caractere na fonte usada."""
    return len(txt) * px * 0.52


def legend(parts, names, x, y):
    """Legenda sempre presente com 2+ séries; identidade nunca só pela cor."""
    for n in names:
        fim = x + 30 + largura_aprox(n)
        if fim > W - 6:
            raise SystemExit(
                f"Legenda '{n}' termina em {fim:.0f}px e a figura tem {W}px. "
                "Encurte o rótulo ou aumente PAD_R — o SVG não reflui texto, "
                "o excedente sairia da figura sem aviso.")
    for i, n in enumerate(names):
        yy = y + i * 19
        parts.append(f'<line x1="{x}" y1="{yy}" x2="{x+22}" y2="{yy}" '
                     f'stroke="{SERIES[i]}" stroke-width="2"'
                     + (f' stroke-dasharray="{DASH[i]}"' if DASH[i] else "") + '/>')
        parts.append(marker(MARK[i], x + 11, yy, SERIES[i]))
        parts.append(f'<text x="{x+30}" y="{yy+4}" font-size="11" fill="{INK}">'
                     f'{esc(n)}</text>')


def line_chart(path, title, subtitle, xlabels, series, ylab, xlab,
               ymax=None, ymin=0, yticks_n=5, yfmt=lambda v: f"{v:g}",
               xvals=None):
    """
    xvals=None deixa o eixo x ORDINAL (espaçamento igual entre rótulos), que é
    o certo quando os pontos não são uma escala — a varredura de perda tem 0 e
    0,1%, que num eixo linear colariam e num log não existiriam.

    Com xvals, o eixo é LINEAR no valor. É obrigatório quando a figura afirma
    algo sobre a INCLINAÇÃO: no eixo ordinal, uma relação exatamente linear
    (estabelecimento = k x RTT) aparece encurvada, e a figura passa a sugerir
    um crescimento super-linear que os dados não têm.
    """
    x0, x1 = PAD_L, W - PAD_R
    y0, y1 = PAD_T + 22, H - PAD_B
    n = len(xlabels)
    if xvals is None:
        xs = [x0 + (x1 - x0) * i / (n - 1) for i in range(n)]
    else:
        lo, hi = min(xvals), max(xvals)
        xs = [x0 + (x1 - x0) * (v - lo) / (hi - lo) for v in xvals]
    allv = [v for _, vals in series for v in vals if v is not None]
    ymax = ymax if ymax is not None else max(allv) * 1.12
    ypix = lambda v: y1 - (v - ymin) / (ymax - ymin) * (y1 - y0)

    parts = header(title, subtitle)
    yt = [(ymin + (ymax - ymin) * i / yticks_n, ypix(ymin + (ymax - ymin) * i / yticks_n))
          for i in range(yticks_n + 1)]
    axes(parts, x0, x1, y0, y1, list(zip(xlabels, xs)), yt, xlab, ylab,
         ylabels=[yfmt(v) for v, _ in yt])

    for si, (name, vals) in enumerate(series):
        pts = [(xs[i], ypix(v)) for i, v in enumerate(vals) if v is not None]
        d = " ".join(("M" if k == 0 else "L") + f"{px:.1f} {py:.1f}"
                     for k, (px, py) in enumerate(pts))
        parts.append(f'<path d="{d}" fill="none" stroke="{SERIES[si]}" '
                     f'stroke-width="2" stroke-linejoin="round"'
                     + (f' stroke-dasharray="{DASH[si]}"' if DASH[si] else "") + '/>')
        for px, py in pts:
            parts.append(marker(MARK[si], px, py, SERIES[si]))
    legend(parts, [n for n, _ in series], x1 + 18, y0 + 6)
    parts.append("</svg>")
    open(path, "w").write("\n".join(parts))
    return path


def log_chart(path, title, subtitle, xlabels, vals, ylab, xlab, annot):
    """Uma série só: sem legenda (o título a nomeia). Log porque são 4 ordens."""
    import math
    x0, x1 = PAD_L, W - PAD_R + 70
    y0, y1 = PAD_T + 22, H - PAD_B
    n = len(xlabels)
    xs = [x0 + (x1 - x0) * i / (n - 1) for i in range(n)]
    decades = [0.01, 0.1, 1, 10, 100]
    lo, hi = math.log10(decades[0]), math.log10(decades[-1])
    ypix = lambda v: y1 - (math.log10(v) - lo) / (hi - lo) * (y1 - y0)

    parts = header(title, subtitle)
    yt = [(d, ypix(d)) for d in decades]
    axes(parts, x0, x1, y0, y1, list(zip(xlabels, xs)), yt, xlab, ylab,
         ylabels=["0,01", "0,1", "1", "10", "100"])
    pts = [(xs[i], ypix(v)) for i, v in enumerate(vals)]
    d = " ".join(("M" if k == 0 else "L") + f"{px:.1f} {py:.1f}"
                 for k, (px, py) in enumerate(pts))
    parts.append(f'<path d="{d}" fill="none" stroke="{SERIES[0]}" stroke-width="2"/>')
    for px, py in pts:
        parts.append(marker("circle", px, py, SERIES[0]))
    # Rótulos diretos seletivos: extremos e o joelho, não um número por ponto.
    for i, txt in annot:
        parts.append(f'<text x="{xs[i]:.1f}" y="{ypix(vals[i])-12:.1f}" font-size="11" '
                     f'text-anchor="middle" font-weight="600" fill="{INK}">{esc(txt)}</text>')
    parts.append("</svg>")
    open(path, "w").write("\n".join(parts))
    return path


def bar_chart(path, title, subtitle, labels, vals, ylab, note):
    """Magnitude por categoria, uma medida: barras de um hue só, sem legenda."""
    x0, x1 = PAD_L, W - PAD_R + 110
    y0, y1 = PAD_T + 22, H - PAD_B
    ymax = max(vals) * 1.25
    ypix = lambda v: y1 - v / ymax * (y1 - y0)
    parts = header(title, subtitle)
    yt = [(ymax * i / 4, ypix(ymax * i / 4)) for i in range(5)]
    axes(parts, x0, x1, y0, y1, [], yt, "", ylab,
         ylabels=[f"{v:.0f}" for v, _ in yt])
    n = len(vals)
    slot = (x1 - x0) / n
    bw = min(64, slot * 0.5)
    for i, v in enumerate(vals):
        cx = x0 + slot * (i + 0.5)
        top = ypix(v)
        # Topo arredondado em 4px, ancorado na linha de base.
        r = 4
        parts.append(f'<path d="M{cx-bw/2:.1f} {y1:.1f} L{cx-bw/2:.1f} {top+r:.1f} '
                     f'Q{cx-bw/2:.1f} {top:.1f} {cx-bw/2+r:.1f} {top:.1f} '
                     f'L{cx+bw/2-r:.1f} {top:.1f} Q{cx+bw/2:.1f} {top:.1f} '
                     f'{cx+bw/2:.1f} {top+r:.1f} L{cx+bw/2:.1f} {y1:.1f} Z" '
                     f'fill="{SERIES[0]}"/>')
        parts.append(f'<text x="{cx:.1f}" y="{top-8:.1f}" font-size="12" '
                     f'text-anchor="middle" font-weight="600" fill="{INK}">+{v:g} B</text>')
        # SVG não quebra linha sozinho: cada linha do rótulo vira um tspan.
        linhas = labels[i].split("\n")
        tspans = "".join(
            f'<tspan x="{cx:.1f}" dy="{0 if k == 0 else 14}">{esc(t)}</tspan>'
            for k, t in enumerate(linhas))
        parts.append(f'<text y="{y1+18:.1f}" font-size="11" '
                     f'text-anchor="middle" fill="{INK2}">{tspans}</text>')
    parts.append(f'<text x="{x0}" y="{H-12}" font-size="10.5" fill="{INK2}">'
                 f'{esc(note)}</text>')
    parts.append("</svg>")
    open(path, "w").write("\n".join(parts))
    return path


def panels_log_chart(path, title, subtitle, xlabels, panels, xlab, lang):
    """
    Dois paineis logaritmicos empilhados, mesmo eixo x.

    Empilhados e nao lado a lado porque a figura vai numa coluna do IEEE: dois
    paineis lado a lado a 700px dariam 350px cada e os rotulos do eixo x
    colidiriam. O eixo x aparece uma vez, embaixo, porque e o mesmo nos dois —
    repeti-lo gastaria altura sem informar nada.
    """
    import math
    x0 = PAD_L
    x1 = W - PAD_R + 40
    ptop, pheight, pgap = PAD_T + 44, 196, 62
    parts = header(title, subtitle)

    for pi, pan in enumerate(panels):
        y0 = ptop + pi * (pheight + pgap)
        y1 = y0 + pheight
        decades = pan["decades"]
        lo, hi = math.log10(decades[0]), math.log10(decades[-1])
        ypix = lambda v: y1 - (math.log10(v) - lo) / (hi - lo) * (y1 - y0)

        parts.append(f'<text x="{x0-44}" y="{y0-12}" font-size="11.5" '
                     f'font-weight="600" fill="{INK}">{esc(pan["caption"])}</text>')
        yt = [(d, ypix(d)) for d in decades]
        ultimo = pi == len(panels) - 1
        axes(parts, x0, x1, y0, y1,
             list(zip(xlabels, [x0 + (x1 - x0) * i / (len(xlabels) - 1)
                                for i in range(len(xlabels))])),
             yt, xlab if ultimo else "", pan["ylab"],
             ylabels=[dec(f"{d:g}", lang) for d in decades])

        xs = [x0 + (x1 - x0) * i / (len(xlabels) - 1) for i in range(len(xlabels))]
        for si, (name, vals) in enumerate(pan["series"]):
            pts = [(xs[i], ypix(v)) for i, v in enumerate(vals) if v]
            d = " ".join(("M" if k == 0 else "L") + f"{px:.1f} {py:.1f}"
                         for k, (px, py) in enumerate(pts))
            parts.append(f'<path d="{d}" fill="none" stroke="{SERIES[si]}" '
                         f'stroke-width="2" stroke-linejoin="round"'
                         + (f' stroke-dasharray="{DASH[si]}"' if DASH[si] else "") + '/>')
            for px, py in pts:
                parts.append(marker(MARK[si], px, py, SERIES[si]))
        if len(pan["series"]) > 1:
            legend(parts, [n for n, _ in pan["series"]], x1 + 18, y0 + 6)
        # Rotulo direto nos extremos: o leitor nao deve interpolar num eixo log.
        for i, txt in pan.get("annot", []):
            v = pan["series"][0][1][i]
            # Âncora pela borda: no meio, o rótulo do último ponto invade a
            # legenda e o do primeiro cobre os rótulos do eixo y.
            anchor = "start" if i == 0 else ("end" if i == len(xs) - 1 else "middle")
            dx = 6 if i == 0 else (-6 if i == len(xs) - 1 else 0)
            parts.append(f'<text x="{xs[i]+dx:.1f}" y="{ypix(v)-11:.1f}" font-size="11" '
                         f'text-anchor="{anchor}" font-weight="600" fill="{INK}">'
                         f'{esc(txt)}</text>')

    alt = ptop + len(panels) * pheight + (len(panels) - 1) * pgap + 56
    parts[0] = parts[0].replace(f'height="{H}"', f'height="{alt}"').replace(
        f'viewBox="0 0 {W} {H}"', f'viewBox="0 0 {W} {alt}"')
    parts[1] = f'<rect width="{W}" height="{alt}" fill="{SURFACE}"/>'
    parts.append("</svg>")
    open(path, "w").write("\n".join(parts))
    return path


# --------------------------------------------------------------------------
# Leitura dos resumos
# --------------------------------------------------------------------------

def load(path):
    if not os.path.exists(path):
        return []
    return list(csv.DictReader(open(path)))


def pick(rows, cond, canal, campo, div=1e6):
    for r in rows:
        if r["condicao"] == cond and r["canal"] == canal:
            try:
                return float(r[campo]) / div
            except (ValueError, KeyError, TypeError):
                return None
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default="docs/figuras")
    ap.add_argument("--lang", default="pt", choices=["pt", "en"],
                    help="idioma dos rótulos (o artigo IEEE usa en)")
    a = ap.parse_args()
    R, OUT, lang = a.results, a.out, a.lang
    t = L[lang]
    os.makedirs(OUT, exist_ok=True)

    atraso = load(f"{R}/campaign-20260916T112433Z-3717e9a/resumo.csv")
    perda = (load(f"{R}/campaign-20260916T142149Z-3717e9a/resumo.csv")
             + load(f"{R}/campaign-20260916T182845Z-606a0af/resumo.csv"))

    feitos = []

    # Fig 1 — estabelecimento = k x RTT
    dconds = ["delay-0", "delay-25", "delay-50", "delay-100", "delay-200"]
    rtt = ["0", "50", "100", "200", "400"]
    c1 = [pick(atraso, c, "C1 control", "handshake_ms") for c in dconds]
    c3 = [pick(atraso, c, "C3 media", "handshake_ms") for c in dconds]
    # C1 e C3 usam o MESMO handshake DTLS 1.3 e ficam a <= 1,2 ms um do outro:
    # duas linhas sobrepostas não informariam nada e gastariam um slot da paleta.
    dtls = [(a_ + b_) / 2 for a_, b_ in zip(c1, c3)]
    feitos.append(line_chart(
        f"{OUT}/fig1-establishment-rtt.svg", t["f1_t"], t["f1_s"], rtt,
        [(t["f1_a"], dtls),
         (t["f1_b"], [pick(atraso, c, "C4 bulk", "handshake_ms") for c in dconds]),
         (t["f1_c"], [pick(atraso, c, "C2 telemetry", "handshake_ms") for c in dconds])],
        t["f1_y"], t["f1_x"],
        # Escala redonda: com ymax automático os rótulos saíam "271.699".
        ymax=1400, yticks_n=7, yfmt=lambda v: f"{v:.0f}",
        xvals=[0, 50, 100, 200, 400]))

    # Fig 2 — entrega vs perda
    lconds = ["loss-0", "loss-0.1", "loss-1", "loss-5", "loss-10", "loss-20"]
    llab = [dec(x, lang) for x in ["0", "0.1", "1", "5", "10", "20"]]
    feitos.append(line_chart(
        f"{OUT}/fig2-delivery-loss.svg", t["f2_t"], t["f2_s"], llab,
        [(t["f2_a"], [pick(perda, c, "C1 control", "entrega_pct", 1) for c in lconds]),
         (t["f2_b"], [pick(perda, c, "C3 media", "entrega_pct", 1) for c in lconds]),
         (t["f2_c"], [100.0] * 6)],
        t["f2_y"], t["f2_x"],
        ymin=75, ymax=102, yticks_n=3, yfmt=lambda v: f"{v:.0f}"))

    # Fig 3 — o preço da entrega de 100%: cauda do C2 e vazão do C4
    p50 = [pick(perda, c, "C2 telemetry", "p50_ms") for c in lconds]
    p95 = [pick(perda, c, "C2 telemetry", "p95_ms") for c in lconds]
    vaz = [pick(perda, c, "C4 bulk", "mbps", 1) for c in lconds]
    feitos.append(panels_log_chart(
        f"{OUT}/fig3-price-of-delivery.svg", t["f3_t"], t["f3_s"], llab,
        [{"caption": t["f3_pa"], "ylab": t["f3_ya"],
          "decades": [10, 100, 1000, 10000, 100000],
          "series": [(t["f3_a"], p95), (t["f3_b"], p50)],
          "annot": [(0, dec(f"{p95[0]:.0f} ms", lang)),
                    (5, dec(f"{p95[5]/1000:.1f} s", lang))]},
         {"caption": t["f3_pb"], "ylab": t["f3_yb"],
          "decades": [0.01, 0.1, 1, 10, 100],
          "series": [("", vaz)],
          "annot": [(0, dec(f"{vaz[0]:.0f}", lang)), (2, dec(f"{vaz[2]:.2f}", lang)),
                    (5, dec(f"{vaz[5]:.3f}", lang))]}],
        t["f3_x"], lang))

    # Fig 4 — custo da segurança em bytes
    feitos.append(bar_chart(
        f"{OUT}/fig4-overhead-bytes.svg", t["f4_t"], t["f4_s"],
        t["f4_l"], [22, 11, 16, 22], t["f4_y"], t["f4_n"]))

    for f in feitos:
        print(f)


if __name__ == "__main__":
    main()
