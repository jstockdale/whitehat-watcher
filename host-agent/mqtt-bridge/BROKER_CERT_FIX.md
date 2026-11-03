# Broker TLS cert fix — mqtt.offx1.com (prepared 2026-10-29)

## Diagnosis (verified from bridge-side primary logs)

- Broker cert (CN br0ken.net, Let's Encrypt, SAN covers mqtt.offx1.com)
  expired **2026-10-28 19:02:17 UTC**.
- Every `subscriber.py` connection since then fails the TLS handshake:
  `ssl.SSLCertVerificationError: certificate verify failed: certificate has expired`
  (see `subscriber.log` in this directory).
- As of **2026-10-29 14:03 UTC** the loop is unchanged: the minutely
  watchdog restarts the subscriber (~1/min), each attempt dies within
  ~1 s on the same error. 1,092 expired-cert failures logged in
  `subscriber.log`; 844 watchdog restarts on 2026-10-29 alone
  (`watchdog.log`). The watchdog is working — the broker is the blocker.
- The console page's MQTT-over-WSS (port 9883) uses the same cert and is
  presumably affected the same way.
- This is the second lapse of this chain: the previous cert expired
  2026-08-29 and was renewed 2026-09-23 (see memory 2026-09-23).

## Fix — run on the broker host

```bash
# 1. See what certbot has on disk
sudo certbot certificates
#   - If the mqtt.offx1.com / br0ken.net cert already shows a future
#     expiry, auto-renew did its half — skip to step 3.
#   - If it still shows 2026-10-28 19:02, renew:
sudo certbot renew

# 2. (only if renew was needed) confirm the new expiry is in the future
sudo certbot certificates

# 3. FULL restart — a reload/SIGHUP did NOT pick up the new cert in
#    September; only a full restart did.
sudo systemctl restart mosquitto
```

## Prevent a third lapse (2 minutes, same visit)

Auto-renew alone is not enough: without a deploy hook, mosquitto keeps
serving the old cert until someone restarts it by hand — very likely
what happened this time.

```bash
# Check the renewal timer exists
systemctl list-timers | grep -i certbot

# Install a deploy hook that restarts mosquitto on every renewal
sudo install -m 755 /dev/stdin /etc/letsencrypt/renewal-hooks/deploy/restart-mosquitto.sh <<'EOF'
#!/bin/sh
systemctl restart mosquitto
EOF
```

## Verify

```bash
# Broker side: served cert should show a future notAfter
echo | openssl s_client -connect mqtt.offx1.com:8883 -servername mqtt.offx1.com 2>/dev/null \
  | openssl x509 -noout -dates

# Bridge side: nothing to do — the watchdog revives the subscriber
# within ~1 minute. REAL recovery = a subscriber that stays alive for
# several minutes AND logs connected traffic in telemetry.log.
# Do NOT trust bridge_health.py alone: it only checks that a pid
# exists and the log is fresh, and the watchdog's dying-every-minute
# process satisfies both, so it can flash UP mid-loop.
```
