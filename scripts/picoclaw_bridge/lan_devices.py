import http.client
import ipaddress
import json
import os
import re
from urllib.parse import urlencode, urlsplit


def request(host, port, method, path, headers, body, limit, timeout):
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        if not 200 <= response.status < 300:
            raise ValueError(f"设备拒绝请求（HTTP {response.status}）")
        data = response.read(limit + 1)
    except (OSError, http.client.HTTPException) as exc:
        raise ValueError("设备连接失败，操作结果请以设备状态为准") from exc
    finally:
        connection.close()
    if len(data) > limit:
        raise ValueError("设备响应超出大小限制")
    try:
        return json.loads(data)
    except (ValueError, UnicodeError) as exc:
        raise ValueError("设备未返回有效的 JSON") from exc


def private_address(value):
    address = ipaddress.IPv4Address(value)
    if not any(address in ipaddress.IPv4Network(net)
               for net in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")):
        raise ValueError("设备地址必须是局域网 IPv4")
    return str(address)


def water_request(method, path, body=None):
    value = os.getenv("WATER_DISPENSER_IPV4", "")
    if not value:
        raise ValueError("请先配置饮水机地址")
    headers = {"Content-Type": "text/plain"} if body is not None else {}
    return request(private_address(value), 80, method, path, headers, body, 512, 3)


def http_address(variable, label):
    url = urlsplit(os.getenv(variable, ""))
    if (url.scheme != "http" or not url.hostname or url.username is not None
            or url.path not in ("", "/") or url.query or url.fragment):
        raise ValueError(f"请配置{label}的局域网 HTTP 地址")
    private_address(url.hostname)
    return url


def media_address():
    return http_address("FNTV_MEDIA_URL", "飞牛")


def media_request(method, path, body=None):
    url = media_address()
    token = os.getenv("FNTV_MEDIA_TOKEN", "")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", token):
        raise ValueError("请先配置飞牛服务令牌")
    headers = {"Authorization": "Bearer " + token}
    if body is not None:
        headers["Content-Type"] = "application/json"
        body = json.dumps(body).encode("utf-8")
    return request(url.hostname, url.port or 80, method, path, headers, body, 16384, 5)


def water_control(action):
    if action == "status":
        result = water_request("GET", "/api/v1/relays")
    elif action in ("heating_on", "heating_off", "cooling_on", "cooling_off"):
        relay, state = action.split("_")
        result = water_request("PUT", f"/api/v1/relays/{relay}", state.encode("ascii"))
    else:
        raise ValueError("不支持的饮水机操作")
    if not isinstance(result, dict):
        raise ValueError("饮水机状态无效")
    for name in ("heating", "cooling"):
        relay = result.get(name)
        if (not isinstance(relay, dict) or type(relay.get("on")) is not bool
                or type(relay.get("elapsed_seconds")) not in (int, float)
                or not relay["elapsed_seconds"] >= 0):
            raise ValueError("饮水机状态无效")
    if action != "status":
        relay_name = action.split("_")[0]
        if result[relay_name]["on"] != action.endswith("_on"):
            raise ValueError(f"饮水机未确认 {relay_name} 已切换，请查询实际状态")
    return result


MEDIA_WRITES = ("pause", "resume", "next", "fullscreen", "maximize", "minimize")
MEDIA_FIELDS = {
    "seek_relative": "seconds", "seek_absolute": "seconds", "volume": "volume",
    "catalog": "page", "search": "title", "seasons": "itemGuid",
    "episodes": "itemGuid", "play": "itemGuid",
}


def media_control(action, args):
    if action == "discover":
        url = media_address()
        media_request("GET", "/v1/status")
        return {"url": url.geturl()}
    if action == "status":
        return media_request("GET", "/v1/status")
    if action in MEDIA_WRITES:
        return media_request("POST", "/v1/" + action)
    if action in ("seek_relative", "seek_absolute"):
        seconds = args["seconds"]
        if action == "seek_absolute" and seconds < 0:
            raise ValueError("绝对跳转时间不能为负数")
        return media_request("POST", "/v1/seek", {"mode": action[5:], "seconds": seconds})
    if action == "volume":
        return media_request("POST", "/v1/volume", {"volume": args["volume"]})
    if action == "play":
        item = args["itemGuid"]
        if not re.fullmatch(r"[A-Za-z0-9_-]+", item):
            raise ValueError("itemGuid 无效")
        return media_request("POST", "/v1/play", {"itemGuid": item})
    if action in ("catalog", "search", "seasons", "episodes"):
        key = MEDIA_FIELDS[action]
        value = args.get("page", 1) if action == "catalog" else args[key]
        query = urlencode({"q" if key == "title" else key: value})
        return media_request("GET", f"/v1/{action}?{query}")
    raise ValueError("不支持的飞牛操作")


LIGHT_READS = {"status": "/api/light", "time": "/api/time", "effect_status": "/api/light/effect"}
LIGHT_WRITES = {
    "on": ("/api/light", {"on": True}, ()),
    "off": ("/api/light", {"on": False}, ()),
    "brightness": ("/api/light", {}, ("brightness",)),
    "rgb": ("/api/light", {}, ("rgb",)),
    "set": ("/api/light", {}, ("on", "brightness", "rgb")),
    "display": ("/api/display", {"duration": 10}, ("text",)),
    "effect_stop": ("/api/light/effect", {"effect": "none"}, ()),
    "blink": ("/api/light/effect",
              {"effect": "blink", "count": 3, "rgb": [255, 255, 255], "brightness": 20,
               "interval_ms": 500}, ()),
    "breathing": ("/api/light/effect",
                  {"effect": "breathing", "rgb": [255, 175, 100], "brightness": 15,
                   "period_ms": 3000, "duration_ms": 30000}, ()),
    "rainbow": ("/api/light/effect",
                {"effect": "rainbow", "speed": 50, "brightness": 15, "duration_ms": 30000}, ()),
}


def light_request(method, path, payload=None):
    url = http_address("ESP32_LIGHT_URL", "智能灯")
    headers = {"Content-Type": "application/json"} if payload is not None else {}
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
    return request(url.hostname, url.port or 80, method, path, headers, body, 4096, 8)


def light_control(action, args):
    if action in LIGHT_READS:
        if args:
            raise ValueError("查询操作无需参数")
        return light_request("GET", LIGHT_READS[action])
    if action not in LIGHT_WRITES:
        raise ValueError("不支持的智能灯操作")
    path, defaults, required = LIGHT_WRITES[action]
    optional = defaults.keys() - {"effect", "on"}
    if set(args) - (set(required) | optional) or set(required) - args.keys():
        raise ValueError("操作参数与智能灯动作不匹配")
    payload = {**defaults, **args}
    if "brightness" in payload:
        state = light_request("GET", "/api/light")
        maximum = state.get("max_brightness") if isinstance(state, dict) else None
        if type(maximum) is not int or not 0 <= maximum <= 100:
            raise ValueError("无法读取智能灯亮度上限")
        if payload["brightness"] > maximum:
            raise ValueError(f"智能灯亮度上限为 {maximum}")
    return light_request("POST", path, payload)


def control(device, action, **args):
    if device == "light":
        return light_control(action, args)
    field = MEDIA_FIELDS.get(action) if device == "fntv" else None
    if set(args) - {field} or (field and field != "page" and field not in args):
        raise ValueError("操作参数与动作不匹配")
    if device == "water_dispenser":
        return water_control(action)
    return media_control(action, args)
