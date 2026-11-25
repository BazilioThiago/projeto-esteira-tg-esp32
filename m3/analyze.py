"""Analisa somente logs reais da M3; executar com o Python do runtime Codex."""
from pathlib import Path
import csv
import io
import json
import re

ROOT = Path(__file__).resolve().parents[1]

def analyze(path):
    lines = path.read_text(encoding='utf-8').splitlines()
    if not lines or not lines[-1].startswith('TOTAL '):
        raise ValueError(f'Ensaio ainda incompleto: {path.name}; aguarde o fim do roteiro')
    config = dict(re.findall(r'(\w+)=([^ ]+)', lines[0]))
    total = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[-1])}
    rows = list(csv.DictReader(io.StringIO('\n'.join(lines[1:-1]))))
    for r in rows:
        for k in r:
            if k != 'type': r[k] = int(r[k])
    assert total['produced'] == total['accepted'] + total['rejected']
    assert total['accepted'] == total['completed'] + total['pending']
    assert total['production'] == total['completed']
    assert total['log_lost'] == 0
    jobs = [r for r in rows if r['type'] == 'J']
    assert len(jobs) == total['completed']
    assert [r['id'] for r in jobs] == sorted(set(r['id'] for r in jobs)), 'FIFO/duplicata'
    def metrics(items):
        if not items: return {'n': 0}
        response = [(r['end_ns'] - r['release_ns']) / 1e6 for r in items]
        return {'n': len(items), 'wcrt_ms': max(response), 'mean_response_ms': sum(response)/len(response),
                'max_wait_ms': max((r['start_ns']-r['release_ns'])/1e6 for r in items),
                'max_span_ms': max((r['end_ns']-r['start_ns'])/1e6 for r in items),
                'max_cpu_ms': max(r['cpu_ns']/1e6 for r in items),
                'misses': sum(r['end_ns']-r['release_ns'] > r['deadline_ns'] for r in items)}
    servers = [r for r in rows if r['type'] == 'S']
    cs = int(config['Cs_ms']) * 1e6
    ts = int(config['Ts_ms']) * 1e6
    # Verifica periodos na grade absoluta e ausencia de liberacoes duplicadas.
    releases = [r['release_ns'] for r in servers]
    assert all(t % ts == 0 for t in releases)
    assert releases == sorted(set(releases))
    assert all(r['start_ns'] >= r['release_ns'] and r['end_ns'] >= r['start_ns'] for r in rows)
    # Jobs devem estar inteiramente dentro de alguma janela de servico.
    assert all(any(s['start_ns'] <= j['start_ns'] <= j['end_ns'] <= s['end_ns'] for s in servers) for j in jobs)
    overrun = [max(0,(r['end_ns']-r['start_ns']-cs)/1e6) for r in servers]
    return {'file': path.name, 'config': config, 'total': total, 'sort': metrics(jobs),
            'periodic': [metrics([r for r in rows if r['type']=='P' and r['task']==i]) for i in range(4)],
            'server': {'n':len(servers), 'overruns':sum(v>0 for v in overrun), 'max_overrun_ms':max(overrun,default=0),
                       'max_span_ms':max((r['end_ns']-r['start_ns'])/1e6 for r in servers),
                       'max_jitter_ms':max((r['start_ns']-r['release_ns'])/1e6 for r in servers),
                       'cpu_fraction':sum(r['cpu_ns'] for r in servers)/(int(config['seconds'])*1e9)}}

if __name__ == '__main__':
    out = ROOT/'output/m3'
    out.mkdir(parents=True,exist_ok=True)
    results = [analyze(p) for p in sorted((ROOT/'data/m3/medicoes').glob('*.csv'))]
    if not results: raise SystemExit('Sem medicoes reais para analisar')
    (out/'results.json').write_text(json.dumps(results,indent=2,ensure_ascii=False),encoding='utf-8')
    with (ROOT/'data/m3/resumo.csv').open('w',newline='',encoding='utf-8-sig') as f:
        w=csv.writer(f)
        w.writerow(['ensaio','gerados','aceitos','rejeitados','concluidos','pendentes','fila_max','WCRT_SORT_ms','misses_SORT','misses_T1_T4','max_span_server_ms','overrun_max_ms'])
        for r in results:
            t=r['total']; s=r['sort']; v=r['server']
            w.writerow([r['file'],*[t[k] for k in ['produced','accepted','rejected','completed','pending','max_depth']],s.get('wcrt_ms','N/A'),s.get('misses',0),sum(p.get('misses',0) for p in r['periodic']),v['max_span_ms'],v['max_overrun_ms']])
    print(f'{len(results)} ensaios analisados; conservacao, FIFO, timestamps e grade de liberacao verificados')
    for r in results:
        print(r['file'],r['total'],r['sort'],r['server'])
