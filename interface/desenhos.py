"""
desenhos.py — Os "carimbos" do mapa cartoon: cada objeto é um <symbol> SVG.

Cada objeto é desenhado UMA vez aqui, dentro de <defs>, e depois "carimbado"
no mapa com <use href="#nome" x=.. y=.. width=.. height=..>. Assim o mapa
tem 16 robôs, mas o desenho do robô existe uma vez só.

Estilo (docs/plano_mapa_cartoon.md): cartoon "flat", contorno escuro grosso
(cores.CONTORNO), cores chapadas, UMA faixa de luz (highlight) e UMA sombra
projetada para baixo-direita (a sombra é desenhada pelo mapa, fora dos
símbolos, para não girar junto com o robô).

Cor variável (ex.: a cor do estado do robô): o símbolo usa
fill="currentColor" e quem carimba define style="color: ...".

Tudo aqui é texto puro (sem interface): ver testes/test_desenhos.py.
"""
import html

import cores

C = cores.CONTORNO


def texto(x, y, conteudo, tamanho=12, cor=cores.TEXTO_MAPA, peso=600, ancora="middle",
          contorno=None):
    """Um <text> na fonte do mapa. O conteúdo é escapado (<, >, &).
    contorno="#cor" desenha uma borda em volta das letras (legível sobre
    qualquer fundo)."""
    extra = (f' stroke="{contorno}" stroke-width="3" paint-order="stroke" stroke-linejoin="round"'
             if contorno else "")
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{tamanho}" fill="{cor}" '
            f'font-weight="{peso}" text-anchor="{ancora}" dominant-baseline="central" '
            f'font-family="Fredoka, \'Baloo 2\', Arial, sans-serif"{extra}>'
            f'{html.escape(str(conteudo))}</text>')


def usar(nome, x, y, largura, altura, extra=""):
    """Carimba o símbolo "nome" com canto superior esquerdo em (x, y)."""
    return (f'<use href="#{nome}" x="{x:.1f}" y="{y:.1f}" width="{largura:.1f}" '
            f'height="{altura:.1f}" {extra}/>')


def sombra(x, y, largura, altura, raio=6):
    """Sombra projetada: a mesma forma, deslocada 3 px para baixo-direita,
    escura e bem transparente. Mesma direção para todos os objetos."""
    return (f'<rect x="{x + 3:.1f}" y="{y + 3:.1f}" width="{largura:.1f}" height="{altura:.1f}" '
            f'rx="{raio}" fill="{cores.SOMBRA}" opacity="0.18"/>')


# ---------- Símbolos ----------

def simbolo_robo():
    """AMR visto de cima, de frente para a DIREITA (o mapa gira o símbolo).
    viewBox 32x32."""
    return f'''<symbol id="robo" viewBox="0 0 32 32">
  <!-- 4 rodinhas aparecendo nas laterais -->
  <rect x="5" y="0.5" width="8" height="5" rx="2" fill="{C}"/>
  <rect x="19" y="0.5" width="8" height="5" rx="2" fill="{C}"/>
  <rect x="5" y="26.5" width="8" height="5" rx="2" fill="{C}"/>
  <rect x="19" y="26.5" width="8" height="5" rx="2" fill="{C}"/>
  <!-- corpo na cor do estado (currentColor), cantos bem arredondados -->
  <rect x="2" y="3" width="28" height="26" rx="8" fill="currentColor" stroke="{C}" stroke-width="2.5"/>
  <!-- faixa de luz no topo -->
  <rect x="6" y="6" width="17" height="4.5" rx="2.2" fill="white" opacity="0.35"/>
  <!-- para-choque escuro na frente (lado direito) -->
  <rect x="25" y="6.5" width="4.5" height="19" rx="2.2" fill="{C}" opacity="0.75"/>
  <!-- farol/sensor: dois pontinhos na frente -->
  <circle cx="27.3" cy="10.5" r="1.4" fill="#FFE58A"/>
  <circle cx="27.3" cy="21.5" r="1.4" fill="#FFE58A"/>
</symbol>'''


def simbolos_led():
    """LED de status no centro do robô (sempre "em pé", não gira).
    viewBox 12x12 cada um."""
    return f'''<symbol id="led-ok" viewBox="0 0 12 12">
  <circle cx="6" cy="6" r="4.6" fill="white" stroke="{C}" stroke-width="1.6"/>
  <circle cx="4.7" cy="4.7" r="1.3" fill="#d1fae5"/>
</symbol>
<symbol id="led-defeito" viewBox="0 0 12 12">
  <circle cx="6" cy="6" r="5" fill="#ef4444" stroke="{C}" stroke-width="1.6"/>
  <rect x="5.1" y="2.6" width="1.8" height="4.4" rx="0.9" fill="white"/>
  <circle cx="6" cy="8.8" r="1" fill="white"/>
</symbol>
<symbol id="led-recarga" viewBox="0 0 12 12">
  <circle cx="6" cy="6" r="5" fill="{C}"/>
  <path d="M6.8 1.8 L3.4 6.6 H5.8 L5.0 10.2 L8.6 5.2 H6.2 Z" fill="{cores.RAIO}"/>
</symbol>
<symbol id="led-manual" viewBox="0 0 12 12">
  <circle cx="6" cy="6" r="5" fill="white" stroke="{C}" stroke-width="1.6"/>
  <!-- joystick: base, haste e bolinha -->
  <rect x="3" y="7.4" width="6" height="2" rx="1" fill="{C}"/>
  <rect x="5.4" y="4" width="1.2" height="4" fill="{C}"/>
  <circle cx="6" cy="3.6" r="1.6" fill="#ea580c"/>
</symbol>'''


def simbolo_caixa():
    """Caixa de papelão vista de cima: abas, fita em cruz e sombra interna.
    viewBox 40x40 (o mapa escolhe o tamanho conforme o tamanho da caixa)."""
    return f'''<symbol id="caixa" viewBox="0 0 40 40">
  <rect x="1.5" y="1.5" width="37" height="37" rx="3" fill="{cores.PAPELAO}" stroke="{C}" stroke-width="2.5"/>
  <!-- lado mais escuro (volume) -->
  <path d="M38.5 4.5 V35.5 Q38.5 38.5 35.5 38.5 H4.5 L8 35 H35 V8 Z" fill="{cores.PAPELAO_SOMBRA}" opacity="0.7"/>
  <!-- abas: costura no meio e dobras -->
  <line x1="3" y1="20" x2="37" y2="20" stroke="{cores.PAPELAO_SOMBRA}" stroke-width="1.5"/>
  <!-- fita adesiva em cruz -->
  <rect x="16.5" y="2.5" width="7" height="35" fill="{cores.PAPELAO_FITA}" opacity="0.95"/>
  <rect x="2.5" y="16.5" width="35" height="7" fill="{cores.PAPELAO_FITA}" opacity="0.8"/>
</symbol>'''


def simbolo_palete():
    """Palete de madeira (ripas) visto de cima. viewBox 40x30."""
    ripas = "".join(
        f'<rect x="2" y="{3 + i * 6.5}" width="36" height="4.5" rx="1" fill="{cores.MADEIRA}" '
        f'stroke="{cores.MADEIRA_ESCURA}" stroke-width="1"/>' for i in range(4))
    return f'''<symbol id="palete" viewBox="0 0 40 30">
  <!-- travessas por baixo das ripas -->
  <rect x="4" y="1" width="5" height="28" rx="1" fill="{cores.MADEIRA_ESCURA}"/>
  <rect x="17.5" y="1" width="5" height="28" rx="1" fill="{cores.MADEIRA_ESCURA}"/>
  <rect x="31" y="1" width="5" height="28" rx="1" fill="{cores.MADEIRA_ESCURA}"/>
  {ripas}
</symbol>'''


def simbolo_carregador():
    """Carregador de parede: caixa azul-petróleo com raio e tomada.
    viewBox 24x20."""
    return f'''<symbol id="carregador" viewBox="0 0 24 20">
  <rect x="1.5" y="1.5" width="21" height="17" rx="3" fill="{cores.RECARGA}" stroke="{C}" stroke-width="2"/>
  <rect x="4" y="4" width="9" height="4" rx="1" fill="#9be7ff" opacity="0.7"/>
  <path d="M17 3.5 L14 10 H16.5 L15.5 16 L19.5 8.5 H17 Z" fill="{cores.RAIO}" stroke="{C}" stroke-width="0.8"/>
</symbol>'''


def simbolo_extintor():
    """Extintor vermelho preso na parede. viewBox 10x20."""
    return f'''<symbol id="extintor" viewBox="0 0 10 20">
  <rect x="3" y="0.5" width="4" height="3" rx="1" fill="{C}"/>
  <rect x="1.5" y="3" width="7" height="16" rx="3" fill="#e5484d" stroke="{C}" stroke-width="1.5"/>
  <rect x="3" y="6" width="4" height="4" fill="white" opacity="0.85"/>
</symbol>'''


def simbolo_quadro():
    """Quadro elétrico cinza com aviso de raio. viewBox 18x22."""
    return f'''<symbol id="quadro" viewBox="0 0 18 22">
  <rect x="1.5" y="1.5" width="15" height="19" rx="2" fill="#9aa0ab" stroke="{C}" stroke-width="1.5"/>
  <path d="M9 5 L4.5 13.5 H13.5 Z" fill="{cores.RAIO}" stroke="{C}" stroke-width="1"/>
  <rect x="5" y="15.5" width="8" height="2" rx="1" fill="{C}" opacity="0.6"/>
</symbol>'''


def simbolo_rosa():
    """Rosa dos ventos. viewBox 40x40."""
    return f'''<symbol id="rosa" viewBox="0 0 40 40">
  <circle cx="20" cy="20" r="12" fill="white" stroke="{C}" stroke-width="2"/>
  <path d="M20 3 L23.5 20 L20 37 L16.5 20 Z" fill="{C}"/>
  <path d="M3 20 L20 16.5 L37 20 L20 23.5 Z" fill="#7a7f8c"/>
  <path d="M20 3 L23.5 20 H16.5 Z" fill="#e5484d"/>
  <circle cx="20" cy="20" r="2.5" fill="white" stroke="{C}" stroke-width="1.5"/>
</symbol>'''


def simbolo_bandeira():
    """Bandeirinha de destino (rota do robô selecionado). viewBox 20x24.
    A bandeira usa a cor de quem carimba (currentColor)."""
    return f'''<symbol id="bandeira" viewBox="0 0 20 24">
  <ellipse cx="5" cy="22" rx="4" ry="1.6" fill="{C}" opacity="0.25"/>
  <rect x="3.8" y="2" width="2.4" height="20" rx="1.2" fill="{C}"/>
  <path d="M6 2.5 H18 L14.5 7 L18 11.5 H6 Z" fill="currentColor" stroke="{C}" stroke-width="1.6"
        stroke-linejoin="round"/>
</symbol>'''


def padroes():
    """Padrões de preenchimento: listras de advertência amarelo/preto e as
    "placas" do piso de concreto."""
    amarelo, preto = cores.ADVERTENCIA
    return f'''<pattern id="advertencia" width="12" height="12" patternUnits="userSpaceOnUse"
         patternTransform="rotate(45)">
  <rect width="12" height="12" fill="{amarelo}"/><rect width="6" height="12" fill="{preto}"/>
</pattern>
<pattern id="restricao" width="10" height="10" patternUnits="userSpaceOnUse"
         patternTransform="rotate(45)">
  <rect width="10" height="10" fill="#ddd6c8"/><rect width="2" height="10" fill="#cfc7b6"/>
</pattern>'''


def defs():
    """Todo o <defs> do mapa (vai uma vez em cada camada SVG)."""
    partes = [padroes(), simbolo_robo(), simbolos_led(), simbolo_caixa(), simbolo_palete(),
              simbolo_carregador(), simbolo_extintor(), simbolo_quadro(), simbolo_rosa(),
              simbolo_bandeira()]
    return "<defs>" + "\n".join(partes) + "</defs>"


def defs_robo():
    """Só os símbolos do robô (corpo e LEDs): para as miniaturas dos cards,
    que não precisam do <defs> inteiro do mapa."""
    return "<defs>" + simbolo_robo() + simbolos_led() + "</defs>"


NOMES_SIMBOLOS = ["robo", "led-ok", "led-defeito", "led-recarga", "led-manual", "caixa",
                  "palete", "carregador", "extintor", "quadro", "rosa", "bandeira"]
