#!/usr/bin/env bash
# Watcher PKI deploy wrapper: tests -> synth -> deploy -> optional cert install.
#
# Usage:
#   ./deploy.sh [--tier1-cert path/to/cert.json] [--profile NAME] [--region us-west-2]
#
# --tier1-cert provisions /watcher/pki/tier1/current-cert after deploy
# (the Tier 1 cert JSON produced by the offline Tier 0 ceremony).
set -euo pipefail
cd "$(dirname "$0")"

TIER1_CERT=""
PROFILE=""
REGION="${AWS_REGION:-us-west-2}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tier1-cert) TIER1_CERT="$2"; shift 2 ;;
    --profile)    PROFILE="$2";    shift 2 ;;
    --region)     REGION="$2";     shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

AWS=(aws --region "$REGION")
[[ -n "$PROFILE" ]] && AWS+=(--profile "$PROFILE")

for tool in python3 node npx aws; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate
pip install -q -r requirements.txt
pip install -q pytest

echo "==> running unit tests"
python -m pytest tests/ -q

echo "==> cdk synth"
# Pin the venv's Python in cdk.json: npx does not reliably resolve `python3`
# to the activated venv, so use the absolute path.
VENV_PYTHON="$(pwd)/.venv/bin/python3"
python3 -c "
import json
with open('cdk.json') as f: d = json.load(f)
d['app'] = '\"$VENV_PYTHON\" app.py'
with open('cdk.json', 'w') as f: json.dump(d, f)
"
npx --yes aws-cdk@2.1143.0 synth

echo "==> cdk deploy"
npx --yes aws-cdk@2.1143.0 deploy --require-approval never

if [[ -n "$TIER1_CERT" ]]; then
  echo "==> provisioning Tier 1 cert SSM parameter"
  "${AWS[@]}" ssm put-parameter \
    --name /watcher/pki/tier1/current-cert \
    --type String --overwrite \
    --value "file://$TIER1_CERT" \
    --description "Watcher PKI current Tier 1 certificate"
fi

API=$( "${AWS[@]}" cloudformation describe-stacks --stack-name WatcherPkiStack \
  --query 'Stacks[0].Outputs[?OutputKey==`ApiUrl`].OutputValue' --output text )
echo "==> deployed. API base: $API"
echo "    smoke test: awscurl --service execute-api --region $REGION \"$API/v1/tier1/pubkey\""
