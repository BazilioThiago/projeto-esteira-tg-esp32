# Teste curto M2 — roteiro atual (v3)

Este roteiro substitui as versões de 120 segundos e os horários exatos dos toques. O professor não fixou essa duração nem três repetições no trecho da metodologia. A proposta atual usa quatro casos de 20 segundos para o relatório e uma bateria repetível na apresentação. Se houver tempo, repetições adicionais aumentam a evidência, sem exigir uma rede perfeita.

## 1. Gravar o firmware atualizado

No terminal ESP-IDF do VS Code, na pasta do projeto:

```powershell
idf.py menuconfig
```

Em **Esteira - M2**, manter seu WiFi e escolher **UDP**. Nas configurações do sistema, selecionar CPU **80 MHz**. Manter unicore, prioridades Custom, preempção e slicing da M1, runtime stats/trace habilitados e contador de runtime ESP timer. Depois:

```powershell
idf.py build
idf.py -p COM3 flash monitor
```

Ajustar COM3 se necessário. Anotar o IP da linha:

```text
M2 IP=192.168.2.54 protocolo=UDP porta=5000 CPU=80 MHz (coleta curta v3)
```

A frase `coleta curta v3` confirma que é o firmware novo. PC e ESP32 precisam estar na mesma rede acessível entre clientes. A configuração WiFi existente não foi alterada por esta revisão. ESP-IDF alvo é o 6.1 instalado: a instrumentação usa `esp_timer_start_periodic_at`.

## 2. Rodar o primeiro caso

Abrir OUTRO terminal PowerShell no VS Code, deixando o monitor serial aberto:

```powershell
cd C:\Users\thiag\projeto-esteira-tg-esp32
python tools/m2_pc.py collect --ip 192.168.2.54 --proto UDP --scenario melhor --freq 80
```

Substituir o IP pelo da sua placa. Não precisa informar duração, nome de arquivo nem executar `analyze` depois: o padrão é **20 segundos**, os nomes são únicos e a análise é automática.

O PC consulta SNTP e aguarda a ESP32 estar sincronizada (até aproximadamente 45 segundos de preparação, fora da medição). Depois envia START com um identificador novo. A ESP32 zera as métricas e define uma janela absoluta de 20 segundos. Mensagens antigas ou o tempo em que a placa ficou ligada antes não entram na nova sessão.

Quando aparecer **MEDINDO 20 s**, fazer contatos breves, um pino por vez:

|Pino|Nome|Ação|
|---|---|---|
|GPIO27|B / Touch 7|Objeto: SORT|
|GPIO33|C / Touch 8|HMI dentro da CTRL|
|GPIO32|D / Touch 9|E-stop: SAFETY|

Pode usar a ordem **27 → 33 → 32**, soltando entre os contatos, e repetir duas ou três vezes durante os 20 segundos. Não há horário exato, cronômetro manual ou clique em botão do programa. Encostar no metal do pino ou na ponta de um fio ligado a ele, sem unir pinos. Não tocar em 3V3, VIN/5V ou EN como sensor.

O PC também envia automaticamente SORT, PING, SORT, PING, ESTOP em ciclo, aproximadamente a cada segundo. Isso demonstra os comandos remotos nos dois protocolos. A HMI continua sem prints; não esperar linhas de toque no monitor serial. O E-stop permanece travado até reiniciar a placa, como na M1. Eventos seguintes ainda executam SAFETY, mas não representam uma retomada de produção.

Depois dos 20 segundos, **parar de tocar e aguardar o terminal terminar**. Há um segundo de margem para concluir jobs da janela e alguns segundos para buscar os seis resumos e as amostras finais. Essa recuperação fica fora da janela de medição; não significa que o teste foi estendido.

## 3. Abrir os resultados

O terminal informa os caminhos. Na pasta `resultados`, cada execução cria:

- `melhor_udp_80mhz_DATA_HORA_ID.jsonl`: pacotes brutos, comandos e amostras.
- `analise_melhor_udp_80mhz_DATA_HORA_ID.md`: resumo para abrir no VS Code.
- O mesmo prefixo `.csv`: tabela das seis tarefas para Excel/relatório.
- O mesmo prefixo `.json`: métricas e diagnóstico completos.

Arquivos antigos não são apagados ou sobrescritos. `--out resultados/nome_novo.jsonl` continua disponível se preferir um nome manual; o coletor recusa arquivo bruto já existente. A análise manual sobrescreve seu próprio prefixo de saída, por isso escolher outro nome ao reanalisar:

```powershell
python tools/m2_pc.py analyze resultados/ARQUIVO.jsonl --out resultados/analise_OUTRO_NOME
```

O analisador mantém suporte aos formatos antigos. Não misturar dados antigos e v3 para concluir melhoria de protocolo: a instrumentação e a carga de logs mudaram.

## 4. Fazer os outros três casos

Para cada linha: mudar protocolo/frequência no menuconfig, fazer **build → flash → monitor**, conferir o IP e executar o comando correspondente. A gravação/reinicialização também libera o E-stop. Usar o mesmo AP, distância, PC, perfil de comandos e quantidade aproximada de toques em todos os casos.

|Caso|Menuconfig|Comando no terminal do PC|
|---|---|---|
|1 — melhor / UDP|80 MHz, UDP|`python tools/m2_pc.py collect --ip 192.168.2.54 --proto UDP --scenario melhor --freq 80`|
|2 — melhor / TCP|80 MHz, TCP|`python tools/m2_pc.py collect --ip 192.168.2.54 --proto TCP --scenario melhor --freq 80`|
|3 — pior / UDP|160 MHz, UDP|`python tools/m2_pc.py collect --ip 192.168.2.54 --proto UDP --scenario pior --freq 160`|
|4 — pior / TCP|160 MHz, TCP|`python tools/m2_pc.py collect --ip 192.168.2.54 --proto TCP --scenario pior --freq 160`|

Substituir o IP em cada comando se mudar. Melhor/pior são os rótulos herdados da M1: Custom/80 MHz teve 0,01% de hard misses; Custom/160 MHz teve 0,63%, contra 0,62% em 240 MHz. O argumento `--freq` registra/confere a configuração, mas não muda a CPU da placa. O coletor recusa começar se o protocolo/frequência declarados não correspondem ao firmware.

Se repetir a MESMA configuração, apertar EN/RESET antes, aguardar o IP e rodar o mesmo comando. Não precisa editar r1/r2/r3: cada arquivo recebe nome novo. Não reiniciar durante a medição/recuperação, pois o resultado fica em RAM.

## 5. Como interpretar sem exigir teste perfeito

A v3 calcula na ESP32, usando todos os jobs concluídos cujo evento pertence à janela:

- WCRT observado = maior `fim - evento`, igual a **HWM(100%)**.
- Tempo de parede = `fim - início`, incluindo preempções.
- Latência = `início - evento`; latência touch usa apenas origem `touch`.
- Miss = resposta maior que D, com contagem e porcentagem por tarefa.
- Jitter = maior desvio absoluto do intervalo entre inícios em relação ao período nominal. CTRL é encadeada e usa período nominal de ENC.
- `(9,10)-firm` das tasks MONITOR e TIME: janela deslizante de dez execuções, pelo menos nove dentro da deadline. Violações, janelas e mínimo atendido são calculados localmente, sem depender de receber todos os pacotes. Menos de dez amostras ou timestamps inválidos torna a conclusão firm indisponível.
- CPU média pela diferença do runtime total e do idle. São usados intervalos de amostragem inteiramente dentro da janela; `cpu_coverage_ms` informa a cobertura. O primeiro/último intervalo parcial não é somado.

O firmware grava em RAM as últimas oito execuções válidas por tarefa e o registro do maior tempo de resposta. Esses registros incluem evento/início/fim/origem. São **amostras do histórico**, não o log de todas as execuções; os máximos, contadores e firm consideram todos os jobs medidos, não só essas oito amostras. HWM(99%) não é calculado na v3; o enunciado permite HWM(100%).

O PC mede RTT de aplicação pelo seu relógio monotônico: envio do comando até a resposta. Apresenta RTT geral e separado para PING/SORT/ESTOP, sem misturar consultas finais GET/SAMPLES com o tráfego do experimento. O RTT inclui a espera pelo ciclo de 500 ms da MONITOR; não representa somente o tempo no ar. Não retransmite estímulos durante os 20 segundos; comandos sem resposta são registrados como resultado da comunicação. A consulta START e a recuperação final podem ser repetidas; o mesmo START não zera nem prolonga a sessão e os resumos congelados não mudam ao serem consultados.

Pacotes incluem run, sequência, protocolo, tamanho, UTC em ms da ESP32 obtido de SNTP e timestamps do PC. `bytes` é o payload de aplicação com newline, sem cabeçalhos de rede. `down_ms_estimate` só é calculado com SNTP nos dois dispositivos; está sujeito a offset, drift e assimetria. O PC guarda o offset/RTT da própria consulta SNTP. Se o SNTP do PC falhar, isso é informado: o RTT monotônico continua utilizável, mas essa execução não fornece o mesmo nível de evidência para latência em um sentido. TIME executa a cada 1 s e a atualização SNTP assíncrona é padrão 60 s; os ciclos desde a última atualização continuam no status.

**Perda de pacote não invalida automaticamente os máximos e contadores locais.** Se os seis resumos finais chegaram íntegros, eles continuam utilizáveis; perdas, erros, comandos sem resposta e tentativas de reenvio entram na comparação dos protocolos. Não é preciso repetir até conseguir uma rede sem perdas. No arquivo `.md`, distinguir:

- **Métricas locais disponíveis:** resumos finais e contagens coerentes; não significa que as deadlines passaram.
- **Prazos observados atendidos:** nenhum miss, timestamp inválido, execução periódica ausente ou ativação perdida identificado na janela. É evidência experimental, não garantia formal de pior caso.
- **Inconclusivo:** faltou resumo, houve timestamp inconsistente ou não foi possível completar a coleta; não substituir por zero misses.

Se houver misses reais ou perda de ativações (`input_drops`, incluindo notificações ENC→CTRL coalescidas), registrar e discutir. Se SORT/SAFETY não tiverem `touch_jobs`, faltou evidência da ISR, mesmo que comandos remotos tenham acionado as tasks. A análise avisa. Não confundir falta de toque com perda da rede.

## 6. Relatório e demonstração

Preencher a tabela por configuração/protocolo/tarefa, com WCRT/HWM100, firm, hard/soft misses, latência touch, RTT, CPU e perdas de comunicação. Comparar protocolos sob a mesma configuração, apontando melhor/pior com evidências. Não escolher vencedor antecipadamente nem esconder cenário que falhou.

Discutir interferência e bloqueio conforme `RESUMO_M2.md`. A comparação mantém a configuração de prioridades/frequência/escalonador da M1, mas altera a instrumentação e passa a liberar as três tasks periódicas por timer absoluto. Essa diferença deve ser declarada; seus custos fazem parte do novo firmware.

Para apresentar, mostrar uma compilação e repetir os quatro comandos curtos. O professor pode pedir novos estímulos ou duração maior; `--seconds 30` ou outro inteiro entre 12 e 300 funciona sem alterar o código. Mostrar os CSV/Markdown e registros reais. O relatório PDF e a postagem/apresentação continuam sendo responsabilidade do grupo.

Esta alteração não foi compilada, gravada ou testada pelo assistente, conforme orientação do usuário. A primeira execução pelo grupo valida a nova versão na placa. O roteiro simplifica o ensaio, sem prometer aprovação ou atendimento integral antes dos resultados.
