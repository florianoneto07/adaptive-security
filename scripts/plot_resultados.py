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
        "ch": ["C1\nDTLS 1.3", "C2\nOSCORE", "C3\nDTLS-SRTP", "C4\nTLS 1.3"],
        "cond": ["enlace limpo", "3GPP C2", "handover"],
        "f1_t": "Tempo de estabelecimento da sessão, por perfil e condição",
        "f1_s": "Mediana de 10 repetições. Eixo logarítmico: as condições separam os valores por três ordens de grandeza.",
        "f1_y": "Estabelecimento (ms, log)",
        "f1_n": "3GPP C2: 50 ms, 0,1% de perda. handover: 200±20 ms, 20% de perda em rajada. netem nas duas pontas.",
        "f2_t": "CPU do estabelecimento: depende do perfil, não do enlace",
        "f2_s": "Mediana de 10 repetições, tempo de CPU do processo cliente (CLOCK_PROCESS_CPUTIME_ID).",
        "f2_y": "CPU do estabelecimento (ms)",
        "f2_n": "Mesmas condições da figura anterior. AES e P-256 em software (sem AES-NI): ver a tabela de plataforma.",
        "f3_t": "Memória residente de pico: o custo de cada pilha de segurança",
        "f3_s": "Cada canal contra seu baseline em claro, mesmo tráfego. Varia menos de 0,07 MB entre as três condições.",
        "f3_y": "RSS de pico (MB)",
        "f3_n": "C1/C4 acrescentam o wolfSSL; C3 acrescenta wolfSSL e libsrtp2; no C2 o libcoap já está nos dois lados.",
        "f3_a": "canal seguro", "f3_b": "sem segurança",
        "f4_t": "Só os perfis que retransmitem seguram a entrega",
        "f4_s": "Mediana de 5 repetições, 50 ms de atraso fixo. C2 e C4 ficam em 100% em todos os pontos.",
        "f4_y": "Entrega (%)", "f4_x": "Perda no enlace (%), cada sentido",
        "f4_a": "DTLS 1.3 (C1)", "f4_b": "DTLS-SRTP (C3)", "f4_c": "OSCORE e TLS (C2, C4)",
        "f5_t": "Estabelecimento cresce com o RTT, e a inclinação identifica o perfil",
        "f5_s": "Mediana de 5 repetições. O netem age nos dois sentidos, então RTT = 2 x atraso.",
        "f5_y": "Estabelecimento (ms)", "f5_x": "RTT do enlace (ms)",
        "f5_a": "DTLS 1.3 (C1, C3) · 3 RTT", "f5_b": "TLS 1.3/TCP (C4) · 2 RTT",
        "f5_c": "OSCORE (C2) · 1 RTT",
        "dec": ",",
    },
    "en": {
        "ch": ["C1\nDTLS 1.3", "C2\nOSCORE", "C3\nDTLS-SRTP", "C4\nTLS 1.3"],
        "cond": ["clean link", "3GPP C2", "handover"],
        "f1_t": "Session establishment time, per profile and link condition",
        "f1_s": "Median of 10 repetitions. Logarithmic axis: the conditions span three orders of magnitude.",
        "f1_y": "Establishment (ms, log)",
        "f1_n": "3GPP C2: 50 ms, 0.1% loss. handover: 200±20 ms, 20% bursty loss. netem applied at both ends.",
        "f2_t": "Establishment CPU depends on the profile, not on the link",
        "f2_s": "Median of 10 repetitions, client-process CPU time (CLOCK_PROCESS_CPUTIME_ID).",
        "f2_y": "Establishment CPU (ms)",
        "f2_n": "Same conditions as the previous figure. AES and P-256 in software (no AES-NI): see the platform table.",
        "f3_t": "Peak resident memory: the cost of each security stack",
        "f3_s": "Each channel against its unprotected baseline, same traffic. Varies by less than 0.07 MB across the three conditions.",
        "f3_y": "Peak RSS (MB)",
        "f3_n": "C1/C4 add wolfSSL; C3 adds wolfSSL and libsrtp2; in C2 libcoap is present on both sides.",
        "f3_a": "secured channel", "f3_b": "no security",
        "f4_t": "Only the profiles that retransmit hold delivery up",
        "f4_s": "Median of 5 repetitions, 50 ms fixed delay. C2 and C4 stay at 100% at every point.",
        "f4_y": "Delivery (%)", "f4_x": "Link loss (%), each direction",
        "f4_a": "DTLS 1.3 (C1)", "f4_b": "DTLS-SRTP (C3)", "f4_c": "OSCORE and TLS (C2, C4)",
        "f5_t": "Establishment grows with RTT, and the slope identifies the profile",
        "f5_s": "Median of 5 repetitions. netem runs at both ends, so RTT = 2 x delay.",
        "f5_y": "Establishment (ms)", "f5_x": "Link RTT (ms)",
        "f5_a": "DTLS 1.3 (C1, C3) · 3 RTT", "f5_b": "TLS 1.3/TCP (C4) · 2 RTT",
        "f5_c": "OSCORE (C2) · 1 RTT",
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


def legend(parts, names, x, y, style="line"):
    """
    Legenda sempre presente com 2+ séries; identidade nunca só pela cor.

    style="bar" desenha um quadrado preenchido: numa figura de barras, um
    símbolo de linha tracejada com marcador não corresponde a nada no gráfico.
    """
    for n in names:
        fim = x + 30 + largura_aprox(n)
        if fim > W - 6:
            raise SystemExit(
                f"Legenda '{n}' termina em {fim:.0f}px e a figura tem {W}px. "
                "Encurte o rótulo ou aumente PAD_R — o SVG não reflui texto, "
                "o excedente sairia da figura sem aviso.")
    for i, n in enumerate(names):
        yy = y + i * 19
        if style == "bar":
            parts.append(f'<rect x="{x}" y="{yy-6}" width="20" height="12" rx="2" '
                         f'fill="{SERIES[i]}"/>')
        else:
            parts.append(f'<line x1="{x}" y1="{yy}" x2="{x+22}" y2="{yy}" '
                         f'stroke="{SERIES[i]}" stroke-width="2"'
                         + (f' stroke-dasharray="{DASH[i]}"' if DASH[i] else "") + '/>')
            parts.append(marker(MARK[i], x + 11, yy, SERIES[i]))
        parts.append(f'<text x="{x+30}" y="{yy+4}" font-size="11" fill="{INK}">'
                     f'{esc(n)}</text>')


def grouped_bar_chart(path, title, subtitle, groups, series, ylab, note, lang,
                      decades=None, ymax=None, vfmt=lambda v: f"{v:g}"):
    """
    Barras agrupadas: um grupo por canal, uma barra por condição (ou por
    variante segura/em claro). É a forma da figura de experimento — compara os
    QUATRO perfis sob os MESMOS tratamentos, lado a lado.

    `decades` liga o eixo logaritmico. Barra em eixo log distorce area, entao
    todo valor recebe rotulo direto: o numero fica legivel mesmo quando a
    altura nao e proporcional.

    A identidade da serie nao depende so da cor: a ordem das barras dentro do
    grupo e a mesma da legenda (posicao), e o rotulo traz o valor.
    """
    import math
    x0, x1 = PAD_L, W - PAD_R + 24
    y0, y1 = PAD_T + 26, H - PAD_B - 10
    k = len(series)
    ng = len(groups)

    if decades:
        lo, hi = math.log10(decades[0]), math.log10(decades[-1])
        ypix = lambda v: y1 - (math.log10(max(v, decades[0])) - lo) / (hi - lo) * (y1 - y0)
        yt = [(d, ypix(d)) for d in decades]
        ylabels = [dec(f"{d:g}", lang) for d in decades]
    else:
        escala = ymax if ymax else max(v for _, vs in series for v in vs) * 1.22
        # Nome próprio, e não 'top': a lambda fecha sobre a variável por
        # referência, então reusar o nome dentro do laço das barras achata
        # tudo o que vem depois da primeira.
        ypix = lambda v: y1 - v / escala * (y1 - y0)
        yt = [(escala * i / 4, ypix(escala * i / 4)) for i in range(5)]
        ylabels = [dec(f"{v:.1f}", lang) for v, _ in yt]

    parts = header(title, subtitle)
    axes(parts, x0, x1, y0, y1, [], yt, "", ylab, ylabels=ylabels)

    gw = (x1 - x0) / ng
    bw = min(26.0, gw * 0.72 / k)
    for gi, g in enumerate(groups):
        cx = x0 + gw * (gi + 0.5)
        for si, (_, vals) in enumerate(series):
            v = vals[gi]
            if v is None:
                continue
            bx = cx + (si - (k - 1) / 2) * (bw + 3) - bw / 2
            ytop = ypix(v)
            r = 3
            parts.append(f'<path d="M{bx:.1f} {y1:.1f} L{bx:.1f} {ytop+r:.1f} '
                         f'Q{bx:.1f} {ytop:.1f} {bx+r:.1f} {ytop:.1f} '
                         f'L{bx+bw-r:.1f} {ytop:.1f} Q{bx+bw:.1f} {ytop:.1f} '
                         f'{bx+bw:.1f} {ytop+r:.1f} L{bx+bw:.1f} {y1:.1f} Z" '
                         f'fill="{SERIES[si]}"/>')
            parts.append(f'<text x="{bx+bw/2:.1f}" y="{ytop-5:.1f}" font-size="9.5" '
                         f'text-anchor="middle" fill="{INK2}">'
                         f'{esc(dec(vfmt(v), lang))}</text>')
        # SVG nao quebra linha sozinho: cada linha do rotulo vira um tspan.
        tspans = "".join(
            f'<tspan x="{cx:.1f}" dy="{0 if j == 0 else 13}">{esc(t)}</tspan>'
            for j, t in enumerate(g.split("\n")))
        parts.append(f'<text y="{y1+18:.1f}" font-size="11" text-anchor="middle" '
                     f'fill="{INK2}">{tspans}</text>')

    legend(parts, [n for n, _ in series], x1 + 18, y0 + 6, style="bar")
    if note:
        parts.append(f'<text x="{x0-44}" y="{H-10}" font-size="10" fill="{INK2}">'
                     f'{esc(note)}</text>')
    parts.append("</svg>")
    open(path, "w").write("\n".join(parts))
    return path


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

    # Campanha principal: as três condições, 10 repetições. É a base das
    # figuras de experimento (latência, CPU, memória), porque só ela tem as
    # três condições sob o mesmo commit.
    main_c = load(f"{R}/campaign-20260915T121452Z-ad89a84/resumo.csv")
    claro = load(f"{R}/campaign-20260916T124934Z-3717e9a/resumo.csv")
    atraso = load(f"{R}/campaign-20260916T112433Z-3717e9a/resumo.csv")
    perda = (load(f"{R}/campaign-20260916T142149Z-3717e9a/resumo.csv")
             + load(f"{R}/campaign-20260916T182845Z-606a0af/resumo.csv"))

    CH = ["C1 control", "C2 telemetry", "C3 media", "C4 bulk"]
    CH_PLAIN = ["C1 claro", "C2 claro", "C3 claro", "C4 claro"]
    CONDS = ["clean", "3gpp-c2", "handover"]
    feitos = []

    # Fig 1 — latência de estabelecimento: 4 perfis x 3 condições.
    # Log porque vai de 1,1 ms a 4,2 s; cada barra leva o valor escrito.
    feitos.append(grouped_bar_chart(
        f"{OUT}/fig1-establishment-latency.svg", t["f1_t"], t["f1_s"], t["ch"],
        [(t["cond"][i], [pick(main_c, c, ch, "handshake_ms") for ch in CH])
         for i, c in enumerate(CONDS)],
        t["f1_y"], t["f1_n"], lang,
        decades=[1, 10, 100, 1000, 10000],
        vfmt=lambda v: f"{v:.0f}" if v >= 10 else f"{v:.1f}"))

    # Fig 2 — CPU do estabelecimento, mesmos eixos da Fig 1. O par conta a
    # história: o enlace custa TEMPO, não CPU.
    feitos.append(grouped_bar_chart(
        f"{OUT}/fig2-establishment-cpu.svg", t["f2_t"], t["f2_s"], t["ch"],
        [(t["cond"][i], [pick(main_c, c, ch, "handshake_cpu_ms") for ch in CH])
         for i, c in enumerate(CONDS)],
        t["f2_y"], t["f2_n"], lang, ymax=4.8, vfmt=lambda v: f"{v:.2f}"))

    # Fig 3 — memória: seguro contra o baseline em claro. Entre as três
    # condições o RSS varia menos de 0,07 MB, então mostrar as três daria três
    # barras idênticas; o contraste que informa é com o canal sem segurança.
    med = lambda rows, ch, campo: sorted(
        v for v in (pick(rows, c, ch, campo, 1024) for c in CONDS) if v)[1]
    feitos.append(grouped_bar_chart(
        f"{OUT}/fig3-memory.svg", t["f3_t"], t["f3_s"], t["ch"],
        [(t["f3_a"], [med(main_c, ch, "rss_kb") for ch in CH]),
         (t["f3_b"], [med(claro, ch, "rss_kb") for ch in CH_PLAIN])],
        t["f3_y"], t["f3_n"], lang, ymax=8.4, vfmt=lambda v: f"{v:.1f}"))

    # Fig 4 — entrega vs perda. Eixo x ORDINAL de propósito: 0 e 0,1% não
    # existem num log e colariam num linear.
    lconds = ["loss-0", "loss-0.1", "loss-1", "loss-5", "loss-10", "loss-20"]
    llab = [dec(x, lang) for x in ["0", "0.1", "1", "5", "10", "20"]]
    feitos.append(line_chart(
        f"{OUT}/fig4-delivery-loss.svg", t["f4_t"], t["f4_s"], llab,
        [(t["f4_a"], [pick(perda, c, "C1 control", "entrega_pct", 1) for c in lconds]),
         (t["f4_b"], [pick(perda, c, "C3 media", "entrega_pct", 1) for c in lconds]),
         (t["f4_c"], [100.0] * 6)],
        t["f4_y"], t["f4_x"],
        ymin=75, ymax=102, yticks_n=3, yfmt=lambda v: f"{v:.0f}"))

    # Fig 5 — RESERVA: estabelecimento = k x RTT. Não cabe num artigo de 6
    # páginas junto com as quatro acima; fica gerada para quem quiser trocá-la
    # pela Fig 1, que mostra o mesmo fenômeno em três pontos em vez de cinco.
    # Eixo x LINEAR: no ordinal, uma relação exatamente linear aparece
    # encurvada e a figura sugere um crescimento que os dados não têm.
    dconds = ["delay-0", "delay-25", "delay-50", "delay-100", "delay-200"]
    c1 = [pick(atraso, c, "C1 control", "handshake_ms") for c in dconds]
    c3 = [pick(atraso, c, "C3 media", "handshake_ms") for c in dconds]
    feitos.append(line_chart(
        f"{OUT}/fig5-establishment-rtt.svg", t["f5_t"], t["f5_s"],
        ["0", "50", "100", "200", "400"],
        [(t["f5_a"], [(a_ + b_) / 2 for a_, b_ in zip(c1, c3)]),
         (t["f5_b"], [pick(atraso, c, "C4 bulk", "handshake_ms") for c in dconds]),
         (t["f5_c"], [pick(atraso, c, "C2 telemetry", "handshake_ms") for c in dconds])],
        t["f5_y"], t["f5_x"],
        ymax=1400, yticks_n=7, yfmt=lambda v: f"{v:.0f}",
        xvals=[0, 50, 100, 200, 400]))

    for f in feitos:
        print(f)


if __name__ == "__main__":
    main()
