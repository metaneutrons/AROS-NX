/* Execute the actual rom/exec/remtask.c body with narrow platform mocks. */
#define _DEFAULT_SOURCE 1
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Let remtask.c's real include directives resolve, but keep their ABI bodies
 * out of this host fixture. The function body below is included verbatim. */
#define EXEC_EXECBASE_H 1
#define EXEC_TASKS_H 1
#define AROS_LIBCALL_H 1
#define PROTO_EXEC_H 1
#define _AROS_SYMBOLSETS_H 1
#define _ETASK_H 1
#define __EXEC_INTERN_H__ 1
#define _EXEC_UTIL_H 1
#define _EXEC_DEBUG_H_ 1

typedef void *APTR;
typedef uint32_t ULONG;
typedef uintptr_t IPTR;

#define DECLARESET(set)

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    char *ln_Name;
};

struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct Task
{
    struct Node tc_Node;
    uint32_t tc_State;
    struct List tc_MemEntry;
    APTR tc_UserData;
};

struct ETask
{
    void *et_RegFrame;
};

struct IntETask
{
    struct ETask iet_ETask;
    uintptr_t iet_CpuNumber;
};

struct MemList
{
    struct Node ml_Node;
};

struct Message
{
    struct Node mn_Node;
};

struct MsgPort
{
    unsigned int unused;
};

struct ExecBase
{
    struct MsgPort *ServicePort;
};

#define TS_INVALID UINT32_C(0)
#define TS_REMOVED UINT32_C(4)
#define TS_TOMBSTONED UINT32_C(5)
#define PrivExecBase(base) ((struct ExecBase *)(base))
#define IntETask(etask) ((struct IntETask *)(etask))

#define AROS_LHA(type, name, reg) type name
#define AROS_LH1(type, name, arg1, base_type, base_name, offset, library) \
    type name(arg1, base_type base_name)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT

static struct Task *mock_current_task;
#if !defined(P4_TEST_NORMAL)
static struct Task *expected_target;
static struct Task *expected_requester;
static int allowed_result;
static int self_transfer;
#endif
static unsigned int checks;
static unsigned int failures;
static char events[32];
static size_t event_count;
static unsigned int allowed_calls;
static unsigned int self_calls;
static unsigned int external_calls;
static unsigned int debug_calls;
static unsigned int forbid_calls;
static unsigned int permit_calls;
static unsigned int remove_calls;
static unsigned int get_etask_calls;
static unsigned int delete_context_calls;
static unsigned int cleanup_etask_calls;
static unsigned int internal_putmsg_calls;
static unsigned int remhead_calls;
static unsigned int freeentry_calls;
static unsigned int dispatch_calls;
static unsigned int disable_calls;
static int td_nest;
static struct ExecBase test_sysbase;
#if !defined(P4_TEST_NORMAL)
static void *task_page;
static size_t task_page_size;
static struct Task *page_task;
#endif

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            ++failures;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
        }                                                                    \
    } while (0)

static void record_event(char event)
{
    if (event_count < sizeof(events))
        events[event_count++] = event;
    else
        ++failures;
}

static void expect_events(const char *expected)
{
    size_t expected_size = strlen(expected);
    CHECK(event_count == expected_size);
    if (event_count == expected_size)
        CHECK(memcmp(events, expected, expected_size) == 0);
}

static void mock_debug(const char *format, ...)
{
    (void)format;
    ++debug_calls;
    record_event('d');
}

#define DREMTASK(...) mock_debug(__VA_ARGS__)

void Forbid(void);
void Permit(void);
void Remove(struct Node *node);
struct ETask *GetETask(struct Task *task);
void KrnDeleteContext(void *context);
void CleanupETask(struct Task *task);
struct Node *RemHead(struct List *list);
void FreeEntry(struct MemList *entry);
void InternalPutMsg(struct MsgPort *port, struct Message *message,
                    struct ExecBase *base);
void Disable(void);
void KrnDispatch(void);
void mock_forbid(void);
void mock_permit(void);
#if !defined(P4_TEST_NORMAL)
static int mock_task_remove_allowed(struct Task *task);
static void mock_remove_self(struct Task *task) __attribute__((noreturn));
static void mock_remove_external(struct Task *task, struct Task *requester);
#endif

#if !defined(P4_TEST_NORMAL)
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define EXEC_PLATFORM_TASK_REMOVE_ALLOWED(task) \
    mock_task_remove_allowed((task))
#define EXEC_PLATFORM_REMOVE_SELF(task) mock_remove_self((task))
#define EXEC_PLATFORM_REMOVE_EXTERNAL(task, requester) \
    mock_remove_external((task), (requester))
#endif

#define GET_THIS_TASK (mock_current_task)
#define Forbid mock_forbid
#define Permit mock_permit
#define TDNESTCOUNT_SET(value) do { td_nest = (int)(value); } while (0)

#include "../../../../rom/exec/remtask.c"

#include <setjmp.h>

static jmp_buf transfer_env;

static void clear_mock_counters(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0u;
    allowed_calls = 0u;
    self_calls = 0u;
    external_calls = 0u;
    debug_calls = 0u;
    forbid_calls = 0u;
    permit_calls = 0u;
    remove_calls = 0u;
    get_etask_calls = 0u;
    delete_context_calls = 0u;
    cleanup_etask_calls = 0u;
    internal_putmsg_calls = 0u;
    remhead_calls = 0u;
    freeentry_calls = 0u;
    dispatch_calls = 0u;
    disable_calls = 0u;
    td_nest = 0;
}

#if !defined(P4_TEST_NORMAL)
static void *make_task_page(int protection)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *mapping;

    if (page_size <= 0)
        return NULL;
    task_page_size = (size_t)page_size;
    mapping = mmap(NULL, task_page_size, protection,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
    {
        task_page_size = 0u;
        return NULL;
    }
    task_page = mapping;
    page_task = (struct Task *)mapping;
    return mapping;
}

static void free_task_page(void)
{
    if (task_page == NULL)
        return;
    if (munmap(task_page, task_page_size) != 0)
    {
        ++failures;
        perror("munmap task");
    }
    task_page = NULL;
    task_page_size = 0u;
    page_task = NULL;
}

static void reset_intercept_case(void)
{
    clear_mock_counters();
    allowed_result = 0;
    self_transfer = 0;
    expected_target = NULL;
    expected_requester = NULL;
    mock_current_task = NULL;
    memset(&test_sysbase, 0, sizeof(test_sysbase));
}

static int invoke_remtask(struct Task *target)
{
    int transfer = setjmp(transfer_env);
    if (transfer == 0)
    {
        RemTask(target, &test_sysbase);
        return 0;
    }
    return transfer;
}
#endif

void mock_forbid(void)
{
    ++forbid_calls;
    record_event('F');
}

void mock_permit(void)
{
    ++permit_calls;
    record_event('P');
}

void Remove(struct Node *node)
{
    ++remove_calls;
    (void)node;
}

struct ETask *GetETask(struct Task *task)
{
    ++get_etask_calls;
    (void)task;
    return NULL;
}

void KrnDeleteContext(void *context)
{
    ++delete_context_calls;
    (void)context;
}

void CleanupETask(struct Task *task)
{
    ++cleanup_etask_calls;
    (void)task;
}

struct Node *RemHead(struct List *list)
{
    ++remhead_calls;
    (void)list;
    return NULL;
}

void FreeEntry(struct MemList *entry)
{
    ++freeentry_calls;
    (void)entry;
}

void InternalPutMsg(struct MsgPort *port, struct Message *message,
                    struct ExecBase *base)
{
    ++internal_putmsg_calls;
    (void)port;
    (void)message;
    (void)base;
}

void Disable(void)
{
    ++disable_calls;
}

void KrnDispatch(void)
{
    ++dispatch_calls;
    longjmp(transfer_env, 4);
}

#if !defined(P4_TEST_NORMAL)
static int mock_task_remove_allowed(struct Task *task)
{
    ++allowed_calls;
    record_event('A');
    CHECK(task == expected_target);
    return allowed_result;
}

static void mock_remove_self(struct Task *task)
{
    ++self_calls;
    record_event('S');
    CHECK(task == expected_target);
    self_transfer = 1;
    longjmp(transfer_env, 2);
}

static void mock_remove_external(struct Task *task, struct Task *requester)
{
    ++external_calls;
    record_event('X');
    CHECK(task == expected_target);
    CHECK(requester == expected_requester);
    CHECK(debug_calls == 0u && forbid_calls == 0u);
    CHECK(get_etask_calls == 0u && delete_context_calls == 0u);
    CHECK(cleanup_etask_calls == 0u && freeentry_calls == 0u);
}
#endif

#if !defined(P4_TEST_NORMAL)
static void test_absent_identity_precheck(void)
{
    struct Task requester = { 0 };
    int transfer;

    reset_intercept_case();
    mock_current_task = &requester;
    expected_requester = &requester;
    CHECK(make_task_page(PROT_NONE) != NULL);
    expected_target = page_task;
    allowed_result = 0;
    transfer = invoke_remtask(expected_target);
    CHECK(transfer == 0);
    expect_events("A");
    CHECK(allowed_calls == 1u && self_calls == 0u && external_calls == 0u);
    CHECK(debug_calls == 0u && forbid_calls == 0u && permit_calls == 0u);
    CHECK(get_etask_calls == 0u && delete_context_calls == 0u);
    CHECK(cleanup_etask_calls == 0u && remhead_calls == 0u &&
          freeentry_calls == 0u);
    free_task_page();
}

static void test_protected_current_identity_precheck(void)
{
    int transfer;

    reset_intercept_case();
    CHECK(make_task_page(PROT_NONE) != NULL);
    mock_current_task = page_task;
    expected_requester = page_task;
    expected_target = page_task;
    allowed_result = 0;
    transfer = invoke_remtask(NULL);
    CHECK(transfer == 0);
    expect_events("A");
    CHECK(allowed_calls == 1u && self_calls == 0u && external_calls == 0u);
    CHECK(debug_calls == 0u && forbid_calls == 0u && permit_calls == 0u);
    CHECK(get_etask_calls == 0u && cleanup_etask_calls == 0u);
    free_task_page();
}

static void test_external_hook_returns_before_generic_cleanup(void)
{
    struct Task requester = { 0 };
    int transfer;

    reset_intercept_case();
    mock_current_task = &requester;
    expected_requester = &requester;
    CHECK(make_task_page(PROT_NONE) != NULL);
    expected_target = page_task;
    allowed_result = 1;
    transfer = invoke_remtask(expected_target);
    CHECK(transfer == 0);
    expect_events("AX");
    CHECK(allowed_calls == 1u && external_calls == 1u && self_calls == 0u);
    CHECK(debug_calls == 0u && forbid_calls == 0u && permit_calls == 0u);
    CHECK(remove_calls == 0u && get_etask_calls == 0u);
    CHECK(delete_context_calls == 0u && cleanup_etask_calls == 0u);
    CHECK(internal_putmsg_calls == 0u && remhead_calls == 0u);
    CHECK(freeentry_calls == 0u && dispatch_calls == 0u && disable_calls == 0u);
    free_task_page();
}

static void test_self_hook_precedes_external_hook(void)
{
    int transfer;

    reset_intercept_case();
    CHECK(make_task_page(PROT_NONE) != NULL);
    mock_current_task = page_task;
    expected_requester = page_task;
    expected_target = page_task;
    allowed_result = 1;
    transfer = invoke_remtask(expected_target);
    CHECK(transfer == 2);
    expect_events("AS");
    CHECK(allowed_calls == 1u && self_calls == 1u);
    CHECK(external_calls == 0u && self_transfer != 0);
    CHECK(debug_calls == 0u && forbid_calls == 0u && permit_calls == 0u);
    CHECK(get_etask_calls == 0u && cleanup_etask_calls == 0u);
    free_task_page();
}

static void test_null_target_resolves_to_current_before_intercepts(void)
{
    int transfer;

    reset_intercept_case();
    CHECK(make_task_page(PROT_NONE) != NULL);
    mock_current_task = page_task;
    expected_requester = page_task;
    expected_target = page_task;
    allowed_result = 1;
    transfer = invoke_remtask(NULL);
    CHECK(transfer == 2);
    expect_events("AS");
    CHECK(allowed_calls == 1u && self_calls == 1u && external_calls == 0u);
    CHECK(debug_calls == 0u && forbid_calls == 0u);
    free_task_page();
}
#endif

#if defined(P4_TEST_NORMAL)
static void test_normal_generic_selection(void)
{
    struct Task removed = { 0 };

    clear_mock_counters();
    removed.tc_Node.ln_Name = (char *)"removed";
    removed.tc_State = TS_REMOVED;
    mock_current_task = NULL;
    RemTask(&removed, &test_sysbase);
    CHECK(debug_calls == 2u);
    CHECK(forbid_calls == 1u && permit_calls == 1u);
    CHECK(allowed_calls == 0u && self_calls == 0u && external_calls == 0u);
    CHECK(get_etask_calls == 0u && cleanup_etask_calls == 0u);
    CHECK(freeentry_calls == 0u && dispatch_calls == 0u);
    CHECK(removed.tc_State == TS_REMOVED);
    expect_events("dFPd");
}
#endif

int main(void)
{
#if defined(P4_TEST_NORMAL)
    test_normal_generic_selection();
#else
    test_absent_identity_precheck();
    test_protected_current_identity_precheck();
    test_external_hook_returns_before_generic_cleanup();
    test_self_hook_precedes_external_hook();
    test_null_target_resolves_to_current_before_intercepts();
#endif
    if (failures != 0u)
    {
        fprintf(stderr, "RemTask intercept: %u failures across %u checks\n",
                failures, checks);
        return 1;
    }
    printf("RemTask intercept: %u checks passed\n", checks);
    return 0;
}
