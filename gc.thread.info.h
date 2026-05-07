
/* ---------- Thread information ---------- */
typedef struct gc_thread_info {
    pthread_t           tid;
    void               *stack_bottom;
    void               *stack_top;
    ucontext_t          suspend_ctx;
    bool                ctx_valid;
    struct gc_thread_info *next;
} gc_thread_info_t;
