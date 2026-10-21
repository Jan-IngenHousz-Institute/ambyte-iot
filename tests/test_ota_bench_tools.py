import contextlib
import base64
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import bench_signed_ota as bench
from tools.check_release_signing import BENCH_KEY, bench_id, check_release
from flash_gui.firmware_metadata import ImageError, _check_image, inspect_image, verify_signature
from ota_fixture import image


class BenchToolsTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.unsigned=self.root/"intermediate.bin"
        self.unsigned.write_bytes(image());self.out=self.root/"fixtures"
        self.records=bench.fixtures(self.unsigned,BENCH_KEY,self.out)

    def test_fixture_versions_and_crypto_or_policy_failure_are_distinct(self):
        self.assertEqual(len(self.records),8)
        candidate=(self.out/"candidate.bin").read_bytes()
        self.assertEqual(inspect_image(candidate).key_id,bench_id())
        self.assertEqual(inspect_image(candidate).version,"2.8.0-a5-r1")
        for i,case in enumerate(bench.CASES[1:],1):
            data=(self.out/f"{case}.bin").read_bytes()
            self.assertEqual(data[48:80].split(b"\0",1)[0].decode(),f"2.8.0-a5-n{i}")
            if case in ("code-flip","manifest-flip"):
                with self.assertRaisesRegex(ImageError,"signature image digest"):
                    inspect_image(data)
            elif case=="wrong-key":self.assertNotEqual(inspect_image(data).key_id,bench_id())
            elif case=="unsigned":self.assertFalse(inspect_image(data).signature_present)
            else:
                self.assertEqual(verify_signature(data,len(data)-4096),bench_id())

    def test_url_limit_is_exact_and_staging_records_lengths_before_refusing(self):
        url="https://s3.example/"+"x"*(2047-len("https://s3.example/"))
        self.assertEqual(len(bench.exact_url(url)),2047)
        with self.assertRaises(ImageError):bench.exact_url(url+"x")
        for n in (2047,2048):
            s3=Mock();s3.generate_presigned_url.return_value="https://s3.example/"+"x"*(n-len("https://s3.example/"))
            s3.put_object.return_value={}
            session=Mock();session.client.return_value=s3
            with self.subTest(length=n):
                if n==2048:
                    with self.assertRaises(ImageError):bench.stage(session,"dev-bucket","run",self.out)
                    s3.put_object.assert_not_called()
                else:
                    bench.stage(session,"dev-bucket","run",self.out)
                    self.assertEqual(s3.put_object.call_count,8)
                lengths=json.loads((self.out/"url-lengths.json").read_text())
                self.assertEqual(set(lengths.values()),{n})
                self.assertNotIn("https://",(self.out/"url-lengths.json").read_text())

    def test_altered_fixtures_pass_image_checksums_but_fail_signature(self):
        for case in ("code-flip","manifest-flip"):
            with self.subTest(case=case):
                data=(self.out/f"{case}.bin").read_bytes()
                self.assertEqual(_check_image(data)[1],self.records[case]["raw_size"])
                with self.assertRaisesRegex(ImageError,"signature image digest"):
                    verify_signature(data,self.records[case]["padded_size"])

    def test_target_and_group_refusals(self):
        for target in ("", "arn:aws:iot:eu-central-1:084375565727:thinggroup/dev", "one,two", "ambyte_00:00:00:00:00:00"):
            with self.subTest(target=target),self.assertRaises(ImageError):bench.require_target(target)

    def test_preflight_detects_old_approval_wait_and_scheduled_group_job(self):
        def gh(args,**kwargs):
            if args[1]=="api":
                return types.SimpleNamespace(stdout=json.dumps({"content":base64.b64encode(b"on:\n  workflow_dispatch:\n").decode()}))
            self.assertIn("--status",args)
            state=args[args.index("--status")+1]
            return types.SimpleNamespace(stdout=json.dumps([{"databaseId":1,"status":"waiting","displayTitle":"old approval"}]
                if state=="waiting" else []))
        with patch.object(bench.subprocess,"run",side_effect=gh):
            with self.assertRaisesRegex(ImageError,"workflow"):bench.preflight(Mock(),self.out)
        def quiet(args,**kwargs):
            if args[1]=="api":
                return types.SimpleNamespace(stdout=json.dumps({"content":base64.b64encode(b"on:\n  workflow_dispatch:\n").decode()}))
            return types.SimpleNamespace(stdout="[]")
        session=Mock()
        iot=session.client.return_value
        group=f"arn:aws:iot:{bench.REGION}:{bench.ACCOUNT}:thinggroup/bench-parent"
        member=f"arn:aws:iot:{bench.REGION}:{bench.ACCOUNT}:thinggroup/bench-child"
        iot.get_paginator.return_value.paginate.side_effect=[
            [{"thingGroups":[{"groupName":"bench-child","groupArn":member}]}],
            [{"jobs":[{"jobId":"group-rollout","status":"SCHEDULED"}]}]]
        iot.describe_thing_group.return_value={"thingGroupMetadata":{"rootToParentThingGroups":[{"groupArn":group}]}}
        iot.describe_job.return_value={"job":{"targets":[group]}}
        with patch.object(bench.subprocess,"run",side_effect=quiet):
            with self.assertRaisesRegex(ImageError,"IoT Job"):bench.preflight(session,self.out)

    def test_preflight_ignores_unrelated_jobs_and_blocks_direct_bench_target(self):
        def quiet(args,**kwargs):
            return types.SimpleNamespace(stdout=json.dumps({"content":base64.b64encode(b"on:\n  workflow_dispatch:\n").decode()})
                if args[1]=="api" else "[]")
        iot=Mock();session=Mock();session.client.return_value=iot
        for targets,blocked in ((["arn:aws:iot:eu-central-1:084375565727:thing/another"],False),
                ([f"arn:aws:iot:{bench.REGION}:{bench.ACCOUNT}:thing/{bench.THING}"],True)):
            iot.get_paginator.return_value.paginate.side_effect=[
                [{"thingGroups":[]}],[{"jobs":[{"jobId":"active-job","status":"IN_PROGRESS"}]}]]
            iot.describe_job.return_value={"job":{"targets":targets}}
            with self.subTest(blocked=blocked),patch.object(bench.subprocess,"run",side_effect=quiet):
                if blocked:
                    with self.assertRaisesRegex(ImageError,"IoT Job"):bench.preflight(session,self.out)
                else:bench.preflight(session,self.out)

    def test_partial_staging_keeps_owned_object_intent_and_returned_version(self):
        s3=Mock();s3.generate_presigned_url.return_value="https://s3.example/object"
        s3.put_object.side_effect=[{"VersionId":"first-version"},RuntimeError("upload interrupted")]
        session=Mock();session.client.return_value=s3
        with self.assertRaises(RuntimeError):bench.stage(session,"dev-bucket","run",self.out)
        state=json.loads((self.out/"delivery.json").read_text())
        self.assertEqual(len(state["objects"]),8)
        self.assertEqual(state["objects"]["candidate"]["version_ids"],["first-version"])
        self.assertNotIn("https://",(self.out/"delivery.json").read_text())

    def test_job_targets_one_exact_thing_with_asset_identity_and_snapshot_selection(self):
        bench.save(self.out/"delivery.json",{"run":"run","jobs":[],"objects":{
            c:{"bucket":"dev-bucket","key":f"bench-a5/run/{c}"} for c in bench.CASES}})
        s3=Mock();s3.generate_presigned_url.return_value="https://s3.example/object"
        iot=Mock();arn=f"arn:aws:iot:{bench.REGION}:{bench.ACCOUNT}:thing/{bench.THING}"
        iot.describe_thing.return_value={"thingArn":arn}
        iot.describe_job_execution.return_value={"execution":{"status":"FAILED","statusDetails":{"reason":"fallback_cannot_read_written"}}}
        session=Mock();session.client.side_effect=lambda name:s3 if name=="s3" else iot
        import fleet_deploy
        with patch.object(bench,"preflight"),patch.object(fleet_deploy,"fleet_ping",return_value={bench.THING:"2.8.0-a5-r1"}):
            self.assertEqual(bench.delivery(session,"job","run","incompatible",self.out),1)
        args=iot.create_job.call_args.kwargs
        self.assertEqual(args["targets"],[arn]);self.assertEqual(args["targetSelection"],"SNAPSHOT")
        document=json.loads(args["document"])
        self.assertEqual(document["version"],self.records["incompatible"]["version"])
        self.assertEqual(document["sha256"],self.records["incompatible"]["sha256"])
        self.assertEqual(json.loads((self.out/"delivery.json").read_text())["jobs"],[args["jobId"]])

    def test_delivery_oversize_url_records_only_length_before_job_or_mqtt(self):
        state={"run":"run","jobs":[],"objects":{c:{"bucket":"dev-bucket","key":f"bench-a5/run/{c}"} for c in bench.CASES}}
        bench.save(self.out/"delivery.json",state)
        session=Mock();s3=session.client.return_value
        s3.generate_presigned_url.return_value="https://s3.example/"+"x"*2030
        for path in ("https","job"):
            with self.subTest(path=path),patch.object(bench,"preflight") as preflight:
                with self.assertRaises(ImageError):bench.delivery(session,path,"run","candidate",self.out)
                preflight.assert_not_called()
                s3.create_job.assert_not_called()
                evidence=(self.out/"delivery.json").read_text()
                self.assertNotIn("https://",evidence)
                self.assertEqual(json.loads(evidence)["objects"]["candidate"]["url_length"],2049)


class DevTransportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0,str(ROOT/"tools/fleet_deploy"))
        import fleet_command
        cls.command=fleet_command

    def test_prod_profile_alias_is_refused_before_publish(self):
        session=Mock();session.client.return_value.get_caller_identity.return_value={"Account":"494249241400"}
        with patch.object(self.command.fleet_deploy,"boto_session",return_value=session),\
             patch.object(self.command.fleet_deploy,"fleet_ota") as send,\
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):self.command.main(["ota","--profile","deceptively-dev",
                "--devices",bench.THING,"--token","a5-trial","--url","https://example.test/app"])
            send.assert_not_called()

    def test_no_discovery_or_multi_target_and_no_workflow_choice(self):
        for targets in ("",bench.THING+",AMBYTE_00:00:00:00:00:00"):
            with patch.object(self.command.fleet_deploy,"boto_session") as session,contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):self.command.main(["ota","--devices",targets,"--token","t","--url","https://example.test/app"])
                session.assert_not_called()
        workflow=(ROOT/".github/workflows/fleet-command.yml").read_text()
        self.assertIn("options: [ping, inventory, replay]",workflow)
        self.assertNotIn("options: [ping, inventory, replay, ota]",workflow)

    def test_dev_command_uses_shared_sender_without_logging_url(self):
        session=Mock();session.client.return_value.get_caller_identity.return_value={"Account":bench.ACCOUNT}
        result={bench.THING:{"state":"failed","detail":"signature_invalid"}}
        output=io.StringIO()
        with patch.object(self.command.fleet_deploy,"boto_session",return_value=session),\
             patch.object(self.command.fleet_deploy,"fleet_ota",return_value=(result,None,0)) as send,\
             contextlib.redirect_stdout(output):
            code=self.command.main(["ota","--devices",bench.THING,"--token","a5-trial","--url","https://example.test/app?secret=hidden"])
            self.assertEqual(code,1)
            self.assertEqual(send.call_args.args[1],[bench.THING])
            self.assertNotIn("secret",output.getvalue())
