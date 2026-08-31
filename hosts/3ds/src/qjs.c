/* Compatibility source path for out-of-tree 3DS build scripts. The 3DS
 * Makefile compiles hosts/shared/qjs.c directly. */
#define POCKETJS_QJS_3DS 1
#include "../../shared/qjs.c"

/* The shared implementation publishes ui.__viewport from
 * ui_viewport_width()/ui_viewport_height() with
 * JS_SetPropertyStr(context, ui, "__viewport", viewport). */
