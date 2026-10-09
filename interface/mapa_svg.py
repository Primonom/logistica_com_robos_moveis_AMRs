"""
mapa_svg.py — Mapa do galpão em estilo cartoon (docs/plano_mapa_cartoon.md).

Duas camadas, cada uma gerada por uma função PURA (recebe dicionários,
devolve texto SVG; não usa a interface — ver testes/test_mapa_svg.py), com
os desenhos ("carimbos") de desenhos.py:
  svg_fundo(mapa, fonte)               gerado uma vez: piso, paredes, pilares,
                                       porta, corredor, baias, zonas,
                                       estações, passagens, decoração, régua.
  svg_vivo(mapa, status, selecionado, direcoes, agora)
                                       a cada atualização: semáforos e
                                       cancelas das passagens, luzes das
                                       baias, caixas, entregas, rota do
                                       selecionado, robôs, equipes e
                                       animações.
  svg_legenda()                        a legenda, com os mesmos desenhos.

Coordenadas: o piso começa em (MARGEM, MARGEM); a célula (x, y) ocupa o
quadrado de CELULA px a partir de (MARGEM + x*CELULA, MARGEM + y*CELULA).
A margem guarda as paredes, a régua, as placas das baias e a rosa dos ventos.
Todas as posições do galpão vêm do atr/mapa (fonte única: mapa.h no C).
"""
import math

import cores
import desenhos as d

CELULA = 24
MARGEM = 44
ROBO = 42                    # lado do robô em px (passa da célula: "fofinho" e legível no vídeo)
LADO_CAIXA = {1: 18, 2: 22, 3: 26, 4: 30}   # caixa pendente cresce com o tamanho


# ---------- Coordenadas ----------

def px(c):
    """Célula -> pixel do canto (x ou y)."""
    return MARGEM + c * CELULA


def centro(x, y):
    return px(x) + CELULA / 2, px(y) + CELULA / 2


def tamanho_total(mapa):
    return 2 * MARGEM + mapa["largura"] * CELULA, 2 * MARGEM + mapa["altura"] * CELULA


def celula_do_ponto(ponto_x, ponto_y):
    """Pixel -> célula (para o clique no mapa)."""
    return int((ponto_x - MARGEM) // CELULA), int((ponto_y - MARGEM) // CELULA)


def _abre_svg(mapa):
    larg, alt = tamanho_total(mapa)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{larg}" height="{alt}" '
            f'viewBox="0 0 {larg} {alt}">')


def regiao(ponto, mapa, lado):
    """Área (em células) de uma baia (lado="esq") ou zona (lado="dir"):
    3 células para cada lado do ponto na vertical, sem invadir o corredor,
    e da parede até 3 células depois do ponto na horizontal."""
    y0, y1 = max(0, ponto["y"] - 3), min(mapa["altura"] - 1, ponto["y"] + 3)
    cy = mapa["corredor_y"]
    if y0 <= cy <= y1:
        if cy > ponto["y"]:
            y1 = cy - 1
        else:
            y0 = cy + 1
    if lado == "esq":
        return 0, y0, ponto["x"] + 3, y1
    return ponto["x"] - 3, y0, mapa["largura"] - 1, y1


def ret(x1, y1, x2, y2):
    """Células (x1,y1)..(x2,y2) -> (x, y, largura, altura) em px."""
    return px(x1), px(y1), (x2 - x1 + 1) * CELULA, (y2 - y1 + 1) * CELULA


# ============================================================
# Camada de FUNDO (gerada uma vez)
# ============================================================

def svg_fundo(mapa, fonte_base64=None):
    """fonte_base64: o arquivo .woff2 da fonte em base64. O fundo é mostrado
    como IMAGEM, e uma imagem SVG não enxerga as fontes da página; por isso
    a fonte vai embutida no próprio SVG (data:)."""
    p = [_abre_svg(mapa), d.defs()]
    if fonte_base64:
        p.append('<style>@font-face { font-family: "Fredoka"; font-weight: 300 700; '
                 f'src: url(data:font/woff2;base64,{fonte_base64}) format("woff2"); }}</style>')
    p += _moldura_e_piso(mapa)
    p += _zona_de_transporte(mapa)
    p += _baias(mapa)
    p += _zonas(mapa)
    p += _passagens(mapa)
    p += _corredor_e_acessos(mapa)
    p += _estacoes(mapa)
    p += _decoracao(mapa)
    p += _paredes(mapa)
    p += _regua(mapa)
    p.append("</svg>")
    return "".join(p)


def _moldura_e_piso(mapa):
    larg, alt = tamanho_total(mapa)
    p = [f'<rect width="{larg}" height="{alt}" rx="18" fill="#d6cfc1"/>']
    # placas de concreto 5x5 alternando dois tons, com junta fina
    for bx in range(0, mapa["largura"], 5):
        for by in range(0, mapa["altura"], 5):
            cor = cores.PISO if (bx // 5 + by // 5) % 2 == 0 else cores.PISO_PLACA
            x, y, w, h = ret(bx, by, min(bx + 4, mapa["largura"] - 1), min(by + 4, mapa["altura"] - 1))
            p.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{cor}" '
                     f'stroke="#d8d1c3" stroke-width="1"/>')
    return p


def _zona_de_transporte(mapa):
    """Área central pintada no chão entre as duas primeiras passagens de
    coluna inteira (P1 e P2), com "ZONA DE TRANSPORTE AMR" escrito, como na
    Figura 2. Só decoração (as posições vêm do atr/mapa)."""
    colunas = [r for pas in mapa["passagens"] for r in pas["retangulos"]
               if r["y1"] == 0 and r["y2"] == mapa["altura"] - 1]
    colunas.sort(key=lambda r: r["x1"])
    if len(colunas) < 2:
        return []
    x1, x2 = colunas[0]["x2"] + 1, colunas[1]["x1"] - 1
    x, y, w, h = ret(x1, 1, x2, mapa["altura"] - 2)
    cx = x + w / 2
    cy = px(mapa["corredor_y"]) - 4.5 * CELULA
    return [f'<rect x="{x + 10}" y="{y}" width="{w - 20}" height="{h}" rx="10" fill="#ddd5c6" '
            f'opacity="0.55" stroke="white" stroke-width="3" stroke-dasharray="14 10"/>',
            d.texto(cx, cy, "ZONA DE TRANSPORTE", 30, "#b3a994", 700),
            d.texto(cx, cy + 36, "AMR", 30, "#b3a994", 700)]


def _baias(mapa):
    """Área pastel de cada baia, marcação de doca no ponto de parada e a
    placa "BAIA n" pendurada na parede esquerda (texto vertical)."""
    p = []
    for i, b in enumerate(mapa["baias"]):
        x, y, w, h = ret(*regiao(b, mapa, "esq"))
        p.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{cores.BAIAS[i % 4]}" '
                 f'stroke="{cores.PAREDE}" stroke-width="2.5"/>')
        p += _marca_doca(b["x"], b["y"])
        # placa na parede esquerda
        cx, cy = MARGEM - 22, centro(0, b["y"])[1]
        p.append(f'<rect x="{cx - 14}" y="{cy - 56}" width="28" height="112" rx="6" fill="white" '
                 f'stroke="{cores.CONTORNO}" stroke-width="2"/>')
        p.append(f'<g transform="rotate(-90 {cx} {cy + 6})">'
                 f'{d.texto(cx, cy + 6, "BAIA " + str(b["id"]), 16, peso=700)}</g>')
    return p


def _marca_doca(x, y):
    """Cantoneiras brancas em volta da célula onde o robô para na baia."""
    x0, y0 = px(x) - 3, px(y) - 3
    t = CELULA + 6
    cantos = [(x0, y0, 1, 1), (x0 + t, y0, -1, 1), (x0, y0 + t, 1, -1), (x0 + t, y0 + t, -1, -1)]
    p = []
    for cx, cy, sx, sy in cantos:
        p.append(f'<path d="M{cx} {cy + 7 * sy} V{cy} H{cx + 7 * sx}" fill="none" stroke="white" '
                 f'stroke-width="3" stroke-linecap="round"/>')
    return p


def _zonas(mapa):
    """Área pastel, dois paletes, porta de enrolar na parede direita e a
    letra grande pintada no chão."""
    p = []
    for i, z in enumerate(mapa["zonas"]):
        x1, y1, x2, y2 = regiao(z, mapa, "dir")
        x, y, w, h = ret(x1, y1, x2, y2)
        p.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{cores.ZONAS[i % 4]}" '
                 f'stroke="{cores.PAREDE}" stroke-width="2.5"/>')
        for px_, py_ in _posicoes_paletes(z):
            p.append(d.sombra(px_, py_, 40, 30, 2))
            p.append(d.usar("palete", px_, py_, 40, 30))
        p += _porta_enrolar(mapa["largura"], z["y"])
        lx, ly = centro(z["x"] + 2, z["y"] - 1.6)
        p.append(d.texto(lx, ly, z["id"], 30, "#2B2A33", 700))
        p += _marca_doca(z["x"], z["y"])
    return p


def _posicoes_paletes(z):
    """Dois paletes à esquerda do ponto de entrega (em cima e embaixo)."""
    x = px(z["x"] - 3) + 6
    return [(x, px(z["y"] - 2) + 2), (x, px(z["y"] + 1) + 4)]


def _porta_enrolar(largura, y):
    """Porta de enrolar (doca) na parede direita, na altura da zona."""
    x, y0, h = px(largura) - 7, px(y - 1), 3 * CELULA
    ripas = "".join(f'<line x1="{x}" y1="{y0 + k}" x2="{x + 14}" y2="{y0 + k}" '
                    f'stroke="#7a7f8c" stroke-width="1.5"/>' for k in range(6, h, 6))
    return [f'<rect x="{x}" y="{y0}" width="14" height="{h}" rx="2" fill="#b8bcc6" '
            f'stroke="{cores.CONTORNO}" stroke-width="2"/>', ripas]


def _passagens(mapa):
    """Zonas de restrição (exatamente como o código as trata) com listras de
    advertência amarelo/preto nas bordas compridas."""
    p = []
    for pas in mapa["passagens"]:
        for r in pas["retangulos"]:
            x, y, w, h = ret(r["x1"], r["y1"], r["x2"], r["y2"])
            p.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="url(#restricao)"/>')
            p.append(f'<rect x="{x}" y="{y}" width="5" height="{h}" fill="url(#advertencia)"/>')
            p.append(f'<rect x="{x + w - 5}" y="{y}" width="5" height="{h}" fill="url(#advertencia)"/>')
    return p


def _corredor_e_acessos(mapa):
    """Faixa amarela do corredor (com setas brancas e marcas de pneu) e
    faixas de acesso mais finas ligando baias e zonas ao corredor."""
    p = []
    cy = mapa["corredor_y"]
    for ponto in mapa["baias"] + mapa["zonas"]:
        a, b = sorted((ponto["y"], cy))
        x = px(ponto["x"]) + CELULA * 0.2
        p.append(f'<rect x="{x}" y="{px(a)}" width="{CELULA * 0.6}" height="{(b - a) * CELULA}" '
                 f'fill="{cores.FAIXA}" opacity="0.85"/>')
    x, y, w, h = ret(0, cy, mapa["largura"] - 1, cy)
    p.append(f'<rect x="{x}" y="{y - 3}" width="{w}" height="{h + 6}" fill="{cores.FAIXA}" '
             f'stroke="{cores.FAIXA_BORDA}" stroke-width="2"/>')
    # marcas de pneu bem suaves
    for dy in (-5, 5):
        p.append(f'<path d="M{x} {y + h / 2 + dy} Q{x + w / 4} {y + h / 2 + dy - 2} {x + w / 2} '
                 f'{y + h / 2 + dy} T{x + w} {y + h / 2 + dy}" fill="none" stroke="{cores.CONTORNO}" '
                 f'stroke-width="2" opacity="0.07"/>')
    # setas brancas, alternando o sentido (mão dupla)
    for k, cx in enumerate(range(7, mapa["largura"] - 4, 6)):
        p.append(_seta(px(cx), y + h / 2, 1 if k % 2 == 0 else -1))
    return p


def _seta(x, y, sentido):
    s = sentido
    return (f'<path d="M{x - 7 * s} {y - 3} H{x + 1 * s} V{y - 6.5} L{x + 8 * s} {y} '
            f'L{x + 1 * s} {y + 6.5} V{y + 3} H{x - 7 * s} Z" fill="white" opacity="0.9"/>')


def _estacoes(mapa):
    """Plataforma de recarga com raio pintado, carregador na parede mais
    próxima com cabo até a plataforma, e "ÁREA DE RECARGA" pintado no chão."""
    p = []
    for e in mapa["estacoes"]:
        cx, cy = centro(e["x"], e["y"])
        p.append(d.sombra(cx - 17, cy - 17, 34, 34, 8))
        p.append(f'<rect x="{cx - 17}" y="{cy - 17}" width="34" height="34" rx="8" '
                 f'fill="{cores.RECARGA}" stroke="{cores.CONTORNO}" stroke-width="2.5"/>')
        p.append(f'<path d="M{cx + 3} {cy - 11} L{cx - 7} {cy + 2} H{cx - 1} L{cx - 4} {cy + 11} '
                 f'L{cx + 7} {cy - 2} H{cx + 1} Z" fill="{cores.RAIO}" opacity="0.9"/>')
        em_cima = e["y"] < mapa["altura"] / 2
        wy = MARGEM - 14 if em_cima else px(mapa["altura"]) - 6
        p.append(f'<path d="M{cx + 24} {wy + 10} C{cx + 30} {cy}, {cx + 26} {cy}, {cx + 17} {cy}" '
                 f'fill="none" stroke="{cores.CONTORNO}" stroke-width="2.5"/>')
        p.append(d.usar("carregador", cx + 14, wy, 24, 20))
        ty = cy + (26 if em_cima else -26)
        p.append(d.texto(cx + 102, ty, "ÁREA DE RECARGA", 14, cores.RECARGA, 700))
    return p


def _decoracao(mapa):
    """Poucos detalhes, discretos: extintor e quadro elétrico na parede de
    cima. Nada que se confunda com um objeto do sistema."""
    return [d.usar("extintor", px(21), MARGEM - 8, 10, 20),
            d.usar("quadro", px(33), MARGEM - 9, 18, 22),
            d.usar("extintor", px(48), px(mapa["altura"]) - 12, 10, 20)]


def _paredes(mapa):
    """Contorno do galpão, pilares a cada 10 células e porta dupla na parede
    de baixo (entrada)."""
    x, y = MARGEM, MARGEM
    w, h = mapa["largura"] * CELULA, mapa["altura"] * CELULA
    p = [f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none" stroke="{cores.PAREDE}" '
         f'stroke-width="8"/>']
    for c in range(0, mapa["largura"] + 1, 10):
        for py_ in (y, y + h):
            p.append(_pilar(px(c), py_))
    for c in range(10, mapa["altura"], 10):
        for px_ in (x, x + w):
            p.append(_pilar(px_, px(c)))
    # porta dupla no meio da parede de baixo
    meio = px(mapa["largura"] // 2)
    p.append(f'<rect x="{meio - 36}" y="{y + h - 5}" width="72" height="10" fill="{cores.PISO}"/>')
    for s in (-1, 1):
        # folha aberta para dentro (dobradiça na ponta do vão) e o arco que
        # ela percorre até o meio da porta
        dob = meio + 36 * s
        p.append(f'<path d="M{dob} {y + h - 36} A36 36 0 0 {1 if s < 0 else 0} {meio} {y + h}" '
                 f'fill="none" stroke="{cores.PAREDE}" stroke-width="1.5" stroke-dasharray="3 3"/>')
        p.append(f'<line x1="{dob}" y1="{y + h}" x2="{dob}" y2="{y + h - 36}" '
                 f'stroke="{cores.PAREDE}" stroke-width="4" stroke-linecap="round"/>')
    return p


def _pilar(x, y):
    return (f'<rect x="{x - 8}" y="{y - 8}" width="16" height="16" rx="2" fill="{cores.PILAR}" '
            f'stroke="{cores.CONTORNO}" stroke-width="1.5"/>')


def _regua(mapa):
    """Régua tipo "trena" amarela no topo e à direita: marca a cada 5
    células, número a cada 10. Rosa dos ventos no canto de baixo à esquerda."""
    larg, alt = mapa["largura"], mapa["altura"]
    p = [f'<rect x="{px(0)}" y="10" width="{larg * CELULA}" height="12" rx="3" fill="{cores.FAIXA}" '
         f'stroke="{cores.FAIXA_BORDA}" stroke-width="1"/>',
         f'<rect x="{px(larg) + 22}" y="{px(0)}" width="12" height="{alt * CELULA}" rx="3" '
         f'fill="{cores.FAIXA}" stroke="{cores.FAIXA_BORDA}" stroke-width="1"/>']
    for c in range(0, larg + 1, 5):
        p.append(f'<line x1="{px(c)}" y1="10" x2="{px(c)}" y2="{17 if c % 10 else 22}" '
                 f'stroke="{cores.CONTORNO}" stroke-width="1.2"/>')
        if c % 10 == 0 and 0 < c < larg:
            p.append(d.texto(px(c) + 9, 16.5, c, 8.5, peso=600))
    for c in range(0, alt + 1, 5):
        x0 = px(larg) + 22
        p.append(f'<line x1="{x0 + 12}" y1="{px(c)}" x2="{x0 + (7 if c % 10 else 0)}" y2="{px(c)}" '
                 f'stroke="{cores.CONTORNO}" stroke-width="1.2"/>')
        if c % 10 == 0 and 0 < c < alt:
            p.append(d.texto(x0 + 6, px(c) + 9, c, 8.5, peso=600))
    p.append(d.usar("rosa", 4, px(alt) + 4, 36, 36))
    return p


# ============================================================
# Camada VIVA (a cada atualização)
# ============================================================

# ---------- Animações ----------
# A camada viva é gerada de novo a cada 200 ms. Uma animação CSS comum
# recomeçaria do zero a cada troca (piscaria "errado"). Solução: cada
# elemento animado recebe animation-delay NEGATIVO = (agora módulo período):
# a animação já começa "adiantada" exatamente na fase em que estaria se
# nunca tivesse sido reiniciada. "agora" vem de fora (relógio da tela).
ANIMACOES = """<style>
.atr-pisca { animation: atr-pisca 1s steps(1, end) infinite; }
@keyframes atr-pisca { 50% { opacity: 0.15; } }
.atr-pulsa { animation: atr-pulsa 1.2s ease-in-out infinite;
             transform-box: fill-box; transform-origin: center; }
@keyframes atr-pulsa { 50% { transform: scale(1.3); } }
.atr-halo { animation: atr-halo 1s ease-in-out infinite; }
@keyframes atr-halo { 50% { opacity: 0.05; } }
</style>"""
PERIODO_S = {"atr-pisca": 1.0, "atr-pulsa": 1.2, "atr-halo": 1.0}


def animado(classe, conteudo, agora):
    """Envolve "conteudo" numa animação que continua da fase certa."""
    atraso = agora % PERIODO_S[classe]
    return f'<g class="{classe}" style="animation-delay: -{atraso:.2f}s">{conteudo}</g>'


def svg_vivo(mapa, status, selecionado=None, direcoes=None, agora=0.0):
    """direcoes: {id_robo: ângulo} (guardado pelo estado.py, só para o
    desenho). agora: relógio em segundos, para a fase das animações."""
    direcoes = direcoes or {}
    p = [_abre_svg(mapa), d.defs(), ANIMACOES]
    if status is not None:
        p += _semaforos(mapa, status, agora)
        p += _todas_as_cancelas(mapa, status, fechadas=False)  # abertas: embaixo dos robôs
        p += _luzes_das_baias(mapa, status)
        p += _caixas_nas_baias(mapa, status)
        p += _entregas(mapa, status)
        p += _recargas(mapa, status, agora)
        p += _rotas(status, selecionado)
        placas = []                                            # (x, y, id) de cada robô
        p += _robos_e_equipes(status, selecionado, direcoes, agora, placas)
        p += _todas_as_cancelas(mapa, status, fechadas=True)   # fechadas: por cima (barreira)
        p += _desenhar_placas(placas)                          # ids por cima de tudo
    p.append("</svg>")
    return "".join(p)


def cor_ocupacao(ocupadas, capacidade):
    if ocupadas <= 0:
        return "verde"
    if ocupadas >= capacidade:
        return "vermelho"
    return "amarelo"


def _ocupacao(pas, status):
    """(ocupadas, capacidade, luz) de uma passagem."""
    livres = {x["nome"]: x["livres"] for x in status["passagens"]}
    cap = pas["capacidade"]
    ocupadas = cap - livres.get(pas["nome"], cap)
    return ocupadas, cap, cor_ocupacao(ocupadas, cap)


def _semaforos(mapa, status, agora=0.0):
    """Na porta de cada passagem: semáforo de 3 luzes e a placa
    "ocupadas/capacidade" (as cancelas vão por último, por cima dos robôs)."""
    p = []
    for pas in mapa["passagens"]:
        ocupadas, cap, luz = _ocupacao(pas, status)
        sx, sy, _ = _posicao_semaforo(pas, mapa)
        p += _semaforo(sx, sy, luz, agora)
        # placa centrada ACIMA do semáforo (ao lado ela encostaria na passagem vizinha)
        p += _placa(sx, sy - 47, f"{pas['nome']}  {ocupadas}/{cap}", luz)
    return p


def _todas_as_cancelas(mapa, status, fechadas):
    """Cancelas das portas (fechadas quando a passagem está cheia). Chamada
    duas vezes: fechadas=False ANTES dos robôs (cancela levantada fica
    embaixo deles) e fechadas=True DEPOIS (a barreira aparece na frente de
    quem está esperando na porta)."""
    p = []
    for pas in mapa["passagens"]:
        cheia = _ocupacao(pas, status)[2] == "vermelho"
        if cheia == fechadas:
            for porta in _posicao_semaforo(pas, mapa)[2]:
                p += _cancelas(porta, mapa, fechada=cheia)
    return p


def _posicao_semaforo(pas, mapa):
    """Semáforo logo acima da porta de cima. Portas: P1/P2 = onde o corredor
    atravessa; P3 = as pontas dos trechos que encostam no corredor."""
    cy = mapa["corredor_y"]
    portas = []
    for r in pas["retangulos"]:
        if r["y1"] <= cy <= r["y2"]:
            portas.append(("horizontal", r["x1"], r["x2"], cy))
        elif r["y2"] < cy:
            portas.append(("vertical", r["x1"], r["x2"], r["y2"]))
        else:
            portas.append(("vertical", r["x1"], r["x2"], r["y1"]))
    _, x1, x2, y = portas[0]
    sx = (px(x1) + px(x2 + 1)) / 2
    sy = px(y) - 40 if portas[0][0] == "horizontal" else px(y) - 10
    if portas[0][0] == "vertical":
        sx += CELULA * 1.3            # P3: ao lado da coluna, para não tapar a passagem
    return sx, sy, portas


def _semaforo(x, y, luz, agora=0.0):
    """Caixa escura com 3 luzes (vermelho em cima). Só a luz ativa acende."""
    acesas = {"vermelho": "#ff4d4d", "amarelo": "#ffc93c", "verde": "#3ddc84"}
    p = [d.sombra(x - 11, y - 27, 22, 52, 6),
         f'<rect x="{x - 2.5}" y="{y + 23}" width="5" height="10" fill="{cores.CONTORNO}"/>',
         f'<rect x="{x - 11}" y="{y - 27}" width="22" height="52" rx="7" fill="#3a3d47" '
         f'stroke="{cores.CONTORNO}" stroke-width="2.5"/>']
    for k, nome in enumerate(("vermelho", "amarelo", "verde")):
        ly = y - 15 + k * 15.5
        cor = acesas[nome] if nome == luz else "#5c5f6b"
        if nome == luz:   # halo da luz acesa; no vermelho (cheia) ele pulsa
            halo = f'<circle cx="{x}" cy="{ly}" r="10" fill="{cor}" opacity="0.4"/>'
            p.append(animado("atr-halo", halo, agora) if nome == "vermelho" else halo)
        p.append(f'<circle cx="{x}" cy="{ly}" r="6" fill="{cor}" stroke="{cores.CONTORNO}" '
                 f'stroke-width="1.5"/>')
        if nome == luz:
            p.append(f'<circle cx="{x - 2}" cy="{ly - 2}" r="1.8" fill="white" opacity="0.75"/>')
    return p


def _placa(cx, y, texto, luz):
    """Placa branca centrada em (cx, y), com a borda na cor da situação."""
    borda = {"vermelho": "#dc2626", "amarelo": "#d97706", "verde": "#16a34a"}[luz]
    largura = 14 + 9.2 * len(texto)
    x = cx - largura / 2
    return [d.sombra(x, y - 14, largura, 28, 8),
            f'<rect x="{x}" y="{y - 14}" width="{largura}" height="28" rx="8" fill="white" '
            f'stroke="{borda}" stroke-width="3"/>',
            d.texto(x + largura / 2, y, texto, 16, peso=700)]


def _cancelas(porta, mapa, fechada):
    """Cancela listrada nas duas entradas da porta: atravessada (fechada)
    quando a passagem está cheia; levantada (só o poste) quando não."""
    orient, x1, x2, y = porta
    p = []
    if orient == "horizontal":            # corredor atravessa: entradas à esquerda e à direita
        entradas = [(px(x1) - 2, px(y) - 5, 0, 1), (px(x2 + 1) + 2, px(y + 1) + 5, 0, -1)]
    else:                                 # coluna do P3: entrada na ponta
        yy = px(y) if y < mapa["corredor_y"] else px(y + 1)
        entradas = [(px(x1) - 5, yy, 1, 0)]
    for ex, ey, dx, dy in entradas:
        if fechada:      # braço atravessado na entrada
            comp = CELULA + 10
            fim = (ex + dx * comp, ey + dy * comp)
        else:            # braço levantado: curto, inclinado para fora da faixa
            fim = (ex - 10 * (dx or 1) + 6 * dy, ey - 10 * (dy or 1) - 6 * dx)
        p += _braco_cancela((ex, ey), fim)
        p.append(f'<rect x="{ex - 4.5}" y="{ey - 4.5}" width="9" height="9" rx="2" fill="#9aa0ab" '
                 f'stroke="{cores.CONTORNO}" stroke-width="1.8"/>')
    return p


def _braco_cancela(inicio, fim):
    """Braço da cancela: listrado vermelho e branco, com contorno."""
    (x1, y1), (x2, y2) = inicio, fim
    linha = f'x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}"'
    return [f'<line {linha} stroke="{cores.CONTORNO}" stroke-width="7" stroke-linecap="round"/>',
            f'<line {linha} stroke="white" stroke-width="4"/>',
            f'<line {linha} stroke="#ef4444" stroke-width="4" stroke-dasharray="5 5"/>']


def _luzes_das_baias(mapa, status):
    """Luz na placa da baia: âmbar se há carga em andamento nela."""
    ocupadas = {c["baia"] for c in status["caixas"] if c["status"] == "EM_ANDAMENTO"}
    p = []
    for b in mapa["baias"]:
        cx, cy = MARGEM - 22, centro(0, b["y"])[1] - 44
        cor = "#ffb020" if b["id"] in ocupadas else "#9aa0ab"
        p.append(f'<circle cx="{cx}" cy="{cy}" r="6.5" fill="{cor}" stroke="{cores.CONTORNO}" '
                 f'stroke-width="1.5"/>')
    return p


# Lugares das caixas pendentes em volta da doca (em células, a partir do
# centro da doca). A coluna da doca fica livre para os robôs.
VAGAS_BAIA = [(dx, dy) for dy in (-1.55, 0.0, 1.55) for dx in (-1.2, 1.2, -2.45, 2.45)]


def _caixas_nas_baias(mapa, status):
    """Caixas ESPERANDO empilhadas dentro da baia, em ordem de chegada. Leve
    variação de posição calculada pelo id (determinística: não "treme")."""
    p = []
    for b in mapa["baias"]:
        fila = [c for c in status["caixas"] if c["baia"] == b["id"] and c["status"] == "ESPERANDO"]
        bx, by = centro(b["x"], b["y"])
        for c, (vx, vy) in zip(fila, VAGAS_BAIA):
            jx, jy = (c["id"] * 37) % 5 - 2, (c["id"] * 53) % 5 - 2
            p += caixa(bx + vx * CELULA + jx, by + vy * CELULA + jy, LADO_CAIXA[c["tamanho"]],
                       c["tamanho"])
        if len(fila) > len(VAGAS_BAIA):
            p += _placa(bx - 1.8 * CELULA, by + 2.6 * CELULA, f"+{len(fila) - len(VAGAS_BAIA)}",
                        "amarelo")
    return p


def caixa(cx, cy, lado, numero):
    """Caixa de papelão centrada em (cx, cy) com o tamanho escrito na tampa
    (no cruzamento da fita, que é claro; borda clara nas letras)."""
    return [d.sombra(cx - lado / 2, cy - lado / 2, lado, lado, 3),
            d.usar("caixa", cx - lado / 2, cy - lado / 2, lado, lado),
            d.texto(cx, cy + 0.5, numero, max(11, lado * 0.52), peso=700,
                    contorno=cores.PAPELAO_FITA)]


def _entregas(mapa, status):
    """Pilha de caixinhas entregues sobre o palete de cima (até 6) e o total."""
    p = []
    for z in mapa["zonas"]:
        n = sum(1 for c in status["caixas"] if c["zona"] == z["id"] and c["status"] == "ENTREGUE")
        if n == 0:
            continue
        x0, y0 = _posicoes_paletes(z)[0]
        for k in range(min(n, 6)):
            p += [d.usar("caixa", x0 + 2 + (k % 3) * 12.5, y0 + 2 + (k // 3) * 13, 12, 12)]
        p += _placa(x0 + 68, y0 + 8, f"✓ {n}", "verde")
    return p


def _rotas(status, selecionado):
    """Caminho de cada robô pelos waypoints que o C publica ("rota"): no
    robô selecionado, pontinhos fortes e uma bandeirinha no destino; nos
    outros, pontinhos pequenos e apagados (bem discretos). Seguidores de uma
    equipe carregando não têm rota própria (o C manda rota vazia)."""
    p = []
    for r in status["robos"]:
        if not r.get("rota"):
            continue
        forte = r["id"] == selecionado
        cor = cores.do_estado(r["estado"])["cor"]
        pontos = [centro(r["x"], r["y"])] + [centro(x, y) for x, y in r["rota"]]
        passo, raio = (12, 3) if forte else (18, 2)
        estilo = 'stroke="white" stroke-width="1.2"' if forte else 'opacity="0.45"'
        for (ax, ay), (bx, by) in zip(pontos, pontos[1:]):
            n = int(math.dist((ax, ay), (bx, by)) // passo)
            for k in range(1, n + 1):
                t = k / n
                p.append(f'<circle cx="{ax + (bx - ax) * t:.1f}" cy="{ay + (by - ay) * t:.1f}" '
                         f'r="{raio}" fill="{cor}" {estilo}/>')
        if forte:
            fx, fy = pontos[-1]
            p.append(d.usar("bandeira", fx - 5, fy - 22, 20, 24, f'style="color: {cor}"'))
    return p


# ---------- Robôs e equipes ----------

# Onde fica cada robô de uma equipe carregando, em volta do centro da caixa.
FORMACAO = {1: [(0, 0)], 2: [(-25, 0), (25, 0)], 3: [(0, -25), (-24, 18), (24, 18)],
            4: [(-24, -24), (24, -24), (-24, 24), (24, 24)]}
LADO_CAIXA_EQUIPE = {1: 33, 2: 45, 3: 51, 4: 54}


def _recargas(mapa, status, agora):
    """Robô RECARREGANDO parado numa estação: raio pulsando em cima da
    plataforma e uma barra vertical de carga (a bateria real dele) ao lado."""
    estacoes = {(e["x"], e["y"]) for e in mapa["estacoes"]}
    p = []
    for r in status["robos"]:
        if r["estado"] != "RECARREGANDO" or (r["x"], r["y"]) not in estacoes:
            continue
        cx, cy = centro(r["x"], r["y"])
        raio = (f'<path d="M{cx + 4} {cy - 52} L{cx - 7} {cy - 37} H{cx - 1} L{cx - 4} {cy - 26} '
                f'L{cx + 8} {cy - 41} H{cx + 2} Z" fill="{cores.RAIO}" stroke="{cores.CONTORNO}" '
                f'stroke-width="2" stroke-linejoin="round"/>')
        p.append(animado("atr-pulsa", raio, agora))
        nivel = 40 * max(0, min(100, r["bateria"])) / 100
        p.append(f'<rect x="{cx - 46}" y="{cy - 20}" width="12" height="40" rx="4" fill="white" '
                 f'stroke="{cores.CONTORNO}" stroke-width="2"/>')
        p.append(f'<rect x="{cx - 44}" y="{cy + 18 - nivel * 0.9:.1f}" width="8" '
                 f'height="{nivel * 0.9:.1f}" rx="2" fill="#16a34a"/>')
    return p


def angulo(r, direcoes):
    """Direção do robô em graus (0 = direita, 90 = baixo...). Quem calcula
    e guarda é o estado.py (andando: do ponto atual para o setpoint;
    parado: a última direção conhecida)."""
    return direcoes.get(r["id"], 0)


def _robos_e_equipes(status, selecionado, direcoes, agora, placas):
    """Desenha as UNIDADES: um robô sozinho, ou uma equipe carregando (os
    robôs + a caixa). Unidades na mesma célula (ex.: fila na porta de uma
    passagem cheia — não há colisão fora das passagens) viram uma FILA, para
    nada ficar escondido (ver _enfileirar)."""
    p = []
    for posicao, tipo, dados in _enfileirar(_unidades(status), direcoes):
        if tipo == "robo":
            p += robo(dados, posicao, angulo(dados, direcoes), dados["id"] == selecionado,
                      equipe=dados["lider"] >= 0, agora=agora, placas=placas)
        else:
            membros, lider, tamanho_caixa = dados
            p += equipe_carregando(membros, posicao, tamanho_caixa, angulo(lider, direcoes),
                                   selecionado, agora, placas)
    return p


RAIO_UNIDADE = {"robo": 24, 1: 26, 2: 52, 3: 50, 4: 54}   # "tamanho" de cada unidade (px)


def _unidades(status):
    """Lista de (célula, raio, tipo, dados). Equipe carregando = uma unidade
    na célula do LÍDER (os seguidores andam 1–2 células atrás só pelo atraso
    do buffer)."""
    tamanho = {c["id"]: c["tamanho"] for c in status["caixas"]}
    por_id = {r["id"]: r for r in status["robos"]}
    # cargas que já estão erguidas (alguém da equipe em OPERANDO_CARGA): TODOS
    # os robôs dessa carga fazem parte da unidade, mesmo um em DEFEITO (ele
    # continua segurando a caixa, só que parado e piscando)
    erguidas = {r["carga"] for r in status["robos"]
                if r["estado"] == "OPERANDO_CARGA" and r["lider"] in por_id}
    equipes, unidades = {}, []
    for r in status["robos"]:
        if r["carga"] in erguidas:
            equipes.setdefault(r["carga"], []).append(r)
        else:
            unidades.append(((r["x"], r["y"]), RAIO_UNIDADE["robo"], "robo", r))
    for carga, membros in equipes.items():
        lider = por_id[membros[0]["lider"]]
        n = min(len(membros), 4)
        unidades.append(((lider["x"], lider["y"]), RAIO_UNIDADE[n], "equipe",
                         (membros, lider, tamanho.get(carga, n))))
    return unidades


def _id_da_unidade(tipo, dados):
    """Robô que "dirige" a unidade: o próprio robô, ou o líder da equipe."""
    return dados["id"] if tipo == "robo" else dados[1]["id"]


def _enfileirar(unidades, direcoes):
    """Unidades na mesma célula viram uma fila: a primeira fica na célula e
    as outras se enfileiram para TRÁS (contra a direção em que andam),
    encostadas umas nas outras conforme o tamanho de cada uma. Na porta de
    uma passagem cheia a fila fica ao longo do corredor, como na vida real.
    Retorna [(posição_px, tipo, dados)]."""
    por_celula = {}
    for celula, raio, tipo, dados in unidades:
        por_celula.setdefault(celula, []).append((raio, tipo, dados))
    saida = []
    for (x, y), lista in por_celula.items():
        lista.sort(key=lambda u: _id_da_unidade(u[1], u[2]))
        cx, cy = centro(x, y)
        ang = math.radians(direcoes.get(_id_da_unidade(lista[0][1], lista[0][2]), 0))
        tras_x, tras_y = -math.cos(ang), -math.sin(ang)
        # a frente da fila para na borda da célula (ex.: na cancela), em vez
        # de a unidade da frente "invadir" a célula seguinte
        recuo = max(0.0, lista[0][0] - CELULA / 2) if len(lista) > 1 else 0.0
        for k, (raio, tipo, dados) in enumerate(lista):
            if k > 0:
                recuo += lista[k - 1][0] + raio
            saida.append(((cx + tras_x * recuo, cy + tras_y * recuo), tipo, dados))
    return saida


def robo(r, posicao, ang, selecionado=False, equipe=False, placa=True, bateria=True,
         agora=0.0, placas=None):
    """Um robô: sombra, corpo girado na direção do movimento, LED de status
    (em pé), plaquinha com o id e barrinha de bateria. DEFEITO: halo
    vermelho pulsando e LED piscando; RECARREGANDO: raio do LED pulsando."""
    x, y = posicao
    cor = cores.do_estado(r["estado"])["cor"]
    m = ROBO / 2
    p = []
    if selecionado:   # "círculo de seleção" no chão, como em jogo de estratégia
        p.append(f'<ellipse cx="{x}" cy="{y + 2}" rx="{m + 7}" ry="{m + 6}" fill="white" '
                 f'fill-opacity="0.55" stroke="{cores.CONTORNO}" stroke-width="2.5"/>')
    if equipe:        # indo para a baia: contorno tracejado curto na cor da equipe
        p.append(f'<circle cx="{x}" cy="{y}" r="{m + 5}" fill="none" stroke="{cores.EQUIPE}" '
                 f'stroke-width="2.5" stroke-dasharray="5 4"/>')
    if r["estado"] == "DEFEITO":
        p.append(animado("atr-halo", f'<circle cx="{x}" cy="{y}" r="{m + 9}" fill="#ef4444" '
                         f'opacity="0.35"/>', agora))
    p.append(d.sombra(x - m, y - m, ROBO, ROBO, 8))
    estilo = f'style="color: {cor}"'          # o símbolo pinta o corpo com currentColor
    corpo = d.usar("robo", x - m, y - m, ROBO, ROBO, estilo)
    p.append(f'<g transform="rotate({ang:.0f} {x:.1f} {y:.1f})">{corpo}</g>')
    led = _led_usado(r["estado"], x, y)
    if r["estado"] == "DEFEITO":
        led = animado("atr-pisca", led, agora)
    elif r["estado"] == "RECARREGANDO":
        led = animado("atr-pulsa", led, agora)
    p.append(led)
    if bateria:
        p += _bateria(x - 16, y + m + 3, r["bateria"])
    if placa:   # com "placas", só anota: quem chama desenha todas no fim
        if placas is None:
            p += placa_id(x + m - 3, y - m - 4, r["id"])
        else:
            placas.append((x + m - 3, y - m - 4, r["id"]))
    return p


def _led(estado):
    return {"DEFEITO": "led-defeito", "RECARREGANDO": "led-recarga",
            "MANUAL": "led-manual"}.get(estado, "led-ok")


def _led_usado(estado, x, y):
    """LED de status (16 px) no centro do robô, sempre "em pé"."""
    return d.usar(_led(estado), x - 8, y - 8, 16, 16)


def _desenhar_placas(placas):
    """Desenha as plaquinhas de id POR ÚLTIMO (ficam por cima de caixas e
    cancelas) e sem uma cobrir a outra: se uma encostaria numa já colocada
    (robôs em células vizinhas), ela sobe até ficar livre."""
    colocadas, p = [], []
    for x, y, rid in sorted(placas, key=lambda t: (t[0], t[1])):
        while any(abs(x - cx) < 26 and abs(y - cy) < 21 for cx, cy in colocadas):
            y -= 21
        colocadas.append((x, y))
        p += placa_id(x, y, rid)
    return p


def placa_id(x, y, rid):
    """Plaquinha branca com o id (sempre legível, sempre em pé)."""
    largura = 20 if rid < 10 else 28
    return [f'<rect x="{x - largura / 2:.1f}" y="{y - 10:.1f}" width="{largura}" height="20" rx="5" '
            f'fill="white" stroke="{cores.CONTORNO}" stroke-width="2.2"/>',
            d.texto(x, y + 0.5, rid, 15, peso=700)]


def _bateria(x, y, pct):
    cor = "#16a34a" if pct >= 30 else ("#d97706" if pct >= 20 else "#dc2626")
    w = 32 * max(0, min(100, pct)) / 100
    return [f'<rect x="{x}" y="{y}" width="32" height="8" rx="4" fill="white" '
            f'stroke="{cores.CONTORNO}" stroke-width="1.8"/>',
            f'<rect x="{x + 1.5}" y="{y + 1.5}" width="{max(0, w - 3):.1f}" height="5" rx="2.5" fill="{cor}"/>']


def equipe_carregando(membros, posicao, tamanho_caixa, ang, selecionado, agora=0.0,
                      placas=None):
    """Equipe carregando = uma unidade: robôs em formação em volta de
    "posicao", a caixa grande POR CIMA deles e as plaquinhas por cima de
    tudo (os ids nunca ficam escondidos)."""
    cx, cy = posicao
    membros = sorted(membros, key=lambda r: r["id"])
    formacao = FORMACAO.get(len(membros), FORMACAO[4])
    p = []
    for r, (dx, dy) in zip(membros, formacao):
        p += robo(r, (cx + dx, cy + dy), ang, r["id"] == selecionado, placa=False, bateria=False,
                  agora=agora)
    lado = LADO_CAIXA_EQUIPE.get(len(membros), 54)
    p += caixa(cx, cy, lado, tamanho_caixa)
    for r, (dx, dy) in zip(membros, formacao):
        n = math.hypot(dx, dy) or 1
        ox, oy = (dx / n, dy / n) if (dx, dy) != (0, 0) else (0.7, -0.7)
        if placas is None:
            p += placa_id(cx + dx + ox * 28, cy + dy + oy * 28, r["id"])
        else:
            placas.append((cx + dx + ox * 28, cy + dy + oy * 28, r["id"]))
    return p


# ---------- Legenda (mesmos desenhos, em miniatura) ----------

def svg_legenda():
    """Uma faixa SVG com cada estado do robô, a caixa, a equipe carregando e
    o semáforo da passagem, desenhados pelas MESMAS funções do mapa."""
    itens = []
    x = 28
    for nome, estilo in cores.ESTADOS.items():
        r = {"id": 1, "estado": nome, "bateria": 80}
        itens += robo(r, (x, 32), 0, placa=False, bateria=False)
        itens.append(d.texto(x + 28, 32, estilo["rotulo"], 14, ancora="start"))
        x += 62 + 8.2 * len(estilo["rotulo"])
    largura = x
    x = 22
    itens += caixa(x, 100, 22, 2)
    itens.append(d.texto(x + 20, 100, "caixa (nº = tamanho)", 14, ancora="start"))
    x += 205
    membros = [{"id": 1, "estado": "OPERANDO_CARGA", "bateria": 80},
               {"id": 2, "estado": "OPERANDO_CARGA", "bateria": 80}]
    for r, (dx, dy) in zip(membros, FORMACAO[2]):
        itens += robo(r, (x + 26 + dx, 100 + dy), 0, placa=False, bateria=False)
    itens += caixa(x + 26, 100, LADO_CAIXA_EQUIPE[2], 2)
    itens.append(d.texto(x + 80, 100, "equipe carregando", 14, ancora="start"))
    x += 235
    itens += _semaforo(x, 100, "vermelho")
    itens.append(d.texto(x + 20, 100, "passagem: semáforo e cancela (vermelho = cheia)", 14,
                         ancora="start"))
    x += 415
    itens += _bateria(x, 96, 60)
    itens.append(d.texto(x + 40, 100, "bateria", 14, ancora="start"))
    x += 110
    itens += placa_id(x + 10, 100, 7)
    itens.append(d.texto(x + 28, 100, "id do robô", 14, ancora="start"))
    largura = max(largura, x + 120)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{largura:.0f}" height="136" '
            f'viewBox="0 0 {largura:.0f} 136">{d.defs()}{"".join(itens)}</svg>')
