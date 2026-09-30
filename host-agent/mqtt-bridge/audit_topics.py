#!/usr/bin/env python3
"""Static ACL-vs-topics audit: catch ungranted MQTT topics BEFORE they ship.

Checks every MQTT publish site in:

  * the bridge's own ``*.py`` scripts -- AST scan for ``client.publish`` /
    ``publish_checked`` / ``bridge_publish`` calls, with the topic argument
    resolved from string literals, f-strings, ``+`` concatenation, and
    module-level constants (``TOPIC_PREFIX``/``PREFIX`` default to
    ``"watcher"``). Assumed to publish as ``$MQTT_USER`` (default ``muse``).

  * the Watcher Console page (``watcher-console.html``) -- topics are read
    from ``mqttTopics()`` and the ``*Topic()`` helpers, expanded with the
    page's default ``"watcher"`` prefix, and resolved through every
    ``mqttPublish(<topic-expr>, ...)`` call site. Checked as the
    ``watcher`` MQTT user (the page's broker login).

Exit 0: every publish topic is granted. Exit 1: violations found (listed
with file:line). Exit 2: the ACL mirror is missing/unparseable --
fail closed, this gate must have the ACL to do its job.

Run this before shipping a bridge or console patch, and after any broker
ACL change (re-mirror the ACL file first).
"""

import ast
import os
import re
import sys

import topic_acl

BRIDGE_DIR = os.path.dirname(os.path.abspath(__file__))
CONSOLE_HTML = os.path.expanduser(
    "~/workspace/your_files/watcher-console/watcher-console.html")

# .py files that are not bridge publish sites (tests, vendored, deps)
SKIP_PY = ("test_", "seal-crypto-bundle", "seal-aead-bundle")
SKIP_DIRS = ("__pycache__", ".testdeps", "audio-captures", "snapshots",
             "vectors")


def _resolve_str(node, consts):
    """Best-effort resolve of an AST expression to a topic string.
    Returns the string, or None if it can't be resolved statically."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        parts = []
        for v in node.values:
            if isinstance(v, ast.Constant) and isinstance(v.value, str):
                parts.append(v.value)
            elif isinstance(v, ast.FormattedValue):
                s = _resolve_str(v.value, consts)
                if s is None:
                    return None
                parts.append(s)
            else:
                return None
        return "".join(parts)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        l, r = _resolve_str(node.left, consts), _resolve_str(node.right,
                                                             consts)
        return l + r if l is not None and r is not None else None
    if isinstance(node, ast.Name):
        return consts.get(node.id)
    return None


def _module_consts(tree):
    """Collect NAME = <resolvable string> assignments.

    Module level first, then function-level names not already known
    (topic constants like command_topic usually live inside main())."""
    consts = {"TOPIC_PREFIX": "watcher", "PREFIX": "watcher", "TOPIC":
              "watcher"}

    def harvest(nodes):
        for node in nodes:
            if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                    and isinstance(node.targets[0], ast.Name):
                name = node.targets[0].id
                if name in consts:
                    continue
                # os.environ.get("MQTT_TOPIC") or "watcher" -> "watcher"
                val = node.value
                if isinstance(val, ast.BoolOp) \
                        and isinstance(val.op, ast.Or):
                    val = val.values[-1]
                # os.environ.get("MQTT_TOPIC", "watcher") -> default arg
                if isinstance(val, ast.Call) and val.args:
                    last = val.args[-1]
                    if isinstance(last, ast.Constant):
                        val = last
                s = _resolve_str(val, consts)
                if s is not None:
                    consts[name] = s

    harvest(tree.body)
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            harvest(node.body)
    return consts


def bridge_publish_sites():
    """Yield (file, lineno, topic|None) for each publish call in bridge
    scripts. topic=None means the topic is dynamic (reported, not failed)."""
    sites = []
    for root, dirs, files in os.walk(BRIDGE_DIR):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in sorted(files):
            if not fn.endswith(".py") or fn.startswith(SKIP_PY):
                continue
            if fn in ("topic_acl.py", "audit_topics.py"):
                continue
            path = os.path.join(root, fn)
            try:
                tree = ast.parse(open(path, encoding="utf-8").read(), path)
            except (OSError, SyntaxError):
                continue
            consts = _module_consts(tree)
            for node in ast.walk(tree):
                if not isinstance(node, ast.Call):
                    continue
                f = node.func
                # (call-shape, topic-arg-index):
                #   client.publish(topic, ...)              -> 0
                #   publish_checked(client, topic, ...)      -> 1
                #   bridge_publish(client, topic, ...)       -> 1
                #   _pub(client, user, topic, ...) [keepalive]-> 2
                shape = None
                if isinstance(f, ast.Attribute) \
                        and f.attr in ("publish", "publish_checked"):
                    shape = ("attr-pub", 1 if f.attr == "publish_checked"
                             else 0)
                elif isinstance(f, ast.Name) and f.id == "publish_checked":
                    shape = ("name-pub", 1)
                elif (isinstance(f, ast.Name) and f.id == "bridge_publish") \
                        or (isinstance(f, ast.Attribute)
                            and f.attr == "bridge_publish"):
                    shape = ("bridge", 1)
                elif isinstance(f, ast.Name) and f.id == "_pub" \
                        and fn == "keepalive.py":
                    # keepalive.py's daemon wrapper _pub(client, user,
                    # topic, ...); scoped to that file only
                    shape = ("kpub", 2)
                if shape is None or len(node.args) <= shape[1]:
                    continue
                topic = _resolve_str(node.args[shape[1]], consts)
                sites.append((os.path.relpath(path, BRIDGE_DIR),
                              node.lineno, topic))
    return sites


def page_publish_topics(html_path):
    """Extract (topic) for every mqttPublish(<topic-expr>, ...) call site
    in the console page. Returns (topics, warnings)."""
    try:
        html = open(html_path, encoding="utf-8").read()
    except OSError as e:
        return [], [f"cannot read console html: {e}"]

    # mqttTopics(): {telemetry:p+"/telemetry", ...}
    m = re.search(r"function mqttTopics\(\)\{var p=[^;]+;return\{([^}]*)\}\}",
                  html)
    tmap = {}
    if m:
        for key, suffix in re.findall(r"(\w+):p\+\"([^\"]*)\"", m.group(1)):
            tmap[key] = "watcher" + suffix
    helpers = {}
    for name, body in re.findall(
            r"function (\w+Topic)\(\)\{return ([^}]*)\}", html):
        body = body.strip()
        mh = re.match(r'mqttCfg\(\)\.topic\+"([^"]*)"', body)
        if mh:
            helpers[name] = "watcher" + mh.group(1)
            continue
        # alias: return bridgeTopic() -> resolve through the named helper
        ma = re.match(r"(\w+)\(\)$", body)
        if ma and ma.group(1) in helpers:
            helpers[name] = helpers[ma.group(1)]

    def resolve(expr):
        expr = expr.strip()
        mt = re.match(r"(?:t|mqttTopics\(\))\s*\.\s*(\w+)", expr)
        if mt:
            return tmap.get(mt.group(1))
        mh = re.match(r"(\w+Topic)\(\)", expr)
        if mh:
            return helpers.get(mh.group(1))
        ms = re.match(r'"([^"]*)"', expr)
        if ms:
            return ms.group(1)
        return None

    topics, warnings, seen = [], [], set()
    for cm in re.finditer(r"mqttPublish\(([^,]+),", html):
        expr = cm.group(1)
        # skip the def line itself: "function mqttPublish(topic,..."
        if expr.strip() == "topic":
            continue
        topic = resolve(expr)
        lineno = html.count("\n", 0, cm.start()) + 1
        if topic is None:
            warnings.append(f"page:{lineno}: unresolvable topic expr "
                            f"'{expr.strip()[:60]}'")
        elif topic not in seen:
            seen.add(topic)
            topics.append((topic, lineno))
    return topics, warnings


def main():
    users, acl_path = topic_acl._load()
    if users is None:
        print("audit_topics: FATAL -- no ACL mirror found "
              f"(looked for {topic_acl.ACL_GLOB} next to topic_acl.py, "
              f"override with ${topic_acl.ACL_ENV_VAR}). Failing closed.",
              file=sys.stderr)
        return 2
    print(f"audit_topics: ACL mirror: {os.path.basename(acl_path)}")

    violations, dynamic = [], []

    bridge_user = os.environ.get("MQTT_USER") or "muse"
    for fn, lineno, topic in bridge_publish_sites():
        if topic is None:
            dynamic.append(f"{fn}:{lineno}")
            continue
        ok, detail = topic_acl.check_write(bridge_user, topic)
        status = "OK  " if ok else "DENY"
        print(f"  [{status}] bridge {fn}:{lineno} -> '{topic}' "
              f"as '{bridge_user}' ({detail})")
        if not ok:
            violations.append(f"bridge {fn}:{lineno}: '{topic}' as "
                              f"'{bridge_user}': {detail}")

    page_topics, warnings = page_publish_topics(CONSOLE_HTML)
    for w in warnings:
        print(f"  [WARN] {w}")
    for topic, lineno in page_topics:
        ok, detail = topic_acl.check_write("watcher", topic)
        status = "OK  " if ok else "DENY"
        print(f"  [{status}] page watcher-console.html:{lineno} -> "
              f"'{topic}' as 'watcher' ({detail})")
        if not ok:
            violations.append(f"page watcher-console.html:{lineno}: "
                              f"'{topic}' as 'watcher': {detail}")

    for d in dynamic:
        print(f"  [DYN ] bridge {d}: topic resolved dynamically -- "
              f"runtime publish_checked() still guards it")

    print()
    if violations:
        print(f"audit_topics: {len(violations)} VIOLATION(S) -- "
              f"these publishes are silently dropped by the broker:")
        for v in violations:
            print(f"  - {v}")
        return 1
    print("audit_topics: PASS -- every publish topic is granted.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
