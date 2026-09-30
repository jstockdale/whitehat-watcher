# seal component: WMS2 verifier (Phase 2) + native MQTT + WIA1 sessions (Phase 3).
# Crypto: upstream Monocypher (CC0/BSD-2) + optional/monocypher-ed25519
# (RFC 8032 — NOT the core's EdDSA-BLAKE2b variant).
idf_component_register(
    SRCS "monocypher.c"
         "optional/monocypher-ed25519.c"
         "wms2.c"
         "seal_dev.c"
         "mqtt_native.c"
         "wia1.c"
    INCLUDE_DIRS "include" "."
    REQUIRES mqtt nvs_flash mbedtls json net task_psram
    PRIV_REQUIRES hid_ble)
