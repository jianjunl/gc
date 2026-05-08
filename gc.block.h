
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
 * gc_find_block - locate a block header by its exact address.
 * (retained for future use; currently not called internally)
 */
gc_block_t* gc_find_block(void *blk);

/*
 * gc_find_block_by_ptr - return the gc_block_t that owns a user data pointer.
 * Uses an internal hash table for O(1) average lookup.
 */
gc_block_t* gc_find_block_by_ptr(void *ptr);

/* Internal hash‑table helpers – used by gc_malloc and gc_sweep */
void gc_hash_insert(void *ptr, gc_block_t *blk);
void gc_hash_remove(void *ptr);
