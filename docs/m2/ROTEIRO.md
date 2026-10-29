# Roteiro de ensaio

Os comandos abaixo sao instrucoes para execucao posterior pelo aluno.
Nenhuma compilacao ou gravacao foi concluida nesta revisao.

## Preparar

ESP32 classica com touch T7/T8/T9, ESP-IDF 6.1. Copie
`config/network.example.defaults` para `config/network.local.defaults` e edite
SSID/senha da rede 2,4 GHz e IPv4 do PC. A porta padrao e 3333. Libere UDP/TCP
no firewall. O arquivo local e os sdkconfigs `*.local` sao ignorados pelo Git.
Nao publique senhas. Crie `output/measurements` para as coletas.

Em menuconfig, `Esteira M2` permite escolher protocolo, politica, preempcao,
slicing, LED, SNTP e multiplicador de carga. UNICORE e tick=1000 Hz sao exigidos
no fonte. Frequencia deve ser fixa; nao habilite DFS/light sleep nos ensaios.
Os runtime stats usam esp_timer. Os valores nominais de C da M1 nao sao
medicoes: calibre a carga em 240 MHz e congele o multiplicador para comparar.
Agora a carga e um numero fixo de operacoes; a espera por tempo da M1 mantinha
a duracao de parede mesmo ao mudar a frequencia e nao fazia a mesma comparacao.

## Configurar cada cenario

Use arquivos/builds novos por cenario: sdkconfig existente prevalece sobre defaults.
Exemplo para executar depois, em terminal ESP-IDF:

```powershell
idf.py -B build/dm-pre-240-udp -D SDKCONFIG=sdkconfig.dm-pre-240-udp.local -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;config/network.local.defaults;config/dm.defaults;config/preemptive.defaults;config/240mhz.defaults;config/udp.defaults" build
idf.py -B build/dm-pre-240-udp -p COM3 flash monitor
```

Overlays: `dm/custom`, `preemptive/cooperative`, `80mhz/160mhz/240mhz`,
`m1-baseline/udp/tcp`. Preemptivo usa slicing=1; cooperativo usa slicing=0.
O extra de slicing exige variar somente esse parametro em preemptivo.

Confira o banner inicial e o cabecalho B do log. Fixar tarefas no core 0 nao
substitui UNICORE. O diretorio `config/freertos_override` entra antes dos
headers do SDK e fornece wrappers para as duas formas de incluir FreeRTOSConfig.h.
Eles carregam o header oficial e depois `scheduler_config.h`, alterando tambem
o kernel, pois o IDF 6.1 fixa as macros em 1. Nao ha inclusao forcada via -include:
ela causava conflito com __getreent na picolibc. Apos configurar novamente,
verifique o caminho `-I...config/freertos_override` antes dos includes do FreeRTOS
nas entradas de tasks.c, queue.c e port.c do compile_commands.json, e confirme
as macros no pre-processamento.
Essa verificacao ainda esta pendente; o banner sozinho nao a comprova.

## Refazer a base M1

DM/custom x preemptivo/cooperativo x 80/160/240 MHz = 12 cenarios. Use o overlay
`m1-baseline` no lugar de `udp`, com nomes proprios para SDKCONFIG e build.
Esse modo desliga Wi-Fi/TIME e envia logs pela UART a **921600 baud**; mantem
as quatro tarefas da esteira mais um logger de baixa prioridade. Registre a
carga desse logger na analise. Salve o monitor serial por pelo menos 35 s:

```powershell
idf.py -B build/m1-dm-pre-240 -p COM3 monitor --no-reset | Tee-Object output/measurements/m1-dm-pre-240.csv
```

Minimo 30 s uteis por cenario; recomendam-se 35 s e tres repeticoes. Em cada
repeticao: 0-10 s B isolado; 10-20 s B/C alternados e em rajadas; 20-30 s
B/C/D proximos ou simultaneos. Reinicie entre repeticoes para liberar a parada.
Toque mantido pode produzir varias interrupcoes: contam-se ISRs, nao gestos.

Escolha melhor/pior pela taxa agregada de misses hard; desempate por latencia
maxima de SAFETY e WCRT. Eventos descartados sao falhas. Se empatar, declare o
empate. Compare coletas completas com mesma duracao, carga e estimulos.

## Coletar M2 pelo PC

O utilitario usa C++17 e sockets do sistema, sem bibliotecas de terceiros.
Para preparar posteriormente, no Developer Command Prompt do Visual Studio:

```text
cl /EHsc /std:c++17 tools\pc_monitor.cpp /Fe:tools\pc_monitor.exe /Fo:tools\pc_monitor.obj ws2_32.lib
```

Linux: `c++ -std=c++17 -O2 tools/pc_monitor.cpp -o tools/pc_monitor`.
O utilitario tambem nao foi compilado nesta revisao.

Para cada melhor/pior configuracao M1, repita UDP e TCP com MONITOR/TIME ativos:
quatro combinacoes, tres repeticoes recomendadas. Sincronize tambem o PC por NTP
e registre servidor/estado da sincronizacao. Inicie o PC antes de reiniciar a ESP:

```text
tools\pc_monitor.exe udp 3333 35 output/measurements/melhor-udp-r1.csv mixed
tools\pc_monitor.exe tcp 3333 35 output/measurements/melhor-tcp-r1.csv mixed
```

Modos: ping (so RTT), sort, estop e mixed (ESTOP/SORT/PING alternados). Comando
a cada 500 ms, sem retry UDP. ACK significa aceitacao na fila, nao conclusao;
confirme o evento E. Nao ha rearme remoto. Reinicie entre ensaios.

A coleta comeca no primeiro dado recebido. Confira B com SNTP=1. Se a sync
chegar tarde, repita com maior duracao para ter >=30 s sincronizados. Registre
warm-up e recortes; nao descarte silenciosamente misses ou janelas firm.

## Analisar e entregar

```text
tools\pc_monitor.exe analyze output/measurements/melhor-udp-r1.csv output/measurements/melhor-udp-r1
```

Gera `_tasks.csv` e `_summary.md`: WCRT, HWM99, misses, latencia, span, espera de
mutex, jitter, (9,10), CPU e RTT. Aceita tambem CSV puro do baseline UART, ignorando
boot/monitor. Nao misture boots/configuracoes no mesmo arquivo.

Confirme n>0 para tarefas esperadas e n>=10 para janelas firm. TIME/MONITOR no
baseline sao N/A. S e cumulativo desde boot; E reflete o recorte recebido.
Examine perdas, gaps e reordenacoes antes de concluir. Sem coleta completa nao
se pode afirmar zero misses. ACKs pendentes ao fim podem ainda estar em transito.

Anexe logs brutos, tabelas, sdkconfigs sem senhas, prints/fotos do LED, placa,
rede e versao IDF. Preencha RELATORIO.md e atualize o PDF final com os dados reais.
