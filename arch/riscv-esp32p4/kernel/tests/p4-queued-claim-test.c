/* Actual registry proof: a GetMsg consumer cannot claim before reservation. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../p4_task_registry.c"
#include "../p4_task_lifetime.c"

static unsigned int checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)

int main(void)
{
    unsigned int hart;
    for (hart = 0; hart < 2u; ++hart)
    {
        struct p4_task_registry registry = {0};
        struct p4_task_registry_record record = {0}, snapshot;
        struct Task *identity = (struct Task *)(uintptr_t)(0x8000u + hart * 16u);
        CHECK(p4_task_registry_register_locked(&registry, &record, identity));
        CHECK(p4_task_registry_running_owner_locked(&registry, identity) == 0);
        CHECK(p4_task_registry_running_owner_locked(&registry, NULL) == -1);
        CHECK(p4_task_registry_pin_locked(&registry, identity));
        CHECK(p4_task_registry_enter_running_locked(&registry, identity, hart));
        CHECK(p4_task_registry_running_owner_locked(&registry, identity) == (int)hart + 1);
        record.lifetime.running_harts = 3u;
        CHECK(p4_task_registry_running_owner_locked(&registry, identity) == -1);
        record.lifetime.running_harts = UINT32_C(1) << hart;
        CHECK(p4_task_registry_retire_for_service_locked(&registry, identity));
        CHECK(p4_task_registry_running_owner_locked(&registry, identity) == -1);
        snapshot = record;
        CHECK(p4_task_registry_claim_queued_service_locked(&registry, identity) == NULL);
        CHECK(memcmp(&snapshot, &record, sizeof(record)) == 0);
        CHECK(p4_task_registry_unpin_locked(&registry, identity));
        CHECK(p4_task_registry_leave_running_locked(&registry, identity, hart));
        snapshot = record;
        CHECK(p4_task_registry_claim_queued_service_locked(&registry, identity) == NULL);
        CHECK(memcmp(&snapshot, &record, sizeof(record)) == 0);
        CHECK(p4_task_registry_take_service_ready_locked(&registry, identity) == identity);
        CHECK(p4_task_registry_claim_queued_service_locked(&registry, identity) == &record);
        CHECK(registry.count == 0 && registry.head == NULL);
        CHECK(record.service_pending == 0 && record.service_queued == 0);
        CHECK(record.lifetime.notification_pins == 0 && record.lifetime.running_harts == 0);
        CHECK(record.lifetime.flags == (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
        CHECK(p4_task_registry_claim_queued_service_locked(&registry, identity) == NULL);
        CHECK(p4_task_registry_claim_queued_service_locked(NULL, identity) == NULL);
        CHECK(p4_task_registry_claim_queued_service_locked(&registry, NULL) == NULL);
    }
    printf("P4 queued service claim: %u checks passed\n", checks);
    return 0;
}
