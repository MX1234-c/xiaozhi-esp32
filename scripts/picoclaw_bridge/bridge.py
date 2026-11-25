import datetime
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

import jsonschema
import websocket


def action(name):
    if name == "time":
        return datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    if name != "wake_pc":
        raise ValueError("unknown action")
    mac = os.environ.get("WOL_MAC", "").replace(":", "").replace("-", "")
    if not re.fullmatch("[0-9a-fA-F]{12}", mac):
        raise ValueError("Configure WOL_MAC first")
    packet = b"\xff" * 6 + bytes.fromhex(mac) * 16
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.sendto(packet, (os.getenv("WOL_BROADCAST", "255.255.255.255"), 9))
    return "唤醒包已发送"


action_field = {"type": "string", "enum": ["time", "wake_pc"]}
tools = {}


def register(name, description, **fields):
    tools[name] = dict(name=name, description=description, inputSchema=dict(
        type="object", properties=fields, required=list(fields), additionalProperties=False))


register("execute_action", "立即查询主机时间或唤醒电脑", action=action_field)
register("create_task", "定时执行固定动作。schedule 为每天上海时间 HH:MM 或含时区的一次性 RFC3339 时间",
         action=action_field, schedule={"type": "string", "maxLength": 40})
register("list_tasks", "列出小智创建的定时动作和任务 ID")
register("cancel_task", "根据列表中的 ID 取消任务", id={"type": "string", "minLength": 1, "maxLength": 80})
register("agent_execute", "仅用于需要 PicoClaw 模型推理的复杂任务",
         prompt={"type": "string", "minLength": 1, "maxLength": 4096})


def dispatch(req, schedule):
    if not isinstance(req, dict) or "id" not in req:
        return None
    response = {"jsonrpc": "2.0", "id": req["id"]}
    method = req.get("method")
    if method == "initialize":
        result = dict(protocolVersion="2024-11-05", capabilities={"tools": {}},
                      serverInfo={"name": "picoclaw", "version": "1.0"})
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": list(tools.values())}
    elif method == "tools/call":
        try:
            params = req["params"]
            name = params["name"]
            args = params.get("arguments", {})
            jsonschema.validate(args, tools[name]["inputSchema"])
            if name == "execute_action":
                output = action(args["action"])
            elif name == "agent_execute":
                run = subprocess.run(["picoclaw", "agent", "-m", args["prompt"]],
                                     capture_output=True, text=True, timeout=90)
                if run.returncode:
                    raise ValueError("PicoClaw agent exited with code %d" % run.returncode)
                output = run.stdout.strip()[-8192:]
            else:
                output = schedule(dict(method=name, **args))
            result = {"content": [{"type": "text", "text": output}]}
        except (ValueError, KeyError, TypeError, OSError, subprocess.TimeoutExpired, jsonschema.ValidationError) as exc:
            result = {"isError": True, "content": [{"type": "text", "text": str(exc)[:500]}]}
    else:
        return dict(response, error={"code": -32601, "message": "Unknown method"})
    return dict(response, result=result)


def main():
    endpoint = os.environ["XIAOZHI_MCP_ENDPOINT"]
    if not endpoint.startswith("wss://"):
        raise ValueError("Expected a wss endpoint")
    script = Path(__file__).resolve()
    reader, writer = os.pipe()
    with os.fdopen(reader, encoding="utf-8") as replies, subprocess.Popen(
            [script.with_name("scheduler"), sys.executable, script, str(writer)],
            pass_fds=(writer,), stdin=subprocess.PIPE, text=True, encoding="utf-8") as proc:
        os.close(writer)

        def schedule(request):
            proc.stdin.write(json.dumps(request) + "\n")
            proc.stdin.flush()
            line = replies.readline()
            if not line:
                raise SystemExit("Scheduler stopped")
            reply = json.loads(line)
            if "error" in reply:
                raise ValueError(reply["error"])
            return json.dumps(reply["result"], ensure_ascii=False)

        while proc.poll() is None:
            try:
                with websocket.create_connection(endpoint, timeout=15) as ws:
                    ws.settimeout(None)
                    while True:
                        raw = ws.recv()
                        if not raw:
                            break
                        response = dispatch(json.loads(raw), schedule)
                        if response is not None:
                            ws.send(json.dumps(response, ensure_ascii=False))
            except (websocket.WebSocketException, OSError, ValueError) as exc:
                print(type(exc).__name__, file=sys.stderr, flush=True)
            time.sleep(3)
        raise SystemExit("Scheduler stopped")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--action":
        print(action(sys.argv[2]))
    else:
        main()
