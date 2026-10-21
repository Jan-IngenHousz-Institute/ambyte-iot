# Console discovery fixture

`identity-2.5.0.raw` is the exact response slice from the Evaluator's
`sess_old.raw` hardware capture on firmware 2.5.0 (A2 round 3):
`identity\r\nUnrecognized command\r\nambyte> `. Tests add the separately
observed prompt log tail and a boot log between echo and reply.

## A3 revision-4 unsupported service probes

The four `*_svc_*.raw` fixtures are exact Evaluator USB captures, copied from
A3 evaluator scratchpad `bench/` without normalizing CRLF or trailing spaces.
`b01_a2_*` came from A2 `3197373` (`v2.6.2-6-g3197373`),
2026-10-21 around09:54UTC. `r4_v270_*` came from release2.7.0
(image SHA256 `1cd543569c0be0ea222eb19c79e59a8402494c95d5a9ca24329ba5542c5e2d00`),
around10:58UTC that day. Both versions returned identical bytes:

- `*_plain.raw`:36B, SHA256 `dc0808efeb7179812485dfd7673239742f219b1a35440bf1f75cf960491fe222`; request `svc\n`.
- `*_framed.raw`:62B, SHA256 `d1181b67321a0caa4a1d8a65d24a42ff34e5a47ac61e471458177710fd6089c8`; request `00000001 svc.version*e97c2d53\n`.

The final prompt is `ambyte>  ` with **two** spaces. The echo and
`Unrecognized command` lines terminate with CRLF.
