# Hookshot for the AMD Radeon RX 5700 XT

**Target hardware: AMD Radeon RX 5700 XT (RDNA1, 8 GB).** Tested with a Ryzen 5 5500, 32 GB RAM, Windows 11.
Other GPUs are not tested here.

An experimental fork of [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) tuned for a PC with an
**AMD Radeon RX 5700 XT** (RDNA1) and a **Ryzen 5 5500**. Everything else about Hookshot is unchanged: you still need
your own copy of the game, and nothing from the game is in this repository.

> Status: work in progress. Each change is tested in Kokiri Forest before the next one.

## What we found on this PC

Measured in Kokiri Forest with the renderer's own tools (`RX_VK_GPU_PROFILE`, `HOS_SAMPLE_ALL` + `scripts/crit.py`):

- The GPU is **not** the limit: about 17 ms of GPU work a frame, and the replay never waits for it.
- The frame is held by the **replay on the game's presentation thread** (~12 ms a frame of CPU on a Ryzen 5 5500):
  descriptor building, uniform reads, texture lookups. The main thread waits for it most of the time.
- The RX 5700 XT has no `VK_EXT_mesh_shader`, so task/mesh draws are left out (missing geometry), and no f16
  cooperative matrices (the upscaling networks run in software or are stood in for).

## Changes in this branch

Round 1 — less CPU on the replay thread (no change to shader translation, so existing shader caches stay valid):

| Change | Where | Cost before |
|---|---|---|
| Per-phase replay timers off unless `RX_VK_PROF_SCOPES=1` | `gpu/vk/vk_internal.h` | ~6% of the replay thread |
| `RX_VK_TAAU_KEYS` read once instead of on every dispatch | `gpu/vk/vk_draw.cpp` | ~2% |
| Page index over pending uniform updates: `ub_read` / `ub_overlay` skip the scan when no update touches the pages | `gpu/vk/vk_replay.cpp` | ~11% |
| One hash lookup per texture handle instead of two (hit statistics only with `RX_VK_LOG_HITS`) | `gpu/vk/vk_draw.cpp` | part of `texture_view_of` |

Driver faults (AMD amdvlk64.dll on RDNA1):

| Change | Where | Why |
|---|---|---|
| Programs whose pipeline build faults inside the driver are written to `hookshot_driver_faults.txt` beside the game. Next time they are built **without the driver's optimizer** at their draw (their draws come back), never in the start's precompile; if that faults too, they are not built again | `gpu/vk/vk_draw.cpp` | On an RX 5700 XT the driver faults on the same two programs (`5fa56485a6b07619`, `6eaa646d07ea4e43`) at every start. The fault is caught, but it can leave the driver's locks held: the start's precompile froze at 1128 of 1130 |
| The precompile's compute pipelines are built through `guarded_compute` like the others | `gpu/vk/vk_draw.cpp` | A fault there took the whole game down |

Result of round 1 in Kokiri (same spot): the replay thread went from ~74% to ~64% busy; frame rate about the same
(~32 fps). The next limit is the GPU's idle time between passes (17.5 ms of GPU time a frame for 5.7 ms of measured
passes: full barriers and ~1100 small mirror copies a frame) — round 2.

Round 2 — the GPU's time between passes:

`RX_VK_GPU_PROFILE=1` now times the gaps between passes too, named after what was recorded there (barriers, write-backs,
clears, copies). In Kokiri: **20.2 ms of GPU time a frame, 12.1 ms of it between passes, ~10.5 ms of that writing GPU results
back to guest memory** (67 MB a frame over the Ryzen 5 5500's PCIe 3.0). Almost all of it is one 15.2 MB buffer that four
lighting compute programs write, each in its own submission: copied back whole four times a frame.

**Write-back by changed chunks (on by default, `gpu/vk/vk_wbdiff.cpp`).** Before copying a written range back, a compute
shader (built with Sirit, no GLSL toolchain needed) goes over it in 4 KB chunks: it fingerprints each chunk of the VRAM
mirror (two 32-bit sums of a per-word mix that is a bijection of the word for its place, so one changed word always changes
the fingerprint), compares with the fingerprint of what it last wrote back, and copies only the chunks that changed.
Reading VRAM is nearly free; only changed chunks cross PCIe. Guest memory ends up as the whole copy left it, except where the
GPU changed nothing since the last write-back (there a CPU write made in between is no longer overwritten with older bytes).
Ranges under 64 KB (`RX_VK_WB_DIFF_MIN`) are still copied whole. `RX_VK_WB_DIFF=0`: whole copies as before.

| Kokiri, RX 5700 XT + Ryzen 5 5500 | Before | After |
|---|---|---|
| GPU time a frame | 20.2 ms | **14.0 ms** |
| Write-back | ~10.5 ms | **0.8 ms** |
| GPU time between passes | 12.1 ms | **2.8 ms** |

Lighting unchanged. ~97% of what is written back goes by changed chunks.

| Option (in `hookshot_env.txt`) | Effect | Status |
|---|---|---|
| `RX_VK_WB_SKIP=<key prefix,...>` | Those programs' storage writes stay in VRAM (not copied back at all) | Experiment; superseded by the changed-chunk write-back |

Round 3 — the present no longer blocks the game's submits (`gpu/vk/vk_replay.cpp`, `present_image`):

amdvlk64.dll on Windows waits on the CPU, inside `vkQueuePresentKHR`, for the frame's semaphore. The present held the queue's
lock (`present_mu`) until the GPU had finished the whole frame, and the game's main thread, submitting its next work through
the same lock, waited there (~19% of its time; 78% of the moments no game thread ran). Now, **with frame generation off**,
the display thread submits under the lock, waits for the frame's fence with no lock held, and presents the finished image
(no semaphore) under the lock again: same queue, same order. `RX_VK_PRESENT_WAIT=0`: as before.

- The main thread's wait on the lock: 78% → 3% of the idle moments; frame pacing tighter (most frames 22–30 ms).
- **The one-frame flicker without frame generation (upstream issue #346) is gone** here, at about the same smoothness as
  with frame generation, so frame generation (and its input delay) is no longer needed to hide it.
- With frame generation on, the old path is kept: waiting there broke its pacing of the made frames (flicker).
- Tried and dropped: presenting on a second queue of the graphics family. amdvlk on Windows flickered with it, no gain.

Round 4 — the idle reserved core (`gpu/vk/vk_replay.cpp`, `pin_presentation_thread`):

On 8+ logical processors the host keeps its last physical core free of the game's threads for the replay thread, but that
thread only works with `RX_VK_ASYNC=1` (off by default). The replay runs on the game's own "Presentation Thread", which was
kept off that core with the others: on a 6-core Ryzen 5 5500 the core sat idle while the frame's critical path shared the
other five with every game thread. Now the Presentation Thread is moved onto that core at its first submit.
`RX_VK_PIN_PRESENT=0`: as before.

Measured live in one session (the thread's affinity switched from outside, alternating, 4 rounds of ~25 s each, fullscreen,
Kokiri): **43.6 fps pinned against 41.2 not pinned** (the last two, steadiest rounds: 44.9 / 44.8 against 43.0 / 43.2).
Frame rate in this game swings by ±5 fps from one 20 s window to the next, so single before/after runs are not enough.

Round 5 — upstream merged (release `no-visual-studio`), and its readback wait replaced:

- Upstream's 116 commits are in (fewer flashes #296, 30 mesh shaders #351, ~15% CPU from SkippySteve's replay PRs, controls,
  text speed, quality presets, Alt+Enter). Where upstream and this branch fixed the same thing (uniform reads, the texture
  lookup), upstream's version is kept.
- Upstream's #296 makes the CPU wait for the GPU before it reads back GPU results. On this PC ~4% of readbacks waited ~18.6 ms
  each: the game's threads spun in their sleeps (worker threads 15–25% → 50–60%) and fps fell from ~43 to ~30. Here it is
  **off by default** (`RX_VK_READBACK_WAIT=1`: on), and what it protected — OoT's exposure meter, which the GPU adds into and
  the CPU resets to 0 — is kept right by the **readback-page refresh from YunaraDebugs' PR #430**: each submission first copies
  the pages the CPU reads back from guest memory into the mirror (`RX_VK_NO_RB_REFRESH=1`: off). Fps back to ~40 fullscreen,
  no white flashes or exposure steps.
- Frame generation now **paces** its made frames even with the menu's Frame pacing off (they used to go out <7 ms behind the
  game's, the same picture rate as without it). The made frames themselves still show optical-flow artifacts when the camera
  turns (flicker): for smoother motion on this card, an external frame generator (e.g. Lossless Scaling's LSFG) with the game's
  own frame generation off looks better.

Tip: play **fullscreen**. Windowed, every frame goes through the desktop compositor: ~34 fps there against ~40+ fullscreen
on this PC.

## Building only the renderer

Only `hookshot_vk.dll` changes. With a game folder already built by Hookshot Builder, build this branch's renderer
with `RX_VK_ONLY=1` (the tools folder of the HookshotBuilder zip gives `EMSDK`, `RX_CPM`, `WGPU_NATIVE`):

```
set RX_VK_ONLY=1
set RX_RELEASE=1
set OOT_WORK=<a work folder>
python -I nx2/build.py --standalone --jobs 6
```

Then copy `<work>/sa/build/hookshot_vk.dll` over the one in the game folder (keep the original).

---

## Português

Versão experimental do Hookshot ajustada para **RX 5700 XT** + **Ryzen 5 5500**. Neste PC o gargalo não é a placa:
é a thread que prepara os comandos de desenho na CPU. A rodada 1 corta desperdícios dessa thread (cronômetros
internos, leituras de configuração repetidas, busca linear nas atualizações de constantes, busca dupla de textura).
Só o `hookshot_vk.dll` muda; o resto do jogo é o mesmo do Hookshot original. Você precisa da sua própria cópia do jogo.
