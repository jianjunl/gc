# libgc - A Conservative Garbage Collector for C

This is a conservative, stop-the-world, mark-and-sweep garbage collector
written entirely in C11.  It can be used as a **drop-in replacement for
malloc/calloc** in both single‑threaded and multi‑threaded programs.  
The library also provides GC‑aware wrappers for pthread synchronisation
primitives, precise root registration, finalizers, weak references,
non‑local jumps (try/throw/catch) and an optional incremental marking mode.

The project was born out of the need of my **Disp** Lisp interpreter and
was built step by step through deep discussion and pair‑debugging with
**DeepSeek**.  There is no commercial purpose – it is purely a
research/entertainment effort made available to the community.

Contributions, suggestions and bug reports are warmly welcomed.

---

## Features

- **Conservative scanning** of the data segment, stacks and registers.
- **Multi‑threaded** with a stop‑the‑world mechanism using POSIX signals
  (`SIGUSR1`/`SIGUSR2`) and semaphores.
- **Incremental marking** (optional) with write‑barrier support.
- **Finalizers** and **weak references** for precise resource management.
- **Precise root registration** for objects that the conservative scanner
  may miss (global variables, dynamic libraries, etc.).
- **Free‑list** allocator backed by `mmap/munmap` to reduce system calls.
- **Thread‑local jump stack** for non‑local exits (`try/throw/catch`
  macros based on `sigsetjmp`/`siglongjmp`).
- **GC‑aware pthread wrappers** (`gc_pthread_create`, `gc_mutex_t`,
  `gc_cond_t`, …) that automatically integrate with the collector.
- **Perfectly integrates with C11** and can be compiled with `-std=c11`.
- **Unit‑tested** – all core features are verified by the included test suite.
- **Licensed under MIT** – free for any use.

---

## Configuration Macros

Before including `gc.h` you may define these macros to control
compile‑time features:

| Macro |   Values   | Description |
|-------|------------|-------------|
| `GC_OPT_MULTITHREAD` | `0` / `1` | Enable multi‑threading support (default `1`) |
| `GC_OPT_INCREMENTAL` | `0` / `1` | Enable incremental marking (default `1`) |
| `GC_OPT_FINALIZING`  | `0` / `1` | Enable finalizers & weak references (default `1`, requires multi‑threading) |
| `GC_OPT_FREELIST`    | `0` / `1` | Enable free‑list caching (default `1`) |

The header translates these into a single internal bitmask and exposes
convenient macros `GC_MULTITHREAD`, `GC_INCREMENTAL`, `GC_FINALIZING`
and `GC_FREELIST` that are either `0` or `1`.

---

## File Structure

- **gc.h** – Public API: types, functions, configuration macros
- **gc.core.c** – gc_malloc / gc_calloc / gc_free / gc_collect / gc_dump_stats
- **gc.init.c** – Stack detection, gc_init, get_stack_top
- **gc.mark.c** – Mark phase: conservative & incremental marking, write barrier
- **gc.sweep.c** – Sweep phase, finalizers, weak references
- **gc.os.c** – Low‑level mmap‑based allocator and free‑list
- **gc.root.c** – Precise root registration (gc_add_root / gc_remove_root)
- **gc.stw.c** – Stop‑the‑world: signal handlers, thread suspension/resume
- **gc.thread.c** – Thread registration, gc_pthread_* wrappers, sync primitives
- **gc.throw.c** – try/throw/catch exception mechanism
- **gc.test.c** – Unit tests (all features covered)
- **log.h** – Tiny logging framework used by all modules
- **gc.mk** – Makefile (static & shared library, test program, install)

---

## Build & Install

A `Makefile` (`gc.mk`) is provided.

```sh
# Build static library, shared library and the test runner
make -f gc.mk

# Run unit tests
./gc.test

# Install to /usr/local (override PREFIX if needed)
sudo make -f gc.mk install
```

The installed files are:

- `${PREFIX}/lib/libgc.a` – static library
- `${PREFIX}/lib/libgc.so` – shared library
- `${PREFIX}/bin/gc.test` – test binary
- `${PREFIX}/include/gc.h` – header
- `${PREFIX}/share/man/man3/gc.3` – manual page (if provided)

Adjust `CFLAGS` inside `gc.mk` for debug builds (e.g. `-DDEBUG`).

## Quick Start
```c
#include "gc.h"

int main() {
    gc_init();          // optional, will be called automatically

    char *str = gc_malloc(100);
    snprintf(str, 100, "Hello, GC!");

    // Let the GC reclaim unreachable objects
    gc_collect();

    return 0;
}
```
### Precise roots
```c
disp_val *NIL = NULL;
gc_add_root(&NIL);   // NIL will never be collected
NIL = DISP_ALLOC(DISP_VOID);
```
### Finalizers
```c
void my_finalizer(void *obj, void *data) {
    printf("Finalizing %p\n", obj);
}

void *p = gc_malloc(256);
gc_register_finalizer(p, my_finalizer, NULL);
// p will invoke my_finalizer before being freed.
```
### Weak references
```c
gc_weak_ref_t *w = gc_make_weak(p, NULL, NULL);
// ... after collection, gc_weak_get(w) returns NULL if p is dead.
```
### Try / Throw / Catch
```c
TRY {
    THROW(42);
}
CATCH {
    printf("Caught: %d\n", THROWN);
}
END_TRY;
```
### Try / Throw / Catch (multi-levels)
```c
    TRY {
        LOG_INFO("Outer TRY start");
        
        TRY {
            LOG_INFO("  Inner TRY start");
            THROW(2);
            LOG_INFO("  This won't print");
        } CATCH {
            caught_code = THROWN;
            LOG_INFO("  Caught inner: %d", caught_code);
        } END_TRY;

        ASSERT(caught_code == 2, "exception code mismatch");

        LOG_INFO("Outer TRY continuing");
        THROW(1);
        
    } CATCH {
        caught_code = THROWN;
        LOG_INFO("Caught outer: %d", caught_code);
    } END_TRY;
```
## Testing

The test program (`gc.test.c`) exercises every major feature:

- Basic allocation & collection
- Reachable graph preservation
- Cyclic structures with precise roots
- `calloc` zero‑initialisation
- Automatic GC trigger on threshold
- Try/throw/catch exception handling
- Multi‑threaded allocation & collection
- Weak references & finalizers
- Write barrier (incremental marking)

Run `make -f gc.mk && ./gc.test` and you should see 9 successful tests.

## Origins & Philosophy
This garbage collector was created as the memory management subsystem
for **Disp**, a tiny Lisp
interpreter written in C. After trying several open‑source GCs I
decided to build my own, with only the essential features I really
needed. The whole design and debugging process was done in a
single‑person conversation with DeepSeek, which acted as a tireless
pair‑programmer over hundreds of iterations.

There is no commercial goal behind this work – it is purely a hobby
project, a chance to learn, and an attempt to give something back to
the C community. If you find it useful or have ideas for improvement,
please open an issue or a pull request. Stars and forks are always
appreciated!

## License
MIT – see the source files for the full license text.
You are free to use, modify and distribute this software for any purpose.

### Enjoy safe and automatic memory management in C!
