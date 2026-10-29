# Roadmap M1 → M2 — esteira industrial

1. Conferir o enunciado da temática 2 e os exemplos do professor, sem alterar `codigos_viel`.
2. Usar os extremos documentados na M1: Custom, preempção ON, slicing ON, unicore; melhor = 80 MHz (0,01% de hard misses), pior = 160 MHz (0,63%). Não substituir por 240 MHz (0,62%). Qualquer hard miss impede declarar sucesso integral.
3. Manter as quatro tarefas e seus algoritmos. Acrescentar instrumentação comum com evento, início, fim, deadline e origem; registrar perdas da instrumentação. Conforme orientação do professor, salvar os tempos em variáveis e fila em RAM, sem prints seriais durante a medição, inclusive na HMI. Calcular as diferenças depois no PC.
4. Acrescentar MONITOR_TASK (T=D=500 ms): WiFi, logs, comandos SORT/ESTOP, protocolos UDP e TCP selecionáveis. Acrescentar TIME_TASK (T=D=1 s): SNTP assíncrono, hora e ciclos desde a última sincronização.
5. Criar cliente de bancada que registre mensagens brutas, timestamps do PC, RTT e respostas; criar análise de WCRT/HWM, misses, jitter, latência e (9,10)-firm para MONITOR/TIME. Ocupação de CPU deve vir de runtime/idle, não da soma de tempos de parede preemptados.
6. Entregar código e scripts para compilação e coleta pelo grupo. Conforme orientação posterior do usuário, não executar testes nem novas compilações nesta alteração.
7. Na bancada: quatro combinações (80/160 MHz × UDP/TCP), pelo menos três repetições por combinação, mesma duração e estímulos; incluir touch e comandos remotos. Salvar configuração e logs de cada execução.
8. Produzir resumo e roteiro detalhado, incluindo limites de sincronização, perdas, interferência/bloqueio e itens restantes para relatório e apresentação.

O roadmap descreve o trabalho autorizado pelo usuário. O PDF é a fonte dos requisitos acadêmicos; não autoriza publicação no GitHub ou envio ao professor.
