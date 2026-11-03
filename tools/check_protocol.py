#!/usr/bin/env python3
"""Cross-check the firmware JSON contract against the browser UI.

The firmware emits the state/config payloads (src/state_json.cpp) and the web UI
consumes them (data/js/*.js). Nothing forces the two to agree at compile time,
so this script does it mechanically:

  1. builds tools/sim/json_dump.exe (the real src/state_json.cpp, host build)
     and runs it to get the firmware's state/config JSON skeleton
  2. extracts the key names the firmware writes for the other payloads
     (/api/info, hello, log, reply) straight out of src/web_server.cpp
  3. extracts the payload properties the UI actually reads, by parsing the
     body of each payload handler (onState, onConfig, renderInfo, ...)
  4. reports every property the UI reads that the firmware does not provide

Fail-closed rules. A gate that silently skips what it cannot find is worse than
no gate at all: a renamed handler or a deleted UI module would leave the
"ui reads" counts at zero while still exiting 0. Therefore a missing expected
file, a missing expected handler, or a parameter list that does not start with
the contracted parameter name(s) is a hard failure, not a `continue`. The
per-namespace "ui reads" count remains the canary: if it collapses toward zero
on a healthy-looking tree, this script should be refusing to pass.

Property extraction limits. Handler bodies are scanned for direct dotted reads
of the payload parameter (`s.temperature`, and now `s?.temperature` too).
Patterns that hide the read set from a regex are treated as contract errors in
their own right: destructuring the parameter (`const {temperature} = s`) and
aliasing it (`const st = s`) both make the gate unable to know which keys the
UI consumes, so they fail loudly instead of under-counting. Delegation to a
helper that receives the payload is still invisible here; keep handler bodies
reading the payload directly (or extend this extractor) if that matters.

Chart contract. data/js/chart.js is part of the wire contract: every entry in
CHART_SAMPLE_FIELDS must still be read from a graph sample. The chart is parsed
separately because it consumes the sample array rather than a named payload
handler, and its field names must match what src/state_json.cpp emits.

Usage:  python tools/check_protocol.py
Exit code 0 = contract satisfied, 1 = mismatch, 2 = tooling problem.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIM = ROOT / "tools" / "sim"
UI_DIR = ROOT / "data" / "js"
WEB_SERVER = ROOT / "src" / "web_server.cpp"

# UI handler -> (payload namespace, parameter name)
UI_HANDLERS = {
    "controls.js": {"onState": ("state", "s"), "onConfig": ("config", "c"),
                    "onHello": ("hello", "h")},
    "debug.js": {"renderState": ("state", "s"), "renderConfig": ("config", "c"),
                 "renderInfo": ("info", "info")},
    # render(state, diag) reads the nested diagnostics object as well; `diag`
    # is the state's "diag" sub-object, so its paths carry that prefix.
    "sensor.js": {"render": [("state", "s"), ("state", "diag", "diag")],
                  "renderConfig": ("config", "c")},
    "console.js": {"addLog": ("log", "entry"), "addReply": ("reply", "reply")},
}

# Sample fields the chart reads (chart.js draws from the sample array).
CHART_SAMPLE_FIELDS = {"t", "temp", "sp", "out"}
CHART_FILE = UI_DIR / "chart.js"

# ------------------------------------------------------------------ toolchain
def find_cxx() -> str:
    found = shutil.which("g++")
    if found:
        return found
    for cand in (r"C:\msys64\ucrt64\bin\g++.exe", r"C:\msys64\mingw64\bin\g++.exe",
                 "/usr/bin/g++"):
        if pathlib.Path(cand).exists():
            return cand
    return "g++"


def build_env() -> dict:
    env = os.environ.copy()
    compiler_dir = os.path.dirname(os.path.abspath(find_cxx()))
    if compiler_dir:
        env["PATH"] = compiler_dir + os.pathsep + env.get("PATH", "")
    return env


def build_dump() -> pathlib.Path:
    exe = SIM / "json_dump.exe"
    sources = [SIM / "json_dump.cpp", ROOT / "src" / "state_json.cpp"]
    missing = [str(s) for s in sources if not s.exists()]
    if missing:
        print(f"missing source files: {', '.join(missing)}", file=sys.stderr)
        sys.exit(2)
    cmd = [find_cxx(), "-std=c++17", "-O1", "-w",
           "-I", str(SIM / "shim"), "-I", str(ROOT / "src"),
           "-I", str(ROOT / ".pio" / "libdeps" / "esp32s3_custom" / "ArduinoJson" / "src"),
           *[str(s) for s in sources], "-o", str(exe)]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=build_env())
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
        sys.exit(2)
    return exe


# ------------------------------------------------------------------ firmware
def firmware_paths(node: dict, prefix: str = "") -> set[str]:
    paths: set[str] = set()
    for key, value in node.items():
        path = f"{prefix}{key}"
        paths.add(path)
        if isinstance(value, dict):
            paths |= firmware_paths(value, f"{path}.")
    return paths


def keys_in_window(text: str, marker: str, variable: str, span: int = 2500) -> set[str]:
    """Collect `variable["key"]` assignments near a marker in the firmware source."""
    index = text.find(marker)
    if index < 0:
        return set()
    window = text[index:index + span]
    return set(re.findall(re.escape(variable) + r'\["([^"]+)"\]', window))


def firmware_sample_keys() -> set[str]:
    """The keys the firmware writes for one graph sample."""
    source = ROOT / "src" / "state_json.cpp"
    if not source.exists():
        return set()
    text = source.read_text(encoding="utf-8", errors="replace")
    index = text.find("void sampleToJson")
    if index < 0:
        return set()
    # Brace-match the function body so neighbouring functions cannot leak in.
    start = text.find("{", index)
    if start < 0:
        return set()
    depth = 0
    body_end = len(text)
    for position in range(start, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                body_end = position
                break
    return set(re.findall(r'[A-Za-z_]\w*\["([^"]+)"\]', text[start:body_end]))


def firmware_payloads(state: dict, config: dict) -> dict[str, set[str]]:
    source = WEB_SERVER.read_text(encoding="utf-8", errors="replace") if WEB_SERVER.exists() else ""
    payloads = {
        "state": firmware_paths(state),
        "config": firmware_paths(config),
        "info": keys_in_window(source, '"/api/info"', "doc"),
        "hello": keys_in_window(source, '"hello"', "payload"),
        "log": keys_in_window(source, "broadcastLog", "payload"),
        "reply": keys_in_window(source, "sendWsReply", "data"),
        "sample": firmware_sample_keys(),
    }
    # Envelope keys are part of every websocket message.
    payloads["hello"] |= {"fw", "version", "protocol", "clients"}
    payloads["log"] |= {"level", "ts", "text"}
    payloads["reply"] |= {"id", "ok", "text"}
    return payloads


# ------------------------------------------------------------------ ui
def function_body(text: str, name: str) -> tuple[list[str], str] | None:
    """Return (parameter names, body) of `name(params) { ... }`."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\(([^)]*)\)\s*\{", text)
    if not match:
        return None
    params = [p.strip() for p in match.group(1).split(",") if p.strip()]
    start = match.end() - 1
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return params, text[start + 1:index]
    return params, text[start + 1:]


def destructure_pattern(param: str) -> re.Pattern[str]:
    """`const {a, b} = s` / `({a} = s)`: reads the regex cannot enumerate."""
    name = re.escape(param)
    object_or_array = r"(\{[^{}]*\}|\[[^\[\]]*\])"
    return re.compile(
        r"(?:\b(?:const|let|var)\s*" + object_or_array + r"\s*=\s*" + name + r"\b"
        r"|\(\s*" + object_or_array + r"\s*=\s*" + name + r"\s*\))"
    )


def alias_pattern(param: str) -> re.Pattern[str]:
    """`const st = s` / `let st = s`: a second name for the whole payload."""
    return re.compile(
        r"\b(?:const|let|var)\s+[A-Za-z_$][\w$]*\s*=\s*" +
        re.escape(param) + r"\s*(?=[;,)\n}])"
    )


def ui_paths() -> tuple[dict[str, set[str]], list[str]]:
    """Extract payload reads; return (paths, hard-failure messages)."""
    found: dict[str, set[str]] = {}
    problems: list[str] = []
    for filename, handlers in UI_HANDLERS.items():
        path = UI_DIR / filename
        if not path.exists():
            problems.append(
                f"{filename}: expected UI module is missing ({path})")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, spec in handlers.items():
            parsed = function_body(text, name)
            if parsed is None:
                problems.append(
                    f"{filename}: expected handler `{name}(...)` was not found; "
                    f"renaming or removing it voids the protocol contract")
                continue
            params, body = parsed
            # A handler may read more than one payload parameter; the value is
            # either a single (namespace, parameter[, prefix]) pair or a list.
            pairs = spec if isinstance(spec, list) else [spec]
            expected_names = list(dict.fromkeys(pair[1] for pair in pairs))
            if params[:len(expected_names)] != expected_names:
                actual = ", ".join(params) if params else "<no parameters>"
                problems.append(
                    f"{filename}: handler `{name}` has parameter list ({actual}) "
                    f"but the contract expects ({', '.join(expected_names)})")
                continue
            for pair in pairs:
                namespace, expected_param = pair[0], pair[1]
                prefix = pair[2] if len(pair) > 2 else ""
                for verb, matcher in (("destructures", destructure_pattern),
                                      ("aliases", alias_pattern)):
                    hidden = matcher(expected_param).search(body)
                    if hidden:
                        snippet = " ".join(hidden.group(0).split())
                        problems.append(
                            f"{filename}: handler `{name}` {verb} payload "
                            f"parameter `{expected_param}` (`{snippet}`); the "
                            f"static extractor cannot see the reads - read "
                            f"properties directly instead")
                # Direct reads, including optional chaining (`s?.field`).
                pattern = re.compile(
                    r"\b" + re.escape(expected_param) +
                    r"((?:\??\.[A-Za-z_$][\w$]*)+)")
                for match in pattern.finditer(body):
                    chain = match.group(1).replace("?.", ".")
                    parts = chain.lstrip(".").split(".")
                    # Only payload leaves: DOM properties are filtered below.
                    if parts[-1] in DOM_LEAVES:
                        continue
                    path_str = ".".join(parts)
                    if prefix:
                        path_str = prefix + "." + path_str
                    found.setdefault(namespace, set()).add(path_str)
    return found, problems


def chart_problems() -> list[str]:
    """Verify chart.js still reads every sample field the firmware emits."""
    if not CHART_FILE.exists():
        return [f"data/js/chart.js: chart module is missing ({CHART_FILE})"]
    text = CHART_FILE.read_text(encoding="utf-8", errors="replace")
    # Ignore comments: a field that only survives in prose is not a read.
    code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    code = re.sub(r"(?m)//.*$", "", code)
    missing = sorted(
        field for field in CHART_SAMPLE_FIELDS
        if not re.search(r"\." + re.escape(field) + r"\b", code)
    )
    if missing:
        return [
            "data/js/chart.js: no longer reads sample field(s) " +
            ", ".join(missing) + "; the chart would silently drop that series"
        ]
    return []


DOM_LEAVES = {
    "textContent", "innerHTML", "innerText", "value", "disabled", "hidden",
    "checked", "style", "classList", "dataset", "addEventListener",
    "removeEventListener", "appendChild", "removeChild", "querySelector",
    "querySelectorAll", "setAttribute", "getAttribute", "children",
    "firstChild", "firstElementChild", "lastElementChild", "parentElement",
    "closest", "focus", "blur", "click", "options", "selectedIndex",
    "placeholder", "title", "href", "src", "type", "id", "name", "width",
    "height", "scrollTop", "scrollHeight", "offsetWidth", "offsetHeight",
    "getContext", "parentNode", "childNodes", "nodeType", "tagName",
}


def main() -> int:
    exe = build_dump()
    output = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
    if output.returncode != 0:
        print(output.stdout + output.stderr, file=sys.stderr)
        return 2
    try:
        payload = json.loads(output.stdout)
    except json.JSONDecodeError as exc:
        print(f"firmware JSON is not parseable: {exc}", file=sys.stderr)
        return 2

    firmware = firmware_payloads(payload.get("state", {}), payload.get("config", {}))
    ui, problems = ui_paths()
    problems += chart_problems()

    if problems:
        print("protocol gate failed - the UI/firmware contract cannot be verified:",
              file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print(f"{'namespace':<10} {'firmware keys':>14} {'ui reads':>10}")
    missing: list[str] = []
    for namespace in sorted(set(firmware) | set(ui)):
        fw = firmware.get(namespace, set())
        reads = ui.get(namespace, set())
        print(f"{namespace:<10} {len(fw):>14} {len(reads):>10}")
        for path in sorted(reads):
            if path in fw:
                continue
            missing.append(f"{namespace}.{path}")

    if missing:
        print("\nUI reads payload properties the firmware does not provide:")
        for path in missing:
            print(f"  - {path}")
        return 1

    # The chart draws from the sample array; its field names must match too.
    sample_missing = sorted(CHART_SAMPLE_FIELDS - firmware.get("sample", set()))
    if sample_missing:
        print("\nThe chart reads sample fields the firmware does not emit:")
        for path in sample_missing:
            print(f"  - {path}")
        return 1

    print("\nOK: every payload property the UI reads is provided by the firmware.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
