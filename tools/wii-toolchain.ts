import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import manifestJson from "./cli/wii-toolchain.json";

export interface WiiToolchainManifest {
  readonly schemaVersion: 1;
  readonly toolchainVersion: string;
  readonly rust: {
    readonly toolchain: string;
    readonly target: string;
    readonly components: readonly string[];
  };
  readonly quickjs: {
    readonly version: string;
    readonly repository: string;
    readonly revision: string;
  };
  readonly sdk: {
    readonly rootEnv: "DEVKITPRO";
    readonly compilerEnv: "DEVKITPPC";
    readonly defaultRoot: string;
    readonly markers: readonly string[];
  };
}

export const WII_TOOLCHAIN = manifestJson as WiiToolchainManifest;

/** ABI flags shared by QuickJS and every Wii C translation unit. */
export const WII_ARCHITECTURE_FLAGS = [
  "-DGEKKO",
  "-mrvl",
  "-mcpu=750",
  "-meabi",
  "-mhard-float",
] as const;

export interface WiiToolchainPaths {
  readonly devkitpro: string;
  readonly devkitppc: string;
  readonly compiler: string;
  readonly archiver: string;
  readonly ranlib: string;
  readonly bin2s: string;
  readonly elf2dol: string;
  readonly wiiRules: string;
  readonly libogcInclude: string;
  readonly libogcLibrary: string;
  readonly ogcSpecs: string;
}

export interface WiiBuildToolchain extends WiiToolchainPaths {
  readonly manifest: WiiToolchainManifest;
  readonly environment: NodeJS.ProcessEnv;
}

export interface WiiToolchainStatus {
  readonly paths: WiiToolchainPaths;
  readonly ok: boolean;
  readonly missing: readonly string[];
}

function explicitPath(value: string | undefined): string | undefined {
  return value?.trim() ? resolve(value.trim()) : undefined;
}

/** Resolve DEVKITPRO/DEVKITPPC without requiring shell startup files. */
export function wiiToolchainPaths(env: NodeJS.ProcessEnv = process.env): WiiToolchainPaths {
  const explicitPpc = explicitPath(env.DEVKITPPC);
  const devkitpro = explicitPath(env.DEVKITPRO) ??
    (explicitPpc ? dirname(explicitPpc) : resolve(WII_TOOLCHAIN.sdk.defaultRoot));
  const devkitppc = explicitPpc ?? join(devkitpro, "devkitPPC");
  const compilerBin = join(devkitppc, "bin");
  const toolsBin = join(devkitpro, "tools", "bin");
  return {
    devkitpro,
    devkitppc,
    compiler: join(compilerBin, "powerpc-eabi-gcc"),
    archiver: join(compilerBin, "powerpc-eabi-gcc-ar"),
    ranlib: join(compilerBin, "powerpc-eabi-gcc-ranlib"),
    bin2s: join(toolsBin, "bin2s"),
    elf2dol: join(toolsBin, "elf2dol"),
    wiiRules: join(devkitppc, "wii_rules"),
    libogcInclude: join(devkitpro, "libogc", "include"),
    libogcLibrary: join(devkitpro, "libogc", "lib", "wii"),
    ogcSpecs: join(devkitpro, "libogc", "share", "ogc.specs"),
  };
}

export function inspectWiiToolchain(env: NodeJS.ProcessEnv = process.env): WiiToolchainStatus {
  const paths = wiiToolchainPaths(env);
  const checks: ReadonlyArray<readonly [string, string]> = [
    ["powerpc-eabi-gcc", paths.compiler],
    ["powerpc-eabi-gcc-ar", paths.archiver],
    ["powerpc-eabi-gcc-ranlib", paths.ranlib],
    ["devkitPPC/wii_rules", paths.wiiRules],
    ["libogc headers", join(paths.libogcInclude, "gccore.h")],
    ["libogc library", join(paths.libogcLibrary, "libogc.a")],
    ["libogc specs", paths.ogcSpecs],
    ["bin2s", paths.bin2s],
    ["elf2dol", paths.elf2dol],
  ];
  const missing = checks.filter(([, path]) => !existsSync(path)).map(([name]) => name);
  return { paths, ok: missing.length === 0, missing };
}

/** Resolve and validate the local devkitPPC + libogc installation. */
export function resolveWiiToolchain(env: NodeJS.ProcessEnv = process.env): WiiBuildToolchain {
  const status = inspectWiiToolchain(env);
  if (!status.ok) {
    const source = env.DEVKITPPC?.trim() || env.DEVKITPRO?.trim() || WII_TOOLCHAIN.sdk.defaultRoot;
    throw new Error(
      `PocketJS Wii toolchain unavailable at ${source}: missing ${status.missing.join(", ")}. ` +
      "Install devkitPPC/libogc or set DEVKITPRO and DEVKITPPC.",
    );
  }
  const { paths } = status;
  return {
    ...paths,
    manifest: WII_TOOLCHAIN,
    environment: {
      ...env,
      DEVKITPRO: paths.devkitpro,
      DEVKITPPC: paths.devkitppc,
      PATH: [join(paths.devkitppc, "bin"), join(paths.devkitpro, "tools", "bin"), env.PATH]
        .filter(Boolean)
        .join(":"),
    },
  };
}

export const resolveWiiBuildToolchain = resolveWiiToolchain;
