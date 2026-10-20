# logistica_com_robos_moveis_AMRs
Um sistema central de logística com robôs móveis (AMRs) num galpão 2D visto de cima. Cada carga vai de uma baia até uma zona de entrega, e cargas maiores exigem de 1 a 4 robôs juntos. Os robôs só acoplam e erguem a carga de forma síncrona, quando todos os necessários chegaram na baia.

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
