"""Executable contract checks for the production v3 payload builders."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PayloadV3Test(unittest.TestCase):
    @classmethod
    def _reference_v2_raw(cls, trace: dict) -> str:
        # Exact DC_EVENT_ENVELOPE_FMT inner object with the raw production v2
        # data/metadata strings. Do not parse/re-dump: that rewrites 24.60.
        string = lambda value: json.dumps(value, separators=(",", ":"))
        return (
            f'{{"v":2,"measure_id":{trace["measure_id"]},'
            f'"startTicks_UTC":{trace["time"]["start_utc"]},'
            f'"endTicks_UTC":{trace["time"]["end_utc"]},'
            '"timestamp_local":"2026-08-05T21:26:00+02:00",'
            '"published":"2026-08-05T19:26:54Z",'
            f'"channel":{string(trace["channel"])},'
            f'"device":{string(trace["device"])},'
            f'"cmd_raw":{string(trace["protocol"]["cmd"])},'
            f'"tag":{string(trace["tag"])},'
            f'"metadata":{cls.raw_records["TRACE_V2_METADATA"]},'
            f'"data":{cls.raw_records["TRACE_V2_DATA"]}}}'
        )

    @classmethod
    def _reference_v2(cls, trace: dict) -> dict:
        return json.loads(cls._reference_v2_raw(trace))

    @staticmethod
    def _published_envelope(sample: dict) -> dict:
        # Exact field set emitted by DC_EVENT_ENVELOPE_FMT and
        # DC_V3_EVENT_ENVELOPE_FMT for the same gateway state.
        return {
            "sample": [sample],
            "timestamp": "2026-08-05T19:26:00Z",
            "device_battery": 3.912,
            "timezone": "Europe/Amsterdam",
            "device_id": "28:37:2F:FF:E7:04",
            "device_name": "AmbyteOnAir",
            "device_version": "V003",
            "device_firmware": "1.6.6",
        }

    @staticmethod
    def _published_envelope_raw(sample: str) -> str:
        return (
            f'{{"sample":[{sample}],"timestamp":"2026-08-05T19:26:00Z",'
            '"device_battery":3.912,"timezone":"Europe/Amsterdam",'
            '"device_id":"28:37:2F:FF:E7:04","device_name":"AmbyteOnAir",'
            '"device_version":"V003","device_firmware":"1.6.6"}'
        )

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="payload-v3-host-")
        binary = Path(cls.tmp.name) / "payload_v3_host"
        compile_cmd = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{ROOT / 'components/domain/include'}",
            str(ROOT / "tests/payload_v3_host.c"),
            str(ROOT / "components/domain/payload_v3.c"),
            "-lm",
            "-o",
            str(binary),
        ]
        subprocess.run(compile_cmd, check=True, cwd=ROOT)
        result = subprocess.run(
            [str(binary)], check=True, cwd=ROOT, capture_output=True, text=True
        )
        cls.raw_records = dict(
            line.split("=", 1) for line in result.stdout.splitlines()
        )
        cls.records = {
            key: json.loads(value) for key, value in cls.raw_records.items()
        }

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_reference_trace_exact_shape_idx8_and_size(self) -> None:
        trace = self.records["TRACE_IDX8"]
        self.assertEqual(
            set(trace),
            {"schema", "measure_id", "channel", "device", "sensor_id", "tag", "time", "protocol", "series"},
        )
        self.assertEqual(trace["schema"], "ambit.trace/3")
        self.assertEqual(trace["sensor_id"], "10:91:A8:4F:4F:D4")
        self.assertEqual(trace["protocol"]["name"], "SS")
        self.assertNotIn("id", trace["protocol"])
        self.assertEqual(trace["protocol"]["tick_factor"], 0.854)
        self.assertEqual(trace["protocol"]["segments"], [{"pulses": 59, "freq": 1, "actinic": 0}])
        self.assertEqual(trace["series"]["leaf_temp"]["t"], [0, 8.6, 17.1, 25.7, 34.2, 42.8])
        self.assertNotIn("t_est", trace["series"]["leaf_temp"])
        self.assertEqual(trace["series"]["fluo_630_signal"]["v"][-1], 65535)
        self.assertNotIn("timing", trace["series"])
        self.assertNotIn("fluo", trace["series"])

        # Reference v2 carries the same dense values plus its historical envelope
        # and metadata. Compare compact JSON, as it actually travels over MQTT.
        v2_size = len(self._reference_v2_raw(trace))
        v3_size = len(self.raw_records["TRACE_IDX8"])
        # Production type-2 reference: four count arrays, no currently
        # undelivered protocol_id, and the actual frozen v2 metadata leaves.
        # Required v3 descriptors remain within the accepted production gate.
        self.assertEqual((v2_size, v3_size), (1655, 1865))

    def test_published_envelope_size_is_within_accepted_budget(self) -> None:
        trace = self.records["TRACE_IDX8"]
        v2 = self._reference_v2(trace)
        v2_envelope = self._published_envelope_raw(self._reference_v2_raw(trace))
        v3_envelope = self._published_envelope_raw(self.raw_records["TRACE_IDX8"])

        # Complete raw production payloads are 1,866 B vs 2,076 B (+11.25%),
        # inside the accepted ≤13% production-reference gate.
        self.assertEqual((len(v2_envelope), len(v3_envelope)), (1866, 2076))
        growth = (len(v3_envelope) - len(v2_envelope)) / len(v2_envelope)
        self.assertAlmostEqual(growth, 0.1125, places=4)
        self.assertLessEqual(growth, 0.13)

        # Stable byte-category evidence for review. Numeric values are almost
        # entirely shared; the increase is schema/identity/time/protocol and the
        # self-describing series names, units, and time models required by v3.
        compact = lambda value: json.dumps(value, separators=(",", ":"))
        v2_value_bytes = sum(len(compact(v)) for v in v2["data"].values())
        v3_value_bytes = sum(len(compact(v["v"])) for v in trace["series"].values())
        v3_identity_bytes = len(compact({
            key: trace[key]
            for key in ("schema", "measure_id", "channel", "device", "sensor_id", "tag")
        }))
        v3_time_protocol_bytes = len(compact({
            "time": trace["time"], "protocol": trace["protocol"]
        }))
        v3_series_bytes = len(compact({"series": trace["series"]}))
        self.assertEqual(
            {
                "outer": len(v3_envelope) - len(self.raw_records["TRACE_IDX8"]),
                "identity": v3_identity_bytes,
                "time_protocol": v3_time_protocol_bytes,
                "series_total": v3_series_bytes,
                "series_values": v3_value_bytes,
                "series_descriptors": v3_series_bytes - v3_value_bytes,
                "v2_values": v2_value_bytes,
            },
            {
                "outer": 211,
                "identity": 137,
                "time_protocol": 276,
                "series_total": 1451,
                "series_values": 1161,
                "series_descriptors": 290,
                "v2_values": 1182,
            },
        )

    def test_fallback_mixed_frequency_and_subsampling(self) -> None:
        fallback = self.records["TRACE_FALLBACK"]
        leaf = fallback["series"]["leaf_temp"]
        self.assertTrue(leaf["t_est"])
        self.assertEqual(leaf["t"], [0, 8, 16, 24, 32, 40])

        mixed = self.records["TRACE_MIXED"]
        self.assertEqual(
            mixed["series"]["fluo_630_signal"]["t"],
            [0, 0.427, 0.854, 1.281, 2.135],
        )
        self.assertNotIn("dt", mixed["series"]["fluo_630_signal"])

        subsampled = self.records["TRACE_SUBSAMPLE"]
        ambient = subsampled["series"]["ambient_sun_vis"]
        self.assertEqual(ambient["t0"], 2.989)
        self.assertEqual(ambient["dt"], 6.832)

    def test_telemetry_is_one_grouped_object_and_empty_snapshot_is_stable(self) -> None:
        telemetry = self.records["TELEMETRY"]
        self.assertEqual(telemetry["schema"], "ambyte.telemetry/1")
        self.assertEqual(telemetry["tag"], "TELEMETRY")
        self.assertEqual(
            telemetry["observations"]["air_temperature"],
            {"u": "Cel", "v": 24.67},
        )
        self.assertEqual(telemetry["observations"]["relative_humidity"]["u"], "%RH")
        self.assertEqual(telemetry["health"]["attached_sensors"][0]["sensor_id"], "10:91:A8:4F:4F:D4")
        self.assertEqual(telemetry["health"]["attached_sensors"][0]["cal_version"], "6a4356a8")
        self.assertNotIn("calibration", telemetry["health"]["attached_sensors"][0])
        raw = self.raw_records["TELEMETRY"]
        self.assertIn('"v":24.67', raw)
        self.assertIn('"v":61.20', raw)
        self.assertIn('"v":101325.0', raw)
        self.assertIn('"battery_v":3.912,"input_v":5.040,"system_v":3.920', raw)
        self.assertNotIn(
            "cal_version",
            self.records["TELEMETRY_NOCAL"]["health"]["attached_sensors"][0],
        )
        self.assertEqual(
            self.records["TELEMETRY_SD_FAIL"]["health"]["storage"],
            {"db_online": True},
        )

        empty = self.records["TELEMETRY_EMPTY"]
        self.assertEqual(empty["observations"], {})
        self.assertEqual(
            set(empty["health"]),
            {"connectivity", "power", "storage", "runtime", "clock", "software", "attached_sensors"},
        )
        self.assertEqual(empty["health"]["attached_sensors"], [])

    def test_device_object_exact_contract(self) -> None:
        device = self.records["DEVICE"]
        self.assertEqual(device["schema"], "ambit.device/1")
        self.assertEqual(device["identity"], {
            "sensor_id": "10:91:A8:4F:4F:D4",
            "name": "AmbitV003",
            "firmware": "1.1.5",
            "hardware_revision": 1,
            "cal_version": "6a4356a8",
        })
        self.assertEqual(device["calibration"]["mlx_coef"], list(range(1, 15)))
        self.assertEqual(device["calibration"]["adpd"], list(range(100, 106)))
        self.assertEqual(device["calibration"]["act"], [12, 24, 36, 48, 60])
        self.assertEqual(
            self.raw_records["DEVICE"],
            '{"schema":"ambit.device/1","measure_id":26339,"channel":"uart_0",'
            '"device":"AmbitV003","tag":"DEVICE_INFO","time":{"observed_utc":1785965214102},'
            '"identity":{"sensor_id":"10:91:A8:4F:4F:D4","name":"AmbitV003",'
            '"firmware":"1.1.5","hardware_revision":1,"cal_version":"6a4356a8"},'
            '"calibration":{"mlx_coef":[1,2,3,4,5,6,7,8,9,10,11,12,13,14],'
            '"adpd":[100,101,102,103,104,105],"temp_offset":0.0000,"temp_slope":1.0000,'
            '"actinic_coef":0.012345,"spec_coef":1.000000,"act":[12,24,36,48,60],'
            '"mlx_emissivity":0.9800,"sun_coef":1.000000,"tick_factor":0.854000}}',
        )

    def test_production_wiring_preserves_backlogs_and_persists_dedupe(self) -> None:
        source = (ROOT / "components/device_commands/device_commands.c").read_text()
        publisher = source.split("cmd_result_t cmd_mqtt_publish_next_event", 1)[1].split(
            "/* ── Status report", 1
        )[0]
        self.assertIn("payload_v3_is_canonical_object", publisher)
        self.assertIn("DC_EVENT_ENVELOPE_FMT", publisher)  # old rows remain v2
        self.assertIn("DC_V3_EVENT_ENVELOPE_FMT", publisher)

        announcement = source.split("static void ambit_announce_load", 1)[1].split(
            "static esp_err_t ambit_info_fetch", 1
        )[0]
        self.assertIn("nvs_get_str", announcement)
        self.assertIn("payload_v3_select_device_tuple_slot", announcement)
        self.assertLess(
            announcement.index("s_cfg.store_event(&d)"),
            announcement.index("ambit_announce_persist(announce_slot, e)"),
        )

        heartbeat = source.split("cmd_result_t cmd_store_status_event", 1)[1].split(
            "/* Last battery voltage", 1
        )[0]
        self.assertIn("cmd_ambit_device_info_cached", heartbeat)
        self.assertNotIn("ambit_info_fetch", heartbeat)

        lua_source = (ROOT / "components/lua_runner/lua_runner.c").read_text()
        self.assertNotIn("AMBIT_RUN_METADATA_CAP", lua_source)
        self.assertIn("payload_v3_build_trace_lossless", lua_source)
        self.assertNotIn("char fallback_metadata[", lua_source)
        self.assertIn("payload + AMBIT_RUN_PAYLOAD_CAP", lua_source)
        self.assertIn("char *candidate = malloc(AMBIT_RUN_BUFFER_CAP)", lua_source)
        self.assertIn("opts.metadata ignored %u unsupported key(s)", lua_source)
        sync_run = lua_source.split("static int l_ambit_run", 1)[1].split(
            "/* Parallel-run phase 1", 1
        )[0]
        self.assertLess(
            sync_run.index("ambit_capture_protocol_ref(L, 3"),
            sync_run.index("cmd_ambit_run(ch"),
        )
        route_store = lua_source.split("static int ambit_decode_store_push", 1)[1].split(
            "static int l_ambit_run", 1
        )[0]
        self.assertEqual(route_store.count("cmd_store_event(&d)"), 1)
        self.assertIn("payload_v3_build_trace_lossless", route_store)
        self.assertNotIn("cJSON_Parse", route_store)
        self.assertIn("char *payload = ambit_payload_reserve_locked()", route_store)
        self.assertIn("char *fallback_metadata = payload + AMBIT_RUN_PAYLOAD_CAP", route_store)
        self.assertIn(".payload_json  = payload", route_store)
        self.assertNotIn("s_ambit_payload +", route_store)
        self.assertNotIn("s_ambit_payload",
                         route_store.replace("s_ambit_payload_mtx", ""))
        self.assertLess(route_store.index("xSemaphoreTake(s_ambit_payload_mtx"),
                        route_store.index("ambit_payload_reserve_locked()"))

        reservation = lua_source.split(
            "static char *ambit_payload_reserve_locked", 1
        )[1].split("/* ambit.run(channel", 1)[0]
        self.assertIn("if (s_ambit_payload == NULL)", reservation)
        self.assertIn("s_ambit_payload = candidate", reservation)
        self.assertEqual(lua_source.count("s_ambit_payload = candidate"), 1)
        self.assertEqual(lua_source.count("malloc(AMBIT_RUN_BUFFER_CAP)"), 1)
        self.assertEqual(lua_source.count("ambit_payload_reserve_locked()"), 2)

        registration = lua_source.split("static void lua_register_ambit_module", 1)[1].split(
            "static void lua_register_device_module", 1
        )[0]
        self.assertLess(registration.index("ambit_payload_mtx_ensure()"),
                        registration.index("ambit_payload_reserve_locked()"))
        self.assertLess(registration.index("xSemaphoreTake(s_ambit_payload_mtx"),
                        registration.index("ambit_payload_reserve_locked()"))
        self.assertLess(registration.index("ambit_payload_reserve_locked()"),
                        registration.index("xSemaphoreGive(s_ambit_payload_mtx)"))
        self.assertNotIn("s_ambit_payload =", registration)

        fetch = lua_source.split("static int l_ambit_fetch", 1)[1].split(
            "/* ── ambit.* bindings", 1
        )[0]
        self.assertLess(
            fetch.index("payload_v3_can_fetch_retained"),
            fetch.index("cmd_ambit_fetch(ch"),
        )

        self.assertNotIn("char payload[1536]", announcement)
        self.assertIn("malloc(AMBIT_DEVICE_PAYLOAD_CAP)", announcement)
        self.assertLess(announcement.index("free(payload)"),
                        announcement.index("ambit_announce_persist(announce_slot, e)"))


if __name__ == "__main__":
    unittest.main()
