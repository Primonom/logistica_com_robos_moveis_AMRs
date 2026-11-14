# logistica_com_robos_moveis_AMRs
Um sistema central de logística com robôs móveis (AMRs) num galpão 2D visto de cima. Cada carga vai de uma baia até uma zona de entrega, e cargas maiores exigem de 1 a 4 robôs juntos. Os robôs só acoplam e erguem a carga de forma síncrona, quando todos os necessários chegaram na baia.

## Arquitetura

- **Núcleo em C** (`./logistica` + um processo `./robo` por robô): Gestor de
  Robôs, Organizador, Coletor de Dados, **Simulador do hardware**
  (`src/simulador.c`: move o robô até o setpoint, simula bateria, recarga e
  garra) e Ponte MQTT. Detalhes de cada mecanismo de sincronização em
  `docs/EXPLICACAO.md`.
- **Tela de Operação** em Python/NiceGUI (`interface/`), via MQTT. O painel
  "Simulação" é também a tela do simulador (gerar defeito, baixar bateria,
  incluir caixas, criar robôs).

Tópicos MQTT:

| Tópico | Quem publica | Conteúdo |
|---|---|---|
| `atr/mapa` | C (retained) | JSON do mapa |
| `atr/status` | C, a cada 200 ms | robôs, caixas, passagens, contadores |
| `atr/eventos` | Coletor | cada registro gravado em `logs/*.csv` |
| `atr/comando` | Tela | linha igual à do Console (`falha 2`, `historico 50`...) |
| `atr/robo/novo` | Tela (simulador) | novo robô: `"x y"` ou vazio (posição padrão) |
| `atr/comando/resposta` | C | `ok`, a saída do comando, ou `erro: ...` |

O comando `historico [n]` devolve as últimas n linhas (padrão 30, máx. 100)
de `logs/eventos.csv` e `logs/cargas.csv`; na tela, botão "Carregar histórico
do disco" no painel Eventos.

Testes: `testes/rodar_todos.sh` (cenários pelo Console, sem precisar do
broker) e `interface/.venv/bin/python -m pytest -q interface`.

## Docker

Requisitos: Docker Desktop com integração WSL 2 habilitada e Docker Compose v2.
No PowerShell, a partir da raiz do repositório, inicie os serviços:

```powershell
docker compose up --build
```

A interface fica em http://localhost:8080. O broker MQTT fica acessível apenas
pela própria máquina em `localhost:1883`; internamente, os containers o acessam
como `broker:1883`. Os logs CSV são gravados na pasta `logs/` do repositório.

Para encerrar a logística com limpeza, digite `sair` no terminal onde está
rodando `docker compose up`. Depois, encerre os outros serviços com `Ctrl+C`
ou rode `docker compose down` em outro terminal PowerShell.
