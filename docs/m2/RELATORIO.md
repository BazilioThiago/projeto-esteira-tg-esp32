# Esteira industrial: latência, WCRT e medições

Universidade do Vale do Itajaí - Engenharia de Computação
Disciplina: Sistemas em Tempo Real | Professor: Felipe Viel
Autores: Guilherme Santana e Thiago Bazilio | Avaliação M2 | Outubro de 2026

**RELATÓRIO-BASE - RESULTADOS EXPERIMENTAIS PENDENTES**

## 1. Problema e objetivo

O sistema didático emula uma esteira industrial em ESP32 com FreeRTOS: amostragem de velocidade, controle PI, triagem de peças, interface e parada de emergência. Sensores touch geram eventos; RPM, posição, PWM e atuadores são simulados. A implementação reaproveita os fluxos da M1 e acrescenta comunicação Wi-Fi e sincronização de tempo.

O enunciado da M2 solicita testar as configurações de melhor e pior atendimento da M1, comparar UDP e TCP, registrar timestamps, medir WCRT por tarefa, avaliar tarefas soft por (m,k)-firm e discutir interferência e bloqueio. O trabalho exige evidências experimentais e relatório em PDF. Este documento prepara a metodologia; não substitui essas evidências.

## 2. Modelo de tarefas

| Tarefa | Ativação | D (ms) | Classe | Justificativa |
|---|---|---|---|---|
| ENC_SENSE | Periódica: 5 ms | 5 | Hard | Amostragem alimenta controle |
| SPD_CTRL | Fim do encoder | 10 | Hard | Atualização tempestiva do PWM |
| SORT_ACT | Touch B ou PC | 10 | Hard | Janela física de triagem |
| SAFETY_TASK | Touch D ou PC | 5 | Hard | Parada com baixa latência |
| MONITOR_TASK | Periódica: 500 ms | 500 | Soft | Telemetria e comandos de supervisão |
| TIME_TASK | Periódica: 1000 ms | 1000 | Soft | Manutenção da referência temporal |
| HMI | Touch C, dentro de CTRL | 500* | Soft | Exibição pode tolerar perda ocasional |

*D de HMI é uma escolha deste projeto. HMI é um trecho de CTRL, não uma sétima task. Nas três funções soft adota-se (9,10)-firm como critério experimental, sem afirmar garantia prévia.

A parada local é o caminho hard. O comando remoto atravessa MONITOR com polling de 500 ms; seu prazo de 5 ms é contado da interpretação na ESP32, e não do envio no PC. A latência completa desse caminho é analisada separadamente por RTT e timestamps.

---

## 3. Reaproveitamento e correções da M1

Mantiveram-se as quatro funções da esteira, o controlador PI, os pads touch T7/T8/T9 e o comportamento de parada travada até reiniciar. O fonte anterior está preservado em docs/m1_original.c. As cargas simuladas agora executam quantidade fixa de operações, permitindo observar o efeito de variar a frequência. As durações nominais da M1 não são consideradas medidas de WCET.

| Política | ENC | CTRL | SORT | SAFETY | MONITOR | TIME |
|---|---|---|---|---|---|---|
| DM | 7 | 5 | 4 | 6 | 3 | 2 |
| Custom | 5 | 4 | 6 | 7 | 3 | 2 |

Maior número significa maior prioridade. DM ordena pelo menor deadline; em empates prioriza ENC antes de SAFETY e CTRL antes de SORT. Custom privilegia segurança e triagem. As duas distribuições são distintas sem violar a regra de DM. RM é extra da M1 e não foi incluído nesta implementação.

A configuração atual da M1 já continha UNICORE. A lacuna identificada na revisão é tratada tornando essa configuração explícita, exigida no fonte e registrada por cenário. A anotação de falta de testes de frequência requer novas evidências; não é possível confirmar o que foi apresentado anteriormente apenas pelo código.

Preempção e time slicing são configurados globalmente, inclusive para o kernel, por um header do projeto. No modo cooperativo, a ISR touch não força troca; as tarefas cedem ou bloqueiam. Drivers e ISRs do SDK podem solicitar yields explícitos. A aplicação dessa configuração às unidades do kernel deve ser confirmada na compilação futura.

## 4. Interferência e bloqueio

Em preemptivo DM: ENC pode sofrer interferência de serviços de prioridade superior do SDK; SAFETY também de ENC; CTRL de ENC/SAFETY; SORT de ENC/SAFETY/CTRL; MONITOR de todas as quatro tarefas; TIME também de MONITOR. Em custom a ordem da aplicação é SAFETY, SORT, ENC, CTRL, MONITOR, TIME. Interrupções e Wi-Fi acrescentam interferência em ambos os modos.

No cooperativo existe espera por jobs que ainda não cederam, inclusive de prioridade inferior. Um mutex com herança de prioridade protege o estado da esteira: ENC, CTRL, SORT e SAFETY podem esperar por um detentor de menor prioridade. As seções são curtas e excluem carga artificial, rede e impressão. Herança limita inversão, sem eliminar bloqueio. HMI executa na prioridade de CTRL e pode interferir nas tarefas inferiores.

---

## 5. Instrumentação e cálculo

Cada job registra tarefa, origem, número sequencial, evento, início, fim, deadline e espera para adquirir mutex. Eventos não são reduzidos a um timestamp global sobrescrito. O buffer de logs é consumido por MONITOR; as tarefas críticas não imprimem por job. Filas cheias e perdas de transporte são contabilizadas.

| Indicador | Definição |
|---|---|
| Resposta R | fim - evento |
| Miss | R maior que D |
| WCRT observado / HWM100 | Maior R da amostra |
| HWM99 | Percentil nearest rank: posição ceil(0,99 n) |
| Latência de ativação | início - evento |
| Span | fim - início; inclui bloqueio e preempção |
| Ocupação de CPU | 1 - delta runtime idle / delta runtime total |
| (9,10)-firm | Nove sucessos em cada dez jobs consecutivos |

WCRT observado não é limite formal para todas as execuções possíveis. HWM99 pode ocultar extremos; decisões hard devem examinar HWM100. Span não é tempo exclusivo C de CPU nem WCET provado. As durações máximas observadas e o tamanho das amostras devem acompanhar as conclusões.

As tarefas periódicas recebem liberações de esp_timer. O evento é marcado no callback, antes da fila: espera e interferência posteriores entram em R; atraso do serviço de timer anterior ao callback exige análise adicional de jitter. Touch mede desde a entrada na ISR, não desde o contato físico.

Nas tarefas soft contam-se todas as janelas deslizantes completas. Gaps de IDs interrompem as janelas no analisador. Menos de dez execuções resulta em N/A. Eventos descartados, logs perdidos ou amostras ausentes impedem concluir que todos os deadlines foram atendidos.

## 6. Comunicação e sincronização

MONITOR envia logs e recebe SORT/ESTOP/PING a cada 500 ms. UDP e TCP usam o mesmo conteúdo, comandos e frequência de polling. TCP recompõe linhas e mantém escritas parciais; UDP não tem retry da aplicação. ACK confirma aceitação na fila, não conclusão da ação. O PC pode acionar o LED de parada pela task SAFETY, atendendo ao experimento extra previsto no enunciado, sujeito à validação na placa.

TIME inicia SNTP e monitora as atualizações assíncronas. O log inclui UTC em microssegundos, validade da sincronização, idade, iterações TIME e ciclos de CPU desde a última sync. CCOUNT é acumulado em 64 bits; o intervalo entre leituras precisa ser menor que seu wrap, aproximadamente 17,9 s a 240 MHz.

---

## 7. Plano experimental

Primeiro refazer DM/custom x preemptivo/cooperativo x 80/160/240 MHz em UNICORE, sem Wi-Fi/TIME, com logger UART a 921600 baud. São 12 cenários de base. Usar ao menos 30 s úteis, preferencialmente 35 s e três repetições, mantendo carga e sequência de toques comparáveis. Registrar a interferência do logger. Não existem resultados suficientes para escolher melhor/pior previamente.

Selecionar melhor/pior pela taxa agregada de misses hard, incluindo descartes como falhas, e desempatar por latência máxima de SAFETY e WCRT. Empates devem ser declarados. Repetir as duas configurações com UDP e TCP e as duas novas tasks ativas: quatro combinações M2, idealmente três repetições cada.

Sincronizar o PC por NTP; aguardar SNTP válido na ESP e registrar warm-up e recortes. Medir RTT com relógio monotonic do PC, sem depender de relógios comuns. Estimativas de ida/volta por UTC exigem ambos sincronizados e incorporam erro de relógio, filas e polling. Não presumir simetria nem usar RTT/2 como medição de atraso unidirecional.

## 8. Resultados a preencher com dados reais

| Base M1 | Política / modo / MHz | Misses hard | Lat. SAFETY | Evidência |
|---|---|---|---|---|
| Melhor | Pendente | Pendente | Pendente | Log + sdkconfig |
| Pior | Pendente | Pendente | Pendente | Log + sdkconfig |

| Cenário M2 | Protocolo | WCRT por task | (9,10) | RTT / perdas |
|---|---|---|---|---|
| Melhor M1 | UDP | Pendente | Pendente | Pendente |
| Melhor M1 | TCP | Pendente | Pendente | Pendente |
| Pior M1 | UDP | Pendente | Pendente | Pendente |
| Pior M1 | TCP | Pendente | Pendente | Pendente |

Anexar tabelas por tarefa geradas pelo utilitário C++, com n, misses, HWM100/99, latência, span, espera de mutex, jitter e janelas firm. Incluir CPU, sequência de pacotes, perdas de logs/eventos, timestamps e capturas do LED. Informar placa, rede, servidor NTP, versão IDF, duração e repetições. Percentuais sem amostra e zeros com n=0 não são evidências.

## 9. Estado e conclusão provisória

A implementação e o protocolo de ensaio estão preparados. Não há compilação concluída ou ensaio de hardware nesta revisão; a compilação permanece pendente por orientação do aluno. Portanto, não se declara atendimento dos prazos, melhor configuração ou superioridade de UDP/TCP. A versão final deve substituir esta conclusão pelas evidências coletadas e pela discussão das interferências e perdas observadas.

## Referências

Viel, Felipe. Avaliação M1 - Modelagem de Sistema. UNIVALI, 2026. Enunciado fornecido pelo aluno.

Viel, Felipe. Avaliação M2 - Latência, WCRT e Medições. UNIVALI, 2026. Enunciado fornecido pelo aluno.

Espressif. ESP-IDF v6.1, fontes locais: FreeRTOSConfig.h, task.h e esp_sntp.h. Documentação oficial: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/system_time.html
