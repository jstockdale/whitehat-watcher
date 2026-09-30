#!/usr/bin/env python3
"""Build-time audit: no NVS/flash access reachable from PSRAM-stack tasks.

Background: the v0.3.9 RAM optimization moved several task stacks to
PSRAM (task_psram_create). A task whose stack lives in PSRAM must NEVER
touch NVS/flash directly (nvs_open/nvs_get_*/nvs_set*/nvs_commit/
spi_flash_*/esp_ota_*/partition writes): the flash driver disables the
CPU cache, and the PSRAM stack behind the disabled cache panics the CPU
("Cache disabled but cached memory region accessed").

The v0.3.9 mitigation was a manual grep ("no NVS/OTA commands exist").
It rotted: set_name (new in v0.4.1) called hid_ble_set_device_name(),
which did nvs_set_str/nvs_commit inline — one call frame below the
PSRAM-stack link worker — and rebooted on every invocation. v0.4.6
proved READS are equally fatal: mqtt_native and ww_feed (PSRAM stacks)
panicked on direct nvs_get_*. A task-body-only check would NOT have
caught any of it. This script follows the call graph transitively from
every PSRAM-stack task and FAILS the build on any reachable NVS/flash
API — reads included.

The dedicated NVS task (nvs_task, internal stack via xTaskCreateStatic
with a DRAM_ATTR static buffer) is allowlisted; it is the ONLY task that
calls nvs_open. All NVS access from any other task must go through the
synchronous seal_dev_nvs_*_sync() APIs (single FIFO queue, caller blocks
until the broker completes the op; see seal_dev.h for the full contract).

v0.4.8: bare xTaskCreate() is UNSAFE for flash-touching tasks even when
the stack "looks" small. xTaskCreate allocates via
pvPortMalloc(MALLOC_CAP_DEFAULT); with SPIRAM_USE_MALLOC=y and
ALWAYSINTERNAL=16384, a 4096-word (16384-byte) stack is NOT < 16384, so
the allocator placed the nvs_task stack in PSRAM and the first
NVS->flash->cache-disable panicked
(esp_task_stack_is_sane_cache_disabled) — the v0.4.7 boot loop. Every
allowlisted (flash-touching) task MUST use xTaskCreateStatic() with a
DRAM_ATTR static stack; this script enforces it.

v0.4.9: xTaskGetHandle() asserts strlen(pcNameToQuery) < 16
(tasks.c:2855). Task creation truncates names to 15 chars, so a lookup
with a longer name panics — the v0.4.8 boot loop ("sscma_client_process"
is 20 chars, stored as "sscma_client_pr"). This script fails the build
if any string passed to xTaskGetHandle() (directly or via a string
array) is >= 16 chars.

Exit 0 = clean, 1 = violation (fails the build).
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIRS = [os.path.join(REPO, "components"), os.path.join(REPO, "main")]

# NVS/flash APIs that must not be reachable from a PSRAM-stack task.
# v0.4.7: reads are included — nvs_get_* panics PSRAM-stack tasks just
# like nvs_set_* (v0.4.6 proved it). The nvs task is the ONLY task that
# calls nvs_open; everyone else goes through seal_dev_nvs_*_sync().
FLASH_APIS = [
    "nvs_open", "nvs_close",
    "nvs_get_", "nvs_set_", "nvs_commit", "nvs_erase_",
    "spi_flash_", "esp_ota_", "esp_partition_write",
    "esp_partition_erase", "esp_partition_erase_range",
]

ALLOWLIST_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "psram_flash_allowlist.txt")

# Identifiers that look like calls but are language/RTOS noise.
NOISE = {
    "if", "for", "while", "switch", "return", "sizeof", "defined",
    "ESP_LOGI", "ESP_LOGW", "ESP_LOGE", "ESP_LOGD", "ESP_LOGV",
    "ESP_ERROR_CHECK", "ESP_GOTO_ON_ERROR",
}


def load_allowlist():
    allowed = set()
    if os.path.exists(ALLOWLIST_FILE):
        with open(ALLOWLIST_FILE) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    allowed.add(line)
    return allowed


def all_sources():
    for root in SRC_DIRS:
        for dirpath, _, filenames in os.walk(root):
            for fn in filenames:
                if fn.endswith((".c", ".cpp")):
                    yield os.path.join(dirpath, fn)


def strip_comments(src):
    """Remove string literals then // and /* */ comments so doc text and
    comment-like sequences inside strings can't false-positive (or, worse,
    swallow real code)."""
    # Strings first: a "/*" inside a string literal is not a comment.
    # (Char literals are NOT blanked: apostrophes in English comments
    # like "don't" would mis-pair the regex and swallow real code. A
    # one-character literal can never contain a flash API name.)
    src = re.sub(r'"(?:\\.|[^"\\])*"', '""', src)
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def parse_functions():
    """Return {func_name: (path, body)} for all definitions in the repo."""
    funcs = {}
    for path in all_sources():
        with open(path, errors="replace") as f:
            src = strip_comments(f.read())
        # Definition: identifier + (...) + { at line start, no trailing ';'
        # before the brace (excludes prototypes).
        for m in re.finditer(
                r"(?m)^[ \t]*[A-Za-z_][\w\s\*]*?\b([A-Za-z_][A-Za-z0-9_]*)"
                r"\s*\([^;{}]*\)\s*\{", src):
            name = m.group(1)
            if name in NOISE:
                continue
            depth = 0
            i = m.end() - 1
            body = None
            for j in range(i, len(src)):
                c = src[j]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        body = src[i:j + 1]
                        break
            if body is not None and name not in funcs:
                funcs[name] = (path, body)
    return funcs


def find_psram_tasks(funcs):
    """Task function names passed to task_psram_create()."""
    tasks = set()
    for path in all_sources():
        with open(path, errors="replace") as f:
            src = strip_comments(f.read())
        for m in re.finditer(
                r"task_psram_create\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)", src):
            name = m.group(1)
            if name != "TaskFunction_t" and name in funcs:
                tasks.add(name)
    return tasks


def check_flash_task_stacks(allowed):
    """v0.4.8: every flash/NVS-touching task must run on a
    linker-guaranteed internal stack.

    The v0.4.7 boot panic: nvs_task used bare xTaskCreate() with a
    4096-word (16384-byte) stack. With SPIRAM_USE_MALLOC=y and
    ALWAYSINTERNAL=16384, a 16384-byte allocation is NOT < 16384, so the
    allocator placed the stack in PSRAM; the first NVS->flash->
    cache-disable then panicked (esp_task_stack_is_sane_cache_disabled).

    Rule: a function in the allowlist (permitted to touch flash/NVS)
    must be created via xTaskCreateStatic() with a DRAM_ATTR static
    stack. Bare xTaskCreate() for such a function is a build failure.
    Returns a list of violation strings.
    """
    violations = []
    sources = {}
    for path in all_sources():
        with open(path, errors="replace") as f:
            sources[path] = strip_comments(f.read())

    for fn in sorted(allowed):
        bare_found = False
        for path, src in sources.items():
            if re.search(r"\bxTaskCreate\s*\(\s*" + re.escape(fn) + r"\b",
                         src):
                bare_found = True
                rel = os.path.relpath(path, REPO)
                violations.append(
                    f"flash-touching task '{fn}' uses bare xTaskCreate() "
                    f"({rel}): with SPIRAM_USE_MALLOC the allocator may "
                    f"place the stack in PSRAM (v0.4.7 boot panic). Use "
                    f"xTaskCreateStatic() with a DRAM_ATTR static stack.")
        # xTaskCreateStatic creation: the stack buffer arg must be a
        # DRAM_ATTR-declared static in the same file.
        static_pat = re.compile(
            r"\bxTaskCreateStatic\s*\(\s*" + re.escape(fn) +
            r"\s*,[^;]*?,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*&", re.S)
        found_static = False
        for path, src in sources.items():
            m = static_pat.search(src)
            if not m:
                continue
            found_static = True
            stack_var = m.group(1)
            if not re.search(r"\bDRAM_ATTR\b[^;]*\b" +
                             re.escape(stack_var) + r"\b", src):
                rel = os.path.relpath(path, REPO)
                violations.append(
                    f"flash-touching task '{fn}' xTaskCreateStatic stack "
                    f"'{stack_var}' is not DRAM_ATTR ({rel}): the stack "
                    f"must be linker-guaranteed in internal DRAM.")
        if not found_static and not bare_found:
            violations.append(
                f"flash-touching task '{fn}' has no xTaskCreateStatic() "
                f"creation site: use xTaskCreateStatic() with a DRAM_ATTR "
                f"static stack (bare xTaskCreate is unsafe with "
                f"SPIRAM_USE_MALLOC).")
    return violations


def callees(body, funcs):
    """Function names called from body that are defined in the repo."""
    out = set()
    for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", body):
        name = m.group(1)
        if name not in NOISE and name in funcs:
            out.add(name)
    return out


def flash_hits(body):
    hits = []
    for api in FLASH_APIS:
        for m in re.finditer(r"\b" + re.escape(api), body):
            hits.append(api)
    return hits


# FreeRTOS configMAX_TASK_NAME_LEN (CONFIG_FREERTOS_MAX_TASK_NAME_LEN).
# xTaskGetHandle() asserts strlen(pcNameToQuery) < 16 (tasks.c:2855), and
# task creation truncates names to 15 chars. A lookup name >= 16 chars
# panics at runtime (v0.4.8 boot loop: "sscma_client_process" is 20 chars).
MAX_TASK_NAME_LEN = 16


def check_task_get_handle_names():
    """Fail if any string passed to xTaskGetHandle() is >= 16 chars.

    Covers both direct calls — xTaskGetHandle("name") — and array-driven
    lookups — xTaskGetHandle(arr[i]) where arr is a brace-initialized
    string array in the same file. FreeRTOS truncates stored task names
    to 15 chars, so the lookup name must also be <= 15 chars.
    """
    violations = []
    for path in all_sources():
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read()
        if "xTaskGetHandle" not in raw:
            continue
        rel = os.path.relpath(path, REPO)
        # Strip comments but KEEP string literals (we need their values).
        src = re.sub(r"/\*.*?\*/", " ", raw, flags=re.S)
        src = re.sub(r"//[^\n]*", " ", src)
        checked = set()

        def check_name(name, origin):
            if name in checked:
                return
            checked.add(name)
            if len(name) >= MAX_TASK_NAME_LEN:
                violations.append(
                    f"{rel}: task name '{name}' ({len(name)} chars) passed "
                    f"to xTaskGetHandle{origin} — max is "
                    f"{MAX_TASK_NAME_LEN - 1} chars (xTaskGetHandle asserts "
                    f"strlen < {MAX_TASK_NAME_LEN}; FreeRTOS truncates "
                    f"stored names to {MAX_TASK_NAME_LEN - 1}).")

        # Direct: xTaskGetHandle("literal")
        for m in re.finditer(
                r"\bxTaskGetHandle\s*\(\s*\"((?:\\.|[^\"\\])*)\"", src):
            check_name(m.group(1), " directly")

        # Indirect: xTaskGetHandle(arr[i]) — find the array's initializer.
        for m in re.finditer(
                r"\bxTaskGetHandle\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\[", src):
            arr = m.group(1)
            am = re.search(
                r"\b" + re.escape(arr) +
                r"\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;",
                src, flags=re.S)
            if not am:
                violations.append(
                    f"{rel}: xTaskGetHandle({arr}[i]) — could not find "
                    f"initializer for '{arr}'; cannot verify name lengths.")
                continue
            for sm in re.finditer(r"\"((?:\\.|[^\"\\])*)\"", am.group(1)):
                check_name(sm.group(1), f" via array '{arr}'")
    return violations


def check_task_create_names():
    """Fail if any task is created with a name > 15 chars.

    FreeRTOS truncates the stored name to configMAX_TASK_NAME_LEN-1
    (15) chars. A longer creation name silently diverges from the
    intended name — and any xTaskGetHandle() lookup with the full name
    panics (tasks.c:2855). Covers xTaskCreate* variants; the name is
    always the 2nd argument.
    """
    violations = []
    create_re = re.compile(
        r"\bxTaskCreate(?:Static)?(?:PinnedToCore)?\s*"
        r"\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*"
        r"\"((?:\\.|[^\"\\])*)\"")
    for path in all_sources():
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read()
        if "xTaskCreate" not in raw:
            continue
        rel = os.path.relpath(path, REPO)
        src = re.sub(r"/\*.*?\*/", " ", raw, flags=re.S)
        src = re.sub(r"//[^\n]*", " ", src)
        for m in create_re.finditer(src):
            name = m.group(1)
            if len(name) >= MAX_TASK_NAME_LEN:
                violations.append(
                    f"{rel}: task created with name '{name}' "
                    f"({len(name)} chars) — max is "
                    f"{MAX_TASK_NAME_LEN - 1} chars (FreeRTOS truncates "
                    f"stored names; longer names silently diverge and "
                    f"break xTaskGetHandle lookups).")
    return violations


def main():
    allowed = load_allowlist()
    funcs = parse_functions()
    tasks = find_psram_tasks(funcs)
    if not tasks:
        print("psram_flash_audit: no PSRAM-stack tasks found?!",
              file=sys.stderr)
        return 1

    violations = []  # (task, via_chain, api)
    for task in sorted(tasks):
        if task in allowed:
            continue
        # DFS over the call graph; chain tracks the path for reporting.
        seen = set()
        stack = [(task, [task])]
        while stack:
            fn, chain = stack.pop()
            if fn in seen or fn in allowed:
                continue
            seen.add(fn)
            path, body = funcs[fn]
            for api in flash_hits(body):
                violations.append((task, " -> ".join(chain), api, path))
            for callee in callees(body, funcs):
                if callee not in seen:
                    stack.append((callee, chain + [callee]))

    # v0.4.8: flash-touching (allowlisted) tasks must use
    # xTaskCreateStatic + DRAM_ATTR static stack. Bare xTaskCreate is
    # unsafe with SPIRAM_USE_MALLOC (v0.4.7 boot panic).
    stack_violations = check_flash_task_stacks(allowed)

    # v0.4.9: xTaskGetHandle() asserts strlen(name) < 16. A lookup name
    # >= 16 chars panics at runtime (v0.4.8 boot loop on
    # "sscma_client_process").
    name_violations = check_task_get_handle_names()

    # v0.4.9: task creation names must also fit — FreeRTOS truncates
    # stored names to 15 chars, so a >15-char creation name silently
    # diverges from the intended name (and any xTaskGetHandle lookup
    # using the full name would panic). Catch it at build time.
    create_violations = check_task_create_names()

    if violations or stack_violations or name_violations or create_violations:
        print("=" * 70)
        print("PSRAM/FLASH AUDIT FAILED")
        print("=" * 70)
        print("A PSRAM-stack task must NEVER reach an NVS/flash API (reads")
        print("included — v0.4.6 proved nvs_get_* panics too): the flash")
        print("driver disables the CPU cache and the PSRAM stack behind it")
        print("panics the CPU. Route through seal_dev_nvs_*_sync() (the")
        print("internal-stack nvs task; single FIFO queue, caller blocks).")
        print()
        for task, chain, api, path in sorted(set(violations)):
            rel = os.path.relpath(path, REPO)
            print(f"  task {task}: {chain} calls '{api}' ({rel})")
        if stack_violations:
            print()
            print("Flash-touching task stack placement:")
            for v in sorted(set(stack_violations)):
                print(f"  {v}")
        if name_violations:
            print()
            print("xTaskGetHandle() name length (max 15 chars):")
            for v in sorted(set(name_violations)):
                print(f"  {v}")
        if create_violations:
            print()
            print("Task creation name length (max 15 chars):")
            for v in sorted(set(create_violations)):
                print(f"  {v}")
        print()
        print("If this is the dedicated NVS task, add its function")
        print(f"name to {os.path.basename(ALLOWLIST_FILE)} AND create it")
        print("with xTaskCreateStatic() + a DRAM_ATTR static stack (bare")
        print("xTaskCreate is unsafe with SPIRAM_USE_MALLOC).")
        return 1
    print(f"psram_flash_audit: clean "
          f"({len(tasks)} PSRAM-stack tasks, call graphs checked, "
          f"{len(allowed)} allowlisted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
