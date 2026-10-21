"""Exercise the explicit cutover path against published bytes and live pongs."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

from ota_fixture import image
from tools.check_release_signing import ImageError, load_boundary
from tools.fleet_deploy import fleet_deploy as fleet

ROOT=Path(__file__).resolve().parents[1]
REPO="Jan-IngenHousz-Institute/ambyte-iot"
TAG="v2.7.0"
URL=f"https://github.com/{REPO}/releases/download/{TAG}/firmware.bin"
THING="AMBYTE_00:11:22:33:44:55"


class LegacyRolloutTest(unittest.TestCase):
    def setUp(self):
        self.raw=image("2.7.0",modern=False)
        self.release={"tag_name":TAG,"draft":False,"published_at":"2026-10-01T00:00:00Z",
            "assets":[{"name":"firmware.bin","browser_download_url":URL,
                       "digest":"sha256:"+hashlib.sha256(self.raw).hexdigest()}]}
        self.pin=self.enterContext(patch("tools.check_release_signing.load_pin",return_value="fw-rsa3072:"+"1"*64))
        self.boundary=self.enterContext(patch("tools.check_release_signing.load_boundary",return_value="2.9.0"))

    def open(self,request,**kwargs):
        return io.BytesIO(json.dumps(self.release).encode() if request.full_url.startswith("https://api.github.com/") else self.raw)

    def test_published_pre_signing_asset_and_github_digest_are_required(self):
        with patch.object(fleet.urllib.request,"urlopen",side_effect=self.open):
            result=fleet.verify_legacy_release(REPO,TAG,URL)
        self.assertEqual(result["sha256"],hashlib.sha256(self.raw).hexdigest())
        for case in ("unpublished","digest","missing_digest","arbitrary_url","boundary","later_tag","wrong_repo"):
            with self.subTest(case=case):
                saved=json.loads(json.dumps(self.release))
                if case=="unpublished":self.release["draft"]=True
                if case=="digest":self.release["assets"][0]["digest"]="sha256:"+"0"*64
                if case=="missing_digest":self.release["assets"][0].pop("digest")
                tag="v2.9.0" if case=="boundary" else "v3.0.0" if case=="later_tag" else TAG
                url="https://example.test/arbitrary.bin" if case=="arbitrary_url" else URL
                repo="someone/ambyte-iot" if case=="wrong_repo" else REPO
                with patch.object(fleet.urllib.request,"urlopen",side_effect=self.open),self.assertRaises((ImageError,RuntimeError)):
                    fleet.verify_legacy_release(repo,tag,url)
                self.release=saved

    def main(self,legacy,signed=False,url=None):
        args=["fleet_deploy.py","--tag",TAG,"--devices",THING,"--stagger","0"]
        if legacy:args.append("--legacy-unsigned-rollout")
        if url:args += ["--url",url]
        self.sender=self.enterContext(patch.object(fleet,"fleet_ota",return_value=(
            {THING:{"ack":True,"state":"success","detail":None,"fw":"2.7.0"}},None,0)))
        replies={THING:{"fw":"2.6.0","signed":signed,"storage":None}} if legacy else {THING:"2.6.0"}
        with patch.object(sys,"argv",args),patch.object(fleet,"boto_session",return_value=Mock()),\
             patch.object(fleet,"fleet_ping",return_value=replies),\
             patch.object(fleet.urllib.request,"urlopen",side_effect=self.open),\
             contextlib.redirect_stdout(io.StringIO()) as output:
            result=fleet.main()
        return result,output.getvalue()

    def test_explicit_flag_uses_legacy_path_and_names_it_in_summary(self):
        with tempfile.TemporaryDirectory() as td,patch.dict("os.environ",{"GITHUB_STEP_SUMMARY":str(Path(td)/"summary")}):
            result,output=self.main(True)
            self.assertEqual(result,0)
            self.sender.assert_called_once()
            self.assertIn("LEGACY UNSIGNED ROLLOUT",output)
            self.assertIn("LEGACY UNSIGNED ROLLOUT",(Path(td)/"summary").read_text())

    def test_signed_target_and_missing_flag_refuse_before_ota_publish(self):
        for flag,signed in ((True,True),(True,None),(False,False)):
            with self.subTest(flag=flag,signed=signed),self.assertRaises((ImageError,RuntimeError)):
                self.main(flag,signed)
            self.sender.assert_not_called()

    def test_silent_target_refused_and_missing_field_is_legacy_unsigned(self):
        fleet.require_unsigned_targets([THING],{THING:{"signed":False}})
        with self.assertRaises(RuntimeError):fleet.require_unsigned_targets([THING],{})

    def test_half_configuration_refuses_both_delivery_paths(self):
        for path in ("signed","legacy"):
            for missing in ("pin","boundary"):
                loader=self.pin if missing=="pin" else self.boundary
                loader.side_effect=ImageError("UNCONFIGURED")
                with self.subTest(path=path,missing=missing),patch.object(fleet.urllib.request,"urlopen") as request,\
                     self.assertRaises(ImageError):
                    if path=="signed":fleet.verify_firmware_url(URL)
                    else:fleet.verify_legacy_release(REPO,TAG,URL)
                request.assert_not_called()
                loader.side_effect=None

    def test_boundary_placeholder_and_malformed_values_refuse(self):
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/"boundary"
            for value in ("UNCONFIGURED","","v2.9.0","2.9","02.9.0","0.0.0"):
                p.write_text(value)
                with self.subTest(value=value),self.assertRaises(ImageError):load_boundary(p)
            p.write_text("2.9.0\n")
            self.assertEqual(load_boundary(p),"2.9.0")

    def test_prod_approval_environment_and_explicit_workflow_opt_in_remain(self):
        source=(ROOT/".github/workflows/fleet-deploy.yml").read_text()
        self.assertIn("environment: fleet-deploy-${{ inputs.environment }}",source)
        self.assertIn("legacy-unsigned-rollout: ${{ inputs.legacy_unsigned_rollout }}",source)
        action=(ROOT/".github/actions/fleet-deploy/action.yml").read_text()
        self.assertIn('ARGS+=(--legacy-unsigned-rollout)',action)
