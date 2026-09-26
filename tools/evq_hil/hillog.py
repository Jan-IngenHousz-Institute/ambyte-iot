"""Parse evq_hil console captures (contract §1.3) into structured records.

Sections: flash inventories (EVQ_FSNAP .. EVQ_FEND), SD inventories
(EVQ_SDSNAP .. EVQ_SDEND), index dumps (EVQ_IXHDR .. EVQ_IXEND), trace drains
(EVQ_TR* .. EVQ_TR_HDR), plus single-line state (EVQ_CUR, EVQ_CLAIMS, EVQ_IO,
EVQ_STATE, EVQ_STACK, HIL_TRACE, HIL_BOOT, HIL_FAULT, evlog text).
"""
from __future__ import annotations

import base64
import re

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# serial_daemon notes ("\n[host <ms>] <msg>\n") may land mid-way through a
# device line; removing the whole insertion rejoins the device's bytes.
HOST_NOTE = re.compile(r"\n\[host \d+\] [^\n]*\n")


def lines_of(text: str):
    text = HOST_NOTE.sub("", text)
    for raw in text.splitlines():
        yield ANSI.sub("", raw).strip("\r")


def _tok(raw: str, tag: str) -> list[str] | None:
    i = raw.find(tag + " ")
    if i < 0 and raw.endswith(tag):
        return []
    if i < 0:
        return None
    return raw[i + len(tag) + 1:].split()


def flash_inventories(text: str) -> list[dict]:
    out, cur = [], None
    for raw in lines_of(text):
        t = _tok(raw, "EVQ_FSNAP")
        if t is not None:
            cur = {"cutoff_id": int(t[0]), "tail_seq": int(t[1]), "nfiles": int(t[2]), "files": {}, "lines": [],
                   "torn": [], "missing": [], "full": {}}
            continue
        if cur is None:
            continue
        if (t := _tok(raw, "EVQ_FL")) is not None:
            if len(t) != 5 or len(t[3]) != 64:
                cur["malformed"] = cur.get("malformed", 0) + 1   # a garbled line voids the inventory
                continue
            cur["lines"].append({"path": t[0], "line": int(t[1]), "id": int(t[2]), "sha256": t[3], "bytes": int(t[4])})
        elif (t := _tok(raw, "EVQ_FLX")) is not None:
            cur["full"][(t[0], int(t[1]))] = base64.b64decode(t[2]) if t[2] != "-" else None
        elif (t := _tok(raw, "EVQ_FF")) is not None:
            cur["files"][t[0]] = {"size": int(t[1]), "sha256": t[2], "crc32": t[3]}
        elif (t := _tok(raw, "EVQ_FT")) is not None:
            cur["torn"].append({"path": t[0], "line": int(t[1]), "bytes": int(t[2])})
        elif (t := _tok(raw, "EVQ_FMISS")) is not None or (t := _tok(raw, "EVQ_FSHORT")) is not None:
            cur["missing"].append(t[0])
        elif (t := _tok(raw, "EVQ_FEND")) is not None:
            cur["hashed"], cur["n_missing"] = int(t[0]), int(t[1])
            cur["complete"] = cur["n_missing"] == 0 and cur["hashed"] == cur["nfiles"] and not cur.get("malformed")
            out.append(cur)
            cur = None
    return out


def sd_inventories(text: str) -> list[dict]:
    out, cur = [], None
    for raw in lines_of(text):
        t = _tok(raw, "EVQ_SDSNAP")
        if t is not None:
            kv = dict(x.split("=", 1) for x in t[1:] if "=" in x)
            cur = {"dir": t[0], "cid": kv.get("cid"), "total": int(kv.get("total", 0)), "free": int(kv.get("free", 0)),
                   "us": int(kv.get("us", 0)), "files": {}, "lines": [], "names_only": {}, "torn": [], "missing": [],
                   "dirs": [], "full": {}}
            continue
        if cur is None:
            continue
        if (t := _tok(raw, "EVQ_FL")) is not None:
            if len(t) != 5 or len(t[3]) != 64:
                cur["malformed"] = cur.get("malformed", 0) + 1   # a garbled line voids the inventory
                continue
            cur["lines"].append({"path": t[0], "line": int(t[1]), "id": int(t[2]), "sha256": t[3], "bytes": int(t[4])})
        elif (t := _tok(raw, "EVQ_FLX")) is not None:
            cur["full"][(t[0], int(t[1]))] = base64.b64decode(t[2]) if t[2] != "-" else None
        elif (t := _tok(raw, "EVQ_FF")) is not None:
            cur["files"][t[0]] = {"size": int(t[1]), "sha256": t[2], "crc32": t[3]}
        elif (t := _tok(raw, "EVQ_FN")) is not None:
            cur["names_only"][t[0]] = int(t[1])
        elif (t := _tok(raw, "EVQ_DIR")) is not None:
            cur["dirs"].append(t[0])
        elif (t := _tok(raw, "EVQ_FT")) is not None:
            cur["torn"].append({"path": t[0], "line": int(t[1]), "bytes": int(t[2])})
        elif (t := _tok(raw, "EVQ_FMISS")) is not None or (t := _tok(raw, "EVQ_FSHORT")) is not None:
            cur["missing"].append(t[0])
        elif (t := _tok(raw, "EVQ_SDEND")) is not None:
            cur["n_files"], cur["n_missing"] = int(t[0]), int(t[1])
            cur["complete"] = cur["n_missing"] == 0 and not cur.get("malformed")
            out.append(cur)
            cur = None
    return out


def index_dumps(text: str) -> list[dict]:
    out, cur = [], None
    for raw in lines_of(text):
        t = _tok(raw, "EVQ_IXHDR")
        if t is not None:
            cur = {"hdr": dict(x.split("=", 1) for x in t if "=" in x), "segs": {}}
            continue
        if cur is None:
            continue
        if (t := _tok(raw, "EVQ_IX")) is not None and len(t) >= 11:
            cur["segs"][int(t[0])] = {"seq": int(t[0]), "state": t[1], "first": int(t[2]), "last": int(t[3]),
                                      "count": int(t[4]), "bytes": int(t[5]), "crc": t[6], "cid": t[7],
                                      "primary": None if t[8] == "-" else t[8],
                                      "mirror": None if t[9] == "-" else t[9], "flash": t[10] == "1"}
        elif _tok(raw, "EVQ_IXEND") is not None:
            out.append(cur)
            cur = None
    return out


def trace(text: str) -> dict:
    entries, hdrs = [], []
    for raw in lines_of(text):
        if (t := _tok(raw, "EVQ_TR_HDR")) is not None:
            hdrs.append({"total": int(t[0]), "lost": int(t[1]), "first": int(t[2]), "last": int(t[3]), "n": int(t[4])})
        elif (t := _tok(raw, "EVQ_TR")) is not None and len(t) >= 7:
            entries.append({"seq": int(t[0]), "us": int(t[1]), "op": t[2], "result": int(t[3]), "bytes": int(t[4]),
                            "a": t[5], "b": " ".join(t[6:])})
    return {"entries": entries, "drains": hdrs}


def singles(text: str) -> dict:
    res: dict = {"cursor": [], "claims": [], "io": [], "state": [], "stacks": [], "hil_trace": [], "boot": [],
                 "faults": [], "evlog": []}
    for raw in lines_of(text):
        if (t := _tok(raw, "EVQ_CUR")) is not None:
            res["cursor"].append(dict(x.split("=", 1) for x in t if "=" in x))
        elif (t := _tok(raw, "EVQ_CLAIMS")) is not None:
            res["claims"].append({k: int(v) for k, v in (x.split("=", 1) for x in t if "=" in x)})
        elif (t := _tok(raw, "EVQ_IO")) is not None:
            res["io"].append({k: int(v) for k, v in (x.split("=", 1) for x in t if "=" in x)})
        elif (t := _tok(raw, "EVQ_STATE")) is not None:
            res["state"].append(raw[raw.find("EVQ_STATE"):])
        elif (t := _tok(raw, "EVQ_STACK")) is not None:
            res["stacks"].append({"task": t[0], "free": int(t[1])})
        elif (t := _tok(raw, "HIL_TRACE")) is not None:
            res["hil_trace"].append({"step": t[0], "us": int(t[1])})
        elif (t := _tok(raw, "HIL_BOOT")) is not None:
            res["boot"].append(dict(x.split("=", 1) for x in t if "=" in x))
        elif (t := _tok(raw, "HIL_FAULT")) is not None:
            res["faults"].append(" ".join(t))
        elif raw.startswith("evlog:") or raw.startswith("  ") and "=" in raw and ("pending" in raw or "sd_" in raw):
            res["evlog"].append(raw)
    return res


def evlog_fields(text: str) -> dict:
    """Last evq_render block (`evlog` CLI) as a dict of key=value tokens."""
    blocks, cur = [], None
    for raw in lines_of(text):
        s = raw.split("ambyte> ")[-1].strip()
        if s.startswith("evlog: available="):
            cur = {}
            blocks.append(cur)
        if cur is not None and "=" in s:
            for tok in s.replace("evlog:", "").split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    cur[k] = v
                elif tok.startswith("pending>="):
                    cur["pending"] = tok.split(">=")[1]
            if s.startswith("sd_retired_names="):
                cur = None
    return blocks[-1] if blocks else {}
