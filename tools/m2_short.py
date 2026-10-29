"""Coleta curta v3: START idempotente, metricas locais e resultado congelado.

Invocado por m2_pc.py; somente biblioteca padrao. Sem execucao ao importar.
"""
import csv
import json
from pathlib import Path
import socket
import statistics
import time
from types import SimpleNamespace

TASKS = ("ENC", "CTRL", "SORT", "SAFETY", "MONITOR", "TIME")


def collect(args, pc_sntp):
    stamp = time.strftime("%Y%m%d_%H%M%S")
    path = Path(args.out or f"resultados/{args.scenario}_{args.proto.lower()}_{args.freq}mhz_{stamp}_{time.time_ns()%1000000:06d}.jsonl")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"Arquivo ja existe: {path}; use outro nome.")
    print("Preparando coleta curta e consultando SNTP no PC...", flush=True)
    try:
        sync = pc_sntp(args.ntp)
    except (OSError, ValueError) as exc:
        sync = {"ok": False, "error": str(exc), "offset_ms": 0}
        print("SNTP do PC indisponivel: RTT continua valido; atraso em um sentido ficara indisponivel.", flush=True)
    rid = (time.time_ns() // 1000) & 0xFFFFFFFF or 1
    meta = {"type": "meta", "mode": "online", "schema": 3, "run_id": rid,
            "scenario": args.scenario, "proto": args.proto, "expected_freq": args.freq,
            "duration_s": args.seconds, "period_s": args.period, "pc_sync": sync,
            "ip": args.ip, "policy": "Custom", "preemption": "ON", "slicing": "ON",
            "unicore": True, "started_pc_ms": time.time_ns()//1000000}
    kind = socket.SOCK_DGRAM if args.proto == "UDP" else socket.SOCK_STREAM
    started = False
    finished = False
    command_seq = 0
    sent = {}
    replies = set()
    buffer = b""
    offset = sync.get("offset_ms", 0)
    with path.open("x", encoding="utf-8") as out, socket.socket(socket.AF_INET, kind) as sock:
        def write(value):
            out.write(json.dumps(value, ensure_ascii=False)+"\n")

        def receive():
            nonlocal buffer
            data = sock.recv(65535)
            if not data:
                raise ConnectionError("Conexao encerrada pela ESP32")
            received_ns = time.perf_counter_ns()
            pc_ms = time.time_ns()//1000000 + offset
            if args.proto == "TCP":
                buffer += data
                lines = buffer.split(b"\n")
                buffer = lines.pop()
                if len(buffer) > 4096:
                    raise ValueError("Mensagem TCP ultrapassou o limite")
            else:
                lines = data.splitlines()
            messages = []
            for line in lines:
                try:
                    message = json.loads(line)
                    if not isinstance(message, dict):
                        raise ValueError("Mensagem nao e objeto JSON")
                except (ValueError, UnicodeDecodeError):
                    write({"type": "invalid", "raw": line.hex()})
                    continue
                message["pc_rx_ms"] = pc_ms
                message["pc_synced"] = sync["ok"]
                message["actual_bytes"] = len(line)+1
                key = message.get("cmd_seq")
                if key in sent:
                    request = sent[key]
                    message["duplicate_ack"] = key in replies
                    message["attempts"] = request["attempts"]
                    message["rtt_ms"] = (received_ns-request["ns"])/1e6
                    replies.add(key)
                if message.get("synced") and sync["ok"]:
                    message["down_ms_estimate"] = pc_ms-message["esp_ms"]
                write(message)
                messages.append(message)
            return messages

        def request(cmd, arg=0, timeout=2.0, retries=2):
            nonlocal command_seq
            seq = command_seq
            command_seq += 1
            pc_ms = int(time.time_ns()//1000000 + offset)
            wire = f"{seq},{pc_ms},{cmd},{arg},{rid}\n".encode("ascii")
            sent[seq] = {"ns": time.perf_counter_ns(), "attempts": 0, "cmd": cmd}
            for attempt in range(retries+1):
                sent[seq]["attempts"] = attempt+1
                sock.settimeout(timeout)
                sock.sendall(wire) if args.proto == "TCP" else sock.send(wire)
                write({"type": "sent", "cmd_seq": seq, "cmd": cmd, "arg": arg,
                       "run": rid, "pc_ms": pc_ms, "attempt": attempt+1, "bytes": len(wire)})
                deadline = time.perf_counter()+timeout
                while time.perf_counter() < deadline:
                    sock.settimeout(min(.2, max(.001, deadline-time.perf_counter())))
                    try:
                        messages = receive()
                    except socket.timeout:
                        continue
                    for message in messages:
                        if message.get("schema") != 3:
                            raise ValueError("Firmware antigo. Faca build/flash da coleta curta v3 antes de coletar.")
                        if message.get("cmd_seq") == seq:
                            if message.get("type") == "error":
                                raise ValueError(f"ESP32: {message.get('error')}")
                            if cmd == "INFO" or message.get("run") == rid:
                                return message
            write({"type": "request_timeout", "cmd_seq": seq, "cmd": cmd})
            raise TimeoutError(f"Sem resposta para {cmd}; conferir rede, IP e firmware v3.")

        write(meta)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
            sock.settimeout(5)
            sock.connect((args.ip, args.port))
            print("Aguardando WiFi/SNTP da ESP32 (ate 45 s)...", flush=True)
            ready_deadline = time.perf_counter()+45
            while True:
                info = request("INFO")
                if info.get("freq_mhz") != args.freq or info.get("proto") != args.proto:
                    raise ValueError("Protocolo/frequencia do firmware diferem do comando. Confira menuconfig.")
                if info.get("synced"):
                    break
                if time.perf_counter() > ready_deadline:
                    raise TimeoutError("ESP32 sem SNTP. Conferir acesso do WiFi ao servidor UDP/123.")
                # Espera somente no PC antes da janela; nenhuma tarefa medida bloqueia aqui.
                time.sleep(.5)
            begin = request("START", args.seconds)
            if begin.get("phase") != "running":
                raise ValueError("ESP32 nao confirmou inicio da medicao")
            started = True
            local_start = time.perf_counter()
            # Janela e encerrada pela ESP32 no instante absoluto acordado no START.
            local_end = local_start + max(0, (begin["end_us"]-begin["tx_us"])/1e6)
            next_command = local_start
            number = 0
            next_progress = local_start
            print(f"MEDINDO {args.seconds} s. Toque algumas vezes em 27 (B), 33 (C) e 32 (D), sem horario exato.", flush=True)
            while time.perf_counter() < local_end:
                now = time.perf_counter()
                if now >= next_progress:
                    print(f"  Restam aproximadamente {max(0, round(local_end-now))} s...", flush=True)
                    next_progress += 5
                if now >= next_command:
                    cmd = "PING" if args.ping_only else ("SORT", "PING", "SORT", "PING", "ESTOP")[number % 5]
                    try:
                        # Estimulos sem retransmissao: perda faz parte do resultado UDP.
                        request(cmd, timeout=1.2, retries=0)
                    except TimeoutError:
                        pass
                    number += 1
                    next_command += args.period
                else:
                    sock.settimeout(min(.1, max(.001, next_command-now), max(.001, local_end-now)))
                    try:
                        receive()
                    except socket.timeout:
                        pass
            print("Medicao encerrada. Buscando o resultado congelado na ESP32...", flush=True)
            for attempt in range(8):
                state = request("INFO")
                if state.get("run") == rid and state.get("phase") == "done":
                    break
            else:
                raise TimeoutError("ESP32 nao confirmou fim da janela")
            # Recuperacao confiavel do resumo: consultar novamente nao refaz o teste.
            for index in range(len(TASKS)):
                result = request("GET", index, retries=3)
                if not result.get("final") or result.get("task") != TASKS[index]:
                    raise ValueError("Resumo final invalido")
                samples = request("SAMPLES", index, retries=3)
                if not samples.get("final") or samples.get("task") != TASKS[index]:
                    raise ValueError("Amostras finais invalidas")
            request("INFO")
            finished = True
        except (KeyboardInterrupt, OSError, ValueError) as exc:
            write({"type": "collection_error", "error": str(exc)})
            print(f"Coleta incompleta: {exc}", flush=True)
            if started:
                # Nao zerar resultado: a ESP32 encerra a janela automaticamente.
                print(f"Resultado local fica na ESP32 com run={rid} ate outro START/reinicio.", flush=True)
        finally:
            write({"type": "collection_end", "started": started, "complete": finished,
                   "requests": command_seq, "replies_unique": len(replies)})
    prefix = path.with_name("analise_"+path.stem).with_suffix("")
    analyze(SimpleNamespace(input=str(path), out=str(prefix), m=9, k=10))
    print(f"Dados: {path}\nAbra o resumo: {prefix}.md", flush=True)


def describe(values):
    if not values:
        return {"n": 0, "mean": None, "p99": None, "max": None}
    ordered = sorted(values)
    return {"n": len(values), "mean": statistics.mean(values),
            "p99": ordered[max(0, (99*len(values)+99)//100-1)], "max": ordered[-1]}


def analyze(args):
    records = [json.loads(line) for line in Path(args.input).read_text(encoding="utf-8").splitlines() if line.strip()]
    meta = next((r for r in records if r.get("type") == "meta"), {})
    rid = meta.get("run_id")
    packets = [r for r in records if r.get("schema") == 3 and r.get("run") == rid]
    statuses = [r for r in packets if r.get("type") == "status"]
    final_status = next((r for r in reversed(statuses) if r.get("phase") == "done"), None)
    summaries = {r["task"]: r for r in packets if r.get("type") == "summary" and r.get("final")}
    samples = {r["task"]: r for r in packets if r.get("type") == "samples" and r.get("final")}
    ending = next((r for r in reversed(records) if r.get("type") == "collection_end"), {})
    issues = []
    rows = []
    for index, name in enumerate(TASKS):
        s = summaries.get(name)
        if s is None:
            issues.append(f"Sem resumo final de {name}; nao substituir por zero misses.")
            rows.append({"task": name, "class": "soft" if index >= 4 else "hard", "jobs": None,
                         "valid_jobs": None, "invalid_jobs": None, "misses": None, "miss_pct": None,
                         "expected_jobs": None,
                         "wcrt_ms": None, "hwm100_ms": None, "wall_exec_max_ms": None,
                         "latency_max_ms": None, "touch_latency_max_ms": None,
                         "start_jitter_abs_max_ms": None, "touch_jobs": None, "pc_jobs": None,
                         "firm": None})
            continue
        valid = s["jobs"]-s["invalid"]
        f = None
        if index >= 4:
            f = {"m": s["firm_m"], "k": s["firm_k"], "windows": s["firm_windows"],
                 "violations": s["firm_violations"],
                 "minimum_met": s["firm_min"] if s["firm_windows"] else None,
                 "meets_requirement": s["firm_violations"] == 0 if s["firm_windows"] and not s["invalid"] else None}
            if not f["windows"]:
                issues.append(f"{name}: menos de dez execucoes validas consecutivas; firm indisponivel.")
        if s["invalid"]:
            issues.append(f"{name}: timestamps invalidos; nao afirmar atendimento integral.")
        if not s["jobs"]:
            issues.append(f"{name}: nenhuma execucao na janela.")
        if index in (2, 3) and not s["touch_jobs"]:
            issues.append(f"{name}: sem evento touch; tocar no pino correspondente para obter latencia da ISR.")
        if s.get("expected_jobs", -1) >= 0 and s["expected_jobs"] != s["jobs"]:
            issues.append(f"{name}: {s['jobs']} jobs concluidos para {s['expected_jobs']} liberacoes nominais; investigar execucoes ausentes.")
        rows.append({"task": name, "class": "soft" if index >= 4 else "hard", "jobs": s["jobs"],
                     "valid_jobs": valid, "invalid_jobs": s["invalid"], "misses": s["misses"],
                     "expected_jobs": s.get("expected_jobs"),
                     "miss_pct": 100*s["misses"]/valid if valid else None,
                     "wcrt_ms": s["response_us"]/1000 if valid else None,
                     "hwm100_ms": s["response_us"]/1000 if valid else None,
                     "wall_exec_max_ms": s["wall_us"]/1000 if valid else None,
                     "latency_max_ms": s["latency_us"]/1000 if valid else None,
                     "touch_latency_max_ms": s["touch_us"]/1000 if s["touch_jobs"] else None,
                     "start_jitter_abs_max_ms": s["jitter_us"]/1000 if valid > 1 and index not in (2, 3) else None,
                     "touch_jobs": s["touch_jobs"], "pc_jobs": s["pc_jobs"], "firm": f})
    if any(name not in samples for name in TASKS):
        issues.append("Faltam amostras finais dos timestamps; conferir arquivo bruto.")
    if not final_status:
        issues.append("Sem confirmacao final da ESP32; resultado inconclusivo.")
    if final_status and final_status.get("clock_errors"):
        issues.append("A ESP32 detectou timestamps invalidos durante a janela.")
    if final_status and final_status.get("input_drops"):
        issues.append("Houve perda de ativacoes/notificacoes: contabilizar na discussao de atendimento.")
    if final_status and final_status.get("send_errors"):
        issues.append("Houve erros de envio; metricas locais permanecem no resumo final.")
    if final_status and final_status.get("cpu_pct", -1) < 0:
        issues.append("Ocupacao de CPU indisponivel.")
    if not meta.get("pc_sync", {}).get("ok"):
        issues.append("PC sem SNTP: RTT utilizavel, atraso em um sentido indisponivel.")
    if any(r.get("type") == "collection_error" for r in records):
        issues.append("A coleta nao terminou normalmente.")
    seqs = sorted({r["seq"] for r in packets})
    gaps = seqs[-1]-seqs[0]+1-len(seqs) if seqs else 0
    responses = {}
    for r in packets:
        if r.get("cmd") in ("PING", "SORT", "ESTOP") and "rtt_ms" in r and not r.get("duplicate_ack"):
            responses.setdefault(r["cmd_seq"], r)
    sent_commands = {r["cmd_seq"] for r in records if r.get("type") == "sent" and r.get("cmd") in ("PING", "SORT", "ESTOP")}
    missing = len(sent_commands-set(responses))
    if missing or gaps:
        issues.append("Perdas de mensagens observadas: sao resultado da comunicacao, nao perda dos contadores locais.")
    correct_config = bool(final_status and final_status.get("freq_mhz") == meta.get("expected_freq")
                          and final_status.get("proto") == meta.get("proto"))
    if final_status and not correct_config:
        issues.append("Configuracao final diferente do protocolo/frequencia declarados.")
    measured_s = (final_status["end_us"]-final_status["begin_us"])/1e6 if final_status else None
    if measured_s is not None and abs(measured_s-meta.get("duration_s", 0)) > .01:
        issues.append("Duracao medida diferente da solicitada.")
    complete = bool(ending.get("complete") and correct_config and len(summaries) == 6
                    and len(samples) == 6 and all(r.get("valid_jobs") and not r.get("invalid_jobs") for r in rows)
                    and all(s.get("expected_jobs", -1) < 0 or s["expected_jobs"] == s["jobs"] for s in summaries.values())
                    and final_status and not final_status.get("clock_errors")
                    and measured_s is not None and abs(measured_s-meta.get("duration_s", 0)) <= .01)
    deadlines_met = bool(complete and not final_status.get("input_drops")
                         and all(r["misses"] == 0 for r in rows))
    report = {"meta": meta, "tasks": rows, "measurement_s": measured_s,
              "local_metrics_complete": complete, "all_observed_deadlines_met": deadlines_met,
              "touch_evidence_complete": all(summaries.get(name, {}).get("touch_jobs", 0) > 0 for name in ("SORT", "SAFETY")),
              "final_status": final_status, "rtt_ms": describe([r["rtt_ms"] for r in responses.values()]),
              "rtt_by_command_ms": {cmd: describe([r["rtt_ms"] for r in responses.values() if r["cmd"] == cmd]) for cmd in ("PING", "SORT", "ESTOP")},
              "down_ms_estimate": describe([r["down_ms_estimate"] for r in packets if "down_ms_estimate" in r]),
              "command_sent": len(sent_commands), "command_unanswered": missing,
              "packet_gaps": gaps, "issues": issues,
              "trace_scope": "Ultimas oito execucoes validas e timestamps do maior R por tarefa; nao e o historico completo.",
              "metrics_scope": "Contadores, HWM100 e firm calculados localmente para todos os jobs concluidos pertencentes a janela. Sem HWM99."}
    prefix = Path(args.out)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix(".json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    with prefix.with_suffix(".csv").open("w", encoding="utf-8-sig", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    status_text = "disponiveis, sem inconsistencias detectadas" if complete else "INCONCLUSIVAS — veja ressalvas"
    lines = [f"# M2 — {meta.get('scenario')} / {meta.get('proto')} / {meta.get('expected_freq')} MHz", "",
             f"Janela ESP32: {measured_s} s. Metricas locais: **{status_text}**.",
             f"Todos os prazos observados atendidos, sem perdas de ativacao: **{'sim' if deadlines_met else 'nao comprovado'}**.", "",
             "|Task|Jobs|Invalidos|Misses|WCRT / HWM100 ms|Latencia touch max ms|(9,10)-firm: violacoes/janelas|",
             "|---|---:|---:|---:|---:|---:|---|"]
    for r in rows:
        f = r["firm"]
        firm_text = f"{f['violations']}/{f['windows']}" if f and f["windows"] else "—"
        lines.append(f"|{r['task']}|{r['jobs']}|{r['invalid_jobs']}|{r['misses']}|{r['wcrt_ms']}|{r['touch_latency_max_ms']}|{firm_text}|")
    lines += ["", f"RTT ms: {report['rtt_ms']}",
              f"Comandos sem resposta: {missing}/{len(sent_commands)}. Lacunas de pacotes: {gaps}.",
              f"CPU media por runtime/idle: {final_status.get('cpu_pct') if final_status else None}%.",
              "", "## Ressalvas", ""]
    lines += ["- "+x for x in issues] or ["- Nenhuma ressalva automatica. Conferir estimulos e configuracao da bancada."]
    lines += ["", report["trace_scope"], report["metrics_scope"],
              "WCRT observado nao e garantia formal. Tempo end-start inclui preempcoes. RTT inclui espera pela MONITOR."
              " Atraso em um sentido depende de erro de sincronizacao SNTP. Perdas de rede devem ser apresentadas, sem exigir rede perfeita."]
    prefix.with_suffix(".md").write_text("\n".join(lines)+"\n", encoding="utf-8")
