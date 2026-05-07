
/* ---------- Block metadata ---------- */
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
