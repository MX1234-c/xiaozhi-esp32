from contextlib import closing
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

import lan_devices


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

tools["device_control"] = {
    "name": "device_control",
    "description": (
        "局域网设备控制。light 调用 status/on/off/brightness/rgb/set/display/time/"
        "effect_status/effect_stop/blink/breathing/rainbow。water_dispenser 调用 "
        "status/heating_on/heating_off/cooling_on/cooling_off，仅按用户明确指示切换。"
        "fntv 调用 discover/status/pause/resume/next/seek_relative/seek_absolute/"
        "fullscreen/maximize/minimize/volume/catalog/search/seasons/episodes/play。"
        "seek 传秒数，播放和剧集查询用工具返回的 itemGuid。"
    ),
    "inputSchema": {
        "type": "object", "required": ["device", "action"], "additionalProperties": False,
        "properties": {
            "device": {"enum": ["light", "water_dispenser", "fntv"]},
            "action": {"type": "string", "maxLength": 32},
            "seconds": {"type": "integer", "minimum": -2147483648, "maximum": 2147483647},
            "volume": {"type": "integer", "minimum": 0, "maximum": 100},
            "page": {"type": "integer", "minimum": 1, "maximum": 10000},
            "title": {"type": "string", "minLength": 1, "maxLength": 200},
            "itemGuid": {"type": "string", "minLength": 1, "maxLength": 128},
            "brightness": {"type": "integer", "minimum": 0, "maximum": 100},
            "rgb": {"type": "array", "minItems": 3, "maxItems": 3,
                    "items": {"type": "integer", "minimum": 0, "maximum": 255}},
            "on": {"type": "boolean"},
            "text": {"type": "string", "minLength": 1, "maxLength": 40},
            "duration": {"type": "integer", "minimum": 1, "maximum": 120},
            "count": {"type": "integer", "minimum": 1, "maximum": 20},
            "interval_ms": {"type": "integer", "minimum": 200, "maximum": 10000},
            "period_ms": {"type": "integer", "minimum": 500, "maximum": 60000},
            "duration_ms": {"type": "integer", "minimum": 1, "maximum": 600000},
            "speed": {"type": "integer", "minimum": 1, "maximum": 100},
        },
    },
}


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
            elif name == "device_control":
                output = json.dumps(lan_devices.control(**args), ensure_ascii=False)
            elif name == "agent_execute":
                run = subprocess.run(["picoclaw", "--no-color", "agent", "--session", "cli:xiaozhi-picoclaw",
                                      "-m", args["prompt"]],
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
    endpoint = os.environ.get("XIAOZHI_MCP_ENDPOINT", "")
    if endpoint and not endpoint.startswith("wss://"):
        raise ValueError("Expected a wss endpoint")
    script = Path(__file__).resolve()
    reader, writer = os.pipe()
    with os.fdopen(reader, encoding="utf-8") as replies, subprocess.Popen(
            [script.with_name("scheduler"), sys.executable, script, str(writer)],
            pass_fds=(writer,), stdin=subprocess.PIPE, text=True, encoding="utf-8") as proc:
        os.close(writer)

        if not endpoint:
            print("MCP endpoint unset; running scheduler offline", flush=True)
            proc.wait()
            raise SystemExit("Scheduler stopped")

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
                with closing(websocket.create_connection(endpoint, timeout=15)) as ws:
                    print("MCP WebSocket connected", flush=True)
                    ws.settimeout(None)
                    while True:
                        raw = ws.recv()
                        if not raw:
                            break
                        request = json.loads(raw)
                        response = dispatch(request, schedule)
                        if response is not None:
                            ws.send(json.dumps(response, ensure_ascii=False))
                            if request.get("method") == "tools/call":
                                params = request.get("params")
                                name = params.get("name") if isinstance(params, dict) else None
                                if isinstance(name, str) and name in tools:
                                    status = "error" if response["result"].get("isError") else "ok"
                                    print(f"MCP tool {name}: {status}", flush=True)
            except (websocket.WebSocketException, OSError, ValueError) as exc:
                print(type(exc).__name__, file=sys.stderr, flush=True)
            time.sleep(3)
        raise SystemExit("Scheduler stopped")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--action":
        print(action(sys.argv[2]))
    else:
        main()
