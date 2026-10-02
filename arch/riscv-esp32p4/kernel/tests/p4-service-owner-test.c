/* Actual leaf registry proof used by the no-save discard transaction. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../p4_task_registry.c"
#include "../p4_task_lifetime.c"
static unsigned int checks;
#define CHECK(value) do { ++checks; if (!(value)) { \
    fprintf(stderr, "failed line %d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    unsigned int hart;
    int identity_storage;
    struct Task *identity = (struct Task *)&identity_storage;
    for (hart = 0; hart < 2; ++hart)
    {
        struct p4_task_registry registry = {0};
        struct p4_task_registry_record record = {0};
        struct p4_task_registry_record before;
        CHECK(p4_task_registry_register_locked(&registry, &record, identity));
        CHECK(p4_task_registry_enter_running_locked(&registry, identity, hart));
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, hart));
        CHECK(p4_task_registry_pin_locked(&registry, identity));
        CHECK(p4_task_registry_retire_for_service_locked(&registry, identity));
        before = record;
        CHECK(p4_task_registry_service_owner_running_locked(&registry, identity, hart));
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, hart ^ 1u));
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, 2));
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, NULL, hart));
        CHECK(memcmp(&record, &before, sizeof(record)) == 0);
        record.service_pending = 2;
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, hart));
        record = before;
        record.lifetime.running_harts = 3;
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, hart));
        record = before;
        CHECK(p4_task_registry_unpin_locked(&registry, identity));
        CHECK(!p4_task_registry_unpin_locked(&registry, identity));
        CHECK(p4_task_registry_service_owner_running_locked(&registry, identity, hart));
        CHECK(p4_task_registry_leave_running_locked(&registry, identity, hart));
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, hart));
        CHECK(p4_task_registry_claim_service_locked(&registry, identity) == &record);
        CHECK(!p4_task_registry_service_owner_running_locked(&registry, identity, hart));
    }
    printf("PASS: service Running-owner proof (%u checks)\n", checks);
    return 0;
}
