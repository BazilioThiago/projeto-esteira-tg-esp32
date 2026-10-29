# Esteira industrial — ESP32 / FreeRTOS — M1 + M2

Guilherme Santana e Thiago Bazilio — Sistemas em Tempo Real.

A M1 permanece em `main/hello_world_main.c`: encoder simulado, PI, triagem,
E-stop e HMI com touch. A M2 em `main/m2.c` acrescenta MONITOR/TIME,
WiFi, TCP/UDP, SNTP e instrumentação sem prints durante a medição.

## Coleta curta atual — v3

A ESP32 calcula WCRT/HWM(100%), misses, latências, jitter e (9,10)-firm em RAM.
O PC inicia uma janela de **20 s**, recebe resumos e busca o resultado congelado.
São mantidas amostras de timestamps, não o histórico de todas as execuções.
Perdas de pacotes são resultados da comunicação e não apagam os contadores locais.

1. `idf.py menuconfig`: configurar WiFi, protocolo e CPU do cenário.
2. `idf.py build` e `idf.py -p COM3 flash monitor` (ajustar porta).
3. Anotar o IP que aparece com **coleta curta v3**.
4. Em outro terminal:

```powershell
python tools/m2_pc.py collect --ip 192.168.2.54 --proto UDP --scenario melhor --freq 80
```

Trocar o IP pelo da placa. Ao aparecer MEDINDO, tocar algumas vezes nos pinos
27 (B), 33 (C) e 32 (D), sem horários exatos. O programa espera SNTP, controla a
janela e gera JSONL, JSON, CSV e Markdown com nomes únicos em `resultados`.
Repetir para TCP/80, UDP/160 e TCP/160 com o firmware correspondente.

- [Passo a passo completo](docs/TESTES_M2.md)
- [Resumo das mudanças e limites](docs/RESUMO_M2.md)
- [Roadmap](docs/ROADMAP_M2.md)

Usar ESP-IDF 6.1, ESP32 com touch, unicore e runtime stats/trace habilitados.
As tasks periódicas usam esp_timer com alarme absoluto; o PI e as prioridades
continuam os da M1. Os exemplos do professor em `codigos_viel` são preservados.
O coletor atual está em `tools/m2_short.py`, chamado por `tools/m2_pc.py`;
a análise manual de arquivos antigos continua disponível.

Não foram executados testes, compilação ou gravação desta revisão pelo assistente.
Resultados antigos do grupo não foram apagados ou modificados por esta alteração.
`pytest_hello_world.py` é legado do exemplo ESP-IDF e não valida esta aplicação.
Não publicar credenciais reais do WiFi presentes no sdkconfig.
