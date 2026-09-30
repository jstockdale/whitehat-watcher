# Watcher PKI — deploy guide

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| AWS CLI v2 | any recent | `aws configure` with an IAM principal that can create KMS keys, IAM roles, Lambda, API Gateway, DynamoDB |
| Node.js | 18+ | CDK CLI runtime |
| CDK CLI | 2.1143.0 (tested) | `npm install -g aws-cdk` |
| Python | 3.12 | matches the Lambda runtime; unit tests run on it |
| awscurl | any | for SigV4-signed endpoint calls (`pip install awscurl`) |

Bootstrap once per account/region (standard CDK):

```bash
export AWS_REGION=us-west-2          # pick your region; used below as $AWS_REGION
cd infra
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
npm install -g aws-cdk              # if needed
cdk bootstrap aws://$(aws sts get-caller-identity --query Account --output text)/$AWS_REGION
```

## Deploy

```bash
./deploy.sh                        # synth + tests + deploy
# or manually:
pytest tests/ -q && cdk deploy --require-approval never
```

`deploy.sh` accepts `--tier1-cert path/to/cert.json` to provision the Tier 1
cert SSM parameter right after deploy (see step 2 below).

## Post-deploy provisioning (out-of-band, in order)

**1. Tier 0 pubkey (John, once).** The rotation Lambda verifies Tier 0
signatures against an SSM SecureString that this stack deliberately does
*not* create:

```bash
aws ssm put-parameter \
  --name /watcher/pki/tier0/pubkey-hex \
  --type SecureString \
  --value "<64 hex chars: Tier 0 Ed25519 pubkey>" \
  --description "Watcher PKI Tier 0 (offline root) Ed25519 pubkey" \
  --region $AWS_REGION
```

**2. Tier 1 cert (Tier 0 ceremony, offline machine).** For the KMS key the
stack just created:

```bash
# a. get the new key's public material (any machine with kms:GetPublicKey)
aws kms get-public-key --key-id alias/watcher-tier1-provisioning \
  --query PublicKey --output text --region $AWS_REGION | base64 -d > /tmp/t1.der
#    -> convert DER SubjectPublicKeyInfo to 65-byte uncompressed point
#       (scripts note: openssl ec -pubin -inform DER -in /tmp/t1.der \
#        -outform DER -conv_form uncompressed | tail -c 65 | xxd -p -c 130)

# b. OFFLINE: issue the cert (serial 1 for a fresh PKI)
./scripts/issue_tier1_cert.py issue \
  --tier0-key-file ~/.ssh/watcher_tier0_seed.hex \
  --p256-pubkey-hex 04abcd... \
  --serial 1 --valid-days 365 \
  --out /tmp/tier1-cert-s1.json

# c. install it as current
aws ssm put-parameter \
  --name /watcher/pki/tier1/current-cert \
  --type String --overwrite \
  --value file:///tmp/tier1-cert-s1.json \
  --region $AWS_REGION
```

**3. Smoke test.**

```bash
API=$(aws cloudformation describe-stacks --stack-name WatcherPkiStack \
  --query 'Stacks[0].Outputs[?OutputKey==`ApiUrl`].OutputValue' --output text --region $AWS_REGION)
awscurl --service execute-api --region $AWS_REGION "$API/v1/tier1/pubkey"
# -> {"p256_pubkey_hex": "04...", "cert": {...}}
```

## Invoking the endpoints

All routes use `AWS_IAM` auth: sign requests with SigV4 (`awscurl` does this
from your AWS CLI credentials). Base URL is the `ApiUrl` stack output.

**POST /sign** -- body `{"command": <watcher-provision-v1>}`:

```bash
cat > /tmp/cmd.json <<'EOF'
{"command": {
  "format": "watcher-provision-v1",
  "cmd": "install_commander_key",
  "device": "watcher-dev-01",
  "nonce": "<32 hex chars, fresh per command>",
  "issued_at": "<now, ISO-8601 Z>",
  "expires_at": "<now + <=15 min, ISO-8601 Z>",
  "params": {"pubkey_hex": "<64 hex chars: new commander Ed25519 pubkey>"}
}}
EOF
awscurl --service execute-api --region $AWS_REGION \
  -X POST -H 'Content-Type: application/json' \
  --data @/tmp/cmd.json "$API/v1/sign"
# -> {"signature_hex": "<128 hex: raw r||s>", "cert": {...}, "key_id": "arn:aws:kms:..."}
```

Validation failures return `400 {"error":"command rejected","reason":"..."}` and
are audit-logged with the reason. The Lambda never signs arbitrary bytes.

**GET /audit** -- `?since=2026-09-27T00:00:00Z&cmd=install_commander_key&device=watcher-dev-01&limit=50&next_token=...`:

```bash
awscurl --service execute-api --region $AWS_REGION \
  "$API/v1/audit?cmd=install_commander_key&limit=50"
# -> {"entries": [...], "next_token": null}
```

**POST /rotation** -- body `{"tier1_cert": <watcher-tier1-cert-v1>}` (Tier 0-signed,
serial greater than current):

```bash
awscurl --service execute-api --region $AWS_REGION \
  -X POST -H 'Content-Type: application/json' \
  --data @/tmp/tier1-cert-s2.json "$API/v1/rotation"
# body file shape: {"tier1_cert": { ...cert... }}
```

## Rotation runbooks

### A. Tier 1 *certificate* rotation (same KMS key; e.g. expiry approaching)

1. Offline: `./scripts/issue_tier1_cert.py issue --serial <N+1> ...` (same
   `--p256-pubkey-hex` as the current cert).
2. `POST /rotation` with `{"tier1_cert": ...}`. The Lambda verifies the Tier 0
   signature, checks the serial increases, stores the new cert as current,
   and writes an audit entry. Done -- `/sign` and `/tier1/pubkey` serve the
   new cert immediately (no redeploy).

### B. Tier 1 *KMS key* rotation (compromise or hygiene)

KMS automatic rotation **does not apply** to asymmetric keys -- this is the
manual procedure (also noted on the KMS key resource):

1. Create the new key (same spec): CloudFormation can't "rotate" it for you;
   add a second `kms.Key` + update the alias target in `watcher_pki/stack.py`,
   or create it via CLI:
   ```bash
   aws kms create-key --key-spec ECC_NIST_P256 --key-usage SIGN_VERIFY \
     --description "Watcher PKI Tier 1 (rotation N)" --region $AWS_REGION
   ```
2. `aws kms get-public-key` on the new key -> 65-byte uncompressed point hex.
3. Offline Tier 0 ceremony: `issue_tier1_cert.py issue --p256-pubkey-hex <new>
   --serial <N+1>`.
4. `POST /rotation` with the new cert. The Lambda stores it as current.
5. Point the alias at the new key:
   ```bash
   aws kms update-alias --alias-name alias/watcher-tier1-provisioning \
     --target-key-id <new-key-id> --region $AWS_REGION
   ```
   The signing Lambda signs via the **alias ARN**, so it picks up the new key
   with no redeploy. Verify: `POST /sign` a test command, confirm `key_id`
   in the response is the new key ARN.
6. Rebake firmware trust anchors (Tier 1 pubkey + expiry) via the firmware
   build pipeline, and schedule the old key's deletion
   (`aws kms schedule-key-deletion`, 7-30 day window) after devices have
   migrated. Old audit entries remain verifiable against old `key_id`s.

### C. Tier 0 rotation (break-glass; catastrophic by design)

Tier 0 compromise means the offline root is burned. There is no API for this:
new Tier 0 keypair by ceremony, new Tier 1 cert, `POST /rotation`, rebake
*all* firmware anchors (Tier 0 pubkey is baked into every device), and treat
every cert the old Tier 0 ever signed as untrusted. John's custody of the
offline key is the mitigation; document the ceremony when it happens.

## Teardown

```bash
cdk destroy
```

Notes:

* The KMS key and the `watcher-pki-audit` table have `RemovalPolicy.RETAIN`:
  `cdk destroy` leaves them (and their data) in place. Delete manually if you
  really mean it:
  ```bash
  aws kms schedule-key-deletion --key-id alias/watcher-tier1-provisioning --pending-window-in-days 7
  aws dynamodb delete-table --table-name watcher-pki-audit
  ```
* SSM parameters (`/watcher/pki/...`) are not stack-managed; delete by hand.
* The audit table has no TTL: entries are kept forever. After `cdk destroy`,
  delete the table manually (`aws dynamodb delete-table --table-name
  watcher-pki-audit`) if you really mean to drop the audit history.

## Security notes for operators

* No credentials anywhere in this repo: Lambdas use IAM roles only; the
  Tier 0 key never leaves John's offline machine; the broker-style rule from
  the seal work applies -- secrets are used transiently, never stored.
* API Gateway access logging has `data_trace_enabled=False`: request bodies
  (which reference key fingerprints) are never written to CloudWatch.
* Every `/sign` and `/rotation` call writes an audit item with `cmd_sha256`
  (SHA-256 of the canonical bytes), never the command/params themselves.
* `/sign` is throttled (10 req/s, burst 5) *and* the signing Lambda has
  reserved concurrency 10 -- two independent backstops.
