
/* -------------------------- Precise roots  --------------------------- */
typedef struct gc_root {
    void              **ptr_addr;   /* address of the pointer variable   */
    struct gc_root     *next;       /* sibling in the same child list    */
    struct gc_root     *more;       /* first child (head of child list)  */
    struct gc_root     *prev;       /* temporary stack link (all phases) */
} gc_root_t;
