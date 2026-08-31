//! Global allocator over the host C runtime's heap, plus the panic and
//! allocation-error handlers a `no_std` staticlib has to provide itself.
//!
//! The C runtimes used by the supported console hosts provide `malloc`,
//! `memalign`, `realloc` and `free`. `malloc` is used for ordinary layouts;
//! over-aligned allocations use `memalign` and are grown by copy because a C
//! `realloc` call cannot preserve that stronger alignment.

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;

/// Alignment the supported newlib heaps guarantee for `malloc`.
const C_MALLOC_ALIGNMENT: usize = 8;

#[inline]
const fn c_allocator_supports_alignment(alignment: usize) -> bool {
    alignment <= C_MALLOC_ALIGNMENT
}

unsafe extern "C" {
    fn malloc(size: usize) -> *mut c_void;
    fn memalign(alignment: usize, size: usize) -> *mut c_void;
    fn realloc(ptr: *mut c_void, size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
    fn abort() -> !;
}

struct CAllocator;

unsafe impl GlobalAlloc for CAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if c_allocator_supports_alignment(layout.align()) {
            malloc(layout.size().max(1)).cast()
        } else {
            memalign(layout.align(), layout.size().max(1)).cast()
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr.cast());
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        // `realloc` only preserves malloc's own alignment. An over-aligned
        // block came from `memalign`, so grow it by hand.
        if c_allocator_supports_alignment(layout.align()) {
            return realloc(ptr.cast(), size.max(1)).cast();
        }
        let grown: *mut u8 = memalign(layout.align(), size.max(1)).cast();
        if !grown.is_null() {
            core::ptr::copy_nonoverlapping(ptr, grown, layout.size().min(size));
            free(ptr.cast());
        }
        grown
    }
}

#[global_allocator]
static ALLOCATOR: CAllocator = CAllocator;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { abort() }
}

#[alloc_error_handler]
fn allocation_error(_layout: Layout) -> ! {
    unsafe { abort() }
}
