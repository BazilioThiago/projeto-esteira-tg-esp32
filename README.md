# Esteira industrial - STR M2

Guilherme Santana e Thiago Bazilio. ESP32 classica, ESP-IDF 6.1, FreeRTOS UNICORE.
RPM, PWM e desvio simulados. Fonte original da M1 preservado em `docs/m1_original.c`, fora do build.

A M2 acrescenta MONITOR (500 ms), TIME (1 s), Wi-Fi UDP/TCP, SNTP, comandos do PC,
logs por job e analise de WCRT observado, HWM99 e (9,10)-firm nas tarefas soft.
Inclui DM/custom, configuracao global de preempcao/slicing e overlays 80/160/240 MHz.

**Estado:** fontes e roteiro preparados; compilacao e ensaios na placa pendentes.
Nenhum resultado experimental foi inventado. Nenhum firmware foi gravado.
A tentativa inicial de build falhou por acesso do ambiente e nao foi repetida
apos a orientacao para nao buildar. O sdkconfig original foi preservado.

| Arquivo | Funcao |
|---|---|
| `main/hello_world_main.c` | Esteira, filas, touch e prioridades |
| `main/m2.c` | Instrumentacao, Wi-Fi, UDP/TCP, SNTP |
| `main/Kconfig.projbuild` | Parametros de menuconfig |
| `config/scheduler_config.h` | Macros aplicadas tambem ao kernel |
| `config/*.defaults` | Configuracoes separadas por experimento |
| `tools/pc_monitor.cpp` | Coleta, comandos e analise em C++17 |
| `docs/m2/ROTEIRO.md` | Preparacao e matriz de testes |
| `docs/m2/PROTOCOLO.md` | Campos e limites das medidas |
| `docs/m2/RELATORIO.md` | Relatorio-base, resultados pendentes |

Touch B=T7/GPIO27, C=T8/GPIO33, D=T9/GPIO32. Nao toque durante a calibracao
inicial de 100 ms. D trava a parada ate reiniciar. GPIO2 acende na parada;
ajuste/desative para outras placas. Melhor/pior cenario e melhor protocolo
dependem dos ensaios descritos no roteiro.
