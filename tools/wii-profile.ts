import {
  POCKET_CAPABILITIES,
  definePlatformContractRegistry,
  defineTargetRegistry,
} from "../contracts/spec/platforms.ts";
import type { ResolvedBuildPlan } from "../framework/src/manifest/plan.ts";
import { validateAndResolveBuildPlan } from "../framework/src/manifest/resolve.ts";

/**
 * Transitional Nintendo Wii profile used by `bun run wii`.
 *
 * The profile stays private until the host has a hardware acceptance pass.
 * Wii homebrew presents one fixed 640x480 takeover surface. PocketJS keeps
 * its standard 480x272 logical surface and presents it in that output; a Wii
 * Remote's buttons are the only input guaranteed by every supported
 * controller setup.
 */
export const WII_DEV_TARGET_ID = "wii-dev";
export const WII_DEV_HOST_ABI = 9;
export const WII_LOGICAL_VIEWPORT = [480, 272] as const;
export const WII_PHYSICAL_VIEWPORT = [640, 480] as const;
/** Compatibility alias: WII_VIEWPORT is the app-facing logical surface. */
export const WII_VIEWPORT = WII_LOGICAL_VIEWPORT;

export const WII_DEV_CONTRACTS = definePlatformContractRegistry(
  POCKET_CAPABILITIES,
  defineTargetRegistry({
    [WII_DEV_TARGET_ID]: {
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
    },
  }),
);

export function resolveWiiBuildPlan(input: unknown): ResolvedBuildPlan {
  const resolution = validateAndResolveBuildPlan(
    input,
    { target: WII_DEV_TARGET_ID },
    WII_DEV_CONTRACTS,
  );
  if (!resolution.ok) {
    throw new Error(
      `pocket wii: manifest did not resolve: ${resolution.diagnostics
        .map((diagnostic) => `${diagnostic.path || "/"}: ${diagnostic.message}`)
        .join("; ")}`,
    );
  }
  return resolution.plan;
}
