#!/bin/bash
# Host unit test for components/pki/pki_verify.c (pure logic side).
#   1. bakes a TEST-VECTOR tier2_keys.h with ephemeral keys
#      (bake_tier2_keys.py --self-test; header is deleted afterwards)
#   2. generates vectors.h (Python canonical bytes + real signatures)
#   3. compiles test_pki.c + pki_verify.c (-DPKI_HOST_TEST) + cJSON.c
#   4. runs the test; the C canonicalization is memcmp'd against Python
set -e
cd "$(dirname "$0")"

FW="$(cd ../../.. && pwd)"
BAKE="$FW/tools/bake_tier2_keys.py"
CJSON_DIR="$FW/managed_components/espressif__cjson/cJSON"

echo "== [1/4] bake test-vector tier2_keys.h =="
python3 "$BAKE" --self-test --out ../tier2_keys.h >/dev/null
echo "   baked (ephemeral keys; deleted after the test)"

echo "== [2/4] generate vectors.h =="
python3 gen_vectors.py

echo "== [3/4] compile =="
gcc -std=c99 -Wall -Wextra -g \
    -DPKI_HOST_TEST \
    -I. -I.. \
    -I"$CJSON_DIR" \
    test_pki.c "$CJSON_DIR/cJSON.c" \
    -o test_pki -lm

echo "== [4/4] run =="
./test_pki
rc=$?

echo "== cleanup =="
rm -f ../tier2_keys.h test_pki
echo "   removed test tier2_keys.h (real build re-bakes with real keys)"
exit $rc
