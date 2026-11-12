#!/usr/bin/env python3
"""One dev bench thing, scoped S3 fixtures and network OTA evidence. No flashing."""
from __future__ import annotations
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
sys.path.insert(0,str(ROOT/"tools/fleet_deploy"))
from flash_gui.firmware_metadata import (ImageError, MANIFEST, MANIFEST_V1, app_layout,
    inspect_image, repair_fixture_checksums, rewrite_fixture, sign_image)
from tools.check_release_signing import BENCH_KEY, bench_id, key_id, private_key

THING="ambyte_28:37:2F:FF:E7:04"
ACCOUNT="084375565727"
REGION="eu-central-1"
CASES=("candidate","code-flip","manifest-flip","wrong-key","unsigned","incompatible","missing-manifest","missing-fallback")
WORKFLOWS=("fleet-deploy.yml","fleet-deploy-schedule.yml","fleet-deploy-ambit.yml","fleet-command.yml")
REPO="Jan-IngenHousz-Institute/ambyte-iot"


def exact_url(url):
    if not url.startswith("https://") or len(url)>=2048:
        raise ImageError(f"presigned URL refused before send: length={len(url)} (maximum2047)")
    return url


def require_target(thing):
    if thing!=THING:
        raise ImageError("only the exact approved dev bench thing is allowed; groups/discovery are refused")


def session_for(profile,thing):
    require_target(thing)
    import boto3
    session=boto3.Session(profile_name=profile,region_name=REGION)
    if session.client("sts").get_caller_identity()["Account"]!=ACCOUNT:
        raise ImageError("bench delivery refuses any account other than dev084375565727")
    return session


def save(path,data):
    path.write_text(json.dumps(data,indent=2)+"\n")


def fixtures(unsigned,key_path,out,candidate_version="2.8.0-a5-r1"):
    from cryptography.hazmat.primitives.asymmetric import rsa
    raw=unsigned.read_bytes()
    inspect_image(raw,require_manifest=True)
    key=private_key(key_path.read_bytes())
    if key_id(key)!=bench_id():raise ImageError("fixture generation requires the explicit public bench key")
    out.mkdir(parents=True,exist_ok=True)
    if (out/"fixtures.json").exists():raise ImageError("preserve existing fixture evidence; use a fresh output directory")
    other=rsa.generate_private_key(public_exponent=65537,key_size=3072)
    records={}
    for index,case in enumerate(CASES):
        version=candidate_version if not index else f"2.8.0-a5-n{index}"
        manifest=MANIFEST_V1
        if case=="incompatible":manifest=MANIFEST.pack(b"AMBYTEM1",1,32,9,0,6,2,1,0)
        elif case=="missing-manifest":manifest=bytes(32)
        elif case=="missing-fallback":manifest=MANIFEST.pack(b"AMBYTEM1",1,32,9,0,2,1,0,0)
        ordinary=rewrite_fixture(raw,version=version,manifest=manifest)
        payload=sign_image(ordinary,other if case=="wrong-key" else key,fixture_manifest=True)
        if case=="unsigned":payload=payload[:-4096]
        if case=="manifest-flip":
            data=bytearray(payload);data[0x120+16]^=1;payload=bytes(data)
        if case=="code-flip":
            offset=24;code=None
            for _ in range(payload[1]):
                address,length=struct.unpack_from("<II",payload,offset)
                if length>16 and (0x42000000<=address<0x44000000 or 0x40370000<=address<0x40400000):
                    code=offset+8+16;break
                offset+=8+length
            if code is None:raise ImageError("fixture image has no executable code segment")
            data=bytearray(payload);data[code]^=1;payload=bytes(data)
        if case in ("code-flip","manifest-flip"):
            payload=repair_fixture_checksums(payload)
        file=out/f"{case}.bin";file.write_bytes(payload)
        _,raw_size=app_layout(ordinary)
        records[case]={"file":file.name,"version":version,"sha256":hashlib.sha256(payload).hexdigest(),
                       "raw_size":raw_size,"padded_size":(raw_size+4095)//4096*4096,"size":len(payload)}
    save(out/"fixtures.json",records)
    return records


def preflight(session,out):
    evidence={"workflows":{},"active_jobs":[]}
    for workflow in WORKFLOWS:
        # Query live states directly: an old approval-waiting run must not
        # disappear behind the newest 100 completed runs.
        runs=[]
        for status in ("queued","in_progress","requested","waiting","pending"):
            result=subprocess.run(["gh","run","list","--repo",REPO,"--workflow",workflow,"--status",status,
                "--limit","1","--json","databaseId,status,displayTitle"],capture_output=True,text=True,check=True)
            runs.extend(json.loads(result.stdout))
        result=subprocess.run(["gh","api",f"repos/{REPO}/contents/.github/workflows/{workflow}"],
                              capture_output=True,text=True,check=True)
        source=base64.b64decode(json.loads(result.stdout)["content"]).decode()
        evidence["workflows"][workflow]=runs
        if re.search(r"^  schedule:",source,re.M) or any(r["status"]!="completed" for r in runs):
            save(out/"preflight.json",evidence)
            raise ImageError("conflicting or scheduled fleet workflow; bench qualification refused")
    # Include static/dynamic memberships and inherited parent group targets.
    iot=session.client("iot")
    bench_targets={f"arn:aws:iot:{REGION}:{ACCOUNT}:thing/{THING}"}
    for page in iot.get_paginator("list_thing_groups_for_thing").paginate(thingName=THING):
        for group in page.get("thingGroups",[]):
            bench_targets.add(group["groupArn"])
            description=iot.describe_thing_group(thingGroupName=group["groupName"])
            for parent in description.get("thingGroupMetadata",{}).get("rootToParentThingGroups",[]):
                bench_targets.add(parent["groupArn"])
    state_path=out/"delivery.json"
    owned=set(json.loads(state_path.read_text()).get("jobs",[])) if state_path.exists() else set()
    pages=iot.get_paginator("list_jobs").paginate()
    for page in pages:
        for job in page.get("jobs",[]):
            if job.get("status") not in ("COMPLETED","CANCELED","DELETION_IN_PROGRESS"):
                detail=iot.describe_job(jobId=job["jobId"])["job"]
                if not bench_targets.intersection(detail.get("targets",[])):
                    continue
                if job["jobId"] in owned:
                    execution=iot.describe_job_execution(jobId=job["jobId"],thingName=THING)["execution"]
                    if execution["status"] in ("SUCCEEDED","FAILED","REJECTED","CANCELED","TIMED_OUT","REMOVED"):
                        continue
                evidence["active_jobs"].append({"id":job["jobId"],"status":job.get("status")})
    save(out/"preflight.json",evidence)
    if evidence["active_jobs"]:raise ImageError("active/scheduled dev IoT Job; finish or clean it before the trial")


def load_records(out):
    records=json.loads((out/"fixtures.json").read_text())
    for case,item in records.items():
        if case not in CASES or item["file"]!=f"{case}.bin":raise ImageError("invalid fixture path")
        data=(out/item["file"]).read_bytes()
        if hashlib.sha256(data).hexdigest()!=item["sha256"]:raise ImageError("fixture bytes changed after evidence was recorded")
    return records


def url_for(s3,bucket,key):
    return s3.generate_presigned_url("get_object",Params={"Bucket":bucket,"Key":key},ExpiresIn=3600)


def stage(session,bucket,run,out):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,24}",run):raise ImageError("run label must contain1..24 safe characters")
    records=load_records(out);s3=session.client("s3")
    # All URLs/limits are checked before the first cloud mutation, including
    # SSO-session tokens. URL credentials are never written to evidence.
    objects={};urls=[]
    for case,item in records.items():
        key=f"bench-a5/{run}/{item['sha256']}/{case}.bin"
        url=url_for(s3,bucket,key)
        urls.append(url)
        objects[case]={"bucket":bucket,"key":key,"url_length":len(url)}
    save(out/"url-lengths.json",{case:item["url_length"] for case,item in objects.items()})
    for url in urls:exact_url(url)
    state_path=out/"delivery.json"
    if state_path.exists():
        state=json.loads(state_path.read_text())
        if state["run"]!=run or {c:i["key"] for c,i in state["objects"].items()}!={c:i["key"] for c,i in objects.items()}:
            raise ImageError("staging differs from owned run; preserve evidence and use a fresh output directory")
    else:
        state={"run":run,"objects":objects,"jobs":[]}
    # Intent is durable before each upload. A lost response still leaves an
    # owned key for cleanup; successful version IDs are recorded immediately.
    save(state_path,state)
    for case,item in objects.items():
        response=s3.put_object(Bucket=bucket,Key=item["key"],Body=(out/records[case]["file"]).read_bytes(),
                               ContentType="application/octet-stream",ExpectedBucketOwner=ACCOUNT)
        if response.get("VersionId"):
            state["objects"][case].setdefault("version_ids",[]).append(response["VersionId"])
        save(state_path,state)


def delivery(session,path,run,case,out):
    records=load_records(out);state=json.loads((out/"delivery.json").read_text())
    if state["run"]!=run:raise ImageError("delivery run does not match recorded staging")
    object=state["objects"][case]
    url=url_for(session.client("s3"),object["bucket"],object["key"])
    object["url_length"]=len(url);save(out/"delivery.json",state)
    exact_url(url)
    preflight(session,out)
    import fleet_deploy
    observed=fleet_deploy.fleet_ping(session,[THING],25).get(THING)
    if not observed or observed.lstrip("vV")==records[case]["version"]:
        raise ImageError("running version unavailable or equals fixture; Jobs shortcut would invalidate the trial")
    request=f"a5-{run}-{case}-{uuid.uuid4().hex[:8]}"
    if path=="https":
        import fleet_command
        return fleet_command.main(["ota","--profile",session.profile_name,"--devices",THING,"--url",url,
            "--token",request,"--wait","2100","--results-json",str(out/f"{request}.json")])
    iot=session.client("iot")
    arn=iot.describe_thing(thingName=THING)["thingArn"]
    if arn!=f"arn:aws:iot:{REGION}:{ACCOUNT}:thing/{THING}":raise ImageError("unexpected thing ARN")
    document={"operation":"firmware-update","family":"ambyte","version":records[case]["version"],
              "sha256":records[case]["sha256"],"url":url}
    # Record the intended ID before create, so cleanup can recover a lost reply.
    state["jobs"].append(request);save(out/"delivery.json",state)
    iot.create_job(jobId=request,targets=[arn],document=json.dumps(document),targetSelection="SNAPSHOT")
    deadline=time.monotonic()+2100
    while time.monotonic()<deadline:
        try:
            execution=iot.describe_job_execution(jobId=request,thingName=THING)["execution"]
        except iot.exceptions.ResourceNotFoundException:
            time.sleep(2)
            continue
        status=execution["status"]
        if status in ("SUCCEEDED","FAILED","REJECTED","CANCELED","TIMED_OUT","REMOVED"):
            save(out/f"{request}.json",{"job_id":request,"status":status,"version":records[case]["version"],
                "sha256":records[case]["sha256"],"url_length":len(url),"details":execution.get("statusDetails",{})})
            return 0 if status=="SUCCEEDED" else 1
        time.sleep(2)
    raise ImageError("Job outcome timed out; preserve evidence and run cleanup")


def cleanup(session,run,out):
    state=json.loads((out/"delivery.json").read_text());iot=session.client("iot")
    if state["run"]!=run:raise ImageError("cleanup run differs from staging")
    evidence={"jobs":[],"objects":[]}
    for job in state["jobs"]:
        if not job.startswith(f"a5-{run}-"):raise ImageError("refusing cleanup outside owned run")
        try:
            status=iot.describe_job(jobId=job)["job"]["status"]
            if status not in ("COMPLETED","CANCELED"):
                iot.cancel_job(jobId=job,force=True)
            iot.delete_job(jobId=job,force=True)
            deadline=time.monotonic()+60
            while True:
                try:iot.describe_job(jobId=job)
                except iot.exceptions.ResourceNotFoundException:break
                if time.monotonic()>=deadline:raise ImageError("job deletion not confirmed")
                time.sleep(1)
        except iot.exceptions.ResourceNotFoundException:pass
        evidence["jobs"].append({"id":job,"absent":True})
        save(out/"cleanup.json",evidence)
    for item in state["objects"].values():
        if not item["key"].startswith(f"bench-a5/{run}/"):raise ImageError("refusing object cleanup outside run")
        s3=session.client("s3")
        # Enumerate only this exact owned key, recovering version IDs even if
        # put_object committed before its response was lost.
        versions=[]
        for page in s3.get_paginator("list_object_versions").paginate(Bucket=item["bucket"],Prefix=item["key"]):
            versions.extend(v for group in ("Versions","DeleteMarkers") for v in page.get(group,[]) if v["Key"]==item["key"])
        if versions:
            for version in versions:
                s3.delete_object(Bucket=item["bucket"],Key=item["key"],VersionId=version["VersionId"],ExpectedBucketOwner=ACCOUNT)
        else:s3.delete_object(Bucket=item["bucket"],Key=item["key"],ExpectedBucketOwner=ACCOUNT)
        evidence["objects"].append({"key":item["key"],"deleted":True})
        save(out/"cleanup.json",evidence)


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("operation",choices=("fixtures","preflight","stage","https","job","cleanup"))
    p.add_argument("--unsigned",type=Path);p.add_argument("--bench-key",type=Path,default=BENCH_KEY)
    p.add_argument("--candidate-version",default="2.8.0-a5-r1")
    p.add_argument("--out",type=Path,required=True);p.add_argument("--profile",default="jii-dev-084")
    p.add_argument("--thing",default=THING);p.add_argument("--bucket");p.add_argument("--run")
    p.add_argument("--case",choices=CASES,default="candidate")
    a=p.parse_args(argv)
    try:
        require_target(a.thing);a.out.mkdir(parents=True,exist_ok=True)
        if a.operation=="fixtures":
            if not a.unsigned:p.error("fixtures needs --unsigned")
            fixtures(a.unsigned,a.bench_key,a.out,a.candidate_version);return 0
        session=session_for(a.profile,a.thing)
        if a.operation=="preflight":preflight(session,a.out);return 0
        if not a.run or not re.fullmatch(r"[A-Za-z0-9_-]{1,24}",a.run):p.error("a safe --run is required")
        if a.operation=="stage":
            if not a.bucket:p.error("stage needs an explicitly selected existing dev --bucket")
            stage(session,a.bucket,a.run,a.out);return 0
        if a.operation=="cleanup":cleanup(session,a.run,a.out);return 0
        return delivery(session,a.operation,a.run,a.case,a.out)
    except ImageError as exc:
        print(f"Bench operation refused: {exc}",file=sys.stderr);return 2
    except Exception as exc:
        # SDK/network exceptions may contain the full presigned request URL.
        print(f"Bench operation failed ({type(exc).__name__}); preserve local evidence and restore/cleanup as needed",file=sys.stderr)
        return 2


if __name__=="__main__":raise SystemExit(main())
