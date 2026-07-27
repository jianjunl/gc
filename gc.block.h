
/* --------- metadata ---------- */
typedef struct gc_block {
    void                 *ptr;
    size_t                size;
    bool                  marked;
    struct gc_block      *next;
#if GC_INCREMENTAL
    struct gc_block      *gray_next;   // gray list
#endif // GC_INCREMENTAL
#if GC_FINALIZING
    gc_finalizer_t       *finalizers;  // finalizer list
#endif // GC_FINALIZING
    const gc_type_info_t *type_info;   // typed allocation layout
} gc_block_t;

/*
 * gc_find_block - return the gc_block_t that owns a user data pointer.
 * Uses an internal hash table for O(1) average lookup.
 */
gc_block_t* gc_find_block(void *ptr);

/* Internal hash‑table helpers – used by gc_malloc and gc_sweep */
void gc_block_insert(void *ptr, gc_block_t *blk);
void gc_block_remove(void *ptr);
