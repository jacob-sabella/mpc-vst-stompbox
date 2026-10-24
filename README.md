# mpc-vst-stompbox

A whole guitar rig as one insert effect for Akai MPC OS standalone devices (MPC Live/One/X/Key, Force; Gen1 and Gen2),
with a native MPC screen skin. Its DSP is the [guitarix](https://github.com/brummer10/guitarix) project's: guitarix's
own modules and the [GxPlugins.lv2](https://github.com/brummer10/GxPlugins.lv2) pedals, 197 models in all.

```
input -> gate -> comp -> wah -> pedal 1 -> pedal 2 -> pedal 3 -> amp -> tone stack -> power amp -> cabinet   (mono)
      -> mod -> delay -> reverb -> EQ -> output                                                              (stereo)
```

| Slot | Models | | Slot | Models |
|---|---|---|---|---|
| Gate | 2 | | Cabinet | 18 |
| Comp | 4 (compressor, expander, sustainer, swell) | | Mod | 12 (chorus, flanger, phaser, vibe, tremolo, widener...) |
| Wah | 15 (wahs, auto wah, envelope filter) | | Delay | 7 |
| Pedal 1-3 | 68 each (boosts, overdrives, distortions, fuzzes) | | Reverb | 4 |
| Amp | 26 (tube preamps, stacks, combos, bass) | | EQ | 8 |
| Tone stack | 27 | | | |
| Power amp | 6 | | | |

Every slot has a model picker, an on switch and up to 12 knobs that take the picked model's own controls: their names,
ranges and value text follow the model. Each model keeps its settings while you try others, and the project saves them
all. Switching a slot or its model crossfades over 5 ms. The skin has five pages (INPUT, PEDALS, AMP, FX, EQ) with
16 Q-Links each, the first knob of every slot on the first Q-Links.

Model names are generic ("Overdrive 1", "Lead Stack", "4x12 Closed").

## How this repo is built

This repo holds **none of guitarix's or GxPlugins' code**. `upstream.env` pins one commit of each, and the build
fetches them (`tools/fetch-upstream.sh`; git checks each by its hash). What's here is the skeleton that makes them an
MPC plugin:

- `models.json`: what the rack offers: slots, and each model's upstream module, its controls and its display name
- `tools/gen_models.py`: models.json and the fetched trees -> `build/gen/`: one translation unit per upstream module
  (compiled unchanged), the model table, the VST parameter list, the skin layout and the port builder's vst.json
- `src/rack.cpp` (the rack, an [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) engine), `src/host.cpp`
  (hosts a guitarix module or a GxPlugins LV2 plugin), `src/builtins.cpp` (the noise gate and the cabinet convolver,
  which uses guitarix's impulse responses)

A weekly workflow (`.github/workflows/upstream.yml`) opens a pull request when either upstream has new commits, after
running the tests and both builds with them.

## Build and test

Needs Docker (with QEMU for the ARM builds) and an mpc-vst-plugins checkout next to this one (or `MPC_VST=<path>`).

```sh
tests/run.sh               # offline, x86, ASan/UBSan: every model of every slot, a random parameter sweep, state; then the wrapper's host test
vst/build.sh               # build/gen/build/stompbox.so (armv7) and aarch64/stompbox.so, the skin, the plugin-list entry
tools/bench_rig.sh <ip>    # CPU on a device, per rig in tests/rigs/ (nothing installed; MPC keeps running)
```

## CPU

On an MPC Key 37 (Gen1, RK3288), percent of one 128-frame block, MPC running (2026-10-24):

| Rig | mean | p99 | worst block |
|---|---|---|---|
| Default: gate, Lead Stack, tone stack, 4x12 | 11.6 | 12.1 | 21.7 |
| Full: every slot but pedal 3, everyday models | 30.2 | 31.3 | 41.8 |
| Heavy: every slot on with its heaviest model | 73.9 | 75.4 | 87.0 |

The tube preamp models ("12AX7", "6V6"... and their push-pull and feedback versions) are the heavy ones, about
14-20% each; the stacks and combos cost 2-7%, the cabinet 3.4%. Picking a model or switching a slot never waits for the
model to load: a loader thread builds it (and clears a model's delay lines before it fades in again) while the audio
plays on, so turning knobs and Q-Links costs no more than the rig itself.

## License

GPL-3.0-or-later (`LICENSE`), as guitarix and GxPlugins are GPL. guitarix and GxPlugins.lv2 are by Hermann Meyer
(brummer10) and the guitarix contributors; this repo only builds them for MPC.
