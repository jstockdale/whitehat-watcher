# Infra choice: CDK (Python)

## The decision

**AWS CDK in Python** (`aws-cdk-lib`), not Terraform/HCL and not raw
CloudFormation.

## Tradeoffs weighed

| | CDK Python | Terraform |
|---|---|---|
| Language | Python -- the same language as the host-agent tooling, the Lambda handlers, the validation layer, and the tests | HCL -- a second language for a one-person, Python-centric project |
| Test story | `pytest` imports the *actual* `pki_common` module that ships in the Lambda layer; `cdk synth` runs in CI without credentials | `terraform validate` needs `terraform init` + provider download (heavy, network-fragile); HCL can't share code with the Python validation logic |
| State | CloudFormation-managed (no state file to guard) | State file must be stored/locked remotely (S3 + DynamoDB) -- real operational burden for a dev-stage project |
| Abstractions | L3-ish conveniences (`Table.grant_write_data`, `Key.grant`) with escape hatches to raw CFn | Fully explicit; more verbose for IAM wiring |
| Deps for synth | Node.js (CDK CLI) + pip packages | Terraform binary + AWS provider (~large) |
| Drift/audit | `cdk diff` before every deploy | `terraform plan` -- excellent, arguably the best in class |
| Team familiarity risk | CDK's jsii layer occasionally surfaces odd API names (we hit one: `MethodSetting` doesn't exist; `method_options` does) | HCL is stable and widely known |

## Why CDK Python fits *this* project

1. **One language across the whole PKI slice.** The security-critical
   artifact here is `pki_common.py` -- canonical encodings, schema
   validation, Ed25519 verification. In CDK Python it is simultaneously
   the Lambda layer source, the pytest target, and is referenced by the
   stack definition. Terraform would leave that sharing impossible: the
   validation logic would live in Python while the infra lived in HCL,
   with the boundary held together by convention.

2. **John's project is Python-heavy.** Host-agent tooling, seal tooling,
   console test harnesses -- all Python. Whoever maintains this infra
   already reads Python; HCL would be the only non-Python artifact.

3. **Verifiable here.** `cdk synth` passes with no AWS credentials and no
   network beyond pip/npm installs (verified: full template synthesized,
   IAM scoping asserted programmatically). A Terraform path would have
   needed a provider `init` against the registry to reach the same level
   of verification.

4. **Dev-stage economics.** Pay-per-request DynamoDB, four small Lambdas,
   CloudFormation-managed state -- nothing here needs Terraform's
   strongest features (multi-cloud, complex state surgery, plan-first
   governance). If the PKI ever grows a second cloud or a compliance
   team, re-evaluating Terraform then is cheap: the resource design
   (KMS key, table schema, API routes, IAM boundaries) ports 1:1.

## Honest caveats

* CDK CLI needs Node.js even for a Python app (we pin the CLI version in
  DEPLOY.md).
* `aws-cdk-lib` moves fast; we pin exact versions in `requirements.txt`
  and re-run `cdk synth` + `pytest` after any bump.
* CloudFormation stack deletes are less forgiving than `terraform
  destroy` plans -- hence `RemovalPolicy.RETAIN` on the KMS key and the
  audit table (documented in DEPLOY.md teardown).
