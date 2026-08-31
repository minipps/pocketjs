import { describe, expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { POCKET_TARGETS } from "../contracts/spec/platforms.ts";
import {
  POCKET_SECTION,
  POCKET_PACKAGE_TARGET_BYTES,
  decodePocketPackage,
  decodeIdentity,
  encodeIdentity,
  encodePocketPackage,
  findSection,
  findVariant,
} from "../contracts/spec/pocket-package.ts";
import { verifyPlanHash } from "../framework/src/manifest/plan.ts";
import {
  validateAndResolveBuildPlan,
  validatePlatformContractRegistry,
} from "../framework/src/manifest/resolve.ts";
import {
  WII_DEV_CONTRACTS,
  WII_DEV_HOST_ABI,
  WII_DEV_TARGET_ID,
  WII_LOGICAL_VIEWPORT,
  WII_PHYSICAL_VIEWPORT,
  WII_VIEWPORT,
  resolveWiiBuildPlan,
} from "../tools/wii-profile.ts";
import {
  WII_TOOLCHAIN,
  inspectWiiToolchain,
  wiiToolchainPaths,
} from "../tools/wii-toolchain.ts";

const repository = new URL("..", import.meta.url).pathname;

function manifest(presentation: "fit" | "integer-fit" = "integer-fit"): Record<string, any> {
  return {
    $schema: "https://pocketjs.dev/schema/pocket-2.json",
    pocket: 2,
    id: "dev.pocket-stack.wii-demo",
    name: "pocketjs-wii-demo",
    title: "PocketJS Wii Demo",
    version: "0.1.0",
    engine: {
      capabilities: {
        requires: ["input.buttons", "text.glyphs.baked"],
        enhances: ["input.analog.left", "audio.pcm"],
      },
    },
    app: {
      entry: "apps/hero/main.tsx",
      output: "wii-demo-main",
      framework: "solid",
      viewport: {
        fixed: { logical: WII_VIEWPORT, presentation },
      },
    },
  };
}

function packageFor(plan: ReturnType<typeof resolveWiiBuildPlan>): Uint8Array {
  const js = new TextEncoder().encode("globalThis.frame = () => {};\0");
  return encodePocketPackage({
    manifest: new TextEncoder().encode(JSON.stringify(manifest())),
    variants: [{
      target: plan.target.id,
      hostAbi: plan.target.hostAbi,
      sections: [
        {
          kind: POCKET_SECTION.identity,
          bytes: encodeIdentity({
            output: plan.app.output,
            id: plan.app.id,
            title: plan.app.title,
          }),
        },
        {
          kind: POCKET_SECTION.plan,
          bytes: new TextEncoder().encode(JSON.stringify(plan)),
        },
        { kind: POCKET_SECTION.js, bytes: js },
        { kind: POCKET_SECTION.pak, bytes: new Uint8Array([0x50, 0x41, 0x4b]) },
      ],
    }],
  });
}

describe("private Nintendo Wii profile", () => {
  test("keeps wii-dev out of the production registry", () => {
    expect(WII_DEV_TARGET_ID).toBe("wii-dev");
    expect(WII_DEV_HOST_ABI).toBe(9);
    expect(WII_LOGICAL_VIEWPORT).toEqual([480, 272]);
    expect(WII_PHYSICAL_VIEWPORT).toEqual([640, 480]);
    expect(POCKET_TARGETS).not.toHaveProperty(WII_DEV_TARGET_ID);
    expect(WII_DEV_CONTRACTS.targets[WII_DEV_TARGET_ID]).toEqual({
      hostAbi: WII_DEV_HOST_ABI,
      platform: "wii",
      form: "takeover",
      display: {
        physicalViewport: WII_PHYSICAL_VIEWPORT,
        logicalViewports: [WII_LOGICAL_VIEWPORT],
        presentations: ["fit", "integer-fit"],
        rasterDensity: 1,
      },
      capabilities: ["input.buttons", "text.glyphs.baked"],
    });
    expect(validatePlatformContractRegistry(WII_DEV_CONTRACTS)).toEqual([]);
    expect(
      Object.values(POCKET_TARGETS).map((profile) => profile.hostAbi),
    ).not.toContain(WII_DEV_HOST_ABI);
    expect(new TextEncoder().encode(WII_DEV_TARGET_ID).length).toBeLessThan(
      POCKET_PACKAGE_TARGET_BYTES,
    );
  });

  test("resolves the fixed 480x272 logical plan into a 640x480 output", () => {
    const plan = resolveWiiBuildPlan(manifest());
    expect(plan.target).toEqual({ id: WII_DEV_TARGET_ID, hostAbi: WII_DEV_HOST_ABI });
    expect(plan.viewport).toEqual({
      logical: WII_LOGICAL_VIEWPORT,
      physical: WII_PHYSICAL_VIEWPORT,
      presentation: "integer-fit",
      rasterDensity: 1,
      policy: "fixed",
    });
    expect(plan.features).toEqual({
      "audio.pcm": false,
      "input.analog.left": false,
      "input.buttons": true,
      "text.glyphs.baked": true,
    });
    expect(plan.app.output).toBe("wii-demo-main");
    expect(verifyPlanHash(plan)).toBe(true);
  });

  test("admits the checked-in Hero integer-fit manifest", () => {
    const hero = JSON.parse(
      readFileSync(join(repository, "apps/hero/pocket.json"), "utf8"),
    );
    const plan = resolveWiiBuildPlan(hero);
    expect(plan.app.output).toBe("hero-main");
    expect(plan.viewport.presentation).toBe("integer-fit");
  });

  test("retains floating fit for manifests that request it", () => {
    expect(resolveWiiBuildPlan(manifest("fit")).viewport.presentation).toBe("fit");
  });

  test("rejects a dynamic viewport and unsupported required capabilities", () => {
    const dynamic = manifest();
    dynamic.app.viewport = { dynamic: { default: WII_LOGICAL_VIEWPORT } };
    expect(() => resolveWiiBuildPlan(dynamic)).toThrow("fixed viewport");

    const needsAnalog = manifest();
    needsAnalog.engine.capabilities.enhances = ["audio.pcm"];
    needsAnalog.engine.capabilities.requires.push("input.analog.left");
    const result = validateAndResolveBuildPlan(
      needsAnalog,
      { target: WII_DEV_TARGET_ID },
      WII_DEV_CONTRACTS,
    );
    expect(result.ok).toBe(false);
    expect(result.ok ? [] : result.diagnostics.map((d) => d.code)).toEqual([
      "capability.unavailable",
    ]);
  });

  test("admits only the exact Wii package variant and preserves its borrowed sections", () => {
    const plan = resolveWiiBuildPlan(manifest());
    const bytes = packageFor(plan);
    const decoded = decodePocketPackage(bytes);
    const variant = findVariant(decoded, WII_DEV_TARGET_ID);
    expect(variant?.hostAbi).toBe(WII_DEV_HOST_ABI);
    expect(findVariant(decoded, "psp")).toBeNull();
    expect(decodeIdentity(findSection(variant!, POCKET_SECTION.identity)!)).toEqual({
      output: plan.app.output,
      id: plan.app.id,
      title: plan.app.title,
    });
    expect(findSection(variant!, POCKET_SECTION.js)?.at(-1)).toBe(0);
    expect(findSection(variant!, POCKET_SECTION.pak)).toEqual(
      new Uint8Array([0x50, 0x41, 0x4b]),
    );
    expect(findSection(variant!, POCKET_SECTION.plan)).toEqual(
      new TextEncoder().encode(JSON.stringify(plan)),
    );

    const admits = (target: string, hostAbi: number) => {
      const candidate = findVariant(decoded, target);
      return candidate !== null && candidate.hostAbi === hostAbi;
    };
    expect(admits(WII_DEV_TARGET_ID, WII_DEV_HOST_ABI)).toBe(true);
    expect(admits("psp", WII_DEV_HOST_ABI)).toBe(false);
    expect(admits(WII_DEV_TARGET_ID, WII_DEV_HOST_ABI + 1)).toBe(false);
  });
});

describe("Nintendo Wii toolchain", () => {
  test("pins the PowerPC target and libogc markers", () => {
    expect(WII_TOOLCHAIN).toMatchObject({
      schemaVersion: 1,
      toolchainVersion: "devkitpro-wii-v1",
      rust: {
        toolchain: "nightly-2026-07-02",
        target: "powerpc-unknown-eabi",
        components: ["rust-src"],
      },
      sdk: {
        rootEnv: "DEVKITPRO",
        compilerEnv: "DEVKITPPC",
        defaultRoot: "/opt/devkitpro",
      },
    });
    expect(WII_TOOLCHAIN.quickjs.revision).toMatch(/^[0-9a-f]{40}$/);
    expect(WII_TOOLCHAIN.sdk.markers).toEqual(expect.arrayContaining([
      "devkitPPC/wii_rules",
      "devkitPPC/bin/powerpc-eabi-gcc",
      "devkitPPC/bin/powerpc-eabi-gcc-ar",
      "devkitPPC/bin/powerpc-eabi-gcc-ranlib",
      "libogc/include/gccore.h",
      "libogc/lib/wii/libogc.a",
      "libogc/share/ogc.specs",
      "tools/bin/bin2s",
      "tools/bin/elf2dol",
    ]));
  });

  test("derives every native tool path from DEVKITPRO and DEVKITPPC", () => {
    const paths = wiiToolchainPaths({ DEVKITPRO: "/opt/wii-sdk" });
    expect(paths).toEqual({
      devkitpro: "/opt/wii-sdk",
      devkitppc: "/opt/wii-sdk/devkitPPC",
      compiler: "/opt/wii-sdk/devkitPPC/bin/powerpc-eabi-gcc",
      archiver: "/opt/wii-sdk/devkitPPC/bin/powerpc-eabi-gcc-ar",
      ranlib: "/opt/wii-sdk/devkitPPC/bin/powerpc-eabi-gcc-ranlib",
      bin2s: "/opt/wii-sdk/tools/bin/bin2s",
      elf2dol: "/opt/wii-sdk/tools/bin/elf2dol",
      wiiRules: "/opt/wii-sdk/devkitPPC/wii_rules",
      libogcInclude: "/opt/wii-sdk/libogc/include",
      libogcLibrary: "/opt/wii-sdk/libogc/lib/wii",
      ogcSpecs: "/opt/wii-sdk/libogc/share/ogc.specs",
    });

    const explicit = wiiToolchainPaths({ DEVKITPPC: "/srv/devkitPPC" });
    expect(explicit.devkitpro).toBe("/srv");
    expect(explicit.devkitppc).toBe("/srv/devkitPPC");
  });

  test("reports missing SDK markers without requiring the SDK", () => {
    const status = inspectWiiToolchain({ DEVKITPRO: "/definitely/missing/pocketjs-wii-sdk" });
    expect(status.ok).toBe(false);
    expect(status.missing).toEqual(expect.arrayContaining([
      "powerpc-eabi-gcc",
      "libogc headers",
      "libogc library",
    ]));
  });

  test("pins the PowerPC target and Wii-only Makefile contract", () => {
    const target = JSON.parse(readFileSync(
      join(repository, "hosts/wii/targets/powerpc-unknown-eabi.json"),
      "utf8",
    )) as Record<string, unknown>;
    expect(target).toMatchObject({
      arch: "powerpc",
      cpu: "750",
      "llvm-target": "powerpc-unknown-eabi",
      "target-endian": "big",
      "target-pointer-width": 32,
      linker: "powerpc-eabi-gcc",
      os: "none",
      env: "eabi",
      "relocation-model": "static",
      "panic-strategy": "abort",
    });

    const makefile = readFileSync(join(repository, "hosts/wii/Makefile"), "utf8");
    for (const flag of ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]) {
      expect(makefile).toContain(flag);
    }
    expect(makefile).toContain("include $(DEVKITPPC)/wii_rules");
    expect(makefile).toContain("-specs=ogc.specs");
    expect(makefile).toContain("-L$(LIBOGC_LIB)");
    expect(makefile).toContain("-lwiiuse");
    expect(makefile).toContain("-logc");
    expect(makefile).toContain("$(bin2o)");
    expect(makefile).toContain("elf2dol");
    expect(makefile).not.toContain("__3DS__");
    expect(makefile).not.toContain("citro3d");
  });

  test("strips HOME and maps held Wii input to the portable mask", () => {
    const input = readFileSync(join(repository, "hosts/wii/src/input.c"), "utf8");
    expect(input).toContain("WPAD_ButtonsHeld");
    expect(input).toContain("held &= ~WPAD_BUTTON_HOME");
    expect(input).toContain("WPAD_CLASSIC_BUTTON_HOME");
    expect(input).toContain("POCKET_BTN_CIRCLE");
    expect(input).toContain("POCKET_BTN_CROSS");
    expect(input).toContain("POCKET_BTN_START");
    expect(input).toContain("POCKET_BTN_SELECT");
  });

  test("provisions a main stack above the QuickJS guard", () => {
    const main = readFileSync(join(repository, "hosts/wii/src/main.c"), "utf8");
    expect(main).toContain("#define POCKET_WII_MAIN_STACK_SIZE (256 * 1024)");
    expect(main).toContain("void *__ppc_main_sp = wii_main_stack + sizeof wii_main_stack;");
    const qjs = readFileSync(join(repository, "hosts/shared/qjs.c"), "utf8");
    expect(qjs).toContain("#define POCKETJS_JS_STACK_SIZE (192 * 1024)");
  });

  test("invalidates the GX font cache on core raster replacement", () => {
    const core = readFileSync(join(repository, "hosts/shared/pocket_core.h"), "utf8");
    expect(core).toContain("uint64_t ui_raster_revision(void);");
    const gx = readFileSync(join(repository, "hosts/wii/src/pocket_wii_gx.c"), "utf8");
    expect(gx).toContain("entry->raster_revision == raster_revision");
    const runtime = readFileSync(join(repository, "hosts/wii/src/pocket_wii.c"), "utf8");
    expect(runtime).toContain("frame_ready = false;");
  });
});
