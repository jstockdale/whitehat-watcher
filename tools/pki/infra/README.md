# Watcher PKI — infrastructure

AWS infrastructure for the Watcher Tier 1 provisioning authority, per
`../PKI_DESIGN_BRIEF.md` (the shared source of truth -- read it first).

## Layout

```
infra/
  INFRA_CHOICE.md        why CDK Python (1 page)
  DEPLOY.md              prerequisites, deploy, invoke, rotate, teardown
  deploy.sh              guided deploy wrapper (calls cdk)
  cdk.json / app.py / requirements.txt
  watcher_pki/stack.py   the stack: KMS, DynamoDB, Lambdas, API Gateway, IAM
  lambda/
    layer/python/pki_common.py   shared core: validation, canonical encodings,
                                 Tier 1 cert verification (pure stdlib).
                                 Ships as a Lambda layer; pytest target.
    functions/
      sign_provisioning/handler.py  POST /sign
      get_tier1_pubkey/handler.py   GET /tier1/pubkey
      audit_log/handler.py          GET /audit
      rotation/handler.py           POST /rotation
  tests/
    test_pki_common.py   81 tests: schema validation, canonical vectors,
                         DER conversion, Ed25519 vs PyNaCl, cert verification
    test_handlers.py     handler tests with stubbed boto3 (no AWS needed)
  scripts/
    issue_tier1_cert.py  Tier 0 ceremony helper (OFFLINE machine only)
```

## What gets deployed

* **KMS** `alias/watcher-tier1-provisioning`: `ECC_NIST_P256`, `SIGN_VERIFY`,
  HSM-backed. Only the signing Lambda gets `kms:Sign` (+`kms:GetPublicKey`).
* **API Gateway** (regional, `v1` stage, `AWS_IAM` auth on all four routes):
  `GET /tier1/pubkey`, `POST /sign` (throttled 10/s, burst 5),
  `GET /audit`, `POST /rotation`.
* **DynamoDB** `watcher-pki-audit`: `pk` = `SIGN#<date>` / `ROTATION#<date>`,
  `sk` = timestamp; no TTL (entries kept forever); GSI `by-cmd`; PITR enabled.
* **SSM** (by convention, created out-of-band -- not in the stack):
  `/watcher/pki/tier1/current-cert` (String), `/watcher/pki/tier0/pubkey-hex`
  (SecureString, John's custody).

## Quick verify (no AWS needed)

```bash
cd infra
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt   # cdk libs (pinned)
pip install pytest pynacl         # test-only
pytest tests/ -q                  # 96 tests
cdk synth                         # full CloudFormation template, no creds
```

## Key management notes

* KMS **automatic rotation does not apply** to asymmetric keys. Tier 1 key
  rotation is manual: new KMS key -> Tier 0 ceremony (`scripts/issue_tier1_cert.py`)
  -> `POST /rotation` -> move the alias -> rebake firmware anchors.
  Full runbook in DEPLOY.md.
* The signing Lambda calls KMS by **alias ARN**, so moving the alias is
  enough -- no Lambda redeploy on rotation.
* Tier 0 lives with John, offline. It is never in AWS, never in this repo.
