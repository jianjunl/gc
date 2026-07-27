
/* -------------------------- Precise roots  --------------------------- */
typedef struct gc_root {
    void              **ptr_addr;   /* address of the pointer variable   */
    struct gc_root     *next;       /* next root in the list             */
} gc_root_t;
