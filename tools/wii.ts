#!/usr/bin/env bun

// Build the private wii-dev target: resolve one manifest, build its guest,
// make a target-thinned .pocket, then link the shared Rust/QuickJS/C pieces
// into a DOL with the local devkitPPC + libogc installation.

import { createHash } from "node:crypto";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, extname, join, resolve } from "node:path";
import { encodePocketPackage } from "../contracts/spec/pocket-package.ts";
import { hostBuildEnvironment } from "../framework/src/manifest/host-build-inputs.ts";
import { canonicalJson } from "../framework/src/manifest/plan.ts";
import {
  buildGuestBundle,
  ensureQuickJsCheckout,
  mustRunCommand,
  quickJsCheckout,
  readGuestBundle,
  type GuestBundle,
  type GuestBundleRequest,
} from "./native-host-build.ts";
import { makeVariant } from "./pocket-pack.ts";
import { pocketStackCacheRoot } from "./psp-toolchain.ts";
import { resolveWiiBuildPlan, WII_DEV_TARGET_ID } from "./wii-profile.ts";
import {
  inspectWiiToolchain,
  resolveWiiToolchain,
  WII_ARCHITECTURE_FLAGS,
  WII_TOOLCHAIN,
  type WiiBuildToolchain,
} from "./wii-toolchain.ts";

const repository = resolve(new URL("..", import.meta.url).pathname);
const hostDirectory = join(repository, "hosts/wii");
const coreDirectory = join(repository, "hosts/shared/core");
const coreTarget = join(hostDirectory, "targets/powerpc-unknown-eabi.json");
const CORE_LIBRARY = "libpocketjs_core_cabi.a";
const QUICKJS_SOURCES = [
  "quickjs.c",
  "cutils.c",
  "libregexp.c",
  "libunicode.c",
  "dtoa.c",
] as const;
const QUICKJS_HEADERS = [
  "cutils.h",
  "dtoa.h",
  "libregexp-opcode.h",
  "libregexp.h",
  "libunicode-table.h",
  "libunicode.h",
  "list.h",
  "quickjs-atom.h",
  "quickjs-opcode.h",
  "quickjs.h",
] as const;

const QUICKJS_COMPILE_FLAGS = [
  ...WII_ARCHITECTURE_FLAGS,
  "-O2",
  "-D_GNU_SOURCE",
  "-DJS_NO_NAN_BOXING",
  '-DCONFIG_VERSION="pocketwii"',
  "-D__TM_GMTOFF=tm_gmtoff",
  "-include",
  "malloc.h",
  "-fno-strict-aliasing",
  "-funsigned-char",
] as const;

/* libogc has no pthread-backed 64-bit atomics, while QuickJS enables its
 * Atomics implementation for every non-Emscripten target. Keep the pinned
 * checkout pristine and apply the three platform guards to a build copy; the
 * rest of QuickJS keeps its native dispatch, stack checks, and allocator
 * paths. */
const QUICKJS_SOURCE_PATCH = "pocketjs-wii-no-atomics-v1";

function patchedQuickJsSource(
  name: string,
  sourceDirectory: string,
  buildDirectory: string,
): string {
  if (name !== "quickjs.c") return join(sourceDirectory, name);
  const source = readFileSync(join(sourceDirectory, name), "utf8");
  const patched = source
    .replace(
      "#elif defined(__PSP__) || defined(__vita__)\n#include <malloc.h>",
      "#elif defined(__PSP__) || defined(__vita__) || defined(GEKKO)\n#include <malloc.h>",
    )
    .replace(
      "#if defined(__PSP__) || defined(__vita__)\n#undef CONFIG_ATOMICS\n#endif",
      "#if defined(__PSP__) || defined(__vita__) || defined(GEKKO)\n#undef CONFIG_ATOMICS\n#endif",
    )
    .replace(
      "#if defined(__PSP__) || defined(__vita__)\n    (void)ti;",
      "#if defined(__PSP__) || defined(__vita__) || defined(GEKKO)\n    (void)ti;",
    );
  if (patched === source) {
    throw new Error(`PocketJS Wii: QuickJS ${name} no longer matches the Wii patch`);
  }
  const patchedDirectory = join(buildDirectory, "quickjs-source");
  mkdirSync(patchedDirectory, { recursive: true });
  const patchedPath = join(patchedDirectory, name);
  writeFileSync(patchedPath, patched);
  return patchedPath;
}

export interface WiiArguments {
  /** Bare app name, or empty when --manifest is supplied. */
  readonly app: string;
  readonly manifestPath?: string;
  readonly planPath?: string;
  readonly projectRoot: string;
  readonly outputDir: string;
  readonly packageDir: string;
  /** Skip tools/build.ts and reuse the verified guest output. */
  readonly skipBuild: boolean;
  /** Stop after writing the target-thinned .pocket package. */
  readonly pocketOnly: boolean;
  readonly help: boolean;
  readonly cargoArgs: readonly string[];
}

export interface WiiParseOptions {
  readonly repositoryRoot?: string;
  readonly workingDirectory?: string;
}

export function parseWiiArguments(
  argv: readonly string[],
  options: WiiParseOptions = {},
): WiiArguments {
  const root = options.repositoryRoot ?? repository;
  let app = "";
  let manifestPath: string | undefined;
  let planPath: string | undefined;
  let projectRoot = options.workingDirectory ?? process.cwd();
  let outputDir = join(root, "dist/wii/guest");
  let packageDir = join(root, "dist/wii");
  let skipBuild = false;
  let pocketOnly = false;
  let help = false;
  let forwarding = false;
  const cargoArgs: string[] = [];

  for (const arg of argv) {
    if (arg === "--") {
      forwarding = true;
    } else if (arg === "--help" || arg === "-h") {
      help = true;
    } else if (arg === "--skip-build") {
      skipBuild = true;
    } else if (arg === "--pocket-only") {
      pocketOnly = true;
    } else if (arg.startsWith("--manifest=")) {
      manifestPath = resolve(arg.slice("--manifest=".length));
    } else if (arg.startsWith("--plan=")) {
      planPath = resolve(arg.slice("--plan=".length));
    } else if (arg.startsWith("--project-root=")) {
      projectRoot = resolve(arg.slice("--project-root=".length));
    } else if (arg.startsWith("--outdir=")) {
      outputDir = resolve(arg.slice("--outdir=".length));
    } else if (arg.startsWith("--package-outdir=")) {
      packageDir = resolve(arg.slice("--package-outdir=".length));
    } else if (forwarding || arg.startsWith("-")) {
      cargoArgs.push(arg);
    } else if (!app) {
      app = arg;
    } else {
      cargoArgs.push(arg);
    }
  }

  return {
    app,
    manifestPath,
    planPath,
    projectRoot,
    outputDir,
    packageDir,
    skipBuild,
    pocketOnly,
    help,
    cargoArgs,
  };
}

const USAGE =
  "usage: bun tools/wii.ts <app> [--manifest=<pocket.json>] [--plan=<plan.json>] " +
  "[--project-root=<dir>] [--outdir=<dir>] [--package-outdir=<dir>] " +
  "[--skip-build] [--pocket-only] [-- --release]";

function manifestCandidates(app: string, projectRoot: string): string[] {
  const raw = resolve(projectRoot, app);
  const path = extname(raw) === ".json" ? raw : join(raw, "pocket.json");
  return [
    path,
    join(projectRoot, "apps", app, "pocket.json"),
    join(repository, "apps", app, "pocket.json"),
  ];
}

export function resolveWiiManifestPath(
  args: Pick<WiiArguments, "app" | "manifestPath" | "projectRoot">,
): string {
  const candidates = args.manifestPath
    ? [resolve(args.manifestPath)]
    : args.app
      ? manifestCandidates(args.app, args.projectRoot)
      : [];
  const manifest = candidates.find((candidate) => existsSync(candidate));
  if (!manifest) {
    throw new Error(
      `${USAGE}\nPocketJS Wii: provide an app name or --manifest=<pocket.json>`,
    );
  }
  return manifest;
}

function planPathFor(args: WiiArguments, manifestPath: string): string {
  if (args.planPath) return args.planPath;
  const name = basename(dirname(manifestPath)) || basename(manifestPath, ".json");
  return join(args.projectRoot, ".pocket/wii-dev", `${name}.plan.json`);
}

function guestRequest(
  args: WiiArguments,
  manifestPath: string,
  planPath: string,
): GuestBundleRequest {
  return {
    label: "PocketJS Wii guest",
    repository,
    target: WII_DEV_TARGET_ID,
    resolvePlan: resolveWiiBuildPlan,
    manifestPath,
    planPath,
    outputDirectory: args.outputDir,
  };
}

function packageGuest(
  bundle: GuestBundle,
  manifestPath: string,
  outputPath: string,
): string {
  const manifest = new Uint8Array(readFileSync(manifestPath));
  const variant = makeVariant({
    target: bundle.inputs.target,
    hostAbi: bundle.inputs.hostAbi,
    planJson: canonicalJson(bundle.plan),
    identity: {
      output: bundle.inputs.appOutput,
      id: bundle.inputs.app.id,
      title: bundle.inputs.app.title,
    },
    js: new Uint8Array(readFileSync(bundle.javaScript)),
    pak: new Uint8Array(readFileSync(bundle.pack)),
  });
  mkdirSync(dirname(outputPath), { recursive: true });
  writeFileSync(outputPath, encodePocketPackage({ manifest, variants: [variant] }));
  console.log(`PocketJS Wii: package -> ${outputPath}`);
  return outputPath;
}

function findFile(root: string, name: string): string | undefined {
  if (!existsSync(root)) return undefined;
  for (const entry of readdirSync(root, { withFileTypes: true })) {
    const path = join(root, entry.name);
    if (entry.isFile() && entry.name === name) return path;
    if (entry.isDirectory()) {
      const found = findFile(path, name);
      if (found) return found;
    }
  }
  return undefined;
}

export function buildWiiCore(
  toolchain: WiiBuildToolchain,
  outputDirectory: string,
  cargoArgs: readonly string[] = [],
): string {
  const rustup = Bun.which("rustup") ?? join(process.env.HOME ?? homedir(), ".cargo/bin/rustup");
  if (!existsSync(rustup)) {
    throw new Error("PocketJS Wii: rustup is required for the shared core build");
  }
  const cargoTarget = join(outputDirectory, "cargo-target");
  mkdirSync(outputDirectory, { recursive: true });
  mustRunCommand(
    "PocketJS Wii Rust core",
    rustup,
    [
      "run",
      WII_TOOLCHAIN.rust.toolchain,
      "cargo",
      "build",
      "-Z",
      "json-target-spec",
      "--release",
      "--locked",
      "--target",
      coreTarget,
      ...cargoArgs,
    ],
    coreDirectory,
    { ...toolchain.environment, CARGO_TARGET_DIR: cargoTarget },
  );

  const expected = join(
    cargoTarget,
    "powerpc-unknown-eabi",
    "release",
    CORE_LIBRARY,
  );
  const source = existsSync(expected) ? expected : findFile(cargoTarget, CORE_LIBRARY);
  if (!source) {
    throw new Error(`PocketJS Wii: cargo did not emit ${CORE_LIBRARY}`);
  }
  const destination = join(outputDirectory, "lib", CORE_LIBRARY);
  mkdirSync(dirname(destination), { recursive: true });
  copyFileSync(source, destination);
  return destination;
}

function quickJsCheckoutRoot(env: NodeJS.ProcessEnv): string {
  if (env.POCKETJS_WII_QUICKJS?.trim()) return resolve(env.POCKETJS_WII_QUICKJS.trim());
  return join(
    pocketStackCacheRoot(env),
    "wii/sources",
    `quickjs-rs-${WII_TOOLCHAIN.quickjs.revision}`,
  );
}

export function ensureWiiQuickJs(
  toolchain: WiiBuildToolchain,
  outputDirectory: string,
  env: NodeJS.ProcessEnv = process.env,
): string {
  const checkoutRoot = quickJsCheckoutRoot(env);
  ensureQuickJsCheckout("PocketJS Wii QuickJS", checkoutRoot, WII_TOOLCHAIN.quickjs);
  const sourceDirectory = quickJsCheckout(checkoutRoot).source;
  const files = [...QUICKJS_SOURCES, ...QUICKJS_HEADERS];
  const digest = createHash("sha256");
  digest.update(toolchain.compiler);
  digest.update(QUICKJS_COMPILE_FLAGS.join(" "));
  digest.update(QUICKJS_SOURCE_PATCH);
  for (const name of files) {
    const source = join(sourceDirectory, name);
    if (!existsSync(source)) {
      throw new Error(`PocketJS Wii: QuickJS source ${name} is missing from ${sourceDirectory}`);
    }
    digest.update(name);
    digest.update(readFileSync(source));
  }
  const stamp = digest.digest("hex");
  const libraryDirectory = join(outputDirectory, "lib");
  const includeDirectory = join(outputDirectory, "include");
  const buildDirectory = join(outputDirectory, "quickjs-build");
  const archive = join(libraryDirectory, "libquickjs.a");
  const stampPath = join(libraryDirectory, ".quickjs.stamp");
  mkdirSync(libraryDirectory, { recursive: true });
  mkdirSync(includeDirectory, { recursive: true });
  for (const name of QUICKJS_HEADERS) {
    copyFileSync(join(sourceDirectory, name), join(includeDirectory, name));
  }
  if (existsSync(archive) && existsSync(stampPath) && readFileSync(stampPath, "utf8").trim() === stamp) {
    return archive;
  }

  rmSync(buildDirectory, { recursive: true, force: true });
  mkdirSync(buildDirectory, { recursive: true });
  const objects: string[] = [];
  for (const name of QUICKJS_SOURCES) {
    const object = join(buildDirectory, name.replace(/\.c$/, ".o"));
    objects.push(object);
    mustRunCommand(
      "PocketJS Wii QuickJS",
      toolchain.compiler,
      [
        ...QUICKJS_COMPILE_FLAGS,
        "-I",
        sourceDirectory,
        "-c",
        patchedQuickJsSource(name, sourceDirectory, buildDirectory),
        "-o",
        object,
      ],
      buildDirectory,
      toolchain.environment,
    );
  }
  mustRunCommand(
    "PocketJS Wii QuickJS archive",
    toolchain.archiver,
    ["rcsD", archive, ...objects],
    buildDirectory,
    toolchain.environment,
  );
  writeFileSync(stampPath, `${stamp}\n`);
  return archive;
}

export function buildWiiNative(
  args: WiiArguments,
  bundle: GuestBundle,
  packagePath: string,
  toolchain: WiiBuildToolchain,
): string {
  const outputDirectory = resolve(args.packageDir);
  const coreLibrary = buildWiiCore(toolchain, outputDirectory, args.cargoArgs);
  const quickJsLibrary = ensureWiiQuickJs(toolchain, outputDirectory, toolchain.environment);
  const includeDirectory = join(outputDirectory, "include");
  mkdirSync(includeDirectory, { recursive: true });
  copyFileSync(join(repository, "hosts/shared/pocket_core.h"), join(includeDirectory, "pocket_core.h"));
  copyFileSync(join(repository, "hosts/shared/qjs.h"), join(includeDirectory, "qjs.h"));
  copyFileSync(join(repository, "hosts/wii/include/pocket_wii.h"), join(includeDirectory, "pocket_wii.h"));

  const buildDirectory = join(outputDirectory, "build", bundle.inputs.appOutput);
  const dol = join(outputDirectory, `${bundle.inputs.appOutput}.dol`);
  const wiiLibrary = join(outputDirectory, "lib", "libpocketjs_wii.a");
  const environment: NodeJS.ProcessEnv = {
    ...toolchain.environment,
    ...hostBuildEnvironment(bundle.inputs, {
      outputDirectory,
      embedApp: true,
    }),
    BUILD: buildDirectory,
    OUT: dol,
    POCKETJS_BUILD_DIR: buildDirectory,
    POCKETJS_OUT_DOL: dol,
    POCKETJS_CORE_LIB: coreLibrary,
    POCKETJS_WII_LIB: wiiLibrary,
    POCKETJS_QUICKJS_DIR: includeDirectory,
    POCKETJS_QUICKJS_LIB: quickJsLibrary,
    POCKETJS_APP_POCKET: packagePath,
  };
  const make = Bun.which("make") ?? "make";
  mustRunCommand(
    "PocketJS Wii host",
    make,
    ["-f", join(hostDirectory, "Makefile")],
    hostDirectory,
    environment,
  );
  if (!existsSync(dol)) throw new Error(`PocketJS Wii: make did not emit ${dol}`);
  return dol;
}

export function wiiDoctor(env: NodeJS.ProcessEnv = process.env): boolean {
  const status = inspectWiiToolchain(env);
  console.log(`PocketJS Wii toolchain: ${status.paths.devkitpro}`);
  if (status.ok) {
    console.log("  [ok] devkitPPC, libogc, bin2s and elf2dol");
  } else {
    for (const missing of status.missing) console.log(`  [missing] ${missing}`);
  }
  return status.ok;
}

export function buildWii(argv: readonly string[] = process.argv.slice(2)): void {
  if (argv[0] === "doctor") {
    process.exitCode = wiiDoctor() ? 0 : 1;
    return;
  }
  const args = parseWiiArguments(argv);
  if (args.help) {
    console.log(USAGE);
    return;
  }
  const manifestPath = resolveWiiManifestPath(args);
  const planPath = planPathFor(args, manifestPath);
  const request = guestRequest(args, manifestPath, planPath);
  const bundle = args.skipBuild ? readGuestBundle(request) : buildGuestBundle(request);
  const packagePath = packageGuest(
    bundle,
    manifestPath,
    join(args.packageDir, `${bundle.inputs.appOutput}.pocket`),
  );
  if (args.pocketOnly) return;

  const toolchain = resolveWiiToolchain();
  const dol = buildWiiNative(args, bundle, packagePath, toolchain);
  console.log(`PocketJS Wii: DOL -> ${dol}`);
}

if (import.meta.main) {
  try {
    buildWii();
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  }
}
