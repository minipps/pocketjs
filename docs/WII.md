# Nintendo Wii host

The Wii host is a **provisional `wii-dev` target**. It runs the normal
QuickJS guest and `pocketjs-core`, then submits the core DrawList through the
Wii GX fixed-function pipeline. The target uses **host ABI 9**, a **640×480
physical output**, and a **480×272 logical viewport rendered with `fit` or
centered `integer-fit`** at raster density 1. Integer-fit uses scale 1 on the
Wii's 640×480 surface, leaving a centered letterbox. It advertises only the
APIs that its host tests cover:
`input.buttons` and `text.glyphs.baked`.

`wii-dev` is intentionally absent from `contracts/spec/platforms.ts`'s
production `POCKET_TARGETS`. A development profile can build and resolve a
Wii package without claiming that every PocketJS app has a supported Wii
release.

## Startup and frame order

The embedding application owns Wii video and GX setup. The public host API is
called in this order:

1. Initialize video and input (`VIDEO_Init`, the preferred `GXRModeObj`, the
   XFB allocation, `VIDEO_Configure`, and the controller subsystem).
2. Initialize GX and its FIFO, then select the EFB/XFB and any clear, viewport,
   scissor, vertex, texture-environment, blend, and depth state needed by the
   embedding application.
3. Call `pocket_wii_init` with one target-thinned `.pocket` package. It admits
   `wii-dev` and its host ABI before exposing JS or PAK sections to QuickJS.
4. On each frame, poll input, call `pocket_wii_update(buttons, 0x8080)`, then
   call `pocket_wii_draw` while the caller's EFB target is current. `wii-dev`
   does not advertise an analog capability, so the shared axis field stays at
   its centered value.
5. Finish and present the frame in the embedding application, then pace with
   `VIDEO_SetNextFramebuffer`, `VIDEO_Flush`, and `VIDEO_WaitVSync`.
6. Call `pocket_wii_shutdown` before releasing the package, GX FIFO, XFB, or
   video mode.

`pocket_wii_update` calls the guest frame hook, drains pending jobs, advances
one fixed 1/60-second core tick, and builds the frame's DrawList. `pocket_wii_draw`
submits the most recent DrawList into the caller-selected EFB rectangle; it
does not copy the EFB, present an XFB, or wait for vblank. The caller performs
`GX_DrawDone` before reusing frame-owned upload memory or tearing down the
guest.

The guest sees the same frame transaction as every other PocketJS host. It
does not call `VIDEO_*`, `GX_*`, `WPAD_*`, or `PAD_*` APIs and does not receive
Wii SDK types.

## Package and resource lifetime

The package reader returns borrowed sections. The allocation containing the
complete `.pocket` file therefore remains live from package admission through
QuickJS evaluation, every frame, PAK lookups, and runtime shutdown. The host
does not split the package into independently owned `app.js` and `app.pak`
files.

Shutdown has one safe boundary:

1. stop submitting guest DrawLists;
2. wait for GX to finish (`GX_DrawDone`);
3. destroy the QuickJS/core runtime and release PAK/font/texture references;
4. release the package allocation only after the runtime no longer borrows it.

An admission or eval failure leaves the host unmounted. A frame or first-draw
failure returns an error and leaves no ready frame; the embedding application
should stop and call `pocket_wii_shutdown` after reporting it. None of these
paths releases the caller-owned package bytes while the runtime can still
reference them.

## GX ownership

The embedding application owns the GX FIFO, EFB/XFB, render-target setup, and
frame begin/end. `hosts/wii` owns only the GX calls needed to walk the
DrawList; the core owns layout, animation, clipping, and DrawList words, and
does not own GX state. The backend assumes that GX is initialized and that the
requested EFB target is current.

State that must not leak between batches is set explicitly: vertex attribute
descriptors, texture bindings, scissor, TEV operation, blend mode, and texture
coordinate generation. The backend does not restore the caller's GX state or
turn Wii GX state into a new application API; an embedding that draws after
`pocket_wii_draw` must bind its own state again.

## Linking and toolchain

The build runs with **devkitPPC and libogc**. The C entry point links the Wii
host objects with the Rust `pocketjs-core` static library, QuickJS, and libogc
in the order required by the linker; the final ELF is converted to the
installable homebrew artifact by the Wii toolchain. The host Makefile is the
source of linker flags and SDK paths. A desktop Rust build is not a substitute
for the Wii link: GX and video symbols come from libogc.

The generated build plan supplies the target id, host ABI, viewport, and
package path to the native build. The C sources do not copy those values into
an independent manifest or infer them from an output filename.

## Controller mapping

Input is sampled once per frame and converted to the hardware-neutral
PocketJS button bitmask. The host currently treats the Wii controller as a
button source; it does not expose a Wii SDK object or encode motion as fake
buttons.

| Wii input | PocketJS button |
| --- | --- |
| D-pad up/right/down/left | `UP`/`RIGHT`/`DOWN`/`LEFT` |
| `A` | `CIRCLE` |
| `B` | `CROSS` |
| `1` | `TRIANGLE` |
| `2` | `SQUARE` |
| `+` / `-` | `START` / `SELECT` |

The mapping follows the existing generic button contract. Nunchuk motion,
IR coordinates, rumble, and audio are not part of `wii-dev`; adding one of
those requires a public capability and a host-independent contract test.

## Verification gate

The profile is ready for development iteration, not a production support
claim. The acceptance order is:

1. host-independent profile, package-admission, conversion, and GX-state tests;
2. a deterministic Dolphin run using the Wii artifact and capture path;
3. a real Wii boot with the same package, input mapping, frame presentation,
   and teardown path;
4. only then, if all required DrawList and input paths are covered, a review of
   the profile for promotion into `POCKET_TARGETS`.

Dolphin is the emulator gate for iteration and regression tests. It does not
replace the hardware gate: GX emulation and a real Wii can disagree on memory
placement, video timing, cache visibility, or controller delivery. Until the
hardware run is recorded, documentation should call the target **provisional**
and the README should list it separately from verified hardware.

## Commands

The Wii tool accepts the standard `480×272` fixed viewport, including the
repository demos' `presentation: "integer-fit"`, and emits a target-thinned
`.pocket`. The default command then links the native DOL; `--pocket-only` stops
after packaging. The exact SDK paths come from the toolchain environment:

```sh
bun tools/wii.ts --manifest=path/to/pocket.json
bun tools/wii.ts --manifest=path/to/pocket.json --pocket-only
```

The Dolphin and hardware acceptance runs are not part of this repository yet;
the commands above are intentionally provisional while those receipts are
being established.
