"""Gera a versao-base extensa em output; nao sobrescreve o Word final editado."""
from pathlib import Path
import json
from collections import defaultdict
from docx import Document
from docx.shared import Cm, Pt, RGBColor
from docx.enum.section import WD_SECTION_START
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK, WD_TAB_ALIGNMENT, WD_TAB_LEADER
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'output/m3'
DATA=json.loads((OUT/'results.json').read_text(encoding='utf-8'))
assert len(DATA)==24
GROUP=defaultdict(list)
for r in DATA: GROUP[(r['config']['Ts_ms'],r['config']['scenario'])].append(r)
for g in GROUP.values(): assert len(g)==3
fmt=lambda x:f'{x:.3f}'.replace('.',',')

# Diagrama tecnico do fluxo efetivamente implementado.
im=Image.new('RGB',(1600,460),'white'); draw=ImageDraw.Draw(im)
font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',28)
bold=ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf',30)
boxes=[(20,80,350,210,'Gerador de pedidos','Normal ou rajadas'),(410,80,740,210,'Fila FIFO','32 pedidos e mutex'),(800,80,1130,210,'Servidor periodico','Ts e Cs'),(1190,80,1580,210,'Operacao SORT','Carga e producao')]
for x1,y1,x2,y2,title,sub in boxes:
    draw.rounded_rectangle((x1,y1,x2,y2),radius=12,fill='#edf2f6',outline='#4b5563',width=2)
    draw.text(((x1+x2)/2,120),title,font=bold,fill='black',anchor='mm')
    draw.text(((x1+x2)/2,165),sub,font=font,fill='black',anchor='mm')
for x in [350,740,1130]:
    draw.line((x,145,x+56,145),fill='black',width=4)
    draw.polygon([(x+56,145),(x+43,137),(x+43,153)],fill='black')
draw.rounded_rectangle((430,300,1160,425),radius=12,fill='#f5f5f5',outline='#4b5563',width=2)
draw.text((795,340),'T1 a T4 e servidor disputam o mesmo processador',font=font,fill='black',anchor='mm')
draw.text((795,385),'SCHED_FIFO e afinidade ao processador 0',font=font,fill='black',anchor='mm')
draw.line((965,210,965,298),fill='black',width=3)
im.save(OUT/'fluxo.png')

doc=Document()
# Remove a borda de titulo herdada do modelo padrao do Word.
for border in doc.styles.element.xpath('.//w:pBdr'):
    border.getparent().remove(border)
sec=doc.sections[0]
sec.page_width=Cm(21); sec.page_height=Cm(29.7)
sec.top_margin=Cm(3); sec.bottom_margin=Cm(2)
sec.left_margin=Cm(3); sec.right_margin=Cm(2)
sec.footer_distance=Cm(1)
normal=doc.styles['Normal']; normal.font.name='Arial'; normal.font.size=Pt(12)
normal.paragraph_format.space_after=Pt(0)
normal.paragraph_format.first_line_indent=Cm(1.25)
normal.paragraph_format.alignment=WD_ALIGN_PARAGRAPH.JUSTIFY
normal.paragraph_format.line_spacing=1.5
for name,size in [('Title',12),('Heading 1',12),('Heading 2',12)]:
    st=doc.styles[name]; st.font.name='Arial'; st.font.size=Pt(size); st.font.color.rgb=RGBColor(0,0,0)
    st.paragraph_format.space_before=Pt(10); st.paragraph_format.space_after=Pt(6)
    st.paragraph_format.keep_with_next=True
    st.paragraph_format.first_line_indent=Cm(0)
    st.paragraph_format.alignment=WD_ALIGN_PARAGRAPH.LEFT
    st.font.bold=True
doc.styles['Heading 1'].paragraph_format.page_break_before=True
doc.core_properties.author='Guilherme Santana e Thiago Bazilio'
doc.core_properties.title='Esteira industrial M3 Servidor periodico para a triagem SORT'
section_number=0
subsection_number=0
table_number=0
def p(text,style=None): return doc.add_paragraph(text,style)
def h(text):
    global section_number,subsection_number
    section_number=int(text.split()[0]); subsection_number=0
    return doc.add_heading(text.upper(),1)
def h2(text):
    global subsection_number
    subsection_number+=1
    return doc.add_heading(f'{section_number}.{subsection_number} {text}',2)
def page(): pass  # As secoes primarias iniciam pagina pelo estilo.
def unnumbered(text,newpage=True):
    par=doc.add_heading(text.upper(),1)
    par.alignment=WD_ALIGN_PARAGRAPH.CENTER
    par.paragraph_format.page_break_before=newpage
    return par
def small(par):
    par.paragraph_format.first_line_indent=Cm(0)
    par.paragraph_format.line_spacing=1
    par.paragraph_format.alignment=WD_ALIGN_PARAGRAPH.LEFT
    for r in par.runs:r.font.size=Pt(10)
    return par
def table(headers,rows,widths=None):
    global table_number
    table_number+=1
    titles=['Configuração do ambiente e das threads','Cenários de geração de pedidos',
            'Resultados agregados da SORT','Análise nominal e WCRT observado em milissegundos',
            'Orçamento e jitter do servidor em milissegundos','Resultados históricos da M2']
    cap=small(p(f'Tabela {table_number} - {titles[table_number-1]}'))
    cap.paragraph_format.keep_with_next=True
    if widths: widths=[w*16/sum(widths) for w in widths]
    t=doc.add_table(rows=1,cols=len(headers)); t.autofit=False
    if widths:
        for col,w in zip(t.columns,widths): col.width=Cm(w)
    for c,text in zip(t.rows[0].cells,headers): c.text=str(text)
    for row in rows:
        for c,text in zip(t.add_row().cells,row): c.text=str(text)
    for i,row in enumerate(t.rows):
        trpr=row._tr.get_or_add_trPr(); cant=OxmlElement('w:cantSplit'); trpr.append(cant)
        if i==0:
            repeat=OxmlElement('w:tblHeader'); trpr.append(repeat)
        for col_idx,c in enumerate(row.cells):
            if widths: c.width=Cm(widths[col_idx])
            tcpr=c._tc.get_or_add_tcPr()
            for par in c.paragraphs:
                par.paragraph_format.space_after=Pt(3); par.paragraph_format.space_before=Pt(3)
                small(par)
                par.paragraph_format.keep_with_next=True
                for run in par.runs: run.bold=i==0
    borders=OxmlElement('w:tblBorders')
    for edge in ['top','bottom']:
        el=OxmlElement('w:'+edge); el.set(qn('w:val'),'single'); el.set(qn('w:sz'),'6'); borders.append(el)
    t._tbl.tblPr.append(borders)
    source=small(p('Fonte: autores (2026).' if table_number!=6 else 'Fonte: Santana e Bazilio (relatório da M2, 2026).'))
    source.paragraph_format.space_after=Pt(12)
    return t
def picture(path,caption,width=16):
    title,source=caption.split('Fonte:',1)
    import re
    title=re.sub(r'^(Figura \d+)  ',r'\1 - ',title).rstrip('. ')
    cap=small(p(title)); cap.paragraph_format.keep_with_next=True
    par=doc.add_paragraph(); small(par); par.paragraph_format.keep_with_next=True
    par.add_run().add_picture(str(path),width=Cm(min(width,16)))
    cap=small(p('Fonte: '+source.strip())); cap.paragraph_format.space_after=Pt(12)

def centered(text,before=0,after=0,bold=False):
    par=p(text); par.alignment=WD_ALIGN_PARAGRAPH.CENTER
    par.paragraph_format.first_line_indent=Cm(0)
    par.paragraph_format.space_before=Pt(before); par.paragraph_format.space_after=Pt(after)
    for r in par.runs:r.bold=bold
    return par

# Capa e elementos pre-textuais sem numero impresso.
centered('UNIVERSIDADE DO VALE DO ITAJAÍ\nENGENHARIA DE COMPUTAÇÃO',after=48,bold=True)
centered('GUILHERME SANTANA\nTHIAGO BAZILIO',after=90)
centered('ESTEIRA INDUSTRIAL M3\nSERVIDOR PERIÓDICO PARA A TRIAGEM SORT',after=190,bold=True)
centered('Itajaí\n2026')
pre=doc.add_section(WD_SECTION_START.NEW_PAGE)
pg=OxmlElement('w:pgNumType'); pg.set(qn('w:start'),'1'); pre._sectPr.append(pg)
centered('GUILHERME SANTANA\nTHIAGO BAZILIO',after=105)
centered('ESTEIRA INDUSTRIAL M3\nSERVIDOR PERIÓDICO PARA A TRIAGEM SORT',after=48,bold=True)
nature=p('Relatório apresentado à disciplina de Sistemas em Tempo Real do curso de Engenharia de Computação da Universidade do Vale do Itajaí, referente à avaliação M3.\n\nProfessor: Felipe Viel.'); nature.paragraph_format.left_indent=Cm(8); nature.paragraph_format.first_line_indent=Cm(0); nature.paragraph_format.line_spacing=1
centered('Itajaí\n2026',before=150)
doc.add_page_break()
title=centered('RESUMO',after=24,bold=True)
summary=('Este trabalho apresenta a reprodução da operação aperiódica de triagem SORT de uma esteira industrial, originalmente desenvolvida na M2, utilizando C e pthreads em uma máquina virtual Lubuntu. A implementação emprega uma fila FIFO de 32 pedidos, protegida por mutex, e um servidor periódico com período e orçamento configuráveis. Foram realizados 24 ensaios de seis segundos, com três repetições por combinação de configuração e cenário. Compararam-se os servidores de 50/20 ms e 5/2 ms em fila vazia, pedidos espaçados, rajadas e sobrecarga. A redução do período diminuiu o maior tempo de resposta observado dos pedidos espaçados de 52,961 ms para 6,588 ms, sem violações do prazo de 10 ms nas três repetições da configuração ajustada. As rajadas continuaram produzindo atrasos e a sobrecarga causou rejeições e pendências. As quatro tarefas periódicas atenderam aos prazos nas execuções observadas. A análise também quantificou ultrapassagens de orçamento permitidas pela execução atômica dos jobs, sem apresentar os resultados experimentais como garantia temporal em hardware físico.')
par=p(summary); par.paragraph_format.first_line_indent=Cm(0)
small(p('Palavras-chave: sistemas em tempo real; servidor periódico; pthreads; análise temporal; esteira industrial.'))
doc.add_page_break(); centered('ABSTRACT',after=24,bold=True)
par=p('This report presents a pthread-based reproduction of the aperiodic SORT operation of an industrial conveyor previously developed in M2. The C implementation runs in a Lubuntu virtual machine and uses a mutex-protected FIFO queue with capacity for 32 requests and a periodic server with configurable period and budget. Twenty-four six-second experiments were performed, with three repetitions for each configuration and scenario. Servers with period/budget settings of 50/20 ms and 5/2 ms were compared under empty queues, spaced requests, bursts, and overload. Reducing the period lowered the maximum observed response time for spaced requests from 52.961 ms to 6.588 ms, with no violations of the 10 ms deadline in the three adjusted-configuration repetitions. Bursts still caused delays, while overload produced rejections and pending requests. The four periodic tasks met their deadlines in all observed runs. The analysis also quantified budget overruns allowed by atomic job execution. These experimental observations do not establish hard real-time guarantees on physical hardware.'); par.paragraph_format.first_line_indent=Cm(0)
small(p('Keywords: real-time systems; periodic server; pthreads; timing analysis; industrial conveyor.'))
doc.add_page_break(); centered('SUMÁRIO',after=24,bold=True)
toc=p(''); toc.paragraph_format.first_line_indent=Cm(0)
field=OxmlElement('w:fldSimple'); field.set(qn('w:instr'),'TOC \\o "1-2" \\h \\z \\u'); toc._p.append(field)
body=doc.add_section(WD_SECTION_START.NEW_PAGE)
for old in body._sectPr.findall(qn('w:pgNumType')): body._sectPr.remove(old)
body.footer.is_linked_to_previous=False
foot=body.footer.paragraphs[0]; small(foot); foot.alignment=WD_ALIGN_PARAGRAPH.RIGHT
field=OxmlElement('w:fldSimple'); field.set(qn('w:instr'),'PAGE'); foot._p.append(field)
h('1 Objetivo e evolução da M2')
p('Reproduzimos a operação aperiódica de triagem SORT da esteira em C com pthreads, usando uma fila de serviço e um servidor periódico baseado no exemplo fornecido pelo professor. Os ensaios foram executados em uma máquina virtual Lubuntu. A redução do período do servidor de 50 ms para 5 ms melhorou o atendimento dos pedidos espaçados; rajadas e sobrecarga continuaram causando violações de prazo.')
h2('Enunciado atendido')
p('Evoluir o trabalho da M2 e a análise temporal, escolhendo uma função aperiódica da aplicação e reproduzindo-a em pthreads com o servidor periódico de exemplo. A thread servidor deve atender a fila com período Ts e orçamento Cs. As execuções e os testes devem ter evidências em prints de tela.')
h2('Integração da MONITOR na M2')
p('Na base da M2, ENC mede o estado simulado da esteira e ativa CTRL. Eventos de objeto ou comandos SORT ativam a triagem, enquanto SAFETY trata a parada. A MONITOR é a ponte entre essas tarefas e o PC: a cada 500 ms, recebe comandos de rede, encaminha SORT ou ESTOP à fila correspondente e transmite logs e estatísticas coletados em RAM. Ela centraliza as operações de socket e não executa o controle de velocidade nem a triagem. TIME acompanha a sincronização SNTP em uma ativação de 1 s.')
p('A M3 utiliza a lógica de SORT, composta por carga computacional e incremento do contador de produção sob mutex. Os eventos de touch e PC são representados por pedidos do gerador. A execução em pthreads é independente da placa; o firmware da ESP32 foi preservado.')

page(); h('2 Implementação e metodologia')
table(['Elemento','Configuração'],[
['Ambiente','Lubuntu 24.04.5 LTS em VirtualBox 7.2.22; 4 GB de RAM; 1 processador virtual; GCC 13.3.0'],
['Servidor','SCHED_FIFO, prioridade 60; Ts/Cs de 50/20 ms e 5/2 ms'],
['T1 a T4','Períodos 40/80/120/160 ms; 5 ms de CPU nominal por job; prioridades 50/49/48/47'],
['SORT','Deadline de 10 ms; LCG de 70 × 240 iterações da M2, complementado até 1 ms de CPU no PC'],
['Gerador','Prioridade 40; semente determinística 42; mesma sequência em cada repetição'],
['Fila','FIFO de 32 pedidos protegida por pthread_mutex_t; rejeição explícita quando cheia'],
['Coleta','Três repetições de 6 s por combinação; 24 ensaios e 144 s de janela experimental']],[3,14])
picture(OUT/'fluxo.png','Figura 1  Fluxo da reprodução da SORT em pthreads. Fonte: autores.',16)
p('Criamos uma thread para o servidor, quatro para as tarefas periódicas e uma para o gerador, todas fixadas no mesmo processador. As prioridades são explícitas; falha ao obter SCHED_FIFO encerra o programa. Uma verificação com ps confirmou as prioridades 60, 50, 49, 48, 47 e 40 e o processador 0 durante uma execução adicional de 3 s.')
p('O servidor usa clock_nanosleep com CLOCK_MONOTONIC e TIMER_ABSTIME. A fila vazia encerra o serviço daquela ativação; novos pedidos aguardam a próxima. Não há acúmulo de orçamento. Quando uma liberação inteira já venceu, o servidor a pula em vez de executar orçamentos atrasados em sequência.')
table(['Cenário','Pedidos gerados'],[
['Vazio','Nenhum pedido; verifica se o servidor devolve o processador'],
['Normal','Um pedido a cada 30 a 119 ms; 75 pedidos por repetição'],
['Rajada','12 pedidos a cada 100 ms; 720 pedidos por repetição'],
['Sobrecarga','48 pedidos a cada 100 ms; 2.880 pedidos por repetição']],[3,14])
p('O exemplo original do professor também foi compilado e executado por 10 s. A reprodução substitui o nanosleep usado como carga no exemplo por trabalho computacional. O custo de 1 ms da SORT e os custos de 5 ms de T1 a T4 são cargas sintéticas do PC, não medições do WCET da ESP32.')
h2('Métricas e integridade')
p('Resposta R = fim − evento ou liberação; espera = início − evento; duração de parede = fim − início. O tempo de CPU é contabilizado por CLOCK_THREAD_CPUTIME_ID no Linux convidado. WCRT observado é o maior R da coleta. Miss ocorre quando R supera o deadline. Rejeitados e pendentes são contados separadamente dos misses de jobs concluídos.')
p('Os CSVs são impressos após os joins, fora da janela medida. A análise verificou gerados = aceitos + rejeitados; aceitos = concluídos + pendentes; produção = concluídos; ordem FIFO, ausência de duplicatas, timestamps e grade absoluta de liberação. Não houve perdas de registros nos 24 ensaios.')

page(); h('3 Resultados da SORT')
p('A tabela agrega as três repetições de cada combinação. WCRT é o maior valor entre as repetições; contadores são somados. Todos os tempos estão em milissegundos. O deadline de 10 ms foi mantido em ambos os servidores.')
rows=[]
labels={'empty':'Vazio','normal':'Normal','burst':'Rajada','overload':'Sobrecarga'}
for cfg in ['50','5']:
    for mode in ['empty','normal','burst','overload']:
        group=GROUP[cfg,mode]
        done=sum(r['total']['completed'] for r in group)
        rows.append([f'{cfg}/{"20" if cfg=="50" else "2"}',labels[mode],str(done),
                     fmt(max(r['sort'].get('wcrt_ms',0) for r in group)) if done else 'N/A',
                     str(sum(r['sort'].get('misses',0) for r in group)),
                     str(sum(r['total']['rejected'] for r in group)),
                     str(sum(r['total']['pending'] for r in group))])
table(['Ts/Cs','Cenário','Concl.','WCRT','Misses','Rejeit.','Pend.'],rows,[1.6,3,1.9,2.3,2,2,2.2])
h2('Pedidos espaçados')
g50=GROUP['50','normal']; g5=GROUP['5','normal']
p(f'No servidor 50/20 ms, os 225 pedidos foram concluídos, mas houve {sum(r["sort"]["misses"] for r in g50)} misses. O WCRT chegou a {fmt(max(r["sort"]["wcrt_ms"] for r in g50))} ms. A espera pela próxima ativação domina o tempo de resposta, mesmo com fila quase sempre unitária. Com 5/2 ms, os mesmos 225 pedidos foram concluídos sem misses; o WCRT máximo foi {fmt(max(r["sort"]["wcrt_ms"] for r in g5))} ms. A reserva nominal permaneceu em 40%, mas a espera de polling diminuiu.')
h2('Rajadas e sobrecarga')
p('Na rajada de 12 pedidos, não houve rejeitados nem pendentes ao fim das janelas, mas o atendimento em série aumentou a espera. Com 50/20 ms, todos os 2.160 pedidos concluídos perderam o prazo. Com 5/2 ms, houve 1.747 misses entre 2.160 concluídos. O período menor melhorou a resposta sem resolver o efeito da fila.')
p('Na sobrecarga, cada lote contém 48 pedidos para uma fila de 32. Assim, já há rejeições instantâneas mesmo quando a fila estava vazia. A chegada média de 480 pedidos/s também supera a capacidade nominal de 400 jobs de 1 ms/s correspondente à reserva de 40%. Os pendentes restantes foram contabilizados ao terminar os 6 s; não foram tratados como jobs concluídos nem drenados fora da janela.')
h2('Conservação e interpretação')
p('Em todos os ensaios, o contador de produção coincidiu com o número de jobs SORT concluídos. Nenhum pedido aceito foi contado duas vezes. Fila vazia produziu zero execuções de SORT; isso valida o caminho sem requisições, mas não constitui teste de prazo da SORT. Os CSVs por repetição preservam os dados completos para conferência.')

page(); h('4 Análise temporal e limites do orçamento')
p('A utilização nominal das quatro tarefas periódicas é 5/40 + 5/80 + 5/120 + 5/160 = 0,260417. Com Cs/Ts = 0,4, o total nominal é 0,660417. Esse valor exclui gerador, instrumentação, sistema operacional e virtualização, e não é uma garantia de escalonabilidade.')
p('Para ilustrar o modelo de prioridades fixas, usamos Rᵢ = Cᵢ + Σ ceil(Rᵢ/Tₕ) Cₕ para os interferentes de maior prioridade, incluindo o servidor. A conta usa custos nominais, bloqueio nulo e orçamento exato. Essas hipóteses não são estritamente satisfeitas pela execução atômica do exemplo e pela VM; os valores abaixo são referências de modelo.')
pr=[]
for i,(period,r50,r5) in enumerate(zip([40,80,120,160],[25,30,35,40],[9,18,25,34])):
    pr.append([f'T{i+1}',period,r50,r5,fmt(max(r['periodic'][i]['wcrt_ms'] for r in DATA if r['config']['Ts_ms']=='50')),fmt(max(r['periodic'][i]['wcrt_ms'] for r in DATA if r['config']['Ts_ms']=='5'))])
table(['Tarefa','D','R modelo\n50/20','R modelo\n5/2','WCRT\n50/20','WCRT\n5/2'],pr,[1.8,1,3.3,3.1,3.4,3.4])
p('Não houve misses de T1 a T4 nos 24 ensaios. Entretanto, por exemplo, o WCRT observado de T4 no servidor 50/20 ms foi maior que os 40 ms do cálculo nominal. Isso mostra por que a recorrência simplificada não pode ser apresentada como limite garantido deste experimento.')
h2('Ultrapassagem do orçamento')
budget=[]
for cfg in ['50','5']:
    rs=[r for r in DATA if r['config']['Ts_ms']==cfg]
    budget.append([f'{cfg}/{"20" if cfg=="50" else "2"}',sum(r['server']['overruns'] for r in rs),fmt(max(r['server']['max_span_ms'] for r in rs)),fmt(max(r['server']['max_overrun_ms'] for r in rs)),fmt(max(r['server']['max_jitter_ms'] for r in rs))])
table(['Ts/Cs','Janelas\n> Cs','Duração\nmáxima','Excesso\nmáximo','Jitter\nmáximo'],budget,[2,3,4,4,4])
p('O exemplo só confere o orçamento entre jobs. Se ainda houver saldo, inicia um job completo; ele não é suspenso quando Cs termina. Mantivemos essa semântica e medimos cada excesso. Logo, Cs/Ts representa a reserva nominal, e não um teto estrito de uso de CPU implementado pelo programa. Preempções, bloqueios por mutex e pausas da VM também afetam as durações de parede.')
p('No servidor 5/2 ms, a grade teria 1.200 liberações em 6 s, mas algumas foram puladas quando já estavam vencidas. Isso aparece na quantidade de janelas dos logs e evita acumular orçamento atrasado. CLOCK_THREAD_CPUTIME_ID descreve a contabilização do convidado; não mede diretamente o uso físico da CPU do Windows.')
p('O deadline da SORT é menor que o período de 50 ms. Um pedido logo após uma janela vazia pode esperar quase Ts, portanto não há garantia de D = 10 ms nessa configuração mesmo com baixa carga. Reduzir Ts favoreceu o cenário normal observado, mas a fila torna o prazo inviável nas cargas em rajada e sobrecarga testadas.')

page(); h('5 Relação com os resultados da M2')
p('O relatório entregue na M2 registrou quatro ensaios na ESP32, combinando UDP/TCP e 80/160 MHz. A tabela abaixo preserva esses dados como histórico da etapa anterior; eles não foram medidos novamente na VM nem são diretamente comparáveis aos tempos de CPU do PC.')
table(['Cenário da M2','Misses ENC','RTT médio ms','CPU estimada'],[
['UDP 80 MHz','7','480,695','47,02%'],['TCP 80 MHz','9','503,415','48,75%'],
['UDP 160 MHz','0','483,212','44,17%'],['TCP 160 MHz','3','1.086,406','46,72%']],[5,3,5,4])
p('Na M2, a MONITOR faz polling da rede a cada 500 ms; o RTT inclui essa espera e não representa apenas atraso do enlace. Na M3, o servidor faz polling da fila a cada Ts. Nos dois casos, a frequência do atendimento influencia a resposta observada. A comparação é conceitual: redes, plataformas, cargas e prioridades diferem.')
h('6 Conclusão')
p('A operação de triagem foi reproduzida com pthreads, fila FIFO protegida por mutex e servidor periódico executado com SCHED_FIFO em um único processador virtual. Os testes confirmaram o processamento ordenado, o incremento da produção e a rejeição explícita ao atingir o limite da fila. A configuração 5/2 ms cumpriu o prazo da SORT nos pedidos espaçados das três repetições, enquanto rajadas e sobrecarga causaram misses e, na sobrecarga, rejeições e pendências.')
p('As tarefas periódicas atenderam aos prazos nas execuções observadas. A execução atômica dos jobs permite ultrapassar Cs, assim como no exemplo utilizado; os excessos foram quantificados. Os resultados demonstram o comportamento nas condições medidas e não provam garantia hard em hardware físico nem superioridade geral de uma configuração.')
h('7 Reprodução dos ensaios')
p('Na VM, abrir /media/sf_projeto e executar bash m3/run_tests.sh. Para demonstrar individualmente, compilar com gcc -std=c11 -O2 -Wall -Wextra -Werror m3/servidor_sort.c -o /tmp/servidor_sort -pthread e executar sudo /tmp/servidor_sort normal 6 5 2. O roteiro completo, os critérios e as opções constam em docs/m3/README.md.')
p('Os arquivos brutos estão em data/m3/medicoes; a síntese em output/m3/summary.csv e results.json; os prints em output/m3/evidence. O apêndice reúne capturas do terminal com resultados reais já gravados, incluindo as três repetições de cada cenário. Os prints individuais das 24 execuções também acompanham o projeto.')
unnumbered('Referências')
for text in [
'LINUX MAN-PAGES. clock_nanosleep(2): high-resolution sleep with specifiable clock. [S. l.], [s. d.]. Disponível em: https://man7.org/linux/man-pages/man2/clock_nanosleep.2.html. Acesso em: 25 nov. 2026.',
'LINUX MAN-PAGES. pthread_setschedparam(3): set/get scheduling policy and parameters of a thread. [S. l.], [s. d.]. Disponível em: https://man7.org/linux/man-pages/man3/pthread_setschedparam.3.html. Acesso em: 25 nov. 2026.',
'LINUX MAN-PAGES. sched(7): overview of CPU scheduling. [S. l.], [s. d.]. Disponível em: https://man7.org/linux/man-pages/man7/sched.7.html. Acesso em: 25 nov. 2026.',
'SANTANA, Guilherme; BAZILIO, Thiago. Trabalho Esteira M2. Itajaí: Universidade do Vale do Itajaí, 2026. Relatório da disciplina de Sistemas em Tempo Real.',
'VIEL, Felipe. Aperiodic_Server_ex.c. [S. l.], [s. d.]. Código-fonte fornecido na disciplina de Sistemas em Tempo Real.',
'VIEL, Felipe. Trabalho M3 26 2: servidor aperiódico. Itajaí: Universidade do Vale do Itajaí, 2026. Enunciado de avaliação.']:
    par=p(text)
    par.paragraph_format.first_line_indent=Cm(0)
    par.paragraph_format.line_spacing=1
    par.paragraph_format.alignment=WD_ALIGN_PARAGRAPH.LEFT
    par.paragraph_format.space_after=Pt(12)

evidence=[('summary-ambiente.png','Compilação sem avisos, entradas inválidas e recusa sem privilégio RT.'),
          ('summary-original.png','Execução do exemplo original do professor por 10 segundos.'),
          ('runtime-policy.png','Verificação de SCHED_FIFO, prioridades e afinidade em execução.')]
for config in ['exemplo','ajustado']:
    for mode in ['empty','normal','burst','overload']:
        evidence.append((f'summary-{config}-{mode}.png',f'Servidor {"50/20" if config=="exemplo" else "5/2"} ms no cenário {labels[mode].lower()}. Três repetições.'))
for idx,(filename,caption) in enumerate(evidence):
    if idx%2==0:
        if idx==0: unnumbered('APÊNDICE A - EVIDÊNCIAS DAS EXECUÇÕES')
        else:
            doc.add_page_break()
            par=p('APÊNDICE A - EVIDÊNCIAS DAS EXECUÇÕES (continuação)')
            par.paragraph_format.first_line_indent=Cm(0)
            par.paragraph_format.alignment=WD_ALIGN_PARAGRAPH.CENTER
            par.paragraph_format.space_after=Pt(12)
            for run in par.runs:run.bold=True
    picture(OUT/'evidence'/filename,f'Figura {idx+2}  {caption} Fonte: execução dos autores na VM.',16)

target=OUT/'relatorio-base.docx'
# Referencias citadas no texto sem modificar os resultados experimentais.
for par in doc.paragraphs:
    if par.text.startswith('Evoluir o trabalho da M2'):
        par.add_run(' A proposta segue o enunciado de Viel (2026).')
    if par.text.startswith('O servidor usa clock_nanosleep'):
        par.add_run(' As interfaces de temporização e escalonamento seguem a documentação Linux man-pages ([s. d.]).')
    if par.text.startswith('O relatório entregue na M2 registrou'):
        par.add_run(' Os valores históricos são de Santana e Bazilio (2026).')
for name in ['TOC 1','TOC 2']:
    if name in doc.styles:
        st=doc.styles[name]; st.font.name='Arial'; st.font.size=Pt(12)
        st.paragraph_format.line_spacing=1.5
doc.save(target)
print(target)
