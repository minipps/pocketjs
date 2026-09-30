# Wii Homebrew library plan

## Goal and boundaries

Build PocketJS as a **static library that a devkitPPC Wii Homebrew application links**. The application owns `main`, controller polling, its GX frame, and video presentation. PocketJS boots one verified `.pocket` guest, advances it at a fixed simulation rate, and draws its UI into a caller supplied rectangle during the application's GX pass.

The first deliverable contains a public C header, the Wii archive, a target specific `.pocket` build command, and a small linking example that produces `boot.dol`. It supports one live PocketJS guest. The guest uses the existing `@pocketjs/framework/*` APIs and imports Solid primitives directly from `solid-js`.

**Wii is a 32 bit, big endian target. PocketJS package, pak, style, font, and packed batch formats are little endian.** Keep their bytes unchanged and decode them explicitly. The `DrawList` pointer exposes native `uint32_t` values; C reads those values directly and extracts fields with shifts. Do not reinterpret its memory as a little endian byte stream.

The first profile is `wii-dev`, outside the production target registry. It uses a fixed 480×272 logical surface with density 1; its physical viewport and presentation mode must be settled against a tested GX video setup before packaging is finalized. Advertise only capabilities implemented by the library and example. No Wii Remote IR pointer, audio, networking, package update service, auxiliary screen, native text, compositor surface, or standalone PocketJS launcher is required for the first deliverable.

## Library contract to implement

| Call | Owner and effect |
| --- | --- |
| `pocket_wii_boot(package_bytes, package_length)` | Caller keeps the `.pocket` bytes alive until shutdown; library verifies target and HostOps ABI, feeds the pak, and starts QuickJS. |
| `pocket_wii_tick(buttons, analog)` | Caller passes the PocketJS button mask and packed 0–255 analog axes once per **1/60 second simulation step**; library calls the guest frame, drains pending jobs, and ticks the core. Center is `0x8080`. |
| `pocket_wii_draw(x, y, width, height)` | Library builds one DrawList and emits GX commands inside the caller's current GX frame. The caller rebinds any GX state it needs afterward and owns copy, swap, and vblank. |
| `pocket_wii_last_error()` / `pocket_wii_shutdown()` | Library reports boot or frame failures and releases guest, core, and GX resources. |

Final names and return types belong in `hosts/wii/include/pocket_wii.h`. Keep the public API synchronous and single instance; add multiple instances only when a Wii application needs them.

## Work rules for subagents

- Claim one task ID at a time. Complete its stated result and hand the coordinator the changed files, command or device evidence, and any blocker. The coordinator updates this checklist so agents do not edit it concurrently.
- Keep changes inside the task's named area unless a dependency exposes a required shared fix. Tell the coordinator before changing another agent's area.
- A task marked **Gate** must pass before dependent implementation begins. If it fails, record the failing command, linker diagnostics, and the smallest viable adjustment in this file.
- Preserve the existing `.pocket`, HostOps, DrawList, and pak formats. Do not fork the PocketJS guest runtime for Wii.
- W02 and W04 can run concurrently after W01. After W07, W12 and W13a can run concurrently. Keep one writer at a time in any shared C renderer, bridge, or build script file.

## Atomic checklist

### A. Toolchain and format gate

- [x] **W01 — Confirm devkitPPC inputs.** Record the compiler, `wii_rules`, libogc headers and archives, `elf2dol`, and versions available to the build environment. **Done when:** a plain libogc example links to `boot.dol`. **Gate.**
- [x] **W02 — Define the Rust Wii target.** Add a pinned custom PowerPC target and standalone Cargo configuration for `core`, `alloc`, and `compiler_builtins`, using devkitPPC compatible EABI and hard float settings. **Done when:** `pocketjs-core` compiles to a Wii object or archive without host OS dependencies. Depends on W01. **Gate.**
- [x] **W03 — Prove Rust/C linkage and allocation.** Link a C probe against the Rust archive and libogc; exercise a Rust call that allocates and frees memory. Check pointer alignment, panic symbols, float arguments, and unresolved runtime symbols. **Done when:** the probe links and runs in Dolphin or on hardware. Depends on W02. **Gate.**
- [x] **W04 — Compile the pinned QuickJS source for Wii.** Use the existing source pin from `tools/3ds.ts`, compile every QuickJS translation unit and caller with matching 32 bit `JSValue` settings, and link it with devkitPPC. **Done when:** a minimal C QuickJS probe links without ABI warnings or unresolved symbols. Depends on W01. **Gate.**
- [x] **W05 — Execute a QuickJS smoke in Wii code.** Evaluate JavaScript using numbers, objects, typed arrays, promises, and an exception; drain pending jobs and inspect the result. **Done when:** behavior matches the same source on a desktop QuickJS build in Dolphin or on hardware. Depends on W04. **Gate.**
- [x] **W06a — Check package and pak byte order.** Decode a known `.pocket` and pak through the Wii Rust build; fix any read that treats little endian bytes as native words. **Done when:** target, ABI, entry names, and entry lengths match desktop decoding. Depends on W03. **Gate.**
- [x] **W06b — Check styles and font byte order.** Load known style and font atlas blobs through the Wii Rust build; fix any native word reads. **Done when:** style values, glyph metrics, and coverage bytes match desktop decoding. Depends on W03. **Gate.**
- [x] **W06c — Check texture and DrawList values.** Decode known texture bytes and numeric DrawList words on Wii. **Done when:** colors, UVs, packed coordinates, and text bytes match desktop values without swapping the `uint32_t` DrawList pointer. Depends on W03. **Gate.**
- [x] **W06d — Check QuickJS buffer byte order.** Check typed array and `DataView` bytes crossing into C. Leave optional `setPropBatch` absent initially: `framework/src/anim.ts` writes native endian `Float64Array` bytes and already has a `setProp` fallback. **Done when:** the guest can pass its required byte buffers without a native endian mismatch. Depends on W05. **Gate.**
- [x] **W07 — Record the gate result.** Add the working compiler and Rust target commands, archive names, QuickJS flags, and any remaining target limitation under “Gate result” below. **Done when:** another developer can repeat W03, W05, and W06a–W06d with the recorded commands. Depends on W03, W05, W06a–W06d. **Gate.**

### B. Guest build and packaging

- [x] **W08 — Define `wii-dev` admission.** Add an out of registry target profile in `tools/wii-profile.ts`, with the tested viewport and the first release capabilities only. **Done when:** a representative `pocket.json` resolves and a manifest requiring an absent capability is rejected. Depends on W07.
- [x] **W09 — Compile a Wii guest.** Make `tools/wii.ts` resolve the profile and invoke the existing `tools/build.ts` pipeline with a verified plan. **Done when:** it emits Wii flavored JS, pak, and plan for a sample app without modifying stock target builds. Depends on W08.
- [x] **W10 — Package the guest.** Use `makeVariant` and `encodePocketPackage` to make one target thinned `.pocket` containing identity, plan, NUL terminated JS, and pak. **Done when:** the package's target and ABI match the generated plan and `pocket_package_open` admits it. Depends on W09 and W13a.
- [x] **W11 — Build the library archive.** Make `tools/wii.ts` and `hosts/wii/Makefile` build the Rust bridge, QuickJS, Wii C bridge, and GX backend into linkable archive output alongside the public header. **Done when:** an external devkitPPC Makefile can link those outputs without copying repository sources. Depends on W16 and W21.

### C. Runtime library

- [x] **W12 — Specify the C header.** Add `hosts/wii/include/pocket_wii.h` with the four operations above, input constants or a link to generated constants, borrowed buffer lifetime, error return behavior, one guest limit, and GX state ownership. **Done when:** a plain C translation unit can include it without PocketJS internal headers. Depends on W07.
- [x] **W13a — Expose package admission.** Build a Wii Rust `staticlib` from `pocketjs-core` and expose `pocket_package_open` using the verifier in `hosts/3ds/core/src/lib.rs`. **Done when:** C admits a small Wii package fixture made with the existing package encoder and rejects an invalid target or ABI. Depends on W07.
- [x] **W13b — Expose core UI calls.** Expose the HostOps, pak feeder, tick, DrawList, texture, and font accessors needed by the Wii C code. Reuse the 3DS C ABI implementation where it applies directly. **Done when:** C can feed a pak, mutate nodes, tick, and read the matching DrawList and resources. Depends on W13a.
- [x] **W14 — Bind the existing HostOps in QuickJS.** Implement `globalThis.ui`, `__host`, `__hostAbi`, `__viewport`, image names, and sprite names using the 3DS bridge as the op reference. Do not expose unimplemented optional operations. **Done when:** a compiled guest creates and updates native nodes. Depends on W12 and W13b.
- [x] **W15a — Implement guest boot.** Wire `pocket_wii_boot` to package admission, core initialization, pak feed, and QuickJS evaluation. **Done when:** valid guest boot succeeds and a wrong target or ABI returns a useful error. Depends on W10 and W14.
- [x] **W15b — Implement guest shutdown.** Wire `pocket_wii_shutdown` to release QuickJS and core resources in a safe order; W21 adds GX resource cleanup. **Done when:** shutdown after partial or successful boot is safe and a second boot succeeds. Depends on W15a.
- [x] **W16 — Implement the fixed step.** Wire `pocket_wii_tick` to `frame(buttons, analog)`, pending job drain, `ui_tick`, and a retained last error. **Done when:** a frame updates visible node state and a thrown guest error reaches `pocket_wii_last_error()`. Depends on W15b.

### D. GX rendering

- [x] **W17 — Decode the DrawList.** Add one bounds checked walk of supported op lengths and one explicit failure for unknown or unsupported ops. **Done when:** a malformed list cannot read beyond its length and the walk recognizes all ops emitted by the `wii-dev` profile. Depends on W13b.
- [x] **W18a — Draw solid geometry.** Translate `RECT` and `TRI` into GX draws with logical to destination rectangle mapping and correct ABGR colors. **Done when:** solid boxes and triangles have the expected position, color, and painter order. Depends on W17.
- [x] **W18b — Draw gradients.** Translate `GRAD_RECT` using the contract's direction and endpoint colors. **Done when:** all four gradient directions match reference output. Depends on W18a.
- [x] **W18c — Apply DrawList clips.** Translate `SCISSOR` and `SCISSOR_POP` into GX clip state, preserving the enclosing clip when a nested clip ends. **Done when:** nested and empty clips match reference output. Depends on W18a.
- [x] **W19a — Decode texture sources.** Read the current pixel formats, palettes, sampling flags, dimensions, and revisions through the core texture registry. **Done when:** known texture fixtures yield the expected RGBA samples on Wii. Depends on W17.
- [x] **W19b — Upload GX textures.** Convert decoded pixels into GX compatible tiled storage, flush CPU cache before GPU reads, and cache by handle and revision. **Done when:** a texture update changes the next draw without stale GPU data. Depends on W19a.
- [x] **W19c — Draw textured geometry.** Translate `TEX_QUAD` and `TEX_TRI` with the contract's UVs, vertex color modulation, and nearest or filtered sampling. **Done when:** opaque, alpha, paletted, and transformed image fixtures match reference output. Depends on W19b.
- [x] **W20 — Draw baked glyphs.** Consume `GLYPH_RUN` and the core font atlas registry; cache GX glyph textures by atlas identity. **Done when:** ordinary text, clipping, and a changed atlas render correctly. Depends on W17.
- [x] **W21 — Integrate drawing into a caller GX pass.** Implement `pocket_wii_draw` using W18a–W20 and release GX resources on shutdown, with no `VIDEO_Init`, `GX_Init`, framebuffer copy, swap, or vblank inside the library. **Done when:** a host can draw its own geometry before and after PocketJS within one pass and a second boot does not retain stale GPU resources. Depends on W18a–W18c, W19c, W20.

### E. Homebrew example and validation

- [x] **W22 — Link an external example.** Add `hosts/wii/example/` using `wii_rules`; link the public archive and header, embed or load the `.pocket`, and produce `boot.dol`. **Done when:** its Makefile consumes built library outputs rather than compiling host sources itself. Depends on W11, W15b, W21.
- [x] **W23a — Map controller buttons in the example.** Poll libogc outside the library and translate held Wii Remote and supported expansion or GameCube controls to the PocketJS `BTN` mask. **Done when:** confirm, cancel, and D pad reach the guest with the expected bit values. Depends on W22.
- [x] **W23b — Map one analog stick in the example.** Convert a supported expansion or GameCube stick to packed PocketJS 0–255 axes, with `0x8080` when absent. **Done when:** the guest sees center and both axis extremes. Depends on W22.
- [x] **W24 — Schedule fixed ticks in the example.** Use elapsed time to call `pocket_wii_tick` at 60 Hz while video presentation follows the selected Wii mode. **Done when:** the guest advances at the same simulation rate in 50 Hz and 60 Hz video modes. Depends on W22.
- [x] **W25 — Run an emulator rendering check.** Run the example in Dolphin and compare captured rectangles, glyphs, alpha images, transforms, clips, and input state to known reference output. **Done when:** no unexplained rendering or input difference remains; record any accepted device specific difference with exact images and affected operations. Depends on W21–W24, including W23a and W23b.
- [ ] **W26 — Run a Wii hardware check.** Boot the example through Homebrew, check startup, controls, repeated boot or shutdown, texture changes, memory use, and both available video modes. **Done when:** the library draws inside the host application without a crash or visible corruption, and measured limits are recorded. Depends on W25.
- [ ] **W27 — Document consumption.** Add `hosts/wii/README.md` with toolchain versions, build commands, archive/header/package outputs, a minimal caller loop, input mapping, GX state rule, package lifetime, and verified limits. **Done when:** a separate Wii Homebrew project can follow the steps without repository internal includes. Depends on W22–W26.
- [ ] **W28 — Decide production target registration.** After W26, add Wii to the production registry and `bun pocket` backend only if the tested host supports every advertised capability; otherwise retain `wii-dev` and record missing checks. **Done when:** target availability matches evidence and `pocket-pack` resolves Wii packages through the chosen profile. Depends on W26 and W27.

## Gate result

**W07 passed.** The build uses `powerpc-eabi-gcc` from devkitPPC r50-1 and the custom Rust target `powerpc-gekko-eabi` in `hosts/wii/core/targets/powerpc-gekko-eabi.json`, pinned to nightly Rust `2026-07-02`. The core archive is `dist/wii/core/powerpc-gekko-eabi/release/libpocketjs_core.rlib`; the probe static archive is `<WORK>/target/powerpc-gekko-eabi/release/libpocketjs_wii_link_probe.a` (default `<WORK>`: `/tmp/pocketjs-wii-core-probe`).

**W01 passed:** `/opt/devkitpro` has devkitPPC r50-1 (GCC 16.1.0), binutils 2.46.0, newlib 4.6.0.20260123-4, `devkitppc-rules` 1.2.1-1, libogc 3.1.0-1, and `gamecube-tools` 1.0.7-1. With `/opt/devkitpro/devkitPPC/bin` and `/opt/devkitpro/tools/bin` on `PATH`, `make -B -C /tmp/w01-probe V=1` linked a libogc ELF and produced `/tmp/w01-probe/boot.dol` (SHA256 `25c4c915eab431973d21de65f8b3e895b6e2e294748fdb2ad23d0e6dbd76ad0b`).

**W02 passed:** `hosts/wii/core` pins nightly Rust `2026-07-02`, builds `core`, `alloc`, and `compiler_builtins` for a 32 bit big endian PowerPC 750 EABI target, and uses devkitPPC GCC for linking. From that directory, `PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH cargo build --manifest-path ../../../engine/core/Cargo.toml --target-dir ../../../dist/wii/core --release --locked -Z json-target-spec` produced `dist/wii/core/powerpc-gekko-eabi/release/libpocketjs_core.rlib` containing PowerPC objects.

**W03 passed:** `PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC make -B -C hosts/wii/core/probe` built a Rust staticlib linked into a libogc ELF and DOL. The final ELF has no undefined symbols. Dolphin logged `W03 PASS: Rust allocation/free, 16-byte alignment, core, f32 ABI`. **The link must select newlib `memset` before Rust's weak `compiler_builtins` copy:** libogc clears BSS while its stack is inside BSS, and the Rust routine erased its own stack frame. The working link uses `-Wl,-u,memset` and places `-lc` before the Rust archive; `probe.map` confirms newlib `memset` at `0x80004778` and `memcpy` at `0x8000dc14`.

**W04 passed:** `DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH make -C hosts/wii/quickjs` compiled pinned QuickJS revision `ba5bdd0dc013518768e76cd9e05cd30ed53dd35b` into `dist/wii/quickjs/libquickjs.a` and linked an ELF32 big endian PowerPC probe with no unresolved symbols. QuickJS and the probe both use `JS_NO_NAN_BOXING`; the probe asserts a 16 byte `JSValue`.

**W05 passed:** `make -C hosts/wii/quickjs check` ran the pinned QuickJS probe on desktop. The PowerPC DOL ran in Dolphin Flatpak 2603a with an isolated `/tmp/pocketjs-wii-w05-user` profile and `Logger.Logs.OSREPORT=True`; its guest log reported `W05 QuickJS smoke passed: number=42 object=42 typed-array=[18,52] promises=drained exception=extracted`. The Wii build guards QuickJS `CONFIG_ATOMICS` because devkitPPC has no 64 bit atomic library. Dolphin remains open after guest `main` returns, so the timed launch exits 124 after the pass marker.

**W06d passed:** The QuickJS C probe observed identical explicit `DataView` little endian bytes on desktop and Wii (`000034120000803f`). Native typed arrays differed as expected: `Uint16Array` bytes were `3412` on desktop and `1234` on Wii; `Float64Array` bytes for 42.5 were `0000000000404540` on desktop and `4045400000000000` on Wii. The Wii DOL logged its pass marker in Dolphin. Wii must omit `setPropBatch` until its byte format is made explicit; the existing `setProp` fallback covers the first host.

**W03 build command:**

```sh
PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC make -B -C hosts/wii/core/probe
```

**W03 and W06a–W06c integrated check:**

```sh
PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC WORK=/tmp/pocketjs-wii-core-probe-w06c make -B -C hosts/wii/core/probe check
```

The check passed the W03 and W06A/B/C markers and matched desktop output against Dolphin. **W06a reported `wii-dev`, ABI 7, a 144-byte pak, and entries `ui:probe` (3 bytes) and `ui:probe-long` (7 bytes).** The recorded W06b command was `make -C hosts/wii/core/probe check WORK=/tmp/pocketjs-wii-core-probe-w06b`; desktop and Dolphin matched the style values, glyph metrics, and 48 coverage bytes. The W06c run used the integrated command above. **Its fixture covers one 4×2 `PSM_T8` texture and one image/text DrawList; it does not cover every texture format or GX rendering.**

**W05/W06d build, desktop check, and Wii run:**

```sh
DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH make -C hosts/wii/quickjs all check
cp dist/wii/quickjs/quickjs-probe.dol /tmp/quickjs-probe.dol
timeout 20s flatpak run --filesystem=/tmp --command=dolphin-emu org.DolphinEmu.dolphin-emu --user=/tmp/pocketjs-wii-w05-user --video_backend=Null --exec=/tmp/quickjs-probe.dol -C Logger.Logs.OSREPORT=True -C Logger.Options.WriteToConsole=True
```

The QuickJS Makefile's `check` target runs the desktop probe; the separate Dolphin launch checks the Wii DOL. **The expected timeout exit is 124 after the W05/W06d pass marker.** The Wii archive is `dist/wii/quickjs/libquickjs.a`; it builds pinned QuickJS revision `ba5bdd0dc013518768e76cd9e05cd30ed53dd35b` from `quickjs.c`, `cutils.c`, `libregexp.c`, `libunicode.c`, and `dtoa.c`. The Wii compile flags are `-O2 -std=gnu11 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -D_GNU_SOURCE -DJS_NO_NAN_BOXING -D__TM_GMTOFF=tm_gmtoff -DCONFIG_VERSION='"pocketjs-wii"' -include malloc.h -DPOCKETJS_NO_ATOMICS -fno-strict-aliasing -funsigned-char -Wno-incompatible-pointer-types -Wno-implicit-function-declaration -ffunction-sections -fdata-sections -I$(QUICKJS_SRC)`; the probe adds `-DPOCKETJS_WII_PROBE` and `-I$(DEVKITPRO)/libogc/include`. The probe link flags are `-O2 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -Wl,--gc-sections`, with `-L$(LIBOGC_LIB) -logc -lm`. **`JS_NO_NAN_BOXING` keeps `JSValue` at 16 bytes. `POCKETJS_NO_ATOMICS` disables QuickJS Atomics because devkitPPC lacks the required 64-bit atomic library.** Native typed arrays retain host byte order, so `setPropBatch` remains omitted.

**W08–W10 passed:** cached Bun 1.3.3 emitted the hero guest JS, pak, and `.pocket` package, and the Wii C verifier admitted that exact package. **W12–W13b passed:** the Wii core C ABI probe matched desktop and Dolphin. **W17 passed:** the DrawList sanitizer check passed. Draft PR #3 is open.

**W14–W16 passed:** Dolphin logged `W14 PASS host=wii-dev abi=7 viewport=480x272 initial=24 updated=40 image=probe sprite=pulse ops=21 batch=absent cursor=absent auxiliary=absent`; desktop and Dolphin both reported `W15B PASS failed boot cleanup, repeated shutdown, and reboot after shutdown` and `W16 PASS fixed-step input, native DrawList mutation, retained guest exception`.

**W18a–W20 passed:** native GX-mock checks and Wii DOL probes reported `W18a PASS` for solid geometry, `W19A PASS` for texture decoding, `W19B PASS` for texture caching, `W19C PASS` for textured draws, and `W20 PASS` for glyph drawing. **W21 passed:** native public-draw mock checks covered ordered dispatch, caller GX drawing before and after PocketJS, and cache handling across reboot; a fresh PowerPC ELF/DOL linked, with actual pixel comparison deferred to W25.

**W11 passed:** `bun tools/wii.ts --library` exported the C header and Rust/QuickJS archives; an external Makefile under `/tmp` linked `boot.dol` with no unresolved symbols and selected newlib `memset` and `memcpy`.

**W22 passed:** `make -B -C hosts/wii/example V=1` linked `boot.dol` from the exported archives and header with no undefined symbols; the map selected newlib `memset` and `memcpy`. Dolphin startup was blocked by Flatpak instance allocation, so W25 will own rendering validation.

**W23a/W23b passed:** the host mapper check printed `W23a/W23b PASS: button mapping and calibrated Nunchuk axis center/extremes` for remote/classic/GameCube masks; a forced devkitPPC build compiled `input.c`, linked `boot.dol` with no undefined symbols, and selected newlib `memset` and `memcpy` in its map.

**W24 passed:** `make -C hosts/wii/example check BUILD=/tmp/pocketjs-wii-example-w24-check` asserted 60 ticks per synthetic one-second 50 Hz and 60 Hz schedule; the forced devkitPPC build linked a fresh `boot.dol` with no undefined symbols and map-selected newlib `memset`/`memcpy`. Hardware cadence remains for W26.

**W25 passed:** Dolphin 2606/Vulkan captures `hosts/wii/check/w25/evidence/before-a.png` and `hosts/wii/check/w25/evidence/after-a.png` passed all 10 samples for rectangles, nested clips and restoration, transformed alpha image, baked glyph, and Wii Remote A input; the indicator changed `#c02020`→`#20c060`, and alpha `#41005f` is within ±4/channel of formula `#400060`. The separate hero capture accepts the current arcade logo against stale Web goldens with the old blue play icon because the source art changed.

## Source contracts

- `contracts/spec/spec.ts`: HostOps codes, button mask, pixel formats, and DrawList format.
- `framework/src/manifest/host-build-inputs.ts`: verified host build inputs.
- `contracts/spec/pocket-package.ts`: package encoding and target variants.
- `hosts/3ds/core/src/lib.rs`, `hosts/3ds/src/qjs.c`, `hosts/3ds/src/gfx.c`: package, QuickJS, and C backend reference implementations.
- `engine/core/src/lib.rs`: retained UI, font and texture registries, tick, and DrawList.
