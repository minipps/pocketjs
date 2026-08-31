/*
 * Wii's guest bridge is the platform-neutral QuickJS/HostOps bridge. Keep
 * this translation unit as a tiny target wrapper so the Wii build cannot
 * accidentally enable 3DS-only auxiliary or DevTools transport operations.
 */
#include "../../shared/qjs.c"
