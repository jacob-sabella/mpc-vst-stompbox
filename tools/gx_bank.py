#!/usr/bin/env python3
"""guitarix presets -> Stompbox states (src/rack.cpp's "state" text).

    tools/gx_bank.py <bank.gx> [--catalog build/gen/catalog.json]    print each preset's state and what didn't convert

tools/gen_models.py calls convert() for guitarix's factory banks (trunk/factorysettings) to build the factory presets
in MPC's PRESET menu, and writes the catalog it needs (build/gen/catalog.json: every model's guitarix unit id and
control ids, what each section's slots offer, guitarix's impulse response tables).

A bank (.gx) is JSON: ["gx_head_file_version", [...], name, {"engine": {...}}, name, {...}, ...]. In "engine", every
control is "<unit>.<control>"; a rack unit has "<unit>.position" and is in the chain when "<unit>.on_off" is set. Mono
units with "<unit>.pp" "pre" run before the amp, "post" after it, each side in position order; units without .pp are
the stereo rack. Around them guitarix runs parts of its own amp stack: a noise gate and a noise shaper at the start,
a soft clip after the amp, a bass booster and the presence convolver at the end of the mono chain, the fizz remover
at the end of the stereo chain.

Stompbox's chain: gate, comp, wah, pedals, amp, tone stack, power amp, cabinet, post FX, mod, delay, reverb, EQ. The
pedal and post FX slots take any model, the four stereo slots any mod, delay, reverb or EQ model. So:
  - the pre-amp units -> pedals, in order
  - the post-amp units -> the tone stack, power amp and cabinet slots while they come in that order, then post FX
  - the stereo rack: units that keep both sides equal (a cabinet, an impulse response, an EQ), up to the first that
    doesn't, run in mono anyway -> post FX; the rest -> the four stereo slots in order
What can't be placed (a fifth stereo unit, a unit with no model here) is reported, and left out.
"""
import json
import math
import os
import re
import sys

STEREO_FIXED = ["mod", "delay", "reverb", "eq"]
WIDENS = ("mod", "delay", "reverb")   # sections whose models make the two sides differ
IGNORED = {"oscilloscope", "vu", "vu_st", "recorder", "st_recorder", "metro", "tuner"}   # meters and tools, no sound
RENAME = {"brett_jcm": "Brett Stack", "crunch_vintage_vox": "Crunch Vintage Chime"}   # neutral names on the screen
CATEGORY = {"Distortion": "Dist", "Effect": "FX", "Acoustic": "Acoustic", "Crunch": "Crunch", "Lead": "Lead",
            "Clean": "Clean", "Bass": "Bass"}


def num(v):
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, (int, float)):
        return "%.9g" % v
    return str(v)


def on(e, k):
    v = e.get(k, 0)
    return v not in (0, "0", False, 0.0, None)


def preset_name(name):
    if name in RENAME:
        return RENAME[name]
    m = re.match(r"^(\w+) - (.*)$", name)
    if m and m.group(1) in CATEGORY:
        name = "%s: %s" % (CATEGORY[m.group(1)], m.group(2))
    name = name.replace("_", " ")
    return name[:1].upper() + name[1:]


class Catalog:
    def __init__(self, cat):
        self.c = cat
        self.models = {m["id"]: m for m in cat["models"]}
        self.by_unit = {}
        for m in cat["models"]:
            for u in m.get("units") or [m.get("unit")]:
                if u and m["kind"] != "MK_BUILTIN":
                    self.by_unit.setdefault(u, m)
        self.section = {s["key"]: s for s in cat["sections"]}

    def offers(self, slot_section, model):
        return model["id"] in self.c["offers"][slot_section]

    def builtin(self, kind, ir=None):
        for m in self.c["models"]:
            if m.get("builtin") == kind and (ir is None or m.get("ir") == ir):
                return m
        return None


def ir_id(conv):
    """a convolver's settings -> the id of its file among the shipped ones (None: no file)"""
    f = conv.get("jconv.IRFile", "")
    if not f:
        return None
    d = conv.get("jconv.IRDir", "")
    sub = {"%S": "", "%S/amps": "amps/", "%S/bands": "bands/"}.get(d)
    return None if sub is None else sub + f


class Unit:
    """one model to place: the model, and its controls as a state names them"""
    def __init__(self, src, model, values):
        self.src, self.model, self.values = src, model, values


def unit_for(cat, e, u, notes):
    """guitarix unit u -> Unit (None: not here, noted)"""
    if u in IGNORED:
        return None
    if u == "amp.tonestack":
        m = cat.by_unit.get(e.get("amp.tonestack.select"))
        if not m:
            notes.append("tone stack %r: no model" % e.get("amp.tonestack.select"))
            return None
        return Unit(u, m, {p: e[p] for p in m["params"] if p in e})
    if u in ("cab", "cab_st", "pre", "pre_st"):
        kind = "cab" if u.startswith("cab") else "pre"
        sel = e.get(u + ".select")
        m = cat.builtin(kind, cat.c[kind].get(sel))
        if not m:
            notes.append("%s %r: no model" % (u, sel))
            return None
        return Unit(u, m, {k: e[u + "." + g] for k, g in (("level", "Level"), ("bass", "bass"), ("treble", "treble"))
                           if u + "." + g in e})
    if u in ("jconv", "jconv_mono"):
        conv = e.get(u + ".convolver") or {}
        i = ir_id(conv)
        if i not in cat.c["irs"]:
            notes.append("%s: impulse response %r not shipped" % (u, i))
            return None
        m = cat.builtin("ir")
        v = {"file": i, "wet_dry": e.get(u + ".wet_dry", 100), "gain": e.get(u + ".gain", 0),
             "ir_gain": conv.get("jconv.Gain", 1) if on(conv, "jconv.GainCor") else 1,
             "offset": conv.get("jconv.Offset", 0), "length": conv.get("jconv.Length", 0),
             "delay": conv.get("jconv.Delay", 0)}
        return Unit(u, m, v)
    if u == "con":
        m = cat.builtin("presence")
        return Unit(u, m, {"level": e.get("con.Level", 1)})
    m = cat.by_unit.get(u)
    if not m:
        notes.append("%s: no model" % u)
        return None
    return Unit(u, m, {p: e[p] for p in m["params"] if p in e})


def convert(name, e, cat, defaults):
    """-> (state text, notes) for one preset's engine settings"""
    notes = []
    units = {k[:-len(".position")] for k in e if k.endswith(".position")}
    live = [u for u in units if on(e, u + ".on_off") or (u == "amp.tonestack" and u + ".on_off" not in e)]
    def weight(u):
        pp = e.get(u + ".pp", "post")
        return float(e[u + ".position"]) - (1000 if pp in ("pre", 1, "1") else 0)
    mono = sorted([u for u in live if u + ".pp" in e], key=weight)
    pre = [u for u in mono if e[u + ".pp"] in ("pre", 1, "1")]
    post = [u for u in mono if u not in pre]
    stereo = sorted([u for u in live if u + ".pp" not in e], key=lambda u: float(e[u + ".position"]))

    fixed = {k: dict(v) for k, v in defaults.items()}   # slot -> {"model": id, "on": 0|1}
    for v in fixed.values():
        v["on"] = 0
    vals = {}                                          # slot -> {control: value}
    def put(slot, unit):
        fixed[slot] = {"model": unit.model["id"], "on": 1}
        vals[slot] = unit.values

    if on(e, "noise_gate.on_off"):   # guitarix: a block's RMS over a tenth of the threshold opens it
        thr = float(e.get("noise_gate.threshold", 0.017))
        gate = cat.builtin("gate")
        put("gate", Unit("noise_gate", gate, {"threshold": max(-80, min(-10, 20 * math.log10(max(thr * 0.1, 1e-6))))}))

    pedals = [unit_for(cat, e, u, notes) for u in (["shaper"] if on(e, "shaper.on_off") else []) + pre]
    pedals = [p for p in pedals if p]

    tube = e.get("tube.select")
    amp = cat.by_unit.get(tube)
    if amp:
        put("amp", Unit("amp", amp, {p: e[p] for p in amp["params"] if p in e}))
    elif tube != "noamp":
        notes.append("amp %r: no model" % tube)

    chain = (["amp.clip"] if on(e, "amp.clip.on_off") else []) + post
    tail = (["amp.bass_boost"] if on(e, "amp.bass_boost.on_off") else []) + (["con"] if on(e, "con.on_off") else [])
    after = [p for p in (unit_for(cat, e, u, notes) for u in chain) if p]
    # the fixed tone stack, power amp and cabinet slots take the post-amp units while they come in that order
    for slot in ("tone", "power", "cab"):
        if after and after[0].model["section"] == slot:
            put(slot, after.pop(0))
    rest = [p for p in (unit_for(cat, e, u, notes) for u in stereo) if p]
    while rest and rest[0].model["section"] not in WIDENS:
        after.append(rest.pop(0))
    after += [p for p in (unit_for(cat, e, u, notes) for u in tail) if p]
    if on(e, "antyfizz.on_off"):
        f = unit_for(cat, e, "antyfizz", notes)
        if f:
            rest.append(f)
    for slot in STEREO_FIXED:
        while rest and not cat.offers(slot, rest[0].model):
            notes.append("%s after a stereo effect: left out" % rest.pop(0).src)
        if rest:
            put(slot, rest.pop(0))
    for r in rest:
        notes.append("%s: no stereo slot left" % r.src)

    out = ["v=1", "input=0", "output=%s" % num(max(-36, min(12, float(e.get("amp.out_master", 0)) +
                                                          float(e.get("amp.out_amp", 0)))))]
    out.append("pedal_count=%d" % len(pedals))
    out.append("post_count=%d" % len(after))
    lines = []
    def slot_lines(key, model, onv, values):
        lines.append("%s_model=%s" % (key, model))
        lines.append("%s_on=%d" % (key, onv))
        for k, v in values.items():
            lines.append("%s.%s.%s=%s" % (key, model, k, num(v)))
    for k, v in fixed.items():
        slot_lines(k, v["model"], v["on"], vals.get(k, {}))
    for i, p in enumerate(pedals):
        slot_lines("pedal%d" % (i + 1), p.model["id"], 1, p.values)
    for i, p in enumerate(after):
        slot_lines("post%d" % (i + 1), p.model["id"], 1, p.values)
    return "\n".join(out + lines) + "\n", notes


def read_bank(path):
    d = json.load(open(path))
    if not d or d[0] != "gx_head_file_version":
        raise ValueError("%s: not a guitarix bank" % path)
    return [(d[i], d[i + 1].get("engine", {})) for i in range(2, len(d) - 1, 2)]


def fixed_defaults(models_json, cat):
    """the fixed slots' default models (models.json "defaults"), by model id"""
    names = {m["name"]: m["id"] for m in cat["models"]}
    out = {}
    for s in cat["sections"]:
        if s.get("paged"):
            continue
        d = models_json.get("defaults", {}).get(s["key"], {})
        out[s["key"]] = {"model": names.get(d.get("model")) or cat["offers"][s["key"]][0], "on": int(d.get("on", 0))}
    return out


def main():
    import argparse
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("bank")
    ap.add_argument("--catalog", default=os.path.join(here, "build", "gen", "catalog.json"))
    a = ap.parse_args()
    cat = json.load(open(a.catalog))
    c = Catalog(cat)
    dfl = fixed_defaults(json.load(open(os.path.join(here, "models.json"))), cat)
    for name, e in read_bank(a.bank):
        state, notes = convert(name, e, c, dfl)
        print("# %s%s\n%s" % (preset_name(name), "".join("\n#   " + n for n in notes), state))


if __name__ == "__main__":
    main()
