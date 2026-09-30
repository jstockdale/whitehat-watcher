#!/usr/bin/env python3
"""CDK app entrypoint: Watcher PKI (Tier 1 provisioning authority)."""

import os

import aws_cdk as cdk

from watcher_pki.stack import WatcherPkiStack

app = cdk.App()
WatcherPkiStack(
    app,
    "WatcherPkiStack",
    env=cdk.Environment(
        account=os.environ.get("CDK_DEFAULT_ACCOUNT"),
        region=os.environ.get("CDK_DEFAULT_REGION", "us-west-2"),
    ),
    description="Watcher PKI: Tier 1 (KMS ECDSA P-256) provisioning authority",
)
app.synth()
