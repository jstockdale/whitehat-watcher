#!/usr/bin/env python3
"""MQTT topic ACL checker for the Watcher bridge.

Parses the broker's mosquitto ACL file (mirrored locally as
``mosquitto-acl-<date>.acl`` next to this module) and answers one question:
may <user> publish to <topic>?

Two uses:

1. Runtime -- :func:`publish_checked` wraps ``paho`` ``client.publish()``.
   A topic the ACL doesn't grant FAILS LOUDLY on this machine instead of
   being silently dropped by the broker (mosquitto default-denies any
   topic with no matching ACL entry once ``acl_file`` is set).

2. Static -- ``audit_topics.py`` imports this module to check every publish
   site in the bridge scripts and the console page *before* a patch ships.

Mosquitto ACL syntax handled::

    user <name>
    topic [read|write|readwrite] <pattern>

``<pattern>`` supports MQTT wildcards: ``+`` (exactly one level) and ``#``
(multi-level, must be the last level). A pattern without wildcards matches
exactly one topic. A bare ``topic <pattern>`` (no access keyword) defaults
to ``readwrite``, matching mosquitto behavior.

Runtime failure policy: if the ACL mirror file is missing or unparseable,
checks are DISABLED with one loud warning per process (fail-open). The
bridge must keep running -- John's MUSE STATUS pill depends on its
check-ins -- so a missing mirror must never take the bridge down. The
static audit script is the fail-closed gate: it exits 2 without the file.
"""

import glob
import os
import sys

ACL_ENV_VAR = "WATCHER_ACL_FILE"
ACL_GLOB = "mosquitto-acl-*.acl"

# user -> list of (access, pattern); access in {"read", "write", "readwrite"}
_ACL = None
_ACL_PATH = None
_ACL_WARNED = False


class TopicNotGrantedError(RuntimeError):
    """Raised by publish_checked(on_deny="raise") when the ACL has no
    write grant for (user, topic). The publish was NOT sent."""


def acl_path():
    """Locate the ACL mirror: $WATCHER_ACL_FILE, else the newest
    mosquitto-acl-*.acl next to this module. Returns None if not found."""
    override = os.environ.get(ACL_ENV_VAR)
    if override and os.path.isfile(override):
        return override
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = sorted(glob.glob(os.path.join(here, ACL_GLOB)))
    return candidates[-1] if candidates else None


def parse_acl(path):
    """Parse a mosquitto acl_file. Returns {user: [(access, pattern)]}."""
    users = {}
    cur = None
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if parts[0] == "user" and len(parts) == 2:
                cur = parts[1]
                users.setdefault(cur, [])
            elif parts[0] in ("topic", "pattern") and cur is not None \
                    and len(parts) >= 2:
                # "topic [read|write|readwrite] <pattern>"
                if parts[1] in ("read", "write", "readwrite") \
                        and len(parts) >= 3:
                    access, pat = parts[1], parts[2]
                else:
                    access, pat = "readwrite", parts[1]
                users[cur].append((access, pat))
            # lines outside a "user" block or malformed: ignored
    return users


def _load():
    """Load (once) the ACL mirror. Returns (users, path) or (None, None)."""
    global _ACL, _ACL_PATH, _ACL_WARNED
    if _ACL is not None or _ACL_WARNED:
        return _ACL, _ACL_PATH
    path = acl_path()
    if path is None:
        _warn_disabled("no mosquitto-acl-*.acl found next to topic_acl.py "
                       f"(override with ${ACL_ENV_VAR})")
        return None, None
    try:
        _ACL = parse_acl(path)
    except OSError as e:
        _warn_disabled(f"cannot read {path}: {e}")
        return None, None
    _ACL_PATH = path
    return _ACL, _ACL_PATH


def _warn_disabled(why):
    global _ACL_WARNED
    _ACL_WARNED = True
    sys.stderr.write(
        "!!! topic_acl: CHECKS DISABLED -- %s\n"
        "!!! publish_checked() will allow everything until the ACL mirror "
        "is restored.\n" % why)
    sys.stderr.flush()


def topic_matches(pattern, topic):
    """MQTT subscription-pattern match of a concrete topic."""
    pl, tl = pattern.split("/"), topic.split("/")
    i = 0
    for i, pseg in enumerate(pl):
        if pseg == "#":
            return True  # must be last by construction; matches the rest
        if i >= len(tl):
            return False
        if pseg == "+":
            continue
        if pseg != tl[i]:
            return False
    return len(tl) == len(pl)


def _grants(users, user, topic, want):
    """True if any ACL entry for user grants `want` ('read'/'write') on
    topic. 'readwrite' grants both."""
    for access, pattern in users.get(user, []):
        if access in (want, "readwrite") and topic_matches(pattern, topic):
            return True, f"{access} {pattern}"
    return False, "no matching entry"


def check_write(user, topic):
    """(ok, detail): may `user` publish to `topic`?

    If the ACL mirror is unavailable, returns (True, "checks disabled")
    -- fail-open with a loud one-time warning (see module docstring).
    """
    users, path = _load()
    if users is None:
        return True, "checks disabled (no ACL mirror)"
    ok, detail = _grants(users, user, topic, "write")
    if ok:
        return True, f"granted by '{detail}' in {os.path.basename(path)}"
    return False, (f"no write grant for user '{user}' on '{topic}' "
                   f"({os.path.basename(path)})")


def check_read(user, topic):
    """(ok, detail): may `user` subscribe to `topic`? (Not wired into the
    publish path; available for audits.)"""
    users, path = _load()
    if users is None:
        return True, "checks disabled (no ACL mirror)"
    ok, detail = _grants(users, user, topic, "read")
    if ok:
        return True, f"granted by '{detail}' in {os.path.basename(path)}"
    return False, (f"no read grant for user '{user}' on '{topic}' "
                   f"({os.path.basename(path)})")


def deny_banner(user, topic, reason):
    """The loud, greppable banner printed on every denied publish."""
    return (
        "\n"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
        "MQTT ACL-DENY: publish BLOCKED -- not sent to the broker\n"
        f"  user   : {user}\n"
        f"  topic  : {topic}\n"
        f"  reason : {reason}\n"
        "  fix    : add a 'topic write <pattern>' line for this user on the\n"
        "           broker, then re-mirror the ACL file next to topic_acl.py.\n"
        "           Until then this publish does not happen. Any log line\n"
        "           claiming it was published is a lie -- check here first.\n"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
    )


def publish_checked(client, topic, payload=None, qos=0, retain=False, *,
                    user=None, on_deny="raise", log=None):
    """Drop-in replacement for ``client.publish()`` with an ACL pre-check.

    - user: MQTT user the client authenticated as. Defaults to
      ``$MQTT_USER`` (else ``"muse"``).
    - on_deny="raise": raise :class:`TopicNotGrantedError` (one-shot
      scripts: fail loudly, non-zero exit).
    - on_deny="skip": print the ACL-DENY banner (to stderr, plus ``log``
      if given) and return False WITHOUT publishing (daemons/loops that
      must stay up, e.g. subscriber.py's bridge_publish).

    Returns paho's MQTTMessageInfo on success.
    """
    if user is None:
        user = os.environ.get("MQTT_USER") or "muse"
    ok, reason = check_write(user, topic)
    if not ok:
        banner = deny_banner(user, topic, reason)
        sys.stderr.write(banner)
        sys.stderr.flush()
        if log is not None:
            try:
                log(f"ACL-DENY publish blocked: user={user} topic={topic} "
                    f"({reason})")
            except Exception:
                pass
        if on_deny == "raise":
            raise TopicNotGrantedError(
                f"ACL-DENY: user '{user}' may not publish to '{topic}': "
                f"{reason}")
        return False
    return client.publish(topic, payload, qos=qos, retain=retain)


def granted_write_topics(user):
    """Sorted list of write patterns granted to `user` (for audits)."""
    users, _ = _load()
    if users is None:
        return []
    return sorted(p for a, p in users.get(user, [])
                  if a in ("write", "readwrite"))
