import json
import sys
import types
import unittest
from unittest import mock

from tools.fleet_deploy import fleet_deploy


class DeviceCasingTest(unittest.TestCase):
    LOWER = "ambyte_20:6E:F1:FB:60:B4"
    UPPER = "AMBYTE_20:6E:F1:FB:60:B4"

    def test_validation_preserves_exact_route_and_identity_is_case_insensitive(self):
        self.assertEqual(fleet_deploy.normalize_device(self.LOWER), self.LOWER)
        self.assertEqual(fleet_deploy.normalize_device(self.UPPER), self.UPPER)
        self.assertEqual(
            fleet_deploy.unique_devices([self.LOWER, self.UPPER]), [self.LOWER]
        )
        self.assertEqual(
            fleet_deploy.selection_key(self.LOWER),
            fleet_deploy.selection_key(self.UPPER),
        )

    def test_discovery_keeps_newest_observed_route_casing(self):
        class Logs:
            def start_query(inner, **kwargs):
                self.assertIn("max(@timestamp) as lastSeen", kwargs["queryString"])
                self.assertIn("sort lastSeen desc", kwargs["queryString"])
                return {"queryId": "q"}

            def get_query_results(inner, **kwargs):
                return {
                    "status": "Complete",
                    "results": [
                        [{"field": "clientId", "value": self.LOWER}],
                        [{"field": "clientId", "value": self.UPPER}],
                    ],
                }

        session = types.SimpleNamespace(client=lambda service: Logs())
        self.assertEqual(fleet_deploy.discover_active_devices(session, 60), [self.LOWER])

    def test_ping_publishes_exact_route_and_correlates_other_case_reply(self):
        holder = {}

        class Future:
            def result(inner):
                return None

        class Connection:
            def __init__(inner, callback):
                inner.callback = callback
                inner.topics = []

            def publish(inner, *, topic, payload, qos):
                inner.topics.append(topic)
                ping = json.loads(payload)
                inner.callback(
                    "experiment/data_ingest/v1/x/multispeq/v1.0/"
                    + self.UPPER
                    + "/status",
                    json.dumps({"type": "pong", "id": ping["id"], "fw": "1.6.6"}).encode(),
                    False,
                    qos,
                    False,
                )
                return Future(), 1

            def disconnect(inner):
                return Future()

        def connection(session, topic, callback, client_id="fleet-deploy"):
            holder["connection"] = Connection(callback)
            return holder["connection"]

        fake_mqtt = types.SimpleNamespace(QoS=types.SimpleNamespace(AT_LEAST_ONCE=1))
        with mock.patch.object(fleet_deploy, "mqtt_connection", side_effect=connection), mock.patch.dict(
            sys.modules, {"awscrt": types.SimpleNamespace(mqtt=fake_mqtt)}
        ):
            result = fleet_deploy.fleet_ping(object(), [self.LOWER], 1)

        self.assertEqual(result, {self.LOWER: "1.6.6"})
        self.assertEqual(
            holder["connection"].topics,
            [fleet_deploy.COMMAND_TOPIC_FMT.format(device=self.LOWER)],
        )


class TerminalProofTest(unittest.TestCase):
    def setUp(self) -> None:
        self.target = fleet_deploy.parse_version("v1.6.3")

    def reason(self, *, ack=False, state=None, fw=None, verified=None):
        return fleet_deploy.deployment_failure_reason(
            {"ack": ack, "state": state, "detail": None, "fw": fw},
            self.target,
            verified,
        )

    def test_only_success_with_exact_target_version_passes(self) -> None:
        self.assertIsNone(self.reason(ack=True, state="success", fw="1.6.3"))
        self.assertEqual(
            self.reason(ack=True, state="success", fw=None),
            "target_version_not_confirmed",
        )
        self.assertEqual(
            self.reason(ack=True, state="success", fw="1.6.2"),
            "target_version_not_confirmed",
        )

    def test_every_unresolved_or_negative_terminal_is_a_failure(self) -> None:
        self.assertEqual(self.reason(ack=False), "no_reply")
        self.assertEqual(self.reason(ack=True), "accepted_no_final")
        self.assertEqual(self.reason(ack=True, state="dropped"), "dropped")
        self.assertEqual(self.reason(ack=True, state="failed"), "failed")

    def test_fresh_target_ping_recovers_only_missing_terminal_proof(self) -> None:
        self.assertIsNone(self.reason(ack=False, verified="1.6.3"))
        self.assertIsNone(self.reason(ack=True, verified="1.6.3"))
        self.assertEqual(
            self.reason(ack=True, state="success", fw=None, verified="1.6.3"),
            "target_version_not_confirmed",
        )
        self.assertEqual(
            self.reason(ack=True, verified="1.6.2"),
            "post_verify_target_mismatch",
        )
        self.assertEqual(
            self.reason(
                ack=True, state="success", fw="1.6.3", verified="1.6.2"
            ),
            None,
        )

    def test_fresh_ping_never_masks_explicit_failure_or_rollback(self) -> None:
        self.assertEqual(
            self.reason(ack=True, state="failed", verified="1.6.3"),
            "failed",
        )
        self.assertEqual(
            self.reason(ack=True, state="dropped", verified="1.6.3"),
            "dropped",
        )

    def test_unparseable_effect_proof_fails_closed(self) -> None:
        self.assertEqual(
            self.reason(ack=True, verified="unknown"),
            "accepted_no_final",
        )


class EffectPollingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.target = fleet_deploy.parse_version("v1.6.3")
        self.results = {
            "AMBYTE_00:11:22:33:44:55": {
                "ack": True,
                "state": None,
                "detail": None,
                "fw": None,
            }
        }

    def test_old_version_is_retried_until_target_is_proven(self) -> None:
        with mock.patch.object(
            fleet_deploy,
            "fleet_ping",
            side_effect=[
                {"AMBYTE_00:11:22:33:44:55": "1.6.1"},
                {"AMBYTE_00:11:22:33:44:55": "1.6.3"},
            ],
        ) as ping, mock.patch.object(fleet_deploy.time, "sleep"):
            verified, attempts = fleet_deploy.verify_missing_by_effect(
                object(), self.results, self.target,
                fleet_deploy.time.time() + 60, 1, 0,
            )
        self.assertEqual(ping.call_count, 2)
        self.assertEqual(verified, {"AMBYTE_00:11:22:33:44:55": "1.6.3"})
        self.assertEqual(len(attempts), 2)

    def test_verify_exception_preserves_fail_closed_missing_proof(self) -> None:
        with mock.patch.object(
            fleet_deploy, "fleet_ping", side_effect=RuntimeError("offline")
        ):
            verified, attempts = fleet_deploy.verify_missing_by_effect(
                object(), self.results, self.target,
                fleet_deploy.time.time() - 1, 1, 0,
            )
        self.assertEqual(verified, {})
        self.assertEqual(attempts[0]["error"], "RuntimeError: offline")
        self.assertEqual(
            fleet_deploy.deployment_failure_reason(
                self.results["AMBYTE_00:11:22:33:44:55"],
                self.target,
                verified.get("AMBYTE_00:11:22:33:44:55"),
            ),
            "accepted_no_final",
        )


if __name__ == "__main__":
    unittest.main()
