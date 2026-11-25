# Esteira industrial — resultados da M2

## Introdução

Este trabalho amplia o sistema de esteira industrial desenvolvido na M1 com comunicação WiFi e sincronização de horário por SNTP. Foram adicionadas as tarefas MONITOR, responsável pela comunicação com o PC, e TIME, responsável pela leitura da hora sincronizada e pelo acompanhamento dos ciclos desde a última atualização. O objetivo foi comparar UDP e TCP e observar o impacto da comunicação no atendimento dos prazos das seis tarefas.

## Metodologia

Foram realizados quatro ensaios de 20 segundos: UDP e TCP a 80 MHz e a 160 MHz. As frequências correspondem, respectivamente, aos cenários de melhor e pior atendimento registrados na M1. Mantiveram-se prioridades Custom, execução unicore, preempção e time slicing ligados.

Durante os testes, o PC enviou comandos automáticos SORT, PING e ESTOP, e os sensores touch geraram eventos de objeto, HMI e emergência. Os timestamps de evento, início e fim foram capturados em variáveis, sem impressão serial durante a medição. A ESP32 calculou contadores, máximos e janelas firm em RAM, enviando resumos e amostras dos registros ao PC. O WCRT corresponde ao maior tempo de resposta observado, equivalente ao HWM(100%). A ocupação de CPU foi estimada pelas estatísticas de runtime/idle.

O horário de envio foi registrado com referência SNTP. O RTT foi calculado pelo relógio monotônico do PC, do envio do comando até sua resposta. Portanto, inclui a espera pelo ciclo de 500 ms da MONITOR, além da comunicação, e não representa somente o atraso da rede. Os dados temporais por tarefa vieram dos quatro CSVs de análise; RTT, CPU e perdas de ativações foram obtidos dos JSONs correspondentes.

## Resultados

### WCRT / HWM(100%) por tarefa

Todos os tempos da tabela estão em milissegundos.

| Tarefa | Deadline | UDP / 80 MHz | TCP / 80 MHz | UDP / 160 MHz | TCP / 160 MHz |
|---|---:|---:|---:|---:|---:|
| ENC | 5 | 5,488 | 6,196 | 3,776 | 6,636 |
| CTRL | 10 | 3,298 | 8,525 | 2,445 | 5,797 |
| SORT | 10 | 4,453 | 3,477 | 1,588 | 1,908 |
| SAFETY | 5 | 3,489 | 3,145 | 2,625 | 1,702 |
| MONITOR | 500 | 29,563 | 39,231 | 21,037 | 26,108 |
| TIME | 1.000 | 3,123 | 7,449 | 2,564 | 2,460 |

### Comparação dos cenários

| Cenário da M1 | Protocolo | Misses ENC (n / %) | Ativações perdidas | RTT médio (ms) | RTT máximo (ms) | CPU (%) | Latência touch SAFETY máx. (ms) |
|---|---|---:|---:|---:|---:|---:|---:|
| Melhor — 80 MHz | UDP | 7 / 0,175% | 7 | 480,695 | 503,104 | 47,015 | 2,582 |
| Melhor — 80 MHz | TCP | 9 / 0,225% | 16 | 503,415 | 521,532 | 48,749 | 2,242 |
| Pior — 160 MHz | UDP | 0 / 0% | 0 | 483,212 | 500,838 | 44,169 | 1,691 |
| Pior — 160 MHz | TCP | 3 / 0,075% | 6 | 1.086,406 | 4.824,602 | 46,716 | 0,362 |

A ENC executou 4.000 vezes em cada cenário, a MONITOR 40 vezes e a TIME 20 vezes, sem timestamps inválidos. As demais tarefas não apresentaram misses nos jobs concluídos. As perdas de ativações/notificações são contabilizadas separadamente dos misses e não devem ser confundidas com perdas de pacotes. Nos quatro casos, os 20 comandos enviados receberam resposta e não houve lacunas nas sequências dos pacotes recebidos.

MONITOR e TIME atenderam ao modelo (9,10)-firm nos quatro cenários. Foram avaliadas 31 janelas para MONITOR e 11 para TIME em cada ensaio, sem violações e com dez prazos atendidos em todas as janelas avaliadas.

Pela distribuição de prioridades, SAFETY pode interferir nas demais tarefas; SORT pode interferir em ENC, CTRL, MONITOR e TIME; ENC pode interferir em CTRL e nas tarefas soft. MONITOR e TIME compartilham prioridade. Tarefas internas de WiFi, rede e timer também interferem. Não há inversão de prioridade clássica por mutex de aplicação no caminho hard, mas existem seções críticas e mecanismos internos da pilha de rede que podem adiar a execução.

## Conclusão

O UDP apresentou menor RTT médio que o TCP nas duas frequências. O melhor resultado global observado foi UDP a 160 MHz, único cenário sem misses ou perdas de ativações, com ocupação estimada de CPU de 44,169%. O TCP a 160 MHz apresentou a maior demora de comunicação, com RTT máximo de aproximadamente 4,825 segundos. Já TCP a 80 MHz apresentou a maior quantidade de misses da ENC: nove.

As tarefas de segurança e triagem atenderam aos prazos nas execuções observadas, e as tarefas soft cumpriram o critério firm. Entretanto, não houve atendimento integral em todos os testes, devido aos misses da ENC e às perdas de ativações nos demais cenários. Os rótulos melhor/pior vieram da M1 e não determinam o resultado da M2. Como houve um ensaio por cenário e a quantidade de eventos touch variou, as conclusões se limitam às condições medidas e não demonstram superioridade geral de um protocolo.
