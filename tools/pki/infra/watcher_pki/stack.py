"""Watcher PKI CDK stack.

Provisions, per PKI_DESIGN_BRIEF.md:

* KMS asymmetric key: ECC_NIST_P256 / SIGN_VERIFY, alias
  ``alias/watcher-tier1-provisioning`` (the Tier 1 provisioning authority).
  NOTE: KMS automatic rotation applies ONLY to symmetric keys. This key is
  rotated manually via the POST /rotation API + Tier 0-signed cert -- the
  full runbook is in DEPLOY.md. ``enable_key_rotation`` is deliberately
  left False so nobody assumes otherwise.
* DynamoDB ``watcher-pki-audit`` (no TTL -- audit entries kept forever,
  GSI ``by-cmd``, PITR enabled).
* Four Python 3.12 Lambdas + shared ``pki_common`` layer (pure stdlib).
* Regional API Gateway REST API, AWS_IAM auth on every route, method-level
  throttling on POST /sign.

Deliberately NOT created by this stack (out-of-band, John's custody):

* ``/watcher/pki/tier0/pubkey-hex`` (SSM SecureString) -- Tier 0 Ed25519
  pubkey, deployed by John. The rotation Lambda reads it; it is NEVER in
  this repo and NEVER in CloudFormation templates/parameters.
* ``/watcher/pki/tier1/current-cert`` (SSM String) -- the current Tier 1
  cert, deployed by ``deploy.sh --tier1-cert`` after the Tier 0 ceremony.
"""

from aws_cdk import (
    CfnOutput,
    Duration,
    RemovalPolicy,
    Stack,
    aws_apigateway as apigw,
    aws_dynamodb as dynamodb,
    aws_iam as iam,
    aws_kms as kms,
    aws_lambda as lambda_,
    aws_logs as logs,
)
from constructs import Construct

# SSM parameter names (single source of truth; Lambdas read these at runtime).
TIER1_CERT_PARAM = "/watcher/pki/tier1/current-cert"
TIER0_PUBKEY_PARAM = "/watcher/pki/tier0/pubkey-hex"

AUDIT_TABLE_NAME = "watcher-pki-audit"
TIER1_ALIAS_NAME = "alias/watcher-tier1-provisioning"


class WatcherPkiStack(Stack):
    def __init__(self, scope: Construct, construct_id: str, **kwargs) -> None:
        super().__init__(scope, construct_id, **kwargs)

        # ------------------------------------------------------------------
        # KMS: Tier 1 provisioning key (ECDSA P-256, HSM-backed)
        # ------------------------------------------------------------------
        tier1_key = kms.Key(
            self,
            "Tier1Key",
            description=(
                "Watcher PKI Tier 1 provisioning authority. "
                "Signs provisioning commands only (ECDSA P-256)."
            ),
            key_spec=kms.KeySpec.ECC_NIST_P256,
            key_usage=kms.KeyUsage.SIGN_VERIFY,
            # Asymmetric keys do NOT support KMS automatic rotation.
            # Rotation is manual: see DEPLOY.md runbook.
            enable_key_rotation=False,
            pending_window=Duration.days(30),
            removal_policy=RemovalPolicy.RETAIN,  # never lose the audit trail's key
        )
        tier1_alias = kms.Alias(
            self,
            "Tier1Alias",
            alias_name=TIER1_ALIAS_NAME,
            target_key=tier1_key,
        )

        # ------------------------------------------------------------------
        # DynamoDB: audit log
        # ------------------------------------------------------------------
        audit_table = dynamodb.Table(
            self,
            "AuditTable",
            table_name=AUDIT_TABLE_NAME,
            partition_key=dynamodb.Attribute(
                name="pk", type=dynamodb.AttributeType.STRING
            ),
            sort_key=dynamodb.Attribute(
                name="sk", type=dynamodb.AttributeType.STRING
            ),
            billing_mode=dynamodb.BillingMode.PAY_PER_REQUEST,
            point_in_time_recovery_specification=(
                dynamodb.PointInTimeRecoverySpecification(
                    point_in_time_recovery_enabled=True
                )
            ),
            removal_policy=RemovalPolicy.RETAIN,  # audit log survives stack delete
            # NOTE: no TTL — audit entries are kept forever (John 2026-09-27).
        )
        # GSI for GET /audit?cmd=... (cheap at this scale; projection ALL so
        # the audit endpoint never needs a second fetch).
        audit_table.add_global_secondary_index(
            index_name="by-cmd",
            partition_key=dynamodb.Attribute(
                name="cmd", type=dynamodb.AttributeType.STRING
            ),
            sort_key=dynamodb.Attribute(
                name="timestamp", type=dynamodb.AttributeType.STRING
            ),
            projection_type=dynamodb.ProjectionType.ALL,
        )

        # ------------------------------------------------------------------
        # Lambda layer: shared pki_common (pure stdlib, no native deps)
        # ------------------------------------------------------------------
        common_layer = lambda_.LayerVersion(
            self,
            "PkiCommonLayer",
            code=lambda_.Code.from_asset("lambda/layer"),
            compatible_runtimes=[lambda_.Runtime.PYTHON_3_12],
            compatible_architectures=[lambda_.Architecture.X86_64],
            description=(
                "Watcher PKI shared core: provisioning validation, canonical "
                "encodings, Tier 1 cert verification (pure stdlib)."
            ),
        )

        def make_fn(
            construct_id: str,
            func_dir: str,
            *,
            env: dict,
            timeout_s: int,
            reserved_concurrency: int | None = None,
        ) -> lambda_.Function:
            log_group = logs.LogGroup(
                self,
                f"{construct_id}Logs",
                retention=logs.RetentionDays.THREE_MONTHS,
                removal_policy=RemovalPolicy.DESTROY,
            )
            return lambda_.Function(
                self,
                construct_id,
                runtime=lambda_.Runtime.PYTHON_3_12,
                architecture=lambda_.Architecture.X86_64,
                handler="handler.handler",
                code=lambda_.Code.from_asset(f"lambda/functions/{func_dir}"),
                layers=[common_layer],
                environment=env,
                memory_size=256,
                timeout=Duration.seconds(timeout_s),
                reserved_concurrent_executions=reserved_concurrency,
                log_group=log_group,
                description=f"Watcher PKI: {func_dir}",
            )

        common_env = {
            "AUDIT_TABLE_NAME": audit_table.table_name,
            "TIER1_CERT_PARAM_NAME": TIER1_CERT_PARAM,
        }

        sign_fn = make_fn(
            "SignFunction",
            "sign_provisioning",
            env={**common_env, "KMS_KEY_ID": tier1_alias.alias_arn},
            timeout_s=30,
            # Abuse backstop for the signing endpoint (API throttling is the
            # first layer; this caps Lambda-side spend if throttling is
            # ever loosened by mistake).
            reserved_concurrency=10,
        )
        pubkey_fn = make_fn(
            "PubkeyFunction",
            "get_tier1_pubkey",
            env={"TIER1_CERT_PARAM_NAME": TIER1_CERT_PARAM},
            timeout_s=10,
        )
        audit_fn = make_fn(
            "AuditFunction",
            "audit_log",
            env={"AUDIT_TABLE_NAME": audit_table.table_name},
            timeout_s=15,
        )
        rotation_fn = make_fn(
            "RotationFunction",
            "rotation",
            env={
                **common_env,
                "TIER0_PUBKEY_PARAM_NAME": TIER0_PUBKEY_PARAM,
            },
            timeout_s=15,
        )

        # ------------------------------------------------------------------
        # IAM: least privilege.
        #
        # KMS: ONLY the signing Lambda gets kms:Sign (+ kms:GetPublicKey, so
        # it can report/verify the key it signs with) and ONLY on the Tier 1
        # key. No Lambda can read private key material -- asymmetric KMS
        # keys cannot export private material at all, and no kms:Get*/Export
        # style permission is granted anywhere in this stack.
        # ------------------------------------------------------------------
        tier1_key.grant(sign_fn, "kms:Sign", "kms:GetPublicKey")

        def allow_ssm(fn: lambda_.Function, param: str, actions: list[str]) -> None:
            fn.add_to_role_policy(
                iam.PolicyStatement(
                    actions=actions,
                    resources=[
                        Stack.of(self).format_arn(
                            service="ssm",
                            resource="parameter",
                            resource_name=param.lstrip("/"),
                        )
                    ],
                )
            )

        audit_table.grant_write_data(sign_fn)
        allow_ssm(sign_fn, TIER1_CERT_PARAM, ["ssm:GetParameter"])

        allow_ssm(pubkey_fn, TIER1_CERT_PARAM, ["ssm:GetParameter"])

        audit_table.grant_read_data(audit_fn)  # covers the by-cmd GSI

        audit_table.grant_write_data(rotation_fn)
        allow_ssm(rotation_fn, TIER1_CERT_PARAM,
                  ["ssm:GetParameter", "ssm:PutParameter"])
        # Tier 0 pubkey is a SecureString: GetParameter alone is not enough,
        # kms:Decrypt on the SSM key is required. The parameter is encrypted
        # with the account's default SSM key (alias/aws/ssm); grant decrypt
        # scoped to that key.
        allow_ssm(rotation_fn, TIER0_PUBKEY_PARAM, ["ssm:GetParameter"])
        rotation_fn.add_to_role_policy(
            iam.PolicyStatement(
                actions=["kms:Decrypt"],
                resources=[
                    Stack.of(self).format_arn(
                        service="kms",
                        resource="alias/aws/ssm",
                    )
                ],
                conditions={
                    "StringEquals": {
                        "kms:ViaService": f"ssm.{Stack.of(self).region}.amazonaws.com"
                    }
                },
            )
        )

        # ------------------------------------------------------------------
        # API Gateway: regional REST API, AWS_IAM auth on every route.
        # ------------------------------------------------------------------
        api = apigw.RestApi(
            self,
            "PkiApi",
            rest_api_name="watcher-pki",
            description="Watcher PKI: Tier 1 provisioning signer, pubkey, audit, rotation.",
            endpoint_types=[apigw.EndpointType.REGIONAL],
            default_method_options=apigw.MethodOptions(
                authorization_type=apigw.AuthorizationType.IAM
            ),
            deploy_options=apigw.StageOptions(
                stage_name="v1",
                throttling_rate_limit=100,
                throttling_burst_limit=50,
                # Tight cap on the endpoint that mints signatures:
                # provisioning is rare and human-driven.
                method_options={
                    "/sign/POST": apigw.MethodDeploymentOptions(
                        throttling_rate_limit=10,
                        throttling_burst_limit=5,
                    )
                },
                logging_level=apigw.MethodLoggingLevel.INFO,
                # Request bodies may reference secrets (fingerprints); never
                # log full payloads.
                data_trace_enabled=False,
                metrics_enabled=True,
            ),
        )

        api.root.add_resource("tier1").add_resource("pubkey").add_method(
            "GET", apigw.LambdaIntegration(pubkey_fn)
        )
        api.root.add_resource("sign").add_method(
            "POST", apigw.LambdaIntegration(sign_fn)
        )
        api.root.add_resource("audit").add_method(
            "GET", apigw.LambdaIntegration(audit_fn)
        )
        api.root.add_resource("rotation").add_method(
            "POST", apigw.LambdaIntegration(rotation_fn)
        )

        # ------------------------------------------------------------------
        # Outputs
        # ------------------------------------------------------------------
        CfnOutput(self, "ApiUrl", value=api.url, description="Base URL (append v1/...)")
        CfnOutput(self, "Tier1KeyAliasArn", value=tier1_alias.alias_arn)
        CfnOutput(self, "Tier1KeyId", value=tier1_key.key_id)
        CfnOutput(self, "AuditTableName", value=audit_table.table_name)
        CfnOutput(self, "Tier1CertParam", value=TIER1_CERT_PARAM)
        CfnOutput(self, "Tier0PubkeyParam", value=TIER0_PUBKEY_PARAM)
