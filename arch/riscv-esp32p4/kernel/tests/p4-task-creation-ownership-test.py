#!/usr/bin/env python3
"""Qualify the extracted NewAddTask/NewCreateTaskA ownership handoff."""
from pathlib import Path
import argparse
import re
import runpy
import subprocess
import tempfile


repo = Path(__file__).resolve().parents[4]
base_path = Path(__file__).with_name("p4-task-create-failure-test.py")
base = runpy.run_path(str(base_path))


def replace_once(source: str, old: str, new: str, label: str) -> str:
    if source.count(old) != 1:
        raise RuntimeError(f"expected one {label} anchor; found {source.count(old)}")
    return source.replace(old, new, 1)


fixture = base["fixture"]
fixture = replace_once(
    fixture,
    "#include <string.h>\n",
    "#include <string.h>\n"
    "#include <setjmp.h>\n"
    "#include <sys/mman.h>\n"
    "#include <unistd.h>\n"
    "#include \"p4_task_creation.h\"\n",
    "standard includes",
)
fixture = replace_once(
    fixture,
    "#define TAG_DONE 0u\n",
    "#define TAG_DONE 0u\n#define TAG_MORE 12u\n",
    "TAG_MORE definition",
)
fixture = replace_once(
    fixture,
    "struct ETask { ULONG dummy; };\n",
    "struct ETask { ULONG dummy; void *et_RegFrame; };\n",
    "ETask register-frame field",
)
fixture = replace_once(
    fixture,
    "static size_t generatedNameEntriesFreed;\n",
    "static size_t generatedNameEntriesFreed;\n"
    "static size_t primaryFreeCalls;\n"
    "static unsigned recordCreateCalls;\n"
    "static unsigned recordReleaseCalls;\n"
    "static unsigned publishCalls;\n"
    "static unsigned deleteContextCalls;\n"
    "static unsigned fatalCalls;\n"
    "#if P4_TEST_WITH_SCOPE\n"
    "static unsigned fatalRefusedSnapshot;\n"
    "#endif\n"
    "static unsigned vectorCalls;\n"
    "static unsigned resolverCalls;\n"
    "static unsigned resolverAccepts;\n"
    "static unsigned scopeBeginCalls;\n"
    "static unsigned scopeEndCalls;\n"
    "static unsigned scopeRefusedCalls;\n"
    "static ULONG vectorMode;\n"
    "static ULONG vectorReturnedOwnership;\n"
    "static ULONG vectorOwnershipAfter;\n"
    "static ULONG poisonBeforeBody;\n"
    "static ULONG poisonValue;\n"
    "static ULONG armResolverForVector;\n"
    "static ULONG inspectMergedTags;\n"
    "static ULONG expectedGenerationTag;\n"
    "static ULONG resolverGeneration;\n"
    "static ULONG *resolverOutput;\n"
    "static struct Task *resolverOwner;\n"
    "static struct Task *resolverTarget;\n"
    "static int resolverArmed;\n"
    "static ULONG scopeBeginRefuses;\n"
    "static ULONG scopeEndRefuses;\n"
    "static ULONG scopeGenerationSerial;\n"
    "static ULONG scopeRefusalFreeCount;\n"
    "static ULONG scopeEndValue;\n"
    "static ULONG nestedConstructorInVector;\n"
    "static ULONG nestedConstructorDepth;\n"
    "static struct Task *scopeTarget;\n"
    "static struct p4_creation_channel *scopeFrame;\n"
    "static struct MemList *serviceOwnedList;\n"
    "static ULONG *lastVectorOutput;\n"
    "static ULONG lastVectorGeneration;\n"
    "static struct Task *lastVectorTarget;\n"
    "static struct TagItem *lastVectorTagList;\n"
    "static APTR lastVectorInitialPC;\n"
    "static APTR lastVectorFinalPC;\n"
    "static struct Hook *lastLaunchHook;\n"
    "static const struct TagItem *expectedCallerTags;\n"
    "static ULONG *expectedCallerCollision;\n"
    "static ULONG expectedCallerCollisionValue;\n"
    "static struct Hook *expectedPrelaunchHook;\n"
    "#if P4_TEST_WITH_SCOPE\n"
    "static void checkCreationTagChain(const struct TagItem *);\n"
    "#else\n"
    "static void checkOriginalCreationTags(const struct TagItem *);\n"
    "#endif\n"
    "#if P4_TEST_WITH_RESOLVER\n"
    "static ULONG *mockCreationOwnershipResolve(IPTR, IPTR, struct Task *);\n"
    "#endif\n"
    "#if P4_TEST_WITH_SCOPE\n"
    "static BOOL mockCreationScopeBegin(struct Task *, struct p4_creation_channel *);\n"
    "static BOOL mockCreationScopeEnd(ULONG, struct p4_creation_channel *);\n"
    "static void mockCreationScopeRefused(void);\n"
    "#endif\n"
    "static int MockTaskRecordCreate(struct Task *);\n"
    "static void MockTaskRecordRelease(struct Task *);\n"
    "#if P4_TEST_WITH_SCOPE\n"
    "static void MockTaskFatal(void);\n"
    "#endif\n"
    "static void MockDeleteContext(void *);\n"
    "static void MockPublishConstructedETask(struct Task *, struct Task *,"
    " struct ExecBase *);\n",
    "ownership globals/prototypes",
)
fixture = replace_once(
    fixture,
    "static void FreeEntry(struct MemList *entry)\n{\n    UWORD i;\n",
    "static void FreeEntry(struct MemList *entry)\n{\n    UWORD i;\n"
    "    if (entry == watchedPrimary)\n        primaryFreeCalls++;\n",
    "primary MemList free counter",
)
fixture = replace_once(
    fixture,
    "static IPTR LibGetTagData(ULONG tag, IPTR defaultValue,\n"
    "                          const struct TagItem *tags)\n{\n"
    "    (void)tag;\n"
    "    (void)tags;\n"
    "    return defaultValue;\n"
    "}\n\n"
    "static struct TagItem *LibNextTagItem(struct TagItem **state)\n{\n"
    "    struct TagItem *tag;\n"
    "    if (!state || !*state || (*state)->ti_Tag == TAG_DONE)\n"
    "        return NULL;\n"
    "    tag = *state;\n"
    "    *state = tag + 1;\n"
    "    return tag;\n"
    "}\n",
    "static struct TagItem *LibNextTagItem(struct TagItem **state);\n"
    "static IPTR LibGetTagData(ULONG tag, IPTR defaultValue,\n"
    "                          const struct TagItem *tags)\n{\n"
    "    struct TagItem *state = (struct TagItem *)(uintptr_t)tags;\n"
    "    struct TagItem *item;\n"
    "    while ((item = LibNextTagItem(&state)) != NULL)\n"
    "    {\n"
    "        if (item->ti_Tag == tag)\n"
    "            return item->ti_Data;\n"
    "    }\n"
    "    return defaultValue;\n"
    "}\n\n"
    "static struct TagItem *LibNextTagItem(struct TagItem **state)\n{\n"
    "    struct TagItem *tag;\n"
    "    while (state && *state)\n"
    "    {\n"
    "        tag = *state;\n"
    "        if (tag->ti_Tag == TAG_DONE)\n"
    "        {\n"
    "            *state = NULL;\n"
    "            return NULL;\n"
    "        }\n"
    "        if (tag->ti_Tag == TAG_MORE)\n"
    "        {\n"
    "            *state = (struct TagItem *)(uintptr_t)tag->ti_Data;\n"
    "            continue;\n"
    "        }\n"
    "        *state = tag + 1;\n"
    "        return tag;\n"
    "    }\n"
    "    return NULL;\n"
    "}\n",
    "tag iteration mocks",
)
fixture = replace_once(
    fixture,
    "static BOOL PrepareContext(struct Task *task, APTR initialPC, APTR finalPC,\n"
    "                           const struct TagItem *tagList,\n"
    "                           struct ExecBase *base)\n{\n"
    "    (void)task;\n"
    "    (void)initialPC;\n"
    "    (void)finalPC;\n"
    "    (void)tagList;\n"
    "    (void)base;\n"
    "    return !prepareContextFails;\n"
    "}\n",
    "static BOOL PrepareContext(struct Task *task, APTR initialPC, APTR finalPC,\n"
    "                           const struct TagItem *tagList,\n"
    "                           struct ExecBase *base)\n{\n"
    "    (void)task;\n"
    "    (void)initialPC;\n"
    "    (void)finalPC;\n"
    "    (void)base;\n"
    "    if (inspectMergedTags)\n"
    "    {\n"
    "#if P4_TEST_WITH_SCOPE\n"
    "        checkCreationTagChain(tagList);\n"
    "#else\n"
    "        checkOriginalCreationTags(tagList);\n"
    "#endif\n"
    "    }\n"
    "    return !prepareContextFails;\n"
    "}\n",
    "PrepareContext tag inspection",
)
fixture = replace_once(
    fixture,
    "static void TaskLaunch(struct Task *parent, struct Task *task,\n"
    "                       struct Hook *preLaunchHook)\n{\n"
    "    (void)parent;\n"
    "    (void)preLaunchHook;\n",
    "static void TaskLaunch(struct Task *parent, struct Task *task,\n"
    "                       struct Hook *preLaunchHook)\n{\n"
    "    (void)parent;\n"
    "    lastLaunchHook = preLaunchHook;\n",
    "TaskLaunch hook observation",
)

ownership_config = r'''
#define EXEC_PLATFORM_ETASK_STAGED 1
#define EXEC_PLATFORM_TASK_RECORD_CREATE(task) MockTaskRecordCreate(task)
#define EXEC_PLATFORM_TASK_RECORD_RELEASE(task) MockTaskRecordRelease(task)
#if P4_TEST_WITH_SCOPE
#define EXEC_PLATFORM_TASK_FATAL() MockTaskFatal()
#else
#define EXEC_PLATFORM_TASK_FATAL() abort()
#endif
#define GetETask(task) ((task)->tc_UnionETask.tc_ETask)
#define KrnDeleteContext(frame) MockDeleteContext(frame)
#if P4_TEST_WITH_RESOLVER
#define EXEC_PLATFORM_CREATION_OWNERSHIP_RESOLVE(raw, generation, target) \
    mockCreationOwnershipResolve(raw, generation, target)
#endif
#if P4_TEST_WITH_SCOPE
#define EXEC_PLATFORM_CREATION_SCOPE_BEGIN(target, frame) \
    mockCreationScopeBegin(target, frame)
#define EXEC_PLATFORM_CREATION_SCOPE_END(generation, frame) \
    mockCreationScopeEnd(generation, frame)
#define EXEC_PLATFORM_CREATION_SCOPE_REFUSED() mockCreationScopeRefused()
#endif

#if P4_TEST_WITH_SCOPE
static jmp_buf fatalJump;
static int fatalJumpArmed;
#endif
static int recordCreationFails;

static int MockTaskRecordCreate(struct Task *task)
{
    CHECK(task != NULL && task->tc_UnionETask.tc_ETask != NULL);
    recordCreateCalls++;
    return !recordCreationFails;
}

static void MockTaskRecordRelease(struct Task *task)
{
    CHECK(task != NULL && task->tc_UnionETask.tc_ETask != NULL);
    recordReleaseCalls++;
}

static void MockDeleteContext(void *frame)
{
    CHECK(frame != NULL);
    deleteContextCalls++;
}

static void MockPublishConstructedETask(struct Task *task,
                                        struct Task *parent,
                                        struct ExecBase *base)
{
    CHECK(task != NULL && parent == currentTask && base == SysBase);
    publishCalls++;
}

static void Exec_PublishConstructedETask(struct Task *task,
                                         struct Task *parent,
                                         struct ExecBase *base)
{
    MockPublishConstructedETask(task, parent, base);
}

#if P4_TEST_WITH_SCOPE
static void MockTaskFatal(void)
{
    fatalCalls++;
    fatalRefusedSnapshot = scopeRefusedCalls;
    if (!fatalJumpArmed)
        abort();
    fatalJumpArmed = 0;
    longjmp(fatalJump, 1);
}
#endif

struct MockCreationScope
{
    struct Task *owner;
    struct Task *target;
    struct p4_creation_channel *frame;
    ULONG generation;
    ULONG claimed;
};

#if P4_TEST_WITH_SCOPE
static struct MockCreationScope mockScopeStack[8];
static size_t mockScopeDepth;
static struct Task *mockScopeBeginTargets[8];
static ULONG mockScopeBeginGenerations[8];
static ULONG mockScopeEndGenerations[8];
#endif

#if P4_TEST_WITH_RESOLVER
static ULONG resolverClaimed;

static ULONG *mockCreationOwnershipResolve(IPTR raw, IPTR generation,
                                            struct Task *target)
{
#if P4_TEST_WITH_SCOPE
    struct MockCreationScope *top;
#endif
    resolverCalls++;
    if (!resolverArmed || resolverOwner != currentTask ||
        target != resolverTarget ||
        generation != (IPTR)resolverGeneration ||
        raw != (IPTR)(uintptr_t)resolverOutput)
        return NULL;
#if P4_TEST_WITH_SCOPE
    if (mockScopeDepth == 0u)
        return NULL;
    top = &mockScopeStack[mockScopeDepth - 1u];
    if (top->owner != currentTask || top->target != target ||
        top->generation != resolverGeneration || top->claimed != 0u ||
        top->frame == NULL ||
        raw != (IPTR)(uintptr_t)&top->frame->value)
        return NULL;
    top->claimed = 1u;
#else
    if (resolverClaimed != 0u)
        return NULL;
    resolverClaimed = 1u;
#endif
    resolverAccepts++;
    return resolverOutput;
}
#endif

#if P4_TEST_WITH_SCOPE
static BOOL mockCreationScopeBegin(struct Task *target,
                                   struct p4_creation_channel *frame)
{
    struct MockCreationScope *scope;
    struct MockCreationScope *parent = NULL;

    scopeBeginCalls++;
    if (scopeBeginRefuses || scopeGenerationSerial == UINT32_MAX ||
        mockScopeDepth >= sizeof(mockScopeStack) / sizeof(mockScopeStack[0]) ||
        target == NULL || frame == NULL)
        return FALSE;

    scope = &mockScopeStack[mockScopeDepth];
    if (mockScopeDepth != 0u)
        parent = &mockScopeStack[mockScopeDepth - 1u];
    frame->value = EXEC_PLATFORM_CREATION_CALLER_OWNED;
    frame->generation = ++scopeGenerationSerial;
    frame->previous_frame = parent ? (uintptr_t)parent->frame : 0u;
    frame->previous_generation = parent ? parent->generation : 0u;
    frame->previous_target = parent ? (uintptr_t)parent->target : 0u;
    frame->previous_claimed = parent ? parent->claimed : 0u;
    scope->owner = currentTask;
    scope->target = target;
    scope->frame = frame;
    scope->generation = frame->generation;
    scope->claimed = 0u;
    mockScopeBeginTargets[mockScopeDepth] = target;
    mockScopeBeginGenerations[mockScopeDepth] = frame->generation;
    scopeTarget = target;
    scopeFrame = frame;
    mockScopeDepth++;
    return TRUE;
}

static BOOL mockCreationScopeEnd(ULONG expectedGeneration,
                                struct p4_creation_channel *frame)
{
    struct MockCreationScope *scope;

    scopeEndCalls++;
    if (mockScopeDepth == 0u)
        return FALSE;
    scope = &mockScopeStack[mockScopeDepth - 1u];
    CHECK(scope->owner == currentTask);
    CHECK(scope->frame == frame);
    CHECK(scope->generation == expectedGeneration);
    if (scopeEndRefuses || scope->owner != currentTask ||
        scope->frame != frame || scope->generation != expectedGeneration)
        return FALSE;

    mockScopeEndGenerations[scopeEndCalls - 1u] = expectedGeneration;
    scopeEndValue = frame->value;
    frame->generation = 0u;
    frame->previous_frame = 0u;
    frame->previous_generation = 0u;
    frame->previous_target = 0u;
    frame->previous_claimed = 0u;
    mockScopeDepth--;
    if (mockScopeDepth == 0u)
    {
        scopeTarget = NULL;
        scopeFrame = NULL;
    }
    else
    {
        scope = &mockScopeStack[mockScopeDepth - 1u];
        scopeTarget = scope->target;
        scopeFrame = scope->frame;
    }
    return TRUE;
}

static void mockCreationScopeRefused(void)
{
    scopeRefusedCalls++;
    scopeRefusalFreeCount = (ULONG)primaryFreeCalls;
}
#endif

#if P4_TEST_WITH_SCOPE
static void checkCreationTagChain(const struct TagItem *tags)
{
    struct TagItem *state = (struct TagItem *)(uintptr_t)tags;
    struct TagItem *item;
    ULONG privateCount = 0u;
    ULONG generationCount = 0u;
    ULONG totalCount = 0u;
    ULONG sawName = 0u;
    ULONG sawPC = 0u;
    ULONG sawFinalPC = 0u;
    ULONG sawUserData = 0u;
    ULONG sawPriority = 0u;
    ULONG sawError = 0u;
    ULONG sawPort = 0u;
    ULONG sawHook = 0u;
    ULONG *injected;

    CHECK(tags != NULL);
    CHECK(tags[0].ti_Tag == EXEC_PLATFORM_CREATION_OWNERSHIP_TAG);
    CHECK(tags[1].ti_Tag == EXEC_PLATFORM_CREATION_GENERATION_TAG);
    CHECK((ULONG)tags[1].ti_Data == expectedGenerationTag);
    CHECK(expectedGenerationTag != 0u);
    CHECK(tags[2].ti_Tag == TAG_MORE);
    CHECK((const struct TagItem *)(uintptr_t)tags[2].ti_Data ==
          expectedCallerTags);
    CHECK(tags[3].ti_Tag == TAG_DONE);
    injected = (ULONG *)(uintptr_t)LibGetTagData(
        EXEC_PLATFORM_CREATION_OWNERSHIP_TAG, 0, tags);
    CHECK(injected == (ULONG *)(uintptr_t)tags[0].ti_Data);
    CHECK(injected != expectedCallerCollision);
    CHECK(*injected == EXEC_PLATFORM_CREATION_CALLER_OWNED);
    CHECK(expectedCallerCollision != NULL);
    CHECK((ULONG *)(uintptr_t)LibGetTagData(
        EXEC_PLATFORM_CREATION_OWNERSHIP_TAG, 0,
        (const struct TagItem *)(uintptr_t)tags[2].ti_Data) ==
          expectedCallerCollision);
    CHECK((ULONG)LibGetTagData(
        EXEC_PLATFORM_CREATION_GENERATION_TAG, 0, tags) ==
          expectedGenerationTag);

    while ((item = LibNextTagItem(&state)) != NULL)
    {
        totalCount++;
        switch (item->ti_Tag)
        {
        case EXEC_PLATFORM_CREATION_OWNERSHIP_TAG:
            if (privateCount == 0u)
                CHECK((ULONG *)(uintptr_t)item->ti_Data == injected);
            else if (privateCount == 1u)
            {
                CHECK((ULONG *)(uintptr_t)item->ti_Data ==
                      expectedCallerCollision);
                CHECK(*expectedCallerCollision ==
                      expectedCallerCollisionValue);
            }
            privateCount++;
            break;
        case EXEC_PLATFORM_CREATION_GENERATION_TAG:
            CHECK(generationCount == 0u);
            CHECK((ULONG)item->ti_Data == expectedGenerationTag);
            generationCount++;
            break;
        case TASKTAG_NAME:
            CHECK(strcmp((const char *)(uintptr_t)item->ti_Data,
                         "ownership-child") == 0);
            sawName = 1u;
            break;
        case TASKTAG_PC:
            CHECK(item->ti_Data == (IPTR)0x3000u);
            sawPC = 1u;
            break;
        case TASKTAG_FINALPC:
            CHECK(item->ti_Data == (IPTR)0x4000u);
            sawFinalPC = 1u;
            break;
        case TASKTAG_USERDATA:
            CHECK(item->ti_Data == (IPTR)0x5000u);
            sawUserData = 1u;
            break;
        case TASKTAG_PRI:
            CHECK(item->ti_Data == 7);
            sawPriority = 1u;
            break;
        case TASKTAG_ERROR:
            CHECK((ULONG *)(uintptr_t)item->ti_Data != NULL);
            sawError = 1u;
            break;
        case TASKTAG_TASKMSGPORT:
            CHECK((struct MsgPort **)(uintptr_t)item->ti_Data != NULL);
            sawPort = 1u;
            break;
        case TASKTAG_PRELAUNCHHOOK:
            CHECK((struct Hook *)(uintptr_t)item->ti_Data ==
                  expectedPrelaunchHook);
            sawHook = 1u;
            break;
        default:
            CHECK(0 && "unexpected constructor tag");
            break;
        }
    }
    CHECK(privateCount == 2u && generationCount == 1u && totalCount == 11u);
    CHECK(sawName && sawPC && sawFinalPC && sawUserData && sawPriority &&
          sawError && sawPort && sawHook);
}
#endif

#if !P4_TEST_WITH_SCOPE
static void checkOriginalCreationTags(const struct TagItem *tags)
{
    CHECK(tags == expectedCallerTags);
    CHECK(tags != NULL && tags[0].ti_Tag ==
          EXEC_PLATFORM_CREATION_OWNERSHIP_TAG);
    CHECK((ULONG *)(uintptr_t)tags[0].ti_Data == expectedCallerCollision);
    CHECK(*expectedCallerCollision == expectedCallerCollisionValue);
    CHECK((ULONG *)(uintptr_t)LibGetTagData(
        EXEC_PLATFORM_CREATION_OWNERSHIP_TAG, 0, tags) ==
          expectedCallerCollision);
    CHECK(LibGetTagData(EXEC_PLATFORM_CREATION_GENERATION_TAG, 0, tags) == 0);
}
#endif
'''

fixture += ownership_config

newadd_function = (
    "static APTR NewAddTaskBody(struct Task *task, APTR initialPC, APTR finalPC,\n"
    "                           struct TagItem *tagList)\n{\n"
    + base["newadd_body"]
    + "\n}\n"
)

adapter = r'''
static APTR NewAddTask(struct Task *task, APTR initialPC, APTR finalPC,
                       struct TagItem *tagList)
{
    ULONG *output = NULL;
    ULONG generation = 0u;
    int constructorChain = 0;
    APTR result;

    vectorCalls++;
    if (tagList)
    {
        output = (ULONG *)(uintptr_t)LibGetTagData(
            EXEC_PLATFORM_CREATION_OWNERSHIP_TAG, 0, tagList);
        constructorChain = tagList[0].ti_Tag ==
                               EXEC_PLATFORM_CREATION_OWNERSHIP_TAG &&
                           tagList[1].ti_Tag ==
                               EXEC_PLATFORM_CREATION_GENERATION_TAG &&
                           tagList[2].ti_Tag == TAG_MORE &&
                           tagList[3].ti_Tag == TAG_DONE;
        if (constructorChain)
            generation = (ULONG)LibGetTagData(
                EXEC_PLATFORM_CREATION_GENERATION_TAG, 0, tagList);
    }

    if (constructorChain)
    {
        expectedGenerationTag = generation;
        expectedCallerTags =
            (const struct TagItem *)(uintptr_t)tagList[2].ti_Data;
        expectedCallerCollision = (ULONG *)(uintptr_t)LibGetTagData(
            EXEC_PLATFORM_CREATION_OWNERSHIP_TAG, 0, expectedCallerTags);
        expectedCallerCollisionValue = expectedCallerCollision != NULL ?
            *expectedCallerCollision : 0u;
        expectedPrelaunchHook = (struct Hook *)(uintptr_t)LibGetTagData(
            TASKTAG_PRELAUNCHHOOK, 0, expectedCallerTags);
    }

#if P4_TEST_WITH_SCOPE
    /* Reenter the public vector with a second NewCreateTaskA while the outer
     * scope is live. This models nested construction; it is not a platform
     * current-Task or RUN-stack binding. */
    if (constructorChain && nestedConstructorInVector != 0u &&
        nestedConstructorDepth == 0u)
    {
        ULONG nestedError = 0xffffffffu;
        ULONG nestedCollision = 0x13572468u;
        struct MsgPort *nestedPort = NULL;
        struct Hook nestedHook = { 0u };
        struct TagItem nestedTags[10];
        struct Task *nestedTask;
        struct MemList *nestedPrimary;
        const struct TagItem *savedCallerTags = expectedCallerTags;
        ULONG *savedCallerCollision = expectedCallerCollision;
        ULONG savedCallerCollisionValue = expectedCallerCollisionValue;
        struct Hook *savedPrelaunchHook = expectedPrelaunchHook;
        ULONG savedGenerationTag = expectedGenerationTag;

        (void)fillOwnershipTags(nestedTags, &nestedError, &nestedPort,
                                &nestedCollision, &nestedHook);
        expectedCallerTags = nestedTags;
        expectedCallerCollision = &nestedCollision;
        expectedCallerCollisionValue = nestedCollision;
        expectedPrelaunchHook = &nestedHook;
        nestedConstructorDepth++;
        nestedTask = NewCreateTaskA(nestedTags);
        CHECK(nestedTask != NULL && nestedError == TASKERROR_OK);
        CHECK(nestedPort != NULL && nestedPort->mp_SigTask == nestedTask);
        CHECK(mockScopeDepth == 1u);
        nestedPrimary = (struct MemList *)nestedTask->tc_MemEntry.lh_Head;
        CleanupETask(nestedTask);
        FreeEntry(nestedPrimary);
        nestedConstructorDepth--;
        expectedCallerTags = savedCallerTags;
        expectedCallerCollision = savedCallerCollision;
        expectedCallerCollisionValue = savedCallerCollisionValue;
        expectedPrelaunchHook = savedPrelaunchHook;
        expectedGenerationTag = savedGenerationTag;
    }
#endif

    lastVectorInitialPC = initialPC;
    lastVectorFinalPC = finalPC;
    lastVectorOutput = output;
    lastVectorGeneration = generation;
    lastVectorTarget = task;
    lastVectorTagList = tagList;

    if (vectorMode != 0u)
    {
#if P4_TEST_WITH_SCOPE
        CHECK(constructorChain && output != NULL);
        serviceOwnedList = (struct MemList *)task->tc_MemEntry.lh_Head;
        *output = vectorReturnedOwnership;
        vectorOwnershipAfter = *output;
#else
        CHECK(!constructorChain);
        serviceOwnedList = (struct MemList *)task->tc_MemEntry.lh_Head;
        vectorOwnershipAfter = 0u;
#endif
        return NULL;
    }

#if P4_TEST_WITH_RESOLVER
    if (constructorChain && armResolverForVector)
    {
        resolverOutput = (ULONG *)(uintptr_t)tagList[0].ti_Data;
        resolverOwner = currentTask;
        resolverTarget = task;
        resolverGeneration = generation;
        resolverClaimed = 0u;
        resolverArmed = 1;
    }
    if (constructorChain && armResolverForVector && poisonBeforeBody)
        *resolverOutput = poisonValue;
#endif

    result = NewAddTaskBody(task, initialPC, finalPC, tagList);

#if P4_TEST_WITH_RESOLVER
    resolverArmed = 0;
    resolverOwner = NULL;
    resolverTarget = NULL;
    resolverOutput = NULL;
#endif
    if (constructorChain)
        vectorOwnershipAfter = *(ULONG *)(uintptr_t)tagList[0].ti_Data;
    else
        vectorOwnershipAfter = 0u;
    return result;
}
'''

newcreate_function = (
    "static struct Task *NewCreateTaskA(struct TagItem *tags)\n{\n"
    + base["newcreate_body"]
    + "\n}\n"
)

baseline_main = replace_once(
    base["main"],
    "int main(void)\n",
    "static int inherited_failure_main(void)\n",
    "inherited main rename",
)

ownership_main = r'''
static void resetOwnershipControls(void)
{
    primaryFreeCalls = 0;
    recordCreateCalls = 0;
    recordReleaseCalls = 0;
    publishCalls = 0;
    deleteContextCalls = 0;
    fatalCalls = 0;
#if P4_TEST_WITH_SCOPE
    fatalRefusedSnapshot = 0u;
#endif
    vectorCalls = 0;
    resolverCalls = 0;
    resolverAccepts = 0;
    scopeBeginCalls = 0;
    scopeEndCalls = 0;
    scopeRefusedCalls = 0;
    vectorMode = 0u;
    vectorReturnedOwnership = EXEC_PLATFORM_CREATION_SERVICE_OWNED;
    vectorOwnershipAfter = 0u;
    poisonBeforeBody = 0u;
    poisonValue = EXEC_PLATFORM_CREATION_SERVICE_OWNED;
    armResolverForVector = 0u;
    inspectMergedTags = 0u;
    expectedGenerationTag = 0u;
    resolverGeneration = 0u;
    resolverOutput = NULL;
    resolverOwner = NULL;
    resolverTarget = NULL;
    resolverArmed = 0;
#if P4_TEST_WITH_RESOLVER
    resolverClaimed = 0u;
#endif
    scopeBeginRefuses = 0u;
    scopeEndRefuses = 0u;
    scopeGenerationSerial = 0u;
    scopeRefusalFreeCount = 0u;
    scopeEndValue = 0u;
    nestedConstructorInVector = 0u;
    nestedConstructorDepth = 0u;
    scopeTarget = NULL;
    scopeFrame = NULL;
#if P4_TEST_WITH_SCOPE
    mockScopeDepth = 0u;
    memset(mockScopeStack, 0, sizeof(mockScopeStack));
    memset(mockScopeBeginTargets, 0, sizeof(mockScopeBeginTargets));
    memset(mockScopeBeginGenerations, 0,
           sizeof(mockScopeBeginGenerations));
    memset(mockScopeEndGenerations, 0, sizeof(mockScopeEndGenerations));
#endif
    serviceOwnedList = NULL;
    lastVectorOutput = NULL;
    lastVectorGeneration = 0u;
    lastVectorTarget = NULL;
    lastVectorTagList = NULL;
    lastVectorInitialPC = NULL;
    lastVectorFinalPC = NULL;
    lastLaunchHook = NULL;
    expectedCallerTags = NULL;
    expectedCallerCollision = NULL;
    expectedCallerCollisionValue = 0u;
    expectedPrelaunchHook = NULL;
    recordCreationFails = 0;
#if P4_TEST_WITH_SCOPE
    fatalJumpArmed = 0;
#endif
}

static size_t fillOwnershipTags(struct TagItem *tags, ULONG *error,
                                struct MsgPort **port,
                                ULONG *callerCollision,
                                struct Hook *hook)
{
    tags[0] = (struct TagItem){ EXEC_PLATFORM_CREATION_OWNERSHIP_TAG,
                               (IPTR)(uintptr_t)callerCollision };
    tags[1] = (struct TagItem){ TASKTAG_NAME,
                               (IPTR)(uintptr_t)"ownership-child" };
    tags[2] = (struct TagItem){ TASKTAG_PC, (IPTR)0x3000u };
    tags[3] = (struct TagItem){ TASKTAG_FINALPC, (IPTR)0x4000u };
    tags[4] = (struct TagItem){ TASKTAG_USERDATA, (IPTR)0x5000u };
    tags[5] = (struct TagItem){ TASKTAG_PRI, 7 };
    tags[6] = (struct TagItem){ TASKTAG_ERROR,
                               (IPTR)(uintptr_t)error };
    tags[7] = (struct TagItem){ TASKTAG_TASKMSGPORT,
                               (IPTR)(uintptr_t)port };
    tags[8] = (struct TagItem){ TASKTAG_PRELAUNCHHOOK,
                               (IPTR)(uintptr_t)hook };
    tags[9] = (struct TagItem){ TAG_DONE, 0 };
    return 9u;
}

static void prepareOwnershipCase(void)
{
    resetCase();
    resetOwnershipControls();
}

static void testActualConstructorFailure(ULONG whichFailure)
{
    ULONG error = 0xffffffffu;
    ULONG callerCollision = 0x87654321u;
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)0xdeadbeefu;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    struct Task *result;
    size_t beforePrimaryFrees;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &callerCollision, &hook);
    capturePrimary = 1;
    armResolverForVector = 1u;
    poisonBeforeBody = 1u;
    poisonValue = EXEC_PLATFORM_CREATION_SERVICE_OWNED;
    beforePrimaryFrees = primaryFreeCalls;
    if (whichFailure == 1u)
        maskAllocationFails = 1;
    else if (whichFailure == 2u)
        prepareContextFails = 1;
    else if (whichFailure == 3u)
        recordCreationFails = 1;
    else
        CHECK(0 && "unknown failure case");

    result = NewCreateTaskA(tags);
    CHECK(result == NULL);
    CHECK(error == TASKERROR_NOMEMORY && port == NULL);
    CHECK(vectorCalls == 1u);
#if P4_TEST_WITH_SCOPE
    CHECK(lastVectorOutput != NULL && lastVectorTagList != tags);
    CHECK(lastVectorGeneration != 0u && lastVectorTarget != NULL);
    CHECK(vectorOwnershipAfter == EXEC_PLATFORM_CREATION_CALLER_OWNED);
    CHECK(scopeBeginCalls == 1u && scopeEndCalls == 1u &&
          scopeRefusedCalls == 0u && mockScopeDepth == 0u);
    CHECK(scopeEndValue == EXEC_PLATFORM_CREATION_CALLER_OWNED);
#else
    CHECK(lastVectorOutput == &callerCollision && lastVectorTagList == tags);
    CHECK(lastVectorGeneration == 0u && vectorOwnershipAfter == 0u);
    CHECK(scopeBeginCalls == 0u && scopeEndCalls == 0u &&
          scopeRefusedCalls == 0u);
#endif
    CHECK(primaryFreeCalls == beforePrimaryFrees + 1u);
    CHECK(watchedPrimary != NULL && live_count == 0u);
    CHECK(callerCollision == 0x87654321u);
    CHECK(fatalCalls == 0u && publishCalls == 0u &&
          recordReleaseCalls == 0u);
#if P4_TEST_WITH_RESOLVER
#if P4_TEST_WITH_SCOPE
    CHECK(resolverCalls == 1u && resolverAccepts == 1u);
#else
    CHECK(resolverCalls == 1u && resolverAccepts == 0u);
#endif
#else
    CHECK(resolverCalls == 0u && resolverAccepts == 0u);
#endif
    if (whichFailure == 1u)
        CHECK(recordCreateCalls == 0u && etaskCleanupCalls == 0u);
    else if (whichFailure == 2u)
        CHECK(recordCreateCalls == 0u && etaskCleanupCalls == 1u);
    else
        CHECK(recordCreateCalls == 1u && etaskCleanupCalls == 1u);
    watchedPrimary = NULL;
}

#if P4_TEST_WITH_SCOPE
static void testServiceOwnedFailure(void)
{
    ULONG error = 0xffffffffu;
    ULONG collision = 0x12345678u;
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)0xdeadbeefu;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    struct Task *result;
    size_t beforePrimaryFrees;
    struct MemList *owned;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &collision, &hook);
    capturePrimary = 1;
    vectorMode = 1u;
    vectorReturnedOwnership = EXEC_PLATFORM_CREATION_SERVICE_OWNED;
    beforePrimaryFrees = primaryFreeCalls;
    result = NewCreateTaskA(tags);
    CHECK(result == NULL && error == TASKERROR_NOMEMORY && port == NULL);
    CHECK(vectorCalls == 1u && vectorOwnershipAfter ==
          EXEC_PLATFORM_CREATION_SERVICE_OWNED);
    CHECK(primaryFreeCalls == beforePrimaryFrees);
    CHECK(watchedPrimary != NULL && serviceOwnedList == watchedPrimary);
    CHECK(live_count != 0u && recordCreateCalls == 0u && publishCalls == 0u);
    CHECK(scopeBeginCalls == 1u && scopeEndCalls == 1u &&
          scopeRefusedCalls == 0u && mockScopeDepth == 0u);
    CHECK(scopeEndValue == EXEC_PLATFORM_CREATION_SERVICE_OWNED);
    CHECK(collision == 0x12345678u && fatalCalls == 0u);
    owned = serviceOwnedList;
    watchedPrimary = NULL;
    FreeEntry(owned);
    CHECK(live_count == 0u);
}
#endif

static void testPreVectorSignalFailureRemainsCallerOwned(void)
{
    ULONG error = 0xffffffffu;
    ULONG collision = 0x12345678u;
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)0xdeadbeefu;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    size_t beforePrimaryFrees;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &collision, &hook);
    capturePrimary = 1;
    signalAllocationFails = 1;
    beforePrimaryFrees = primaryFreeCalls;
    CHECK(NewCreateTaskA(tags) == NULL);
    CHECK(error == TASKERROR_NOMEMORY && port == NULL);
    CHECK(vectorCalls == 0u && recordCreateCalls == 0u &&
          publishCalls == 0u);
    CHECK(primaryFreeCalls == beforePrimaryFrees + 1u);
    CHECK(watchedPrimary != NULL && live_count == 0u);
    CHECK(collision == 0x12345678u && fatalCalls == 0u);
#if P4_TEST_WITH_RESOLVER
    CHECK(resolverCalls == 0u && resolverAccepts == 0u);
#endif
    watchedPrimary = NULL;
}

#if P4_TEST_WITH_SCOPE
static void testInvalidOwnershipFailsClosed(ULONG badOwnership)
{
    ULONG error = 0xffffffffu;
    ULONG collision = 0x12345678u;
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)0xdeadbeefu;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    size_t beforePrimaryFrees;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &collision, &hook);
    capturePrimary = 1;
    vectorMode = 2u;
    vectorReturnedOwnership = badOwnership;
    beforePrimaryFrees = primaryFreeCalls;
    if (setjmp(fatalJump) == 0)
    {
        fatalJumpArmed = 1;
        (void)NewCreateTaskA(tags);
        CHECK(0 && "invalid ownership returned instead of fatal");
    }
    CHECK(fatalCalls == 1u);
    CHECK(vectorCalls == 1u && vectorOwnershipAfter == badOwnership);
    CHECK(port == NULL && primaryFreeCalls == beforePrimaryFrees);
    CHECK(scopeBeginCalls == 1u && scopeEndCalls == 1u &&
          scopeRefusedCalls == 0u && mockScopeDepth == 0u);
    CHECK(scopeEndValue == badOwnership);
    CHECK(watchedPrimary != NULL && live_count != 0u);
    CHECK(collision == 0x12345678u && recordCreateCalls == 0u);
    /* The harness reclaims the deliberately fail-closed fixture allocation. */
    serviceOwnedList = watchedPrimary;
    watchedPrimary = NULL;
    FreeEntry(serviceOwnedList);
    serviceOwnedList = NULL;
    CHECK(live_count == 0u);
}
#endif

#if P4_TEST_WITH_SCOPE
static void testScopeBeginRefusalRollsBackBeforePolicy(void)
{
    ULONG error = 0xffffffffu;
    ULONG collision = 0x24681357u;
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)0xdeadbeefu;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    size_t beforePrimaryFrees;
    struct Task *created;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &collision, &hook);
    capturePrimary = 1;
    scopeBeginRefuses = 1u;
    beforePrimaryFrees = primaryFreeCalls;
    created = NewCreateTaskA(tags);
    CHECK(created == NULL);
    CHECK(error == TASKERROR_NOMEMORY && port == NULL);
    CHECK(scopeBeginCalls == 1u && scopeEndCalls == 0u &&
          scopeRefusedCalls == 1u && mockScopeDepth == 0u);
    CHECK(scopeRefusalFreeCount == (ULONG)(beforePrimaryFrees + 1u));
    CHECK(primaryFreeCalls == beforePrimaryFrees + 1u);
    CHECK(vectorCalls == 0u && recordCreateCalls == 0u &&
          publishCalls == 0u && fatalCalls == 0u);
    CHECK(watchedPrimary != NULL && live_count == 0u);
    CHECK(collision == 0x24681357u && scopeGenerationSerial == 0u);
}

static void testScopeEndRefusalIsTerminal(void)
{
    ULONG error = 0xffffffffu;
    ULONG collision = 0x24681357u;
    struct MsgPort *port = NULL;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    volatile int returnedNormally = 0;
    size_t beforePrimaryFrees;
    struct Task *created;
    struct MemList *primary;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &collision, &hook);
    capturePrimary = 1;
    armResolverForVector = 1u;
    poisonBeforeBody = 1u;
    poisonValue = EXEC_PLATFORM_CREATION_SERVICE_OWNED;
    scopeEndRefuses = 1u;
    beforePrimaryFrees = primaryFreeCalls;

    if (setjmp(fatalJump) == 0)
    {
        fatalJumpArmed = 1;
        (void)NewCreateTaskA(tags);
        returnedNormally = 1;
    }

    CHECK(returnedNormally == 0);
    CHECK(fatalCalls == 1u && fatalRefusedSnapshot == 1u);
    CHECK(scopeBeginCalls == 1u && scopeEndCalls == 1u &&
          scopeRefusedCalls == 1u && mockScopeDepth == 1u);
    CHECK(primaryFreeCalls == beforePrimaryFrees);
    CHECK(error == 0xffffffffu && port != NULL);
    CHECK(vectorCalls == 1u && resolverCalls == 1u && resolverAccepts == 1u);
    CHECK(vectorOwnershipAfter == EXEC_PLATFORM_CREATION_REGISTERED);
    CHECK(recordCreateCalls == 1u && recordReleaseCalls == 1u &&
          publishCalls == 1u && etaskCleanupCalls == 0u);
    CHECK(watchedPrimary != NULL && live_count != 0u);
    CHECK(collision == 0x24681357u);

    /* The fixture's mock service reclaims the deliberately stranded success
     * after checking that NewCreateTaskA did not roll it back. */
    created = scopeTarget;
    primary = watchedPrimary;
    CHECK(created != NULL && primary != NULL);
    port = NULL;
    CleanupETask(created);
    FreeEntry(primary);
    watchedPrimary = NULL;
    mockScopeDepth = 0u;
    scopeTarget = NULL;
    scopeFrame = NULL;
    CHECK(live_count == 0u);
}

static void testNestedScopeTransportAndTagMore(void)
{
    ULONG error = 0xffffffffu;
    ULONG collision = 0x87654321u;
    struct MsgPort *port = NULL;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    struct Task *created;
    struct MemList *primary;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &collision, &hook);
    expectedCallerTags = tags;
    expectedCallerCollision = &collision;
    expectedCallerCollisionValue = collision;
    expectedPrelaunchHook = &hook;
    inspectMergedTags = 1u;
    capturePrimary = 1;
    armResolverForVector = 1u;
    poisonBeforeBody = 1u;
    poisonValue = EXEC_PLATFORM_CREATION_SERVICE_OWNED;
    nestedConstructorInVector = 1u;

    created = NewCreateTaskA(tags);
    CHECK(created != NULL && error == TASKERROR_OK);
    CHECK(port != NULL && port->mp_SigTask == created);
    CHECK(vectorCalls == 2u && resolverCalls == 2u && resolverAccepts == 2u);
    CHECK(scopeBeginCalls == 2u && scopeEndCalls == 2u &&
          scopeRefusedCalls == 0u && mockScopeDepth == 0u);
    CHECK(mockScopeBeginTargets[0] == created &&
          mockScopeBeginTargets[1] != created);
    CHECK(mockScopeBeginGenerations[0] == 1u &&
          mockScopeBeginGenerations[1] == 2u);
    CHECK(mockScopeEndGenerations[0] == 2u &&
          mockScopeEndGenerations[1] == 1u);
    CHECK(lastVectorTagList != tags && lastVectorTarget == created &&
          lastVectorGeneration == 1u);
    CHECK(vectorOwnershipAfter == EXEC_PLATFORM_CREATION_REGISTERED);
    CHECK(recordCreateCalls == 2u && recordReleaseCalls == 2u &&
          publishCalls == 2u && launchCalls == 2u);
    CHECK(collision == 0x87654321u);

    primary = (struct MemList *)created->tc_MemEntry.lh_Head;
    CHECK(primary == watchedPrimary && primary->ml_NumEntries == 4u);
    port = NULL;
    CleanupETask(created);
    FreeEntry(primary);
    watchedPrimary = NULL;
    CHECK(live_count == 0u);
}
#endif

static void testSuccessfulRegistrationAndTagChain(void)
{
    ULONG error = 0xffffffffu;
    ULONG callerCollision = 0x87654321u;
    struct MsgPort *port = NULL;
    struct Hook hook = { 0u };
    struct TagItem tags[10];
    struct Task *created;
    struct MemList *primary;

    prepareOwnershipCase();
    (void)fillOwnershipTags(tags, &error, &port, &callerCollision, &hook);
    expectedCallerTags = tags;
    expectedCallerCollision = &callerCollision;
    expectedCallerCollisionValue = callerCollision;
    expectedPrelaunchHook = &hook;
    inspectMergedTags = 1u;
    capturePrimary = 1;
    armResolverForVector = 1u;
    poisonBeforeBody = 1u;
    poisonValue = EXEC_PLATFORM_CREATION_SERVICE_OWNED;

    created = NewCreateTaskA(tags);
    CHECK(created != NULL && error == TASKERROR_OK);
    CHECK(port != NULL && port->mp_SigTask == created);
    CHECK(vectorCalls == 1u && lastVectorOutput != NULL);
    CHECK(lastVectorInitialPC == (APTR)(uintptr_t)0x3000u);
    CHECK(lastVectorFinalPC == (APTR)(uintptr_t)0x4000u);
    CHECK(lastLaunchHook == &hook && launchCalls == 1u);
    CHECK(recordCreateCalls == 1u && recordReleaseCalls == 1u &&
          publishCalls == 1u);
    CHECK(callerCollision == 0x87654321u);
#if P4_TEST_WITH_SCOPE
    CHECK(lastVectorTagList != tags && lastVectorGeneration != 0u);
    CHECK(lastVectorTarget == created);
    CHECK(scopeBeginCalls == 1u && scopeEndCalls == 1u &&
          scopeRefusedCalls == 0u && mockScopeDepth == 0u);
    CHECK(scopeEndValue == EXEC_PLATFORM_CREATION_REGISTERED);
#else
    CHECK(lastVectorTagList == tags && lastVectorOutput == &callerCollision);
    CHECK(lastVectorGeneration == 0u && vectorOwnershipAfter == 0u);
    CHECK(scopeBeginCalls == 0u && scopeEndCalls == 0u &&
          scopeRefusedCalls == 0u);
#endif
#if P4_TEST_WITH_RESOLVER
#if P4_TEST_WITH_SCOPE
    CHECK(vectorOwnershipAfter == EXEC_PLATFORM_CREATION_REGISTERED);
    CHECK(resolverCalls == 1u && resolverAccepts == 1u);
#else
    CHECK(resolverCalls == 1u && resolverAccepts == 0u);
#endif
#else
    CHECK(resolverCalls == 0u && resolverAccepts == 0u);
#endif
    CHECK(created->tc_UserData == (APTR)(uintptr_t)0x5000u);
    CHECK(created->tc_Node.ln_Pri == 7);
    CHECK(live_count != 0u);

    primary = (struct MemList *)created->tc_MemEntry.lh_Head;
    CHECK(primary == watchedPrimary && primary->ml_NumEntries == 4u);
    launchPortSlot = NULL;
    CleanupETask(created);
    FreeEntry(primary);
    watchedPrimary = NULL;
    port = NULL;
    CHECK(live_count == 0u);
}

static void testUntrustedTagUserValueIsIgnored(void *protectedPage)
{
    struct Task task;
    struct MemList callerList;
    struct TagItem tags[] = {
        { EXEC_PLATFORM_CREATION_OWNERSHIP_TAG,
          (IPTR)(uintptr_t)protectedPage },
        { TAG_DONE, 0 }
    };
    APTR result;

    prepareOwnershipCase();
    initCallerTask(&task, &callerList);
    task.tc_Node.ln_Name = "direct-child";
    result = NewAddTask(&task, NULL, NULL, tags);
    CHECK(result == &task && vectorCalls == 1u);
    CHECK(lastVectorOutput == (ULONG *)(uintptr_t)protectedPage);
#if P4_TEST_WITH_RESOLVER
    CHECK(resolverCalls == 1u && resolverAccepts == 0u);
#else
    CHECK(resolverCalls == 0u && resolverAccepts == 0u);
#endif
    CHECK(recordCreateCalls == 1u && recordReleaseCalls == 1u &&
          publishCalls == 1u);
    CleanupETask(&task);
    CHECK(live_count == 0u);
}

#if P4_TEST_WITH_RESOLVER && !P4_TEST_WITH_SCOPE
static void testMockResolverRequiresCurrentOwnerAndExactFrame(void)
{
    ULONG frame = EXEC_PLATFORM_CREATION_CALLER_OWNED;
    struct Task otherTask;
    struct Task targetTask;

    prepareOwnershipCase();
    memset(&otherTask, 0, sizeof(otherTask));
    memset(&targetTask, 0, sizeof(targetTask));
    resolverOutput = &frame;
    resolverOwner = &parentTask;
    resolverTarget = &targetTask;
    resolverGeneration = 73u;
    resolverArmed = 1;
    CHECK(mockCreationOwnershipResolve((IPTR)(uintptr_t)&frame, 73,
        &targetTask) == &frame);
    CHECK(mockCreationOwnershipResolve((IPTR)(uintptr_t)&frame, 73,
        &targetTask) == NULL); /* one-use */
    resolverClaimed = 0u;
    CHECK(mockCreationOwnershipResolve((IPTR)(uintptr_t)&frame, 72,
        &targetTask) == NULL);
    CHECK(mockCreationOwnershipResolve((IPTR)(uintptr_t)&frame, 73,
        &otherTask) == NULL);
    CHECK(mockCreationOwnershipResolve((IPTR)(uintptr_t)0x1234u, 73,
        &targetTask) == NULL);
    currentTask = &otherTask;
    CHECK(mockCreationOwnershipResolve((IPTR)(uintptr_t)&frame, 73,
        &targetTask) == NULL);
    currentTask = &parentTask;
    resolverArmed = 0;
    resolverOutput = NULL;
    resolverOwner = NULL;
    resolverTarget = NULL;
    CHECK(resolverCalls == 6u && resolverAccepts == 1u);
}
#endif

int main(void)
{
    long pageSize;
    void *protectedPage;
    unsigned inheritedChecks;

    if (inherited_failure_main() != 0)
        return 1;
    inheritedChecks = checks;
    pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0)
        return 2;
#if defined(MAP_ANONYMOUS)
    protectedPage = mmap(NULL, (size_t)pageSize, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    protectedPage = mmap(NULL, (size_t)pageSize, PROT_NONE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (protectedPage == MAP_FAILED)
        return 2;

    testActualConstructorFailure(1u);
    testActualConstructorFailure(2u);
    testActualConstructorFailure(3u);
    testPreVectorSignalFailureRemainsCallerOwned();
#if P4_TEST_WITH_SCOPE
    testServiceOwnedFailure();
    testInvalidOwnershipFailsClosed(EXEC_PLATFORM_CREATION_REGISTERED);
    testInvalidOwnershipFailsClosed(0xdeadbeefu);
    testScopeBeginRefusalRollsBackBeforePolicy();
    testScopeEndRefusalIsTerminal();
    testNestedScopeTransportAndTagMore();
#endif
    testSuccessfulRegistrationAndTagChain();
    testUntrustedTagUserValueIsIgnored(protectedPage);
#if P4_TEST_WITH_RESOLVER && !P4_TEST_WITH_SCOPE
    testMockResolverRequiresCurrentOwnerAndExactFrame();
#endif

    CHECK(munmap(protectedPage, (size_t)pageSize) == 0);
    CHECK(live_count == 0u);
    printf("P4 task creation ownership: %u checks passed; inherited %u; "
           "test resolver %u; test scope %u\n",
           checks - inheritedChecks, inheritedChecks,
           (unsigned)P4_TEST_WITH_RESOLVER,
           (unsigned)P4_TEST_WITH_SCOPE);
    return 0;
}
'''

newadd_prototype = r'''
static APTR NewAddTaskBody(struct Task *task, APTR initialPC, APTR finalPC,
                           struct TagItem *tagList);
static APTR NewAddTask(struct Task *task, APTR initialPC, APTR finalPC,
                       struct TagItem *tagList);
static struct Task *NewCreateTaskA(struct TagItem *tags);
static size_t fillOwnershipTags(struct TagItem *tags, ULONG *error,
                                struct MsgPort **port, ULONG *callerCollision,
                                struct Hook *hook);
'''

program = (
    fixture
    + "\n"
    + base["prelude"]
    + "\n"
    + base["mem_template"]
    + "\n"
    + base["etask_prefix"]
    + "\n"
    + newadd_prototype
    + newadd_function
    + adapter
    + "\n"
    + newcreate_function
    + "\n"
    + baseline_main
    + "\n"
    + ownership_main
)


def compile_and_run(optimization: str, resolver: bool, scope: bool,
                    artifact_label: str = "author") -> tuple[int, bytes]:
    if re.fullmatch(r"[A-Za-z0-9-]+", artifact_label) is None:
        raise ValueError("artifact label must contain only letters, digits or hyphens")
    artifact_dir = Path(
        "/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/"
        "smp-priority-2026-10-01"
    )
    variant = "mock-scope" if scope else (
        "mock-resolver" if resolver else "default-deny"
    )
    binary = artifact_dir / (
        f"p4-task-creation-ownership-{variant}-{optimization[1:].lower()}-{artifact_label}-20261002"
    )
    with tempfile.TemporaryDirectory(prefix="p4-task-creation-ownership-") as tmp:
        src = Path(tmp) / "test.c"
        src.write_text(program)
        command = [
            "clang",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            optimization,
            "-DP4_TEST_WITH_RESOLVER=" + ("1" if resolver else "0"),
            "-DP4_TEST_WITH_SCOPE=" + ("1" if scope else "0"),
            "-I",
            str(repo / "arch/riscv-esp32p4/exec"),
            str(src),
            "-o",
            str(binary),
        ]
        subprocess.run(command, check=True)
        result = subprocess.run(
            [str(binary)],
            check=True,
            capture_output=True,
            text=True,
            timeout=20,
            env={
                **__import__("os").environ,
                "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        return binary.stat().st_size, result.stdout.encode()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-label", default="author",
                        help="distinct evidence label; preserves earlier run artifacts")
    arguments = parser.parse_args()
    variants = ((False, False), (True, False), (True, True))
    for opt in ("-O1", "-O2"):
        for with_resolver, with_scope in variants:
            size, output = compile_and_run(
                opt, with_resolver, with_scope, arguments.artifact_label
            )
            print(
                f"{opt} resolver={int(with_resolver)} "
                f"scope={int(with_scope)} size={size}"
            )
            print(output.decode(), end="")
