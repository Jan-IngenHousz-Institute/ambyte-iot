"""Bounded read-only IoT log inspection. No device commands or policy changes."""
import boto3
import json
import time
from pathlib import Path

CLIENTS = ['ambyte_20:6E:F1:F5:E2:D4', 'ambyte_20:6E:F1:F6:E1:80', 'ambyte_28:37:2F:FF:E6:A8', 'ambyte_28:37:2F:FF:E7:10', 'ambyte_28:37:2F:FF:FD:44', 'ambyte_E8:F6:0A:AF:A2:B8', 'ambyte_E8:F6:0A:B1:1C:B0', 'ambyte_E8:F6:0A:B1:1D:0C', 'ambyte_E8:F6:0A:B1:1D:18', 'ambyte_E8:F6:0A:B1:1E:FC']
logs = boto3.client("logs", region_name="eu-central-1")
now = int(time.time())
client_filter = "clientId in " + json.dumps(CLIENTS)
queries = {
    "delivery_by_topic": "fields clientId, eventType, status, topicName, reason | filter " + client_filter + " | stats count(*) as events, max(@timestamp) as latest by clientId,eventType,status,topicName,reason | limit 1000",
    "recent_failures": "fields @timestamp,clientId,eventType,status,topicName,reason | filter " + client_filter + " | filter status = 'Failure' OR eventType = 'Disconnect' | sort @timestamp desc | limit 100",
}
results = {}
for name, query in queries.items():
    qid = logs.start_query(logGroupName="AWSIotLogsV2", startTime=now-14*3600, endTime=now, queryString=query, limit=1000)["queryId"]
    deadline = time.monotonic() + 120
    while True:
        response = logs.get_query_results(queryId=qid)
        status = response["status"]
        if status not in ("Scheduled", "Running"):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("log query exceeded two-minute deadline")
        time.sleep(2)
    if status != "Complete":
        raise RuntimeError("log query failed: " + status)
    results[name] = [{item["field"]:item["value"] for item in row if item["field"] != "@ptr"} for row in response["results"]]
Path("delivery-diagnostic.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
