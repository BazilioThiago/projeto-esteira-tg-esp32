# Roadmap M1 → M2 — versão curta v3

1. Preservar PI, touch, ações e prioridades da M1; consultar e manter os exemplos de `codigos_viel`.
2. Acrescentar MONITOR (500 ms), TIME (1 s), WiFi/SNTP, UDP/TCP e comandos SORT/ESTOP.
3. Capturar evento/início/fim em variáveis e atualizar métricas em RAM, sem prints durante a janela.
4. Corrigir a referência temporal usando liberações absolutas do esp_timer para ENC/MONITOR/TIME, com os mesmos períodos/prioridades. Declarar o mecanismo na comparação com M1.
5. Controlar START e duração pelo PC, com identificador de sessão e resultado congelado recuperável. Eliminar dependência de transmitir todos os jobs para medir WCRT/misses/firm.
6. Calcular localmente HWM(100%), misses, latências, jitter, firm (9,10) e CPU via runtime/idle. Reter amostras de timestamps e registro do maior R. Calcular RTT no PC e registrar perdas como resultado da comunicação.
7. Executar pelo grupo quatro casos de 20 s: Custom/preempção ON/slicing ON/unicore, 80 e 160 MHz × UDP e TCP. Melhor/pior são os extremos documentados na M1. Toques breves nos três pinos, sem cronograma exato. Repetições adicionais se houver tempo.
8. Gerar JSON/CSV/Markdown automaticamente, montar tabela comparativa e relatório PDF, discutir interferência/bloqueio e demonstrar compilação/funcionamento presencial.

Roteiro vigente: `TESTES_M2.md`. Os 120 s, três repetições obrigatórias e horários exatos de toque foram retirados: eram escolhas anteriores do procedimento, não exigências fixadas no trecho da metodologia. O assistente não executa testes/compilação/gravação, conforme pedido do usuário. Logs antigos do grupo são preservados. Nenhuma publicação/envio ao professor é feita automaticamente.
