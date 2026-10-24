#!/usr/bin/env python3
"""Generate the rack's glue from models.json and the fetched upstream trees (tools/fetch-upstream.sh).

    tools/gen_models.py [--upstream build/upstream] [--out build/gen]    write the build files
    tools/gen_models.py --list-lv2                                      print every GxPlugins pedal's controls

models.json (ours, the only list of what the rack offers):
    "sections": [{"key", "name", "stereo": bool, "slots": [[key, name], ...]}]   chain order; a section may have
                 several slots (two pedal slots share the pedal models)
    "models":   [{"id", "name", "section",
                  one of  "gx": "<path under guitarix/trunk/src>"   a guitarix PluginDef module (faust-generated/x.cc,
                                                                    plugins/generated/x.cc)
                          "lv2": "<GxPlugins bundle>"               a GxPlugins pedal (e.g. "GxSD1.lv2"), built from its
                                                                    own LV2 wrapper
                          "builtin": "gate" | "cab"                 written here (src/builtins.cpp)
                  optional "show": [ids]    controls to offer, in order (default: all)
                          "names": {id: label}  labels for controls (upstream leaves some empty)
                          "hide":  [ids]    LV2 controls not to offer (a bypass port is always hidden)
                          "factory": name   gx: the module's factory function, when it is not plugin()
                          "defines": [...]  gx: macros for that module's own translation unit}]
    "tabs":     [{"name", "rows": [[slot key, ...], ...]}]   the skin's pages: each row is a frame per slot (model
                 popup, on/off, knobs); "rack" is the input and output levels. vst/theme.conf gives the colours.

Writes into --out: models.cpp (the model table), gx_<n>.cpp and lv2_<bundle>.cpp (each upstream module in a
translation unit of its own, or grouped where upstream allows), lv2/<bundle>/ (a flattened copy of each pedal's
plugin/ and dsp/ files, so its own #includes resolve), params.json (the VST parameter list), layout.conf (the skin)
and vst.json (the port builder's config: vst/vst.json plus the generated sources). Upstream names stay in build/; display names are ours.
"""
import io
import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MAX_KNOBS = 12
FAUST_PER_TU = 8


def c(s):
    return json.dumps(s if s is not None else "")


def fl(x):
    s = "%.9g" % float(x or 0)
    return s + ("" if any(ch in s for ch in ".en") else ".0") + "f"


def ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


# ---- GxPlugins TTL (Turtle) ----
def ttl_ports(path):
    """-> list of port dicts from an LV2 TTL file (only what GxPlugins' files use)."""
    t = open(path, errors="ignore").read()
    t = re.sub(r"#[^\n\"]*\n", "\n", t)
    ports = []
    i = 0
    while True:
        i = t.find("lv2:port", i)
        if i < 0:
            break
        j = t.find("[", i)
        # the port list: [ ... ] , [ ... ] ... until the next ";" or "." at bracket depth 0
        k = j
        depth = 0
        start = None
        while k < len(t):
            ch = t[k]
            if ch == '"':
                k = t.find('"', k + 1)
            elif ch == "[":
                if depth == 0:
                    start = k
                depth += 1
            elif ch == "]":
                depth -= 1
                if depth == 0:
                    ports.append(parse_port(t[start + 1:k]))
            elif depth == 0 and ch in ";.":
                break
            k += 1
        i = k
    return ports


def parse_port(b):
    def num(key):
        m = re.search(r"lv2:" + key + r"\s+(-?[0-9.eE+-]+)", b)
        return float(m.group(1)) if m else None

    def s(key):
        m = re.search(r"lv2:" + key + r"\s+\"([^\"]*)\"", b)
        return m.group(1) if m else None
    head = b.split(";")[0]
    p = {
        "index": int(num("index")),
        "symbol": s("symbol"), "name": s("name"),
        "audio": "AudioPort" in head, "control": "ControlPort" in head,
        "input": "InputPort" in head, "output": "OutputPort" in head,
        "default": num("default"), "min": num("minimum"), "max": num("maximum"),
        "toggled": "lv2:toggled" in b, "integer": "lv2:integer" in b or "lv2:enumeration" in b,
        "log": "logarithmic" in b,
        "points": [(float(v), l) for l, v in re.findall(
            r"rdfs:label\s+\"([^\"]*)\"\s*;\s*rdf:value\s+(-?[0-9.eE+-]+)", b)],
    }
    return p


def lv2_bundle(up, bundle):
    d = os.path.join(up, "gxplugins", bundle)
    pdir = os.path.join(d, "plugin")
    cpp = [f for f in sorted(os.listdir(pdir)) if f.endswith(".cpp") and "lv2_descriptor" in open(os.path.join(pdir, f), errors="ignore").read()]
    ttl = [f for f in sorted(os.listdir(pdir)) if f.endswith(".ttl") and not f.startswith("manifest")]
    if len(cpp) != 1 or len(ttl) < 1:
        sys.exit("%s: expected one LV2 wrapper .cpp and a .ttl in plugin/ (found %s, %s)" % (bundle, cpp, ttl))
    return d, cpp[0], os.path.join(pdir, ttl[0])


def stage_lv2(up, bundle, out):
    """flatten plugin/ and dsp/ (and dsp's zita-resampler copy) into out/lv2/<bundle>/"""
    d, cpp, _ = lv2_bundle(up, bundle)
    dst = os.path.join(out, "lv2", bundle)
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    for sub in ("dsp", "plugin"):
        src = os.path.join(d, sub)
        for root, dirs, files in os.walk(src):
            rel = os.path.relpath(root, src)
            parts = rel.split(os.sep)
            if parts[0].startswith("zita-resampler"):     # dsp/zita-resampler-1.1.0/x -> x
                rel = os.sep.join(parts[1:]) or "."
            for f in files:
                if f.endswith((".cc", ".cpp", ".h")):
                    to = os.path.normpath(os.path.join(dst, rel, f))
                    os.makedirs(os.path.dirname(to), exist_ok=True)
                    shutil.copy2(os.path.join(root, f), to)
    return dst, cpp


# ---- guitarix modules ----
def gx_info(up, path, factory="plugin"):
    """-> (qualified factory function, includes its own support header, file)"""
    f = os.path.join(up, "guitarix", "trunk", "src", path)
    s = open(f, errors="ignore").read()
    ns = re.findall(r"^\s*namespace\s+(\w+)\s*\{", s, re.M)
    if not ns:
        sys.exit("%s: no namespace" % path)
    if not re.search(r"PluginDef\s*\*\s*" + factory + r"\s*\(\s*\)", s):
        sys.exit("%s: no %s() factory" % (path, factory))
    qual = "::".join(ns[:2]) if ns[0] == "pluginlib" else ns[0]
    return qual + "::" + factory, '"gx_faust_support.h"' in s, os.path.relpath(f, REPO)


class gen_file(io.StringIO):
    """open(path, "w") that leaves an unchanged file (and its timestamp) alone"""
    written = set()

    def __init__(self, path):
        super().__init__()
        self.path = os.path.abspath(path)

    def __exit__(self, *a):
        gen_file.written.add(self.path)
        new = self.getvalue()
        try:
            same = open(self.path).read() == new
        except OSError:
            same = False
        if not same:
            with open(self.path, "w") as f:
                f.write(new)
        return super().__exit__(*a)


def uses_valve(f):
    return '"valve.h"' in open(f, errors="replace").read()


def count_gx_params(f):
    s = open(f, errors="ignore").read()
    n = 0
    for r in re.finditer(r'register(?:Float|Int|Bool)Var\(\s*"([^"]+)"\s*,[^,]*,\s*"([^"]*)"', s):
        if not any(ch in r.group(2) for ch in "oOM"):
            n += 1
    return n


def lv2_controls(m, ports):
    hide = set(x.lower() for x in m.get("hide", [])) | {"bypass"}
    show = []
    for p in ports:
        if p["control"] and p["input"] and p["symbol"]:
            show.append(p)
    return [p for p in show if p["symbol"].lower() not in hide], [p for p in show if p["symbol"].lower() in hide]


def write_layout(path, tabs, slots, knobs, by_section, theme):
    """layout.conf: one page per tab, a row of frames per tab row (shadow coords: x 0-1280, y 86-714)."""
    slot = {k: (n, si) for k, n, si in slots}
    L = [l.rstrip("\n") for l in open(theme)] + [""]
    top, bottom, gap = 100, 708, 12
    for t in tabs:
        L.append("[tab %s]" % t["name"])
        rows = t["rows"]
        rh = (bottom - top - gap * (len(rows) - 1)) // len(rows)
        firsts, rests = [], []   # Q-Links: every slot's first knob, then the others in order
        for ri, row in enumerate(rows):
            y = top + ri * (rh + gap)
            if row == ["rack"]:
                ctl = [("input", "Input"), ("output", "Output")]
                L.append('frame x=20 y=%d w=1240 h=%d title="RACK"' % (y, rh))
                for j, (k, n) in enumerate(ctl):
                    L.append("knob cx=%d cy=%d r=40 key=%s bw=160" % (520 + 240 * j, y + rh // 2 + 8, k))
                firsts.append("input"); rests.append("output")
                continue
            weights = [2.4 + knobs[slot[k][1]] for k in row]
            x = 20
            avail = 1240 - gap * (len(row) - 1)
            for k, wgt in zip(row, weights):
                name, si = slot[k]
                fw = int(avail * wgt / sum(weights))
                nk = knobs[si]
                L.append('frame x=%d y=%d w=%d h=%d title="%s"' % (x, y, fw, rh, name.upper()))
                cx = x + 105
                n_opt = len(by_section[si])   # a long list: whole columns of 14 (the screen's height), narrow enough to fit
                cols = -(-n_opt // 14)
                fit = ' cols=%d cw=%d' % (cols, min(230, 1256 // cols - 4)) if n_opt > 14 else ' cw=230'
                L.append('popup cx=%d cy=%d w=190 h=44 key=%s_model label="MODEL"%s' % (cx, y + rh // 2 - 24, k, fit))
                L.append('toggle cx=%d cy=%d label="ON" key=%s_on bw=120 ns=0' % (cx, y + rh // 2 + 58, k))
                kx0, kx1 = x + 215, x + fw - 10
                step = (kx1 - kx0) / max(nk, 1)
                r = 30 if step >= 90 else 26
                for j in range(nk):
                    L.append("knob cx=%d cy=%d r=%d key=%s_k%d bw=%d" % (kx0 + step * (j + 0.5), y + rh // 2 - 14, r, k, j + 1,
                                                                    min(int(step) - 4, 150)))
                firsts.append("%s_k1" % k)
                rests += ["%s_k%d" % (k, j + 1) for j in range(1, nk)]
                x += fw + gap
        L.append('qlinks "%s" = %s' % (t["name"], ",".join((firsts + rests)[:16])))
        L.append("")
    with gen_file(path) as f:
        f.write("# generated by tools/gen_models.py from models.json \"tabs\" and vst/theme.conf\n" + "\n".join(L))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", default=os.path.join(REPO, "build", "upstream"))
    ap.add_argument("--out", default=os.path.join(REPO, "build", "gen"))
    ap.add_argument("--models", default=os.path.join(REPO, "models.json"))
    ap.add_argument("--list-lv2", action="store_true")
    a = ap.parse_args()
    up = os.path.abspath(a.upstream)

    if a.list_lv2:
        for b in sorted(os.listdir(os.path.join(up, "gxplugins"))):
            if not b.endswith(".lv2"):
                continue
            _, _, ttl = lv2_bundle(up, b)
            ps = ttl_ports(ttl)
            au = [p for p in ps if p["audio"]]
            ct = ["%s[%s %g..%g=%g%s]" % (p["symbol"], p["name"], p["min"] or 0, p["max"] or 0, p["default"] or 0,
                                         " " + "/".join(l for _, l in p["points"]) if p["points"] else "")
                  for p in ps if p["control"] and p["input"]]
            print("%s (%d audio): %s" % (b, len(au), ", ".join(ct)))
        return

    cfg = json.load(open(a.models))
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)

    sections = cfg["sections"]
    skeys = [s["key"] for s in sections]
    slots = []
    for si, s in enumerate(sections):
        for k, n in s.get("slots", [[s["key"], s["name"]]]):
            slots.append((k, n, si))

    sources = []
    decls = []
    model_rows = []
    port_tables = []
    faust_group = []
    knobs = [0] * len(sections)
    by_section = [[] for _ in sections]
    seen = set()

    valve_group = []   # modules using valve.h, which defines its tube tables as plain globals: all in one unit

    def flush_faust(group=faust_group, name=None):
        if not group:
            return
        n = len([x for x in sources if os.path.basename(x).startswith("gx_")])
        fn = os.path.join(out, name or "gx_%02d.cpp" % n)
        with gen_file(fn) as f:
            f.write("// generated by tools/gen_models.py: guitarix modules, unchanged\n#define _(s) (s)\n"
                    "#include <string.h>\n#include \"gx_faust_support.h\"\n#include \"gx_plugin.h\"\n")
            for mid, qual, path in group:
                f.write('#include "%s"\n' % path)
            for mid, qual, path in group:
                f.write("PluginDef *gxm_%s() { return %s(); }\n" % (mid, qual))
        sources.append(fn)
        group.clear()

    for m in cfg["models"]:
        mid = ident(m["id"])
        if mid in seen:
            sys.exit("models.json: duplicate id %s" % mid)
        seen.add(mid)
        si = skeys.index(m["section"])
        names = m.get("names", {})
        row = {"id": m["id"], "name": m["name"], "section": si, "kind": "MK_BUILTIN", "gx": "nullptr",
               "lv2": "nullptr", "ports": "nullptr", "nports": 0, "ain": "{-1, -1}", "aout": "{-1, -1}",
               "show": "nullptr", "names": "nullptr", "builtin": 0, "arg": None}
        nk = 0
        if "gx" in m:
            qual, own_support, path = gx_info(up, m["gx"], m.get("factory", "plugin"))
            decls.append("PluginDef *gxm_%s();" % mid)
            row.update(kind="MK_GX", gx="gxm_" + mid)
            if own_support or m.get("defines"):   # includes the (unguarded) support header itself: a unit of its own
                fn = os.path.join(out, "gxp_%s.cpp" % mid)
                with gen_file(fn) as f:
                    f.write("// generated by tools/gen_models.py: a guitarix module, unchanged\n#define _(s) (s)\n")
                    f.write("".join("#define %s\n" % d for d in m.get("defines", [])))
                    if not own_support:
                        f.write('#include "gx_faust_support.h"\n#include "gx_plugin.h"\n')
                    f.write("#include <string.h>\n#include \"%s\"\nPluginDef *gxm_%s() { return %s(); }\n" % (path, mid, qual))
                sources.append(fn)
            elif uses_valve(os.path.join(REPO, path)):
                valve_group.append((mid, qual, path))
            else:
                faust_group.append((mid, qual, path))
                if len(faust_group) >= FAUST_PER_TU:
                    flush_faust()
            nk = len(m["show"]) if "show" in m else count_gx_params(os.path.join(REPO, path))
            if "show" in m:
                arr = "show_%s" % mid
                port_tables.append("static const char *const %s[] = { %s, nullptr };" % (arr, ", ".join(c(x) for x in m["show"])))
                row["show"] = arr
            if names:
                arr = "names_%s" % mid
                port_tables.append("static const name_t %s[] = { %s{nullptr, nullptr} };" % (
                    arr, "".join("{%s, %s}, " % (c(k), c(v)) for k, v in names.items())))
                row["names"] = arr
        elif "lv2" in m:
            b = m["lv2"]
            dst, cpp = stage_lv2(up, b, out)
            _, _, ttl = lv2_bundle(up, b)
            ports = ttl_ports(ttl)
            ain = [p["index"] for p in ports if p["audio"] and p["input"]]
            aout = [p["index"] for p in ports if p["audio"] and p["output"]]
            if not ain or len(ain) != len(aout) or len(ain) > 2:
                sys.exit("%s: unexpected audio ports %s -> %s" % (b, ain, aout))
            shown, hidden = lv2_controls(m, ports)
            fn = os.path.join(out, "lv2_%s.cpp" % mid)
            with gen_file(fn) as f:
                f.write("// generated by tools/gen_models.py: GxPlugins' %s LV2 wrapper, unchanged, in a namespace of its own\n" % b)
                for h in ("cmath", "cstdlib", "cstring", "iostream", "cassert", "cstdint", "unistd.h", "pthread.h",
                          "stdlib.h", "stdio.h", "string.h", "math.h", "assert.h", "algorithm"):
                    f.write("#include <%s>\n" % h)
                f.write('#include "lv2.h"\n#define lv2_descriptor lv2d_%s\nnamespace lv2m_%s {\n#include "%s"\n}\n'
                        "#undef lv2_descriptor\nconst LV2_Descriptor *lv2_%s(uint32_t i) { return lv2m_%s::lv2d_%s(i); }\n"
                        % (mid, mid, os.path.relpath(os.path.join(dst, cpp), REPO), mid, mid, mid))
            sources.append(fn)
            decls.append("const LV2_Descriptor *lv2_%s(uint32_t);" % mid)
            rows = []
            for p in shown + hidden:
                labels = "nullptr"
                pts = sorted(p["points"])
                lo, hi = p["min"] or 0.0, p["max"] or 0.0
                if pts and all(abs(v - (lo + i)) < 1e-6 for i, (v, _) in enumerate(pts)) and len(pts) == int(round(hi - lo)) + 1:
                    arr = "lab_%s_%d" % (mid, p["index"])
                    port_tables.append("static const char *const %s[] = { %s };" % (arr, ", ".join(c(l) for _, l in pts)))
                    labels = arr
                name = names.get(p["symbol"], (p["name"] or p["symbol"]).title()) if p in shown else None
                rows.append("{%d, %s, %s, %s, %s, %s, %d, %d, %d, %d, %s}" % (
                    p["index"], c(p["symbol"]), c(name) if name else "nullptr", fl(p["default"]), fl(lo), fl(hi),
                    p["toggled"], p["integer"], p["log"], len(pts) if labels != "nullptr" else 0, labels))
            arr = "ports_%s" % mid
            port_tables.append("static const lv2_port_t %s[] = { %s };" % (arr, ", ".join(rows)))
            row.update(kind="MK_LV2", lv2="lv2_" + mid, ports=arr, nports=len(rows),
                       ain="{%d, %d}" % (ain[0], ain[1] if len(ain) > 1 else -1),
                       aout="{%d, %d}" % (aout[0], aout[1] if len(aout) > 1 else -1))
            nk = len(shown)
        elif "builtin" in m:
            row["builtin"] = {"gate": "BI_GATE", "cab": "BI_CAB"}[m["builtin"]]
            row["arg"] = m.get("ir")
            nk = m.get("knobs", 6)
        else:
            sys.exit("models.json: %s has no gx, lv2 or builtin" % m["id"])
        if nk > MAX_KNOBS:
            sys.exit("models.json: %s offers %d controls, more than %d: give it a \"show\" list" % (m["id"], nk, MAX_KNOBS))
        knobs[si] = max(knobs[si], nk)
        by_section[si].append(m["name"])
        model_rows.append(row)
    flush_faust()
    flush_faust(valve_group, "gx_valve.cpp")

    # models.cpp
    with gen_file(os.path.join(out, "models.cpp")) as f:
        f.write("// generated by tools/gen_models.py from models.json\n#include \"models.h\"\n\n")
        f.write("\n".join(decls) + "\n\n" + "\n".join(port_tables) + "\n\n")
        f.write("const model_t MODELS[] = {\n")
        for r in model_rows:
            f.write("    {%s, %s, %d, %s, %s, %s, %s, %d, %s, %s, %s, %s, %s, %s},\n" % (
                c(r["id"]), c(r["name"]), r["section"], r["kind"], r["gx"], r["lv2"], r["ports"], r["nports"],
                r["ain"], r["aout"], r["show"], r["names"], r["builtin"], c(r["arg"]) if r["arg"] else "nullptr"))
        f.write("};\nconst int NMODELS = %d;\n\n" % len(model_rows))
        f.write("const section_t SECTIONS[] = {\n")
        slot0 = 0
        for si, s in enumerate(sections):
            ns = len(s.get("slots", [[s["key"], s["name"]]]))
            f.write("    {%s, %s, %d, %d, %d, %d},\n" % (c(s["key"]), c(s["name"]), int(s.get("stereo", False)),
                                                       max(knobs[si], 1), slot0, ns))
            slot0 += ns
        f.write("};\nconst int NSECTIONS = %d;\n" % len(sections))
        f.write("const char *const SLOT_KEYS[] = { %s };\n" % ", ".join(c(k) for k, _, _ in slots))
        f.write("const char *const SLOT_NAMES[] = { %s };\n" % ", ".join(c(n) for _, n, _ in slots))
        f.write("const int SLOT_SECTION[] = { %s };\nconst int NSLOTS = %d;\n" % (", ".join(str(si) for _, _, si in slots), len(slots)))
        dfl = cfg.get("defaults", {})
        def dmodel(k, si):
            n = dfl.get(k, {}).get("model")
            return by_section[si].index(n) if n in by_section[si] else 0
        f.write("const int SLOT_DEFAULT_MODEL[] = { %s };\n" % ", ".join(str(dmodel(k, si)) for k, _, si in slots))
        f.write("const int SLOT_DEFAULT_ON[] = { %s };\n" % ", ".join(str(int(dfl.get(k, {}).get("on", 0))) for k, _, _ in slots))
    sources.append(os.path.join(out, "models.cpp"))

    # params.json: global controls, then per slot: model, on, knobs
    P = [{"key": "input", "name": "Input", "min": -24, "max": 24, "default": 0, "unit": "dB"},
         {"key": "output", "name": "Output", "min": -36, "max": 12, "default": 0, "unit": "dB"}]
    secs = [{"label": "Rack", "keys": ["input", "output"]}]
    defaults = cfg.get("defaults", {})
    for k, n, si in slots:
        opts = by_section[si]
        dm = defaults.get(k, {}).get("model")
        P.append({"key": k + "_model", "name": n, "options": opts, "default": opts.index(dm) if dm in opts else 0,
                  "qlink_ticks": 6, "refresh_display": True, "nudge_pct": 10})
        P.append({"key": k + "_on", "name": n + " On", "options": ["Off", "On"], "default": int(defaults.get(k, {}).get("on", 0)),
                  "qlink_ticks": 6})
        keys = [k + "_model", k + "_on"]
        for j in range(knobs[si]):
            P.append({"key": "%s_k%d" % (k, j + 1), "name": "%s %d" % (n, j + 1), "min": 0, "max": 1, "default": 0.5,
                      "dynamic_name": True, "dynamic_display": True})
            keys.append("%s_k%d" % (k, j + 1))
        secs.append({"label": n, "keys": keys})
    with gen_file(os.path.join(out, "params.json")) as f:
        json.dump({"name": cfg.get("name", "Stompbox"), "params": P, "sections": secs}, f, indent=1)

    # vst.json for the port builder
    base = json.load(open(os.path.join(REPO, "vst", "vst.json")))
    rel = lambda p: os.path.relpath(p, REPO)
    b = base.setdefault("build", {})
    b["root"] = os.path.relpath(REPO, out)
    g = os.path.relpath(os.path.join(up, "guitarix", "trunk", "src"), REPO)
    # the oversampling resampler that several faust modules (od, dist, sustainer...) run inside. Its source includes
    # guitarix's whole engine.h for one header (and the wrapper's engine.h comes first on our path), so a glue unit
    # supplies that header and what engine.h would have pulled in.
    glue = os.path.join(out, "gx_resampler_glue.cpp")
    with gen_file(glue) as f:
        f.write("#include <cassert>\n#include <cstring>\n#include <cmath>\n#include \"gx_resampler.h\"\n"
                "#include \"%s/gx_head/engine/gx_resampler.cpp\"\n" % g)
    support = [rel(glue), g + "/zita-resampler-1.1.0/resampler.cc", g + "/zita-resampler-1.1.0/resampler-table.cc"]
    b["sources"] = b.get("sources", []) + support + [rel(s) for s in sources]
    b["cflags"] = ["-I" + os.path.relpath(os.path.join(out, "build"), REPO)] + b.get("cflags", []) + ["-I.", "-I" + g + "/headers", "-I" + g + "/gx_head/engine/tabels", "-I" + g + "/gx_head/engine",
                                         "-I" + g + "/plugins", "-I" + g + "/faust-generated",
                                         "-I" + g + "/zita-resampler-1.1.0"]
    base["params"] = "params.json"
    write_layout(os.path.join(out, "layout.conf"), cfg["tabs"], slots, knobs, by_section, os.path.join(REPO, "vst", "theme.conf"))
    base["layout"] = "layout.conf"
    for k in ("tile", "presets"):
        if k in base:
            base[k] = os.path.relpath(os.path.join(REPO, "vst", base[k]), out)
    with gen_file(os.path.join(out, "vst.json")) as f:
        json.dump(base, f, indent=1)
    for f in os.listdir(out):   # units of models no longer listed
        if f.endswith(".cpp") and os.path.join(out, f) not in gen_file.written:
            os.remove(os.path.join(out, f))
    print("%d models in %d sections (%d slots), %d sources, knobs per section: %s" % (
        len(model_rows), len(sections), len(slots), len(sources), ", ".join("%s %d" % (s["key"], knobs[i]) for i, s in enumerate(sections))))


if __name__ == "__main__":
    main()
