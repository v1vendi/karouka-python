"""[NeedsCallerRealm] members called from Python, where no JS function is running.

Run by tools/dev.sh python (and check). A Python call enters the document realm's execution
context (Call: HTML::TemporaryExecutionContext with callbacks enabled), which has no function,
so a member whose C++ reads its caller from the running execution context crashes there:
Fetch::fetch did (FetchMethod.cpp, `vm.running_execution_context().function`), and Window.fetch
is an override for that. This test loads tests/python/pages/caller_realm.html headless, whose
Python calls every bound [NeedsCallerRealm] member, and checks that each call returned or raised
(a crash ends this process, and the log's last "caller_realm:" line names the member), that
_karouka._bindings_report()'s "caller_realm" list has no member the page doesn't call, and what
the calls did. It also calls a few through the app's interpreter (View.window). Exits 1 on any
failure.
"""

import json
import os
import sys

import _karouka

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}", file=sys.stderr)


# Bound [NeedsCallerRealm] members no Python code can reach: Python runs only in documents.
UNREACHABLE = {
    "DedicatedWorkerGlobalScope.postMessage/2(message, options)": "a worker's global; Python runs in no worker",
    "DedicatedWorkerGlobalScope.postMessage/2(message, transfer)": "a worker's global; Python runs in no worker",
    "Worker.postMessage/2(message, options)": "workers are pruned: no Worker to call it on",
    "Worker.postMessage/2(message, transfer)": "workers are pruned: no Worker to call it on",
}
# Members whose call raises by design: a policy without a createHTML callback.
RAISES = {"TrustedTypePolicy.createHTML/2 (no callback)": "TypeError"}

source = os.environ["KR_SOURCE_DIR"]
page = os.path.join(source, "tests/python/pages/caller_realm.html")
with open(os.path.join(source, "tests/python/pages/async-data.txt"), encoding="utf-8") as f:
    data = f.read()

system = _karouka.System(backend="software")
view = system.view(400, 300)
view.load_file(page)

clock = 0.0
published = {}
for frame in range(300):
    clock += 16.0
    view.advance_time(clock)
    view.render()
    published = json.loads(view.document.getElementById("results").textContent)
    if published.get("results", {}).get("done") or "main" in published.get("results", {}):
        break
print(f"test_caller_realm: settled after {frame + 1} frames of 16 ms", file=sys.stderr)
results = published.get("results", {})
values = published.get("values", {})
check(f"the page ran to the end ({results.get('main')!r})", results.get("done") == "ok")

report = _karouka._bindings_report()
caller_realm = set(report["caller_realm"])
unbound = set(report["unbound"])
print(f"test_caller_realm: {len(caller_realm)} bound [NeedsCallerRealm] members; {len(unbound)} of "
      f"{report['members']} members unbound: {sorted(unbound)}", file=sys.stderr)
check("the report lists [NeedsCallerRealm] members", len(caller_realm) > 0)
for member in sorted(caller_realm):
    if member in UNREACHABLE:
        continue
    outcome = results.get(member)
    check(f"{member} is called by the page", outcome is not None)
    if outcome is not None:
        check(f"{member} returned ({outcome})", outcome == "ok")
for member, outcome in sorted(results.items()):
    name = member.split(" ")[0].rstrip("=")
    if member in RAISES:
        check(f"{member} raised {RAISES[member]} ({outcome})", outcome.startswith(RAISES[member]))
    elif name in unbound:
        check(f"{member} is unbound and raised NotImplementedError ({outcome})", outcome == "unbound")
    elif member.endswith("="):
        check(f"{member} returned ({outcome})", outcome == "ok")
for member in UNREACHABLE:
    check(f"{member} (unreachable: {UNREACHABLE[member]}) is still a bound [NeedsCallerRealm] member",
          member in caller_realm)

# What the calls did.
check(f"location.protocol {values.get('location.protocol')!r}", values.get("location.protocol") == "file:")
check(f"location.href {values.get('location.href')!r}", str(values.get("location.href", "")).endswith("caller_realm.html"))
check(f"location.hash= ({values.get('hash after hash=')!r})", values.get("hash after hash=") == "#h1")
check(f"location.href= ({values.get('hash after href=')!r})", values.get("hash after href=") == "#h2")
check(f"location.assign() ({values.get('hash after assign')!r})", values.get("hash after assign") == "#h3")
check(f"location.replace() ({values.get('hash after replace')!r})", values.get("hash after replace") == "#h4")
check(f"same-value setters kept the document ({values.get('hash after setters')!r})", values.get("hash after setters") == "#h4")
check(f"history.pushState() ({values.get('hash after pushState')!r}, {values.get('state after pushState')!r})",
      values.get("hash after pushState") == "#pushed" and values.get("state after pushState") == 1)
check(f"history.replaceState() ({values.get('state after replaceState')!r})", values.get("state after replaceState") == 2)
check(f"messages delivered ({values.get('messages')!r})",
      values.get("messages") == ["to port", "to port, transfer", "to window", "to window, origin"])
check(f"window.fetch() ({values.get('fetch status')!r})", values.get("fetch status") == 200)
check(f"Request.clone() ({values.get('request clone url')!r})", str(values.get("request clone url", "")).endswith("async-data.txt"))
check(f"Response.clone() ({values.get('response clone status')!r})", values.get("response clone status") == 200)
check(f"whenDefined() resolved to the class ({values.get('whenDefined is XPy')!r})", values.get("whenDefined is XPy") is True)
check(f"createHTML() through a Python callback ({values.get('TrustedTypePolicy.createHTML')!r})",
      values.get("TrustedTypePolicy.createHTML") == "x!")
check(f"createScript() ({values.get('TrustedTypePolicy.createScript')!r})", values.get("TrustedTypePolicy.createScript") == "y;")
check(f"createScriptURL() ({values.get('TrustedTypePolicy.createScriptURL')!r})",
      values.get("TrustedTypePolicy.createScriptURL") == "z#")
check(f"setKeyframes() ({values.get('keyframes')!r})", values.get("keyframes") == 2)
for interface in ("Request", "Response"):
    check(f"{interface}.json() ({values.get(interface + ' json')!r})", values.get(f"{interface} json") == 1)
    check(f"{interface}.formData() ({values.get(interface + ' formData')!r})", values.get(f"{interface} formData") == "1")
check(f"AbortSignal.abort(reason) ({values.get('AbortSignal.abort')!r})", values.get("AbortSignal.abort") == "why")
check(f"AbortSignal.timeout() ({values.get('AbortSignal.timeout')!r})", values.get("AbortSignal.timeout") is False)
check(f"ReadableStream.from() ({values.get('ReadableStream.from')!r})", values.get("ReadableStream.from") == "s")
check(f"ClipboardItem.getType() ({values.get('clipboard blob size')!r})", values.get("clipboard blob size") == 4)

# The app's interpreter: no page loop and no running script either.
window = view.window
check("View.window.location.href", window.location.href.endswith("caller_realm.html#pushed"))
window.location.hash = "app"
check(f"View.window.location.hash= ({window.location.hash!r})", window.location.hash == "#app")
window.history.replaceState(3, "")
check(f"View.window.history.replaceState() ({window.history.state!r})", window.history.state == 3)
window.postMessage("from app")

view.close()
if failures:
    print(f"test_caller_realm: {len(failures)} failure(s)", file=sys.stderr)
    sys.exit(1)
print("test_caller_realm: ok", file=sys.stderr)
