"""Bancada M2 curta v3, apenas biblioteca padrao. Nao executa ao importar.

python tools/m2_pc.py collect --ip IP --proto UDP --scenario melhor --freq 80
Padrao: 20 segundos, nomes novos e analise automatica. Analise legada preservada.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import socket
import statistics
import struct
import time

TASKS = ("ENC", "CTRL", "SORT", "SAFETY", "MONITOR", "TIME")


def pc_sntp(server):
    """Offset NTP com quatro timestamps; nao altera o relogio do Windows.

    Uma amostra no inicio: salvar RTT e offset; assimetria/drift limitam one-way.
    """
    delta = 2208988800
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(3)
        s.connect((server, 123))
        request = bytearray(48)
        request[0] = 0x23  # cliente NTP v4
        t1 = time.time()
        sec = int(t1) + delta
        frac = int((t1 % 1) * 2**32)
        struct.pack_into("!II", request, 40, sec, frac)
        s.send(request)
        reply = s.recv(512)
        t4 = time.time()
    if len(reply) < 48 or reply[0] >> 6 == 3 or reply[0] & 7 != 4 or not 1 <= reply[1] <= 15:
        raise ValueError("Resposta SNTP invalida/nao sincronizada")
    if reply[24:32] != request[40:48]:
        raise ValueError("Resposta SNTP nao corresponde a requisicao")
    def stamp(pos):
        a, b = struct.unpack_from("!II", reply, pos)
        return a - delta + b / 2**32
    t2, t3 = stamp(32), stamp(40)
    return {"ok": True, "server": server,
            "offset_ms": ((t2-t1)+(t3-t4))*500,
            "rtt_ms": ((t4-t1)-(t3-t2))*1000}


def collect_legacy(args):
    path = Path(args.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        sync = pc_sntp(args.ntp)
    except (OSError, ValueError) as exc:
        sync = {"ok": False, "error": str(exc), "offset_ms": 0}
    meta = {"type": "meta", "scenario": args.scenario, "proto": args.proto,
            "expected_freq": args.freq, "duration_s": args.seconds,
            "period_s": args.period, "pc_sync": sync, "ip": args.ip,
            "policy": "Custom", "preemption": "ON", "slicing": "ON",
            "unicore": True, "started_pc_ms": time.time_ns()//1000000}
    kind = socket.SOCK_DGRAM if args.proto == "UDP" else socket.SOCK_STREAM
    sent = {}
    received_acks = set()
    seq = 0
    pending = b""
    started = time.perf_counter()
    next_send = started
    offset_ms = sync["offset_ms"]
    with path.open("x", encoding="utf-8") as out, socket.socket(socket.AF_INET, kind) as s:
        def write(obj):
            out.write(json.dumps(obj, ensure_ascii=False) + "\n")
        write(meta)
        # Receber rajadas de lotes UDP sem depender do pequeno buffer padrao.
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
        s.settimeout(5)
        try:
            s.connect((args.ip, args.port))
            s.settimeout(0.1)
            # Dois segundos finais sem novos comandos para drenar logs/acks.
            while time.perf_counter()-started < args.seconds + 2:
                now = time.perf_counter()
                if now < started+args.seconds and now >= next_send:
                    # Padrao reprodutivel: SORT, PING, SORT, PING, ESTOP.
                    cmd = ("SORT", "PING", "SORT", "PING", "ESTOP")[seq % 5]
                    if args.ping_only:
                        cmd = "PING"
                    pc_ms = time.time_ns()//1000000 + offset_ms
                    wire = f"{seq},{int(pc_ms)},{cmd}\n".encode("ascii")
                    sent[seq] = time.perf_counter_ns()
                    s.sendall(wire) if args.proto == "TCP" else s.send(wire)
                    write({"type": "sent", "cmd_seq": seq, "cmd": cmd,
                           "pc_ms": pc_ms, "bytes": len(wire)})
                    seq += 1
                    next_send += args.period
                try:
                    data = s.recv(65535)
                except socket.timeout:
                    continue
                if not data:
                    raise ConnectionError("ESP32 encerrou a conexao TCP")
                receive_ns = time.perf_counter_ns()
                pc_ms = time.time_ns()//1000000 + offset_ms
                if args.proto == "TCP":
                    pending += data
                    lines = pending.split(b"\n")
                    pending = lines.pop()
                    if len(pending) > 4096:
                        raise ValueError("Linha TCP ultrapassou limite")
                else:
                    lines = data.splitlines()
                for line in lines:
                    try:
                        msg = json.loads(line)
                    except (ValueError, UnicodeDecodeError):
                        write({"type": "invalid", "raw": line.hex(), "pc_rx_ms": pc_ms})
                        continue
                    msg["pc_rx_ms"] = pc_ms
                    msg["pc_synced"] = sync["ok"]
                    msg["actual_bytes"] = len(line)+1
                    if msg.get("type") == "ack" and msg.get("cmd_seq") in sent:
                        key = msg["cmd_seq"]
                        # Descartar duplicatas para RTT e contar separadamente.
                        msg["duplicate_ack"] = key in received_acks
                        received_acks.add(key)
                        msg["rtt_ms"] = (receive_ns-sent[key])/1e6
                        if msg.get("synced") and sync["ok"]:
                            msg["up_ms_estimate"] = msg["esp_ms"]-msg["pc_ms"]
                    if msg.get("synced") and sync["ok"]:
                        msg["down_ms_estimate"] = pc_ms-msg["esp_ms"]
                    write(msg)
        except (KeyboardInterrupt, OSError, ValueError) as exc:
            write({"type": "collection_error", "error": str(exc)})
        finally:
            write({"type": "collection_end", "sent": seq,
                   "ack_unique": len(received_acks), "unanswered": seq-len(received_acks),
                   "elapsed_s": time.perf_counter()-started})
    print(f"Coleta salva em {path}; analisar antes de tirar conclusoes.")


def percentile(values, fraction):
    """HWM pelo nearest-rank, sem interpolar o maximo observado."""
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values)*fraction)-1)]


def summary(values):
    return {"n": len(values), "mean": statistics.mean(values) if values else None,
            "p99": percentile(values, .99), "max": max(values) if values else None}


def firm(results, m, k, ids=None):
    windows = []
    skipped = 0
    for i in range(len(results)-k+1):
        if ids is not None and ids[i+k-1]-ids[i] != k-1:
            skipped += 1
            continue
        windows.append(sum(results[i:i+k]))
    return {"m": m, "k": k, "windows": len(windows),
            "violations": sum(n < m for n in windows),
            "minimum_met": min(windows) if windows else None,
            "skipped_nonconsecutive": skipped,
            "meets_requirement": all(n >= m for n in windows) if windows and not skipped else None}


def analyze_legacy(args):
    records = [json.loads(line) for line in Path(args.input).read_text(encoding="utf-8").splitlines() if line.strip()]
    meta = next((r for r in records if r.get("type") == "meta"), {})
    packets = [r for r in records if "seq" in r]
    # UDP pode duplicar/reordenar: nao contar o mesmo pacote duas vezes.
    unique = {r["seq"]: r for r in packets}
    duplicates = len(packets)-len(unique)
    seqs = sorted(unique)
    gaps = seqs[-1]-seqs[0]+1-len(seqs) if seqs else 0
    records_unique = list(unique.values())
    status = sorted((r for r in records_unique if r.get("type") == "status"),
                    key=lambda r: r.get("tx_us", 0))
    counters = {key: max((r.get(key,0) for r in status),default=None)
                for key in ("log_drops", "input_drops", "send_errors", "disconnects", "clock_errors")}
    # A ultima foto pode incluir jobs recem-gerados ainda na fila. Conferir uma
    # foto com pelo menos 1 s de margem de escoamento antes do ultimo status.
    cutoff = status[-1].get("tx_us", 0)-1000000 if status else 0
    coverage_status = next((r for r in reversed(status)
                            if r.get("tx_us", 0) <= cutoff and "generated" in r), None)
    global_loss = bool(gaps or any(v for v in counters.values()))
    rows = []
    jobs_by_task = {}
    malformed_batches = 0
    for r in records_unique:
        if r.get("type") == "job":
            jobs_by_task.setdefault(r["task"], []).append(r)
        elif r.get("type") == "jobs":
            # Schema 2: [task, job_id, evento, inicio, fim, deadline, origem].
            # Deduplicacao e feita por pacote antes de expandir seus registros.
            if r.get("schema") != 2 or not isinstance(r.get("jobs"), list):
                malformed_batches += 1
                continue
            for item in r["jobs"]:
                if (not isinstance(item, list) or len(item) != 7 or item[0] not in TASKS
                    or not all(isinstance(x, int) and not isinstance(x, bool) for x in item[1:6])
                    or not isinstance(item[6], str)):
                    malformed_batches += 1
                    continue
                task, job, evt, start, end, deadline, origin = item
                jobs_by_task.setdefault(task, []).append({"task": task, "job": job,
                    "evt": evt, "start": start, "end": end, "d": deadline, "origin": origin})
    for task_index, task in enumerate(TASKS):
        received = jobs_by_task.get(task, [])
        sequenced = bool(received) and all("job" in r for r in received)
        if sequenced:
            unique_jobs = {r["job"]: r for r in received}
            duplicate_jobs = len(received)-len(unique_jobs)
            received = sorted(unique_jobs.values(), key=lambda r: r["job"])
            job_gaps = max(unique_jobs)-len(unique_jobs) if unique_jobs else 0
        else:
            received = sorted(received, key=lambda r: (r["evt"], r["start"]))
            duplicate_jobs = 0
            job_gaps = None
        # Nunca converter um evento futuro em uma resposta/latencia negativa.
        jobs = [r for r in received if 0 <= r["evt"] <= r["start"] <= r["end"] and r["d"] > 0]
        invalid_jobs = len(received)-len(jobs)
        responses = [(r["end"]-r["evt"])/1000 for r in jobs]
        execution = [(r["end"]-r["start"])/1000 for r in jobs]
        latency = [(r["start"]-r["evt"])/1000 for r in jobs]
        touch = [(r["start"]-r["evt"])/1000 for r in jobs if r["origin"] == "touch"]
        met = [r["end"]-r["evt"] <= r["d"] for r in jobs]
        period = {"ENC": 5, "CTRL": 5, "MONITOR": 500, "TIME": 1000}.get(task)
        intervals = [(b["start"]-a["start"])/1000 for a,b in zip(jobs,jobs[1:])
                     if (sequenced and b["job"] == a["job"]+1)
                     or (not sequenced and not global_loss and not invalid_jobs)]
        generated = coverage_status["generated"][task_index] if coverage_status else None
        coverage_received = sum(r["job"] <= generated for r in received) if generated is not None and sequenced else None
        coverage_missing = generated-coverage_received if coverage_received is not None else None
        sequence_complete = (sequenced and not job_gaps and not invalid_jobs and not duplicate_jobs
                             and coverage_missing == 0 and not global_loss)
        row = {"task": task, "class": "soft" if task in ("MONITOR", "TIME") else "hard",
               "jobs": len(received), "valid_jobs": len(jobs), "invalid_jobs": invalid_jobs,
               "job_gaps": job_gaps, "duplicate_jobs": duplicate_jobs,
               "coverage_generated": generated, "coverage_received": coverage_received,
               "coverage_missing": coverage_missing, "sequence_complete": sequence_complete,
               "misses": len(jobs)-sum(met),
               "miss_pct": 100*(1-sum(met)/len(met)) if met else None,
               "wcrt_ms": max(responses) if responses else None,
               "hwm99_ms": percentile(responses,.99),
               "wall_exec_max_ms": max(execution) if execution else None,
               "latency_max_ms": max(latency) if latency else None,
               "touch_latency_max_ms": max(touch) if touch else None,
               "start_jitter_abs_max_ms": max((abs(x-period) for x in intervals), default=None) if period else None,
               "jitter_pairs": len(intervals) if period else 0}
        row["firm"] = firm(met,args.m,args.k, [r["job"] for r in jobs] if sequenced else None) if row["class"] == "soft" else None
        if row["firm"] is not None and not sequence_complete:
            row["firm"]["meets_requirement"] = None
        rows.append(row)
    acks = [r for r in records_unique if r.get("type") == "ack" and not r.get("duplicate_ack")]
    issues = []
    if gaps or duplicates:
        issues.append("Lacunas/duplicatas de pacotes; conferir perdas e reordenacao UDP.")
    if not status:
        issues.append("Sem status de integridade/CPU.")
    if any(v for v in counters.values()):
        issues.append("Perdas/erros reportados pela ESP32; contagens de jobs nao representam toda a execucao.")
    if malformed_batches:
        issues.append("Lotes/linhas de jobs com formato invalido foram excluidos.")
    if any(r["invalid_jobs"] for r in rows):
        issues.append("Timestamps invalidos (evento negativo/futuro ou fim antes do inicio) excluidos das metricas; nao foram corrigidos artificialmente.")
    if any(r["job_gaps"] or r["coverage_missing"] for r in rows):
        issues.append("Faltam registros por tarefa; comparar com generated/dropped e filas no status.")
    if any(r["duplicate_jobs"] for r in rows):
        issues.append("IDs de jobs repetidos: verificar reinicio/troca de sessao durante a coleta.")
    if not coverage_status:
        issues.append("Sem contagem de jobs gerados com margem de escoamento: firmware antigo ou coleta curta; integridade nao comprovada.")
    if any(r["firm"] is not None and r["firm"]["meets_requirement"] is None for r in rows):
        issues.append("(m,k)-firm inconclusivo em tarefa soft: amostras insuficientes, invalidas ou sequencia incompleta.")
    if any(not r["jobs"] for r in rows):
        issues.append("Uma ou mais tasks sem amostras: falta de evidencia.")
    if any(r.get("type") in ("invalid", "collection_error") for r in records):
        issues.append("Coleta interrompida ou mensagem invalida.")
    if not meta.get("pc_sync",{}).get("ok") or any(not r.get("synced") for r in packets):
        issues.append("SNTP ausente em parte da coleta: nao usar one-way como evidencia nesse trecho.")
    config_match = (bool(status)
                    and all(r.get("freq_mhz") == meta.get("expected_freq") for r in status)
                    and all(r.get("proto") == meta.get("proto") for r in packets))
    if not config_match:
        issues.append("Protocolo/frequencia nao correspondem ao cliente ou faltou status para confirmar a configuracao.")
    ending = next((r for r in reversed(records) if r.get("type") == "collection_end"),{})
    if ending.get("unanswered",0):
        issues.append("Comandos sem ACK; ausencia de ACK nao prova ausencia de acionamento.")
    if not ending or ending.get("elapsed_s",0) < meta.get("duration_s",0):
        issues.append("Coleta nao completou a duracao solicitada.")
    complete = (all(r["sequence_complete"] and r["valid_jobs"] for r in rows)
                and not malformed_batches and bool(ending) and config_match
                and ending.get("elapsed_s",0) >= meta.get("duration_s",0)
                and not ending.get("unanswered",0)
                and not any(r.get("type") in ("invalid", "collection_error") for r in records))
    report = {"meta": meta, "tasks": rows, "packet_gaps_internal": gaps,
              "duplicate_packets": duplicates, "esp_counters": counters,
              "rtt_ms": summary([r["rtt_ms"] for r in acks if "rtt_ms" in r]),
              "down_ms_estimate": summary([r["down_ms_estimate"] for r in records_unique if "down_ms_estimate" in r]),
              "cpu_pct": summary([r["cpu_pct"] for r in status if r["cpu_pct"] >= 0]),
              "collection_end": ending, "issues": issues, "evidence_complete": complete,
              "note": "Metricas apenas dos jobs validos recebidos. WCRT/HWM sao observados; zero misses com perdas nao comprova atendimento. Execucao inclui preempcoes."}
    base = Path(args.out)
    base.parent.mkdir(parents=True,exist_ok=True)
    base.with_suffix(".json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    with base.with_suffix(".csv").open("w",newline="",encoding="utf-8-sig") as out:
        writer = csv.DictWriter(out,fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    lines = [f"# M2 — {meta.get('scenario','?')} / {meta.get('proto','?')}", "",
             f"**Integridade da evidencia: {'sem falhas detectadas nas verificacoes disponiveis' if complete else 'INCONCLUSIVA — nao declarar atendimento integral'}.**", "",
             "|Task|Tipo|Jobs validos/recebidos|Invalidos|Misses observados|WCRT ms|HWM99 ms|Latencia max ms|", "|---|---|---:|---:|---:|---:|---:|---:|"]
    for r in rows:
        lines.append(f"|{r['task']}|{r['class']}|{r['valid_jobs']}/{r['jobs']}|{r['invalid_jobs']}|{r['misses']}|{r['wcrt_ms']}|{r['hwm99_ms']}|{r['latency_max_ms']}|")
    lines += ["",f"RTT ms: {report['rtt_ms']}",f"CPU %: {report['cpu_pct']}",
              f"Integridade: {counters}; lacunas internas: {gaps}.","", "## Ressalvas", ""]
    lines += ["- "+x for x in issues] or ["- Nenhuma ressalva automatica; conferir protocolo de bancada e estimulos manuais."]
    lines += ["",report["note"],"As janelas firm e todas as metricas estao no JSON/CSV. Janelas com menos de k jobs nao sustentam conclusao."]
    base.with_suffix(".md").write_text("\n".join(lines)+"\n",encoding="utf-8")
    print(f"Analise gravada em {base}.json/.csv/.md")


def collect(args):
    from m2_short import collect as collect_short
    return collect_short(args, pc_sntp)


def analyze(args):
    with Path(args.input).open(encoding="utf-8") as source:
        meta = json.loads(next(line for line in source if line.strip()))
    if meta.get("mode") == "online":
        from m2_short import analyze as analyze_short
        return analyze_short(args)
    return analyze_legacy(args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action",required=True)
    c = sub.add_parser("collect")
    c.add_argument("--ip",required=True)
    c.add_argument("--proto",choices=("UDP","TCP"),required=True)
    c.add_argument("--scenario",choices=("melhor","pior"),required=True)
    c.add_argument("--freq",type=int,choices=(80,160),required=True)
    c.add_argument("--port",type=int,default=5000)
    c.add_argument("--seconds",type=int,default=20)
    c.add_argument("--period",type=float,default=1)
    c.add_argument("--ntp",default="a.st1.ntp.br")
    c.add_argument("--ping-only",action="store_true")
    c.add_argument("--out",help="Opcional: sem este argumento, cria nome unico em resultados/")
    c.set_defaults(func=collect)
    a = sub.add_parser("analyze")
    a.add_argument("input")
    a.add_argument("--out",required=True)
    a.add_argument("--m",type=int,default=9)
    a.add_argument("--k",type=int,default=10)
    a.set_defaults(func=analyze)
    args = parser.parse_args()
    if args.action == "collect" and (not 12 <= args.seconds <= 300 or args.period < .5):
        parser.error("Use 12 a 300 segundos (padrao 20) e periodo >= 0.5 s.")
    if args.action == "analyze" and not 1 <= args.m <= args.k:
        parser.error("Exigido 1 <= m <= k")
    args.func(args)


if __name__ == "__main__":
    main()
