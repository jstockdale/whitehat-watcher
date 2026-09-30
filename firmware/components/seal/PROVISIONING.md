# Watcher Phase-2 Seal — Bench Provisioning Ceremony

How John brings a Watcher from factory-fresh to sealed native-MQTT operation.
All provisioning happens over the USB-serial console (115200 baud, one JSON
object per line, newline-terminated). **Provisioning commands are serial-only:**
over native MQTT they are refused with `{"t":"err","msg":"serial console only"}`.

## 0. Generate the secrets (host side)

On the host (never on the device, never pasted anywhere except the serial
console lines below):

```bash
# Commander Ed25519 keypair (RFC 8032). The DEVICE gets only the public key.
python3 -c "
from nacl.signing import SigningKey
sk = SigningKey.generate()
print('PRIVATE (host keeps):', sk.encode().hex())
print('PUBLIC  (paste below):', sk.verify_key.encode().hex())
"

# Telemetry seed (32 bytes). Both sides derive k_tel_mac / k_tel_aead from it.
python3 -c "import secrets; print(secrets.token_hex(32))"
```

Test values in examples below are obviously fake and labeled as such.

## 1. Order of operations

### 1a. Pin the commander key

```json
{"t":"seal_provision","pk":"<64 hex chars of the commander PUBLIC key>"}
```

Reply: `{"t":"ok","cmd":"seal_provision"}` (correlated `_mid` if you sent one).

Effect: the 32-byte public key is pinned to NVS (`seal` namespace, `cmd_pk`
blob) via the single NVS writer task. From now on the device re-verifies
every inbound WMS2 envelope itself, and the heartbeat reports
`"seal_dev_verify":true` — the page sees this and switches to forwarding
sealed envelopes opaquely instead of verifying in grace itself.

### 1b. Provision the telemetry seed

```json
{"t":"seal_tel_seed","seed":"<64 hex chars>"}
```

Reply: `{"t":"ok","cmd":"seal_tel_seed"}`.

The device derives `k_tel_mac` / `k_tel_aead` with the labeled KDF
(`BLAKE2b(key=seed, "WMS2-tel-mac-v1"|"WMS2-tel-aead-v1")`, 32 bytes each —
byte-identical to `mqtt_seal.py derive_tel_keys` and the page). Watch the
serial log for:

```
I (....) seal_dev: telemetry seed stored, fp=<16 hex chars>
```

**The fingerprint must match the page's Setup-card fingerprint exactly.**
The fingerprint identifies the seed; it is safe to read off the screen.

### 1c. Wi-Fi credentials

```json
{"t":"wifi_cred","ssid":"<ssid>","pass":"<pass>"}
```

Reply: `{"t":"ok","cmd":"wifi_cred"}`. This is a thin wrapper over the net
component (same NVS keys the existing `{"t":"wifi",...}` command uses).
Expect the `{"t":"wifi","state":"connected","ip":"..."}` event shortly.

### 1d. Broker credentials (native MQTT)

```json
{"t":"mqtt_cred","user":"<watcher-role user>","pass":"<pass>"}
```

Reply: `{"t":"ok","cmd":"mqtt_cred"}`.

**Use the watcher-role credentials** (write: `<prefix>/telemetry`,
`<prefix>/status`; read: `<prefix>/command`), not the muse-role ones. Stored
to NVS (`seal:mqtt_user` / `seal:mqtt_pass`, NUL-terminated blobs) through
the NVS writer task. The native-MQTT task notices within ~1s and starts the
TLS client to the configured broker URI (default
`mqtts://mqtt.offx1.com:8883`).

### 1e. Broker URI (native MQTT)

```json
{"t":"mqtt_broker","url":"mqtts://mqtt.offx1.com:8883"}
```

Reply: `{"t":"ok","cmd":"mqtt_broker"}`. `mqtts://` or `mqtt://`
(plaintext warns loudly); persisted to NVS (`seal:mqtt_uri`) and applied
on the next native-MQTT (re)connect, within ~1s.

### 1f. Topic prefix (native MQTT)

```json
{"t":"mqtt_topic","prefix":"watcher"}
```

Reply: `{"t":"ok","cmd":"mqtt_topic"}`. One topic level,
`[A-Za-z0-9_-]{1,32}`; persisted to NVS (`seal:mqtt_prefix`). The native
client resubscribes as `<prefix>/command` and publishes to
`<prefix>/telemetry|heartbeat|status`.

### 1g. Device name

```json
{"t":"set_name","name":"Watcher-HID"}
```

Reply: `{"t":"ok","cmd":"set_name","name":"<applied>"}`. 1–32 printable
ASCII chars; persisted to NVS (`dev:dev_name`) and loaded before BLE init
on every boot. The GAP device name updates immediately; the advertising
payload, on-screen title, and boot `ready` name field pick it up on the
next advertising cycle / reboot.

## 2. Verify

### 2a. Heartbeat (serial, every 60s; the page forwards it to MQTT)

```json
{"t":"heartbeat","uptime_s":123,"fw":"0.3.11",
 "mqtt_mode":"native","seal_floor":32,"seal_armed":false,
 "seal_dev_verify":true,"seal_session":null}
```

| field | meaning |
|---|---|
| `mqtt_mode` | `"native"` once the device is the MQTT endpoint, else `"bridged"` |
| `seal_floor` | D4 boot floor: `stored_high_water + 32` (0 if never persisted) |
| `seal_armed` | WIA1 Phase-3 session armed (false until the arm ceremony) |
| `seal_dev_verify` | **true iff a commander key is pinned** — the page's arbitration bit |
| `seal_session` | 16-hex session id, or `null` when disarmed |
| `seal_seclog` | present only after rejections: `"BAD-SIG=2,REPLAY=1"` |

### 2b. Native-mode checks

- The page's MUSE STATUS pill should stop forwarding (it sees
  `mqtt_mode:"native"` in status) — bridged and native never double-drive.
- Retained status: subscribe `watcher/status` — on every (re)connect the
  device publishes a retained, MAC'd status there.
- Heartbeats arrive on `watcher/heartbeat` with a `wms2_mac` field when the
  tel seed is provisioned; other serial lines are mirrored verbatim to
  `watcher/telemetry`.

### 2c. D4 high-water behavior

- The device persists the accepted-command high-water every 32nd accept
  (`SEAL_HW_PERSIST_EVERY`), or immediately if a counter gap jumps the
  high-water 32 past the last persisted value.
- Reboot, then read `seal_floor` from the heartbeat: it equals
  `stored + 32`, which clears everything executed before the crash
  (replay budget after crash = 0).
- Commander resync rule: `next_ctr = max(own_ctr, seal_floor) + 1`.
  Anti-oracle: reject `seal_floor > own_ctr + 2^20`, or trust the floor
  only from an authenticated status.

### 2d. Host-side verification of `wms2_mac`

```python
import hashlib
body = b'{"t":"heartbeat",...}'   # the JSON WITHOUT the wms2_mac field
topic = b'watcher/heartbeat'
msg = len(topic).to_bytes(2, 'little') + topic + body
# NOTE: digest_size=16 directly — NOT blake2b(...,32).digest()[:16];
# BLAKE2b mixes the digest length into the parameter block, so the two
# differ. (Same convention as mqtt_seal.seal_telemetry's TAG_LEN=16.)
tag = hashlib.blake2b(msg, digest_size=16, key=k_tel_mac).digest()
assert tag.hex() == published["wms2_mac"]
```

## 3. Strict mode

```json
{"t":"seal_strict","on":true}
```

Default is grace (`on:false`, persisted as u8 `strict` in NVS). In strict
mode unsealed commands are rejected everywhere (`UNSEALED` / `NO-KEY`) and
sealed envelopes require a valid signature against the pinned key. Flip
back with `"on":false`.

## 4. Reset

```json
{"t":"seal_unpin"}
```

Wipes the pinned key from RAM (`crypto_wipe`) and deletes the NVS blob.
`seal_dev_verify` returns to `false`; the device stops verifying and the
page falls back to grace-mode local dispatch. (The telemetry seed,
Wi-Fi, and broker creds are left in place; re-run §1b–§1d to rotate them.)

## 5. Notes and sharp edges

- **NVS writes are asynchronous.** The single `seal_nvs` writer task owns
  all flash writes; command replies confirm the write was *queued*.
  Persistence lands milliseconds later — verify via the heartbeat fields,
  not by yanking power mid-ceremony.
- **`seal_grace` is serial-only and separate.** `{"t":"seal_grace","on":…}`
  toggles the WIA1 Phase-3 grace marker; it is refused over MQTT by
  construction (the `_via_mqtt` gate).
- **TLS is TOFU-grade for now.** The native client negotiates TLS to
  `mqtt.offx1.com:8883` but does not yet verify the broker certificate
  (encrypted, not authenticated). Broker CA pinning is a coordinator TODO
  before this leaves the bench; the log warns at every connect.
- **Sealed command shape.** The host seals `{"id":"<mid>","cmd":{…}}`
  as a WMS2 kind-2 envelope. Bridged: the page forwards it as
  `{"t":"sealed_fwd","topic":"<claimed>","b64":"…"}` and the device
  re-verifies against the *claimed* topic, then dispatches the inner
  command with `_mid=id` (one terminal reply, correlated with the inner
  id). Native: the envelope arrives as raw bytes or `{"sealed":"<b64>"}`
  on `watcher/command` and is verified against the *delivered* topic.
- **Sealed firmware chunks stay page-verified** in Phase 2 (they exceed
  the 2048-byte sealed-command body limit); chunked sealed_fwd is future
  work.
- **Never** paste the telemetry *seed* or any private key into a file,
  log, or chat — only the 64-hex public key and seed *fingerprint*
  (16 hex) are safe to display.
