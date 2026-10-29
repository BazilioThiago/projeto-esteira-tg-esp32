# Roteiro de testes da M2 — para execução pelo grupo

Este é o passo a passo **a executar**, não um registro de ensaios já realizados. A pedido do usuário, não foram feitos testes nem concluída a compilação nesta alteração.

## 1. Fixar os cenários da M1

O relatório `docs/Relatório Trabalho.docx` apresenta Custom, preempção ON, slicing ON e 80/160/240 MHz. Pelas porcentagens de hard misses, melhor = 80 MHz (0,01%), pior = 160 MHz (0,63%); 240 MHz teve 0,62%. Logo, usar:

|Cenário|Prioridades SAFETY/SORT/ENC/CTRL/MONITOR/TIME|Preempção|Slicing|CPU|Core|Protocolo|
|---|---|---|---|---|---|---|
|melhor UDP|5/4/3/2/1/1|ON|ON|80 MHz|unicore, 0|UDP|
|melhor TCP|5/4/3/2/1/1|ON|ON|80 MHz|unicore, 0|TCP|
|pior UDP|5/4/3/2/1/1|ON|ON|160 MHz|unicore, 0|UDP|
|pior TCP|5/4/3/2/1/1|ON|ON|160 MHz|unicore, 0|TCP|

Executar pelo menos três repetições de 120 s por combinação (12 coletas). O objetivo é repetir os extremos reais da M1, não criar novas políticas RM/DM que não constam dos resultados apresentados. Não mudar slicing/preempção, carga, tráfego ou prioridades entre UDP e TCP.

## 2. Preparar a rede e a placa

1. Usar a ESP32 com touch da M1. B=TOUCH7/GPIO27, C=TOUCH8/GPIO33, D=TOUCH9/GPIO32. Conferir os fios antes de energizar. Sem tocar nos pads durante a calibração inicial.
2. Conectar PC e ESP32 à mesma rede WiFi compatível com a ESP32. Permitir comunicação entre clientes do ponto de acesso. Usar mesmo local, distância, AP e alimentação em todos os ensaios.
3. Liberar no PC, se necessário, tráfego de saída e respostas da porta 5000 escolhida. SNTP precisa de acesso UDP/123 ao servidor configurado. Não é necessário executar `codigos_viel/tcp_client.py` ou `udp_server.py`: o novo cliente já faz coleta e comandos nos dois protocolos.
4. Usar Python 3 no PC; o coletor não exige pacotes adicionais.
5. No terminal ESP-IDF do VS Code, abrir esta pasta. O ambiente configurado no projeto aponta para ESP-IDF 6.1. Se a ativação automática selecionar Python errado, usar no PowerShell:

```powershell
$env:IDF_PYTHON_ENV_PATH = 'C:\Users\thiag\.espressif\python_env\idf6.1_py3.11_env'
$env:PYTHONUTF8 = '1'
. C:\Users\thiag\esp\v6.1\esp-idf\export.ps1
```

## 3. Configurar e compilar cada combinação

```powershell
idf.py menuconfig
```

Em **Esteira - M2**, informar SSID/senha, escolher UDP ou TCP e manter porta 5000 e SNTP `a.st1.ntp.br`. O intervalo de consulta SNTP padrão é 60000 ms. TIME continua executando a cada 1000 ms. Se a placa tiver LED acessível, conferir seu GPIO e configurar o extra; o padrão -1 não aciona LED.

Manter FreeRTOS unicore, tick 1000 Hz, trace/runtime stats habilitados e relógio de estatísticas ESP timer. Escolher frequência padrão de CPU 80 ou 160 MHz nas configurações do sistema. Salvar. Não publicar sdkconfig com senha real.

```powershell
idf.py build
idf.py -p COM3 flash monitor
```

Ajustar COM3 se necessário. Anotar o IP e o protocolo do aviso `M2 IP=...`, emitido somente no aquecimento. Durante a coleta não há prints de HMI ou logs de medição na UART, e os logs normais do ESP-IDF são silenciados ao iniciar a sessão. As medições são guardadas em RAM e enviadas ao PC pela MONITOR. Salvar uma cópia da configuração sem senha junto aos resultados e registrar o commit/versão usada.

## 4. Aquecer e iniciar a coleta

1. Aguardar aproximadamente 20 s para conexão e primeira atualização SNTP. O campo `synced` nos pacotes confirma se ocorreu. Não presumir sincronização só porque há WiFi.
2. Iniciar a coleta em outro terminal. Usar um arquivo novo para cada repetição: o script não sobrescreve coletas existentes.

```powershell
python tools/m2_pc.py collect --ip 192.168.1.50 --proto UDP --scenario melhor --freq 80 --seconds 120 --out resultados/melhor_udp_r1.jsonl
```

Trocar o IP pelo impresso pela placa. O PC consulta SNTP uma vez antes da coleta e registra offset/RTT da sincronização. Se essa consulta falhar, o RTT de aplicação continua utilizável, mas a análise marca que não há referência para atraso em um sentido.

No primeiro contato, o firmware descarta os logs de aquecimento e zera os contadores de perda da coleta. Não desconectar nem trocar de PC durante a repetição. Reiniciar a ESP32 antes de **cada** repetição; isso reinicia o E-stop travado e também a sessão UDP.

## 5. Aplicar os estímulos

O coletor envia a cada 1 s o ciclo `SORT, PING, SORT, PING, ESTOP`, idêntico nos dois protocolos. Assim, aciona SORT e SAFETY remotamente, com ACK correlacionado pelo número do comando. `PING` mede o mesmo caminho de comunicação sem acionar tarefa hard. O protocolo da requisição é uma linha `sequencia,pc_epoch_ms,COMANDO`; respostas são JSON separados por newline.

Durante cada repetição, aplicar manualmente a mesma sequência, usando um cronômetro:

|Tempo após iniciar coleta|Estímulo|
|---|---|
|10, 30, 50, 70, 90 e 110 s|Touch B: objeto/SORT|
|15, 35, 55, 75, 95 e 115 s|Touch C: HMI|
|20, 40, 60, 80 e 100 s|Touch D: E-stop/SAFETY|

Fazer contatos breves. A ISR pode registrar vários eventos durante um toque; documentar rebotes/repetições. Latência de touch é medida a partir da ISR que reconhece o touch, não desde o primeiro contato físico. Eventos remotos têm origem `pc` e não entram na coluna de latência touch.

O E-stop continua travado até reiniciar, como na M1. Chamadas seguintes medem novamente o caminho da task, mas o motor simulado já está parado. O cenário mede temporização e comunicação; não representa ciclos completos de retomada de produção. Anotar esse comportamento no relatório.

Ao fim, o coletor reserva 2 s sem novos comandos para receber respostas pendentes. Não desligar a placa nesse intervalo.

## 6. Repetir os quatro casos

1. Melhor/80 MHz/UDP: repetições r1, r2 e r3, reiniciando antes de cada uma.
2. Melhor/80 MHz/TCP: selecionar TCP, compilar/gravar e repetir. Exemplo:

```powershell
python tools/m2_pc.py collect --ip 192.168.1.50 --proto TCP --scenario melhor --freq 80 --seconds 120 --out resultados/melhor_tcp_r1.jsonl
```

3. Pior/160 MHz/UDP: mudar CPU e protocolo, compilar/gravar e repetir.
4. Pior/160 MHz/TCP: mudar protocolo, compilar/gravar e repetir.

Usar `--scenario pior --freq 160` nos dois últimos. Alternar a ordem dos protocolos entre repetições, se possível, para reduzir influência de mudanças na rede. Não chamar uma configuração de melhor/pior da M2 apenas por herdar o nome da M1.

## 7. Analisar cada arquivo

```powershell
python tools/m2_pc.py analyze resultados/melhor_udp_r1.jsonl --out resultados/analise_melhor_udp_r1 --m 9 --k 10
```

O script gera `.json` com detalhes, `.csv` por task e `.md` para leitura. Repetir para os 12 arquivos. Não passar um prefixo de saída que coincida com o arquivo de entrada. As fórmulas são:

Conforme a orientação do professor, os tempos não são obtidos de prints no monitor. O firmware salva evento, início e fim em variáveis usando `esp_timer_get_time()`; depois do timestamp final, copia o registro para uma fila em RAM. O PC subtrai os timestamps. As unidades são microssegundos, convertidos em milissegundos na análise, sem estimar ciclos por multiplicação pela frequência da CPU.

- Resposta `R = end - evt`; WCRT observado/HWM(100%) = maior R. HWM(99%) usa percentil nearest-rank.
- Tempo de execução observado `C_parede = end - start`, incluindo preempções. Não equivale a tempo exclusivo de CPU.
- Latência de ativação `L = start - evt`. Touch usa somente origem `touch`; para PC o evento ocorre no despacho local do comando, após a espera na rede e no monitor.
- Miss quando `R > D`. Contagem e porcentagem por task e classe. Exatamente em D é atendimento.
- Jitter: maior desvio absoluto entre intervalos consecutivos de início e período nominal. CTRL tem gatilho encadeado com período nominal de ENC; não é uma task periódica independente.
- CPU: `100 × (1 - delta_idle / delta_total)` das estatísticas runtime, incluindo tarefas internas. O primeiro intervalo é marcado como -1 e descartado. É estimativa do runtime do escalonador, não medição elétrica de ocupação.
- (9,10)-firm: em cada janela deslizante de dez jobs soft, pelo menos nove devem cumprir D. JSON/CSV informam número de janelas, violações e mínimo atendido. Menos de dez amostras não permite avaliação. Janelas baseadas em logs incompletos não sustentam uma conclusão de atendimento.
- RTT: tempo monotônico do envio PC até a recepção do ACK. Inclui rede, espera pela MONITOR (até aproximadamente um período, ou mais se atrasar), despacho e resposta; não exige sincronização entre relógios.

O campo `bytes` é o número de bytes da mensagem de aplicação, incluindo newline; não inclui cabeçalhos WiFi/IP/TCP/UDP nem retransmissões. `actual_bytes` permite conferir o tamanho recebido. Sequências detectam lacunas internas e duplicatas; não detectam, sozinhas, pacotes perdidos no início/fim da coleta.

O horário `esp_ms` é UTC Unix com milissegundos. `tx_us` e timestamps de jobs usam relógio monotônico desde boot: não subtrair um desses tempos de um timestamp UTC. O timestamp de envio é capturado antes da chamada de socket; bufferização TCP pode acrescentar atraso até a transmissão física.

`down_ms_estimate = pc_rx_ms - esp_ms` só é registrado quando ESP32 e PC indicam SNTP. Não constitui prova de atraso em um sentido: erro de sincronização, assimetria, drift e resolução em ms podem produzir valores negativos. `up_ms_estimate` do ACK mede envio PC até geração do ACK na ESP32 e inclui o despacho; não é atraso puro no uplink. Para comparação principal, preferir RTT. Conservar o offset/RTT SNTP e `sync_age_ms` como evidência dessas limitações. O PC consulta SNTP uma vez por execução; a ESP32 se ressincroniza periodicamente.

## 8. Conferir integridade antes de comparar

Verificar `log_drops`, `input_drops`, `send_errors`, `disconnects`, lacunas de sequência, comandos sem ACK e erros de coleta. `input_drops` inclui fila de eventos cheia e notificações ENC→CTRL coalescidas; o ACK confirma recepção/despacho, não garante que a fila aceitou o evento. Ausência de ACK UDP também não prova que o comando não executou; não há retransmissão automática de comando para evitar acionamento duplicado.

Se houver perdas, registrá-las como resultado de confiabilidade. Não concluir “zero misses em toda execução” a partir de logs incompletos. Uma task sem amostras também não equivale a zero misses. Repetir coleta comprometida e conservar a original, explicando o motivo. Toda conclusão WCRT é limitada ao intervalo e aos estímulos observados.

A MONITOR limita o trabalho de envio a um orçamento de parede de aproximadamente 100 ms por ciclo e usa sockets não bloqueantes. Congestionamento pode acumular logs e causar perdas, contabilizadas. TCP pode entregar parte de uma mensagem e o firmware mantém o restante para o ciclo seguinte. Essa espera faz parte do comportamento comparado. TIME e MONITOR registram execução após a inicialização de infraestrutura; a partida do WiFi não integra suas métricas periódicas.

Nenhum trecho cronometrado imprime resultados na UART. Em ENC/CTRL/SORT/SAFETY/TIME, o registro na fila ocorre depois da captura do fim. Em MONITOR, montar/enviar pacotes é trabalho da própria tarefa e integra seu tempo medido; seu registro de execução só é enfileirado após capturar o fim. Isso permite avaliar o custo real da comunicação sem incluir prints seriais. As leituras do relógio, as filas e a instrumentação ainda têm custo e podem interferir em outras ativações; não afirmar que a medição é isenta de perturbações.

## 9. Montar comparação e relatório

Para cada combinação, informar as três repetições e consolidar máximos/p99 sem esconder repetições ruins. Separar WCRT por tarefa; não usar apenas um máximo global. Identificar quais tasks sofrem interferência e quais mecanismos podem bloquear, conforme `RESUMO_M2.md`.

|Configuração M1|Protocolo|Task|Repetição|Jobs|WCRT/HWM100 ms|HWM99 ms|(9,10)-firm|Hard misses|Soft misses|Latência touch ms|RTT médio/p99/máx ms|CPU %|Perdas|Conclusão|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|preencher após medir|||||||||||||||

Melhor protocolo deve ser escolhido pelos resultados de RTT, WCRT/misses das tasks hard e integridade, mantendo o mesmo perfil de carga. UDP pode perder mensagens e TCP pode sofrer espera por retransmissão/ordenação; são hipóteses para comparar, não resultados antecipados.

O critério de sucesso do enunciado exige deadlines atendidos em vários testes. Um único hard miss observado impede afirmar atendimento integral naquele caso; janelas firm aceitáveis de tasks soft não compensam esse hard miss.

O PDF final do grupo deve incluir identificação, temática/enunciado, técnicas, configurações, tabelas/gráficos e imagens reais, análise e limitações. Demonstrar compilação presencial e funcionamento de touch/comandos; se usar o extra, mostrar LED no ESTOP remoto. Este roteiro não substitui o relatório de resultados. Não houve publicação/envio automático ao professor.
