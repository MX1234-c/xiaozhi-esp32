import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "picoclaw_bridge"))
import bridge


class DeviceControlTests(unittest.TestCase):
    def call(self, **arguments):
        return bridge.dispatch({"id": 1, "method": "tools/call",
                                "params": {"name": "device_control", "arguments": arguments}},
                               lambda _: self.fail("unexpected scheduler call"))["result"]

    def test_water_state_and_write(self):
        status = {
            "heating": {"on": False, "elapsed_seconds": 0},
            "cooling": {"on": False, "elapsed_seconds": 0},
        }
        with patch("lan_devices.water_request", return_value=status) as request:
            self.assertFalse(self.call(device="water_dispenser", action="heating_off").get("isError"))
            request.assert_called_once_with("PUT", "/api/v1/relays/heating", b"off")

    def test_media_seek_and_invalid_volume(self):
        with patch("lan_devices.media_request", return_value={}) as request:
            self.assertFalse(self.call(device="fntv", action="seek_relative", seconds=-30).get("isError"))
            request.assert_called_once_with("POST", "/v1/seek", {"mode": "relative", "seconds": -30})
            self.assertTrue(self.call(device="fntv", action="volume", volume=101)["isError"])
            self.assertEqual(1, request.call_count)

    def test_light_checks_device_brightness_limit_before_writing(self):
        with patch("lan_devices.light_request", return_value={"max_brightness": 80}) as request:
            self.assertTrue(self.call(device="light", action="brightness", brightness=81)["isError"])
            request.assert_called_once_with("GET", "/api/light")
