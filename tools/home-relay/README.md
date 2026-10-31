# Optional U1 home relay (development)

This helper runs on the home Mac, alongside the existing 3D Print application. The Mac must
stay awake and connected to the printer. It currently supports U1 only; Bambu is deliberately
not routed through the U1 protocol. It is not yet deployed to a public endpoint.

The iPhone keeps slicing locally. Add a U1 with its normal LAN address, plus an optional
HTTPS relay address ending `/v1/u1` and its pairing token. The app checks real LAN status
first, then the relay. Route selection occurs before upload; there is no fallback/resend
after an uncertain print start. Existing toolhead/preparation controls and filename/status
acceptance checks remain. U1 camera support here is periodic JPEG snapshots, not live video.

## Deployment prerequisites

1. Use a stable HTTPS tunnel/reverse proxy that supports WebSockets to `127.0.0.1:9841`.
   Keep TLS certificate verification enabled. Do not forward a router port directly to this
   loopback HTTP service. The public endpoint has not been configured in this session.
2. Store a cryptographically random token (at least 32 characters) in macOS Keychain as a
   generic password, service `3DPrint.HomeRelay`, account `owner`. Pair the same token in
   the iPhone form. Never put it in a URL, source file or chat. Changing the Keychain token
   and restarting the helper revokes old clients. iPhone tokens are device-only Keychain items.
3. Install dependencies in a virtual environment: `pip install -r requirements.txt`.
4. Run `python server.py --printer http://YOUR-U1-IP --state "$HOME/Library/Application Support/3DPrint/HomeRelay"`.
   It binds only to 127.0.0.1. It does not change printer settings or start a job on launch.
5. Keep the relay state directory: it contains durable dispatch claims. Do not remove it to
   retry an uncertain job; check the printer first. A tunnel/service account is still needed.

Only selected status objects, the camera snapshot, upload (forced print=false), and
`server.files.start_local_print` are exposed. Requests require a bearer token; printer
credentials are never forwarded to callers. Other Moonraker endpoints and G-code commands
are not exposed. Starts require a file uploaded through this relay, and a durable claim
prevents the same uploaded path being dispatched twice, including after a restart.
Provider upload limits can be lower than the helper's 256 MiB limit. Confirm the chosen
provider's limit before sending a large file. Do not assume an HTTP upload means acceptance.

Tests: `python test_server.py` uses a fake printer only. Real hardware checks performed:
GET status and GET camera snapshot through a temporary local authenticated relay. No real
upload/start, off-LAN HTTPS session, cellular connection, or public deployment was tested.

`snapmaker_gateway_probe.py --port PORT` inspects an already-running official Snapmaker
Control gateway on localhost. It returns sanitized readiness/login flags only. It never
exports the raw account response, launches the CLI with an official-client identity, logs in,
binds a printer, or sends print requests. The gateway remains a candidate integration,
not a promised general-purpose manufacturer API.

## Pairing and testing (build 6)

Compile the macOS Keychain pairing helper with `swiftc PairRelay.swift -o pair-relay`.
After configuring your stable HTTPS tunnel, run:

```
./pair-relay --endpoint https://YOUR-HOST/v1/u1 --printer U1-IP --output /PRIVATE-FOLDER/U1-relay.json
```

The helper creates or reuses a random Keychain credential. It never prints the token or
passes it in command arguments; it creates a new pairing file with owner-only permissions.
Transfer that credential file privately (for example, AirDrop), then import it from
**Printers → U1 → Remote access → Import pairing file**. You can also import while adding
a new U1. Test remote connection explicitly, then save. Delete transfer copies after pairing;
anyone holding the file can access the relay. Removing the remote connection on the phone
removes that phone's credential but does not revoke copies; rotate the Mac Keychain token
and restart the relay to revoke all previously paired clients.

For read-only verification, start `server.py` with `--read-only`. This rejects both uploads
and WebSockets, while allowing authenticated status and camera snapshot reads. Do not
use this mode for normal printing. No printing occurs merely by starting the normal relay;
the iPhone still asks for confirmation before sending its single start request.

The relay accepts text and binary WebSocket JSON, including the iPhone's native binary
messages. A durable dispatch claim survives relay restarts. Unknown outcomes must be checked
at the printer; do not resend automatically. Uploads are serialized to prevent duplicate
filename races. The relay exposes only the existing single-toolhead phone print options.

Cloudflare Quick Tunnels are suitable for temporary development checks, not a permanent
installation: their address changes and they have no uptime guarantee. A permanent endpoint
still requires a configured service/account and an awake Mac. Do not publish the unauthenticated
printer itself. Camera support remains periodically fetched JPEG snapshots, not continuous
video. Snapmaker's account password or cloud tokens are not needed for this owner LAN relay.
