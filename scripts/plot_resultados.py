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
               ymax=None, ymin=0, yticks_n=5, yfmt=lambda v: f"{v:g}"):
    x0, x1 = PAD_L, W - PAD_R
    y0, y1 = PAD_T + 22, H - PAD_B
    n = len(xlabels)
    xs = [x0 + (x1 - x0) * i / (n - 1) for i in range(n)]
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
    a = ap.parse_args()
    R, OUT = a.results, a.out
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
    dtls = [(a_ + b_) / 2 for a_, b_ in zip(c1, c3)]
    feitos.append(line_chart(
        f"{OUT}/fig1-estabelecimento-rtt.svg",
        "Estabelecimento cresce com o RTT, e a inclinação identifica o perfil",
        "Mediana de 5 repetições. O netem age nos dois sentidos, então RTT = 2 x atraso.",
        rtt,
        [("DTLS 1.3 (C1, C3) · 3 RTT", dtls),
         ("TLS 1.3/TCP (C4) · 2 RTT",
          [pick(atraso, c, "C4 bulk", "handshake_ms") for c in dconds]),
         ("OSCORE (C2) · 1 RTT",
          [pick(atraso, c, "C2 telemetry", "handshake_ms") for c in dconds])],
        "Estabelecimento (ms)", "RTT do enlace (ms)",
        # Escala redonda: com ymax automático os rótulos saíam "271.699".
        ymax=1400, yticks_n=7, yfmt=lambda v: f"{v:.0f}"))

    # Fig 2 — entrega vs perda
    lconds = ["loss-0", "loss-0.1", "loss-1", "loss-5", "loss-10", "loss-20"]
    llab = ["0", "0,1", "1", "5", "10", "20"]
    feitos.append(line_chart(
        f"{OUT}/fig2-entrega-perda.svg",
        "Só os perfis que retransmitem seguram a entrega",
        "Mediana de 5 repetições. C2 e C4 ficam em 100% em todos os pontos.",
        llab,
        [("DTLS 1.3 (C1)", [pick(perda, c, "C1 control", "entrega_pct", 1) for c in lconds]),
         ("DTLS-SRTP (C3)", [pick(perda, c, "C3 media", "entrega_pct", 1) for c in lconds]),
         ("OSCORE e TLS (C2, C4)", [100.0] * 6)],
        "Entrega (%)", "Perda no enlace (%), cada sentido",
        ymin=75, ymax=102, yticks_n=3, yfmt=lambda v: f"{v:.0f}"))

    # Fig 3 — vazão do C4
    vaz = [pick(perda, c, "C4 bulk", "mbps", 1) for c in lconds]
    feitos.append(log_chart(
        f"{OUT}/fig3-vazao-perda.svg",
        "A vazão do TLS sobre TCP cai quatro ordens de grandeza",
        "Canal C4, mediana de 5 repetições. Eixo logarítmico.",
        llab, vaz, "Vazão (Mbps, log)", "Perda no enlace (%), cada sentido",
        [(0, f"{vaz[0]:.0f} Mbps"), (2, f"{vaz[2]:.2f}"), (5, f"{vaz[5]:.3f} Mbps")]))

    # Fig 4 — custo da segurança em bytes
    feitos.append(bar_chart(
        f"{OUT}/fig4-custo-seguranca.svg",
        "O que cada perfil acrescenta a cada mensagem",
        "Diferença entre o canal seguro e seu baseline em claro. Constante nas três condições.",
        ["C1\nDTLS 1.3", "C2\nOSCORE", "C3\nSRTP", "C4\nTLS 1.3"],
        [22, 11, 16, 22], "Bytes acrescentados por mensagem",
        "C2 medido no pcap (requisição 46->57 B, resposta 35->46 B); os demais pelos contadores de fio."))

    for f in feitos:
        print(f)


if __name__ == "__main__":
    main()
