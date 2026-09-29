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

- [ ] **W01 — Confirm devkitPPC inputs.** Record the compiler, `wii_rules`, libogc headers and archives, `elf2dol`, and versions available to the build environment. **Done when:** a plain libogc example links to `boot.dol`. **Gate.**
- [ ] **W02 — Define the Rust Wii target.** Add a pinned custom PowerPC target and standalone Cargo configuration for `core`, `alloc`, and `compiler_builtins`, using devkitPPC compatible EABI and hard float settings. **Done when:** `pocketjs-core` compiles to a Wii object or archive without host OS dependencies. Depends on W01. **Gate.**
- [ ] **W03 — Prove Rust/C linkage and allocation.** Link a C probe against the Rust archive and libogc; exercise a Rust call that allocates and frees memory. Check pointer alignment, panic symbols, float arguments, and unresolved runtime symbols. **Done when:** the probe links and runs in Dolphin or on hardware. Depends on W02. **Gate.**
- [ ] **W04 — Compile the pinned QuickJS source for Wii.** Use the existing source pin from `tools/3ds.ts`, compile every QuickJS translation unit and caller with matching 32 bit `JSValue` settings, and link it with devkitPPC. **Done when:** a minimal C QuickJS probe links without ABI warnings or unresolved symbols. Depends on W01. **Gate.**
- [ ] **W05 — Execute a QuickJS smoke in Wii code.** Evaluate JavaScript using numbers, objects, typed arrays, promises, and an exception; drain pending jobs and inspect the result. **Done when:** behavior matches the same source on a desktop QuickJS build in Dolphin or on hardware. Depends on W04. **Gate.**
- [ ] **W06a — Check package and pak byte order.** Decode a known `.pocket` and pak through the Wii Rust build; fix any read that treats little endian bytes as native words. **Done when:** target, ABI, entry names, and entry lengths match desktop decoding. Depends on W03. **Gate.**
- [ ] **W06b — Check styles and font byte order.** Load known style and font atlas blobs through the Wii Rust build; fix any native word reads. **Done when:** style values, glyph metrics, and coverage bytes match desktop decoding. Depends on W03. **Gate.**
- [ ] **W06c — Check texture and DrawList values.** Decode known texture bytes and numeric DrawList words on Wii. **Done when:** colors, UVs, packed coordinates, and text bytes match desktop values without swapping the `uint32_t` DrawList pointer. Depends on W03. **Gate.**
- [ ] **W06d — Check QuickJS buffer byte order.** Check typed array and `DataView` bytes crossing into C. Leave optional `setPropBatch` absent initially: `framework/src/anim.ts` writes native endian `Float64Array` bytes and already has a `setProp` fallback. **Done when:** the guest can pass its required byte buffers without a native endian mismatch. Depends on W05. **Gate.**
- [ ] **W07 — Record the gate result.** Add the working compiler and Rust target commands, archive names, QuickJS flags, and any remaining target limitation under “Gate result” below. **Done when:** another developer can repeat W03, W05, and W06a–W06d with the recorded commands. Depends on W03, W05, W06a–W06d. **Gate.**

### B. Guest build and packaging

- [ ] **W08 — Define `wii-dev` admission.** Add an out of registry target profile in `tools/wii-profile.ts`, with the tested viewport and the first release capabilities only. **Done when:** a representative `pocket.json` resolves and a manifest requiring an absent capability is rejected. Depends on W07.
- [ ] **W09 — Compile a Wii guest.** Make `tools/wii.ts` resolve the profile and invoke the existing `tools/build.ts` pipeline with a verified plan. **Done when:** it emits Wii flavored JS, pak, and plan for a sample app without modifying stock target builds. Depends on W08.
- [ ] **W10 — Package the guest.** Use `makeVariant` and `encodePocketPackage` to make one target thinned `.pocket` containing identity, plan, NUL terminated JS, and pak. **Done when:** the package's target and ABI match the generated plan and `pocket_package_open` admits it. Depends on W09 and W13a.
- [ ] **W11 — Build the library archive.** Make `tools/wii.ts` and `hosts/wii/Makefile` build the Rust bridge, QuickJS, Wii C bridge, and GX backend into linkable archive output alongside the public header. **Done when:** an external devkitPPC Makefile can link those outputs without copying repository sources. Depends on W16 and W21.

### C. Runtime library

- [ ] **W12 — Specify the C header.** Add `hosts/wii/include/pocket_wii.h` with the four operations above, input constants or a link to generated constants, borrowed buffer lifetime, error return behavior, one guest limit, and GX state ownership. **Done when:** a plain C translation unit can include it without PocketJS internal headers. Depends on W07.
- [ ] **W13a — Expose package admission.** Build a Wii Rust `staticlib` from `pocketjs-core` and expose `pocket_package_open` using the verifier in `hosts/3ds/core/src/lib.rs`. **Done when:** C admits a small Wii package fixture made with the existing package encoder and rejects an invalid target or ABI. Depends on W07.
- [ ] **W13b — Expose core UI calls.** Expose the HostOps, pak feeder, tick, DrawList, texture, and font accessors needed by the Wii C code. Reuse the 3DS C ABI implementation where it applies directly. **Done when:** C can feed a pak, mutate nodes, tick, and read the matching DrawList and resources. Depends on W13a.
- [ ] **W14 — Bind the existing HostOps in QuickJS.** Implement `globalThis.ui`, `__host`, `__hostAbi`, `__viewport`, image names, and sprite names using the 3DS bridge as the op reference. Do not expose unimplemented optional operations. **Done when:** a compiled guest creates and updates native nodes. Depends on W12 and W13b.
- [ ] **W15a — Implement guest boot.** Wire `pocket_wii_boot` to package admission, core initialization, pak feed, and QuickJS evaluation. **Done when:** valid guest boot succeeds and a wrong target or ABI returns a useful error. Depends on W10 and W14.
- [ ] **W15b — Implement guest shutdown.** Wire `pocket_wii_shutdown` to release QuickJS and core resources in a safe order; W21 adds GX resource cleanup. **Done when:** shutdown after partial or successful boot is safe and a second boot succeeds. Depends on W15a.
- [ ] **W16 — Implement the fixed step.** Wire `pocket_wii_tick` to `frame(buttons, analog)`, pending job drain, `ui_tick`, and a retained last error. **Done when:** a frame updates visible node state and a thrown guest error reaches `pocket_wii_last_error()`. Depends on W15b.

### D. GX rendering

- [ ] **W17 — Decode the DrawList.** Add one bounds checked walk of supported op lengths and one explicit failure for unknown or unsupported ops. **Done when:** a malformed list cannot read beyond its length and the walk recognizes all ops emitted by the `wii-dev` profile. Depends on W13b.
- [ ] **W18a — Draw solid geometry.** Translate `RECT` and `TRI` into GX draws with logical to destination rectangle mapping and correct ABGR colors. **Done when:** solid boxes and triangles have the expected position, color, and painter order. Depends on W17.
- [ ] **W18b — Draw gradients.** Translate `GRAD_RECT` using the contract's direction and endpoint colors. **Done when:** all four gradient directions match reference output. Depends on W18a.
- [ ] **W18c — Apply DrawList clips.** Translate `SCISSOR` and `SCISSOR_POP` into GX clip state, preserving the enclosing clip when a nested clip ends. **Done when:** nested and empty clips match reference output. Depends on W18a.
- [ ] **W19a — Decode texture sources.** Read the current pixel formats, palettes, sampling flags, dimensions, and revisions through the core texture registry. **Done when:** known texture fixtures yield the expected RGBA samples on Wii. Depends on W17.
- [ ] **W19b — Upload GX textures.** Convert decoded pixels into GX compatible tiled storage, flush CPU cache before GPU reads, and cache by handle and revision. **Done when:** a texture update changes the next draw without stale GPU data. Depends on W19a.
- [ ] **W19c — Draw textured geometry.** Translate `TEX_QUAD` and `TEX_TRI` with the contract's UVs, vertex color modulation, and nearest or filtered sampling. **Done when:** opaque, alpha, paletted, and transformed image fixtures match reference output. Depends on W19b.
- [ ] **W20 — Draw baked glyphs.** Consume `GLYPH_RUN` and the core font atlas registry; cache GX glyph textures by atlas identity. **Done when:** ordinary text, clipping, and a changed atlas render correctly. Depends on W17.
- [ ] **W21 — Integrate drawing into a caller GX pass.** Implement `pocket_wii_draw` using W18a–W20 and release GX resources on shutdown, with no `VIDEO_Init`, `GX_Init`, framebuffer copy, swap, or vblank inside the library. **Done when:** a host can draw its own geometry before and after PocketJS within one pass and a second boot does not retain stale GPU resources. Depends on W18a–W18c, W19c, W20.

### E. Homebrew example and validation

- [ ] **W22 — Link an external example.** Add `hosts/wii/example/` using `wii_rules`; link the public archive and header, embed or load the `.pocket`, and produce `boot.dol`. **Done when:** its Makefile consumes built library outputs rather than compiling host sources itself. Depends on W11, W15b, W21.
- [ ] **W23a — Map controller buttons in the example.** Poll libogc outside the library and translate held Wii Remote and supported expansion or GameCube controls to the PocketJS `BTN` mask. **Done when:** confirm, cancel, and D pad reach the guest with the expected bit values. Depends on W22.
- [ ] **W23b — Map one analog stick in the example.** Convert a supported expansion or GameCube stick to packed PocketJS 0–255 axes, with `0x8080` when absent. **Done when:** the guest sees center and both axis extremes. Depends on W22.
- [ ] **W24 — Schedule fixed ticks in the example.** Use elapsed time to call `pocket_wii_tick` at 60 Hz while video presentation follows the selected Wii mode. **Done when:** the guest advances at the same simulation rate in 50 Hz and 60 Hz video modes. Depends on W22.
- [ ] **W25 — Run an emulator rendering check.** Run the example in Dolphin and compare captured rectangles, glyphs, alpha images, transforms, clips, and input state to known reference output. **Done when:** no unexplained rendering or input difference remains; record any accepted device specific difference with exact images and affected operations. Depends on W21–W24, including W23a and W23b.
- [ ] **W26 — Run a Wii hardware check.** Boot the example through Homebrew, check startup, controls, repeated boot or shutdown, texture changes, memory use, and both available video modes. **Done when:** the library draws inside the host application without a crash or visible corruption, and measured limits are recorded. Depends on W25.
- [ ] **W27 — Document consumption.** Add `hosts/wii/README.md` with toolchain versions, build commands, archive/header/package outputs, a minimal caller loop, input mapping, GX state rule, package lifetime, and verified limits. **Done when:** a separate Wii Homebrew project can follow the steps without repository internal includes. Depends on W22–W26.
- [ ] **W28 — Decide production target registration.** After W26, add Wii to the production registry and `bun pocket` backend only if the tested host supports every advertised capability; otherwise retain `wii-dev` and record missing checks. **Done when:** target availability matches evidence and `pocket-pack` resolves Wii packages through the chosen profile. Depends on W26 and W27.

## Gate result

Pending W07. Record the exact devkitPPC, libogc, Rust, and QuickJS versions and the successful commands here before starting W08–W28. At planning time, `powerpc-eabi-gcc` was absent from this workspace shell's `PATH`; W01 must use the available devkitPPC installation or build environment.

## Source contracts

- `contracts/spec/spec.ts`: HostOps codes, button mask, pixel formats, and DrawList format.
- `framework/src/manifest/host-build-inputs.ts`: verified host build inputs.
- `contracts/spec/pocket-package.ts`: package encoding and target variants.
- `hosts/3ds/core/src/lib.rs`, `hosts/3ds/src/qjs.c`, `hosts/3ds/src/gfx.c`: package, QuickJS, and C backend reference implementations.
- `engine/core/src/lib.rs`: retained UI, font and texture registries, tick, and DrawList.
