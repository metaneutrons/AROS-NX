#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../p4_remove_bank.c"
#include "../p4_remove_completion.c"

static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "p4-remove-bank-test: failure at line %d\n", __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    struct p4_remove_bank bank = {0};
    struct p4_remove_bank_slot slots[3] = {{0}};
    struct p4_remove_bank bank_before;
    struct p4_remove_bank_slot slots_before[3];
    struct p4_remove_handle h1 = {UINT32_MAX, UINT32_MAX};
    struct p4_remove_handle h2 = {UINT32_MAX, UINT32_MAX};
    struct p4_remove_handle h3 = {UINT32_MAX, UINT32_MAX};
    struct p4_remove_handle h4 = {UINT32_MAX, UINT32_MAX};
    struct p4_remove_handle handle_before;
    uintptr_t notify;
    int reclaim;
    int ready;
    uint32_t count;
    uintptr_t overlap_before;
    union
    {
        uintptr_t whole;
        int part;
    } overlapping_outputs;

    CHECK(p4_remove_bank_init(&bank, slots, 3u) == 1);
    CHECK(p4_remove_bank_init(&bank, slots, 3u) == -1);

    CHECK(p4_remove_bank_reserve(&bank, (uintptr_t)0x101u,
                                 (uintptr_t)0x900u, &h1) == 1);
    CHECK(p4_remove_bank_reserve(&bank, (uintptr_t)0x202u,
                                 (uintptr_t)0x900u, &h2) == 1);
    CHECK(p4_remove_bank_reserve(&bank, (uintptr_t)0x303u,
                                 (uintptr_t)0x900u, &h3) == 1);
    CHECK(h1.index == 0u && h2.index == 1u && h3.index == 2u);
    CHECK(h1.generation == 1u && h2.generation == 2u &&
          h3.generation == 3u);

    handle_before = (struct p4_remove_handle){77u, 88u};
    h4 = handle_before;
    CHECK(p4_remove_bank_reserve(&bank, (uintptr_t)0x404u,
                                 (uintptr_t)0x900u, &h4) == 0);
    CHECK(h4.index == handle_before.index &&
          h4.generation == handle_before.generation);

    count = UINT32_MAX;
    CHECK(p4_remove_bank_count_target(&bank, (uintptr_t)0x900u, &count) == 1);
    CHECK(count == 3u);
    h4 = (struct p4_remove_handle){99u, 101u};
    CHECK(p4_remove_bank_next_target(&bank, (uintptr_t)0x900u, 0u, &h4) == 1);
    CHECK(h4.index == h1.index && h4.generation == h1.generation);
    CHECK(p4_remove_bank_next_target(&bank, (uintptr_t)0x900u,
                                     h4.index + 1u, &h4) == 1);
    CHECK(h4.index == h2.index && h4.generation == h2.generation);
    CHECK(p4_remove_bank_next_target(&bank, (uintptr_t)0x900u,
                                     h4.index + 1u, &h4) == 1);
    CHECK(h4.index == h3.index && h4.generation == h3.generation);
    handle_before = h4;
    CHECK(p4_remove_bank_next_target(&bank, (uintptr_t)0x900u, 3u, &h4) == 0);
    CHECK(h4.index == handle_before.index &&
          h4.generation == handle_before.generation);
    CHECK(p4_remove_bank_next_target(&bank, (uintptr_t)0x900u, 4u, &h4) == -1);
    CHECK(h4.index == handle_before.index &&
          h4.generation == handle_before.generation);

    ready = -7;
    CHECK(p4_remove_bank_ready(&bank, h1, (uintptr_t)0x999u, &ready) == 0);
    CHECK(ready == -7);
    CHECK(p4_remove_bank_ready(&bank, h1, (uintptr_t)0x101u, &ready) == 1);
    CHECK(ready == 0);
    notify = (uintptr_t)0x777u;
    reclaim = -9;
    CHECK(p4_remove_bank_ack(&bank, h1, (uintptr_t)0x901u,
                             &notify, &reclaim) == 0);
    CHECK(notify == (uintptr_t)0x777u && reclaim == -9);
    CHECK(p4_remove_bank_drop(&bank, h1, (uintptr_t)0x999u, 1,
                              &reclaim) == 0);
    CHECK(reclaim == -9);

    /* Cancellation drops requester ownership only; the service reference
     * remains discoverable until target-side ACK. */
    CHECK(p4_remove_bank_drop(&bank, h1, (uintptr_t)0x101u, 1,
                              &reclaim) == 1);
    CHECK(reclaim == 0);
    ready = -7;
    CHECK(p4_remove_bank_ready(&bank, h1, (uintptr_t)0x101u, &ready) == 0);
    CHECK(ready == -7);
    count = 0u;
    CHECK(p4_remove_bank_count_target(&bank, (uintptr_t)0x900u, &count) == 1);
    CHECK(count == 3u);
    notify = (uintptr_t)0x777u;
    reclaim = -9;
    CHECK(p4_remove_bank_ack(&bank, h1, (uintptr_t)0x900u,
                             &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)0 && reclaim == 1);
    CHECK(p4_remove_bank_ready(&bank, h1, (uintptr_t)0x101u, &ready) == 0);
    CHECK(p4_remove_bank_drop(&bank, h1, (uintptr_t)0x101u, 1,
                              &reclaim) == 0);

    /* Reuse the cleared slot. The old handle must not touch the new ticket. */
    CHECK(p4_remove_bank_reserve(&bank, (uintptr_t)0x404u,
                                 (uintptr_t)0xa00u, &h4) == 1);
    CHECK(h4.index == h1.index && h4.generation > h1.generation);
    ready = -7;
    CHECK(p4_remove_bank_ready(&bank, h1, (uintptr_t)0x101u, &ready) == 0);
    CHECK(ready == -7);
    bank_before = bank;
    memcpy(slots_before, slots, sizeof(slots));
    reclaim = -9;
    CHECK(p4_remove_bank_drop(&bank, h1, (uintptr_t)0x101u, 1,
                              &reclaim) == 0);
    CHECK(reclaim == -9 && memcmp(&bank, &bank_before, sizeof(bank)) == 0 &&
          memcmp(slots, slots_before, sizeof(slots)) == 0);

    /* ACK with a live requester publishes its opaque identity; requester
     * removal then reclaims the slot exactly once. */
    notify = (uintptr_t)0x777u;
    reclaim = -9;
    CHECK(p4_remove_bank_ack(&bank, h2, (uintptr_t)0x900u,
                             &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)0x202u && reclaim == 0);
    ready = 0;
    CHECK(p4_remove_bank_ready(&bank, h2, (uintptr_t)0x202u, &ready) == 1);
    CHECK(ready == 1);
    count = UINT32_MAX;
    CHECK(p4_remove_bank_count_target(&bank, (uintptr_t)0x900u, &count) == 1);
    CHECK(count == 1u);
    CHECK(p4_remove_bank_drop(&bank, h2, (uintptr_t)0x202u, 0,
                              &reclaim) == 1);
    CHECK(reclaim == 1);
    CHECK(p4_remove_bank_drop(&bank, h2, (uintptr_t)0x202u, 0,
                              &reclaim) == 0);

    /* ACK-first then ordinary requester drop is a separate normal order. */
    notify = 0u;
    reclaim = -9;
    CHECK(p4_remove_bank_ack(&bank, h3, (uintptr_t)0x900u,
                             &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)0x303u && reclaim == 0);
    CHECK(p4_remove_bank_drop(&bank, h3, (uintptr_t)0x303u, 0,
                              &reclaim) == 1);
    CHECK(reclaim == 1);

    /* Test malformed/output alias refusal without changing bytes or outputs. */
    bank_before = bank;
    memcpy(slots_before, slots, sizeof(slots));
    CHECK(p4_remove_bank_reserve(
              &bank, (uintptr_t)0x505u, (uintptr_t)0xb00u,
              (struct p4_remove_handle *)(void *)&slots[0]) == -1);
    CHECK(memcmp(&bank, &bank_before, sizeof(bank)) == 0 &&
          memcmp(slots, slots_before, sizeof(slots)) == 0);
    count = 0xfeedu;
    CHECK(p4_remove_bank_count_target(
              &bank, (uintptr_t)0xa00u, (uint32_t *)(void *)&slots[0]) == -1);
    CHECK(count == 0xfeedu &&
          memcmp(&bank, &bank_before, sizeof(bank)) == 0 &&
          memcmp(slots, slots_before, sizeof(slots)) == 0);
    CHECK(p4_remove_bank_count_target(
              &bank, (uintptr_t)0xa00u, (uint32_t *)(void *)&bank.serial) == -1);
    CHECK(memcmp(&bank, &bank_before, sizeof(bank)) == 0);
    overlapping_outputs.whole = (uintptr_t)0x777u;
    overlap_before = overlapping_outputs.whole;
    CHECK(p4_remove_bank_ack(&bank, h4, (uintptr_t)0xa00u,
                             &overlapping_outputs.whole,
                             &overlapping_outputs.part) == -1);
    CHECK(overlapping_outputs.whole == overlap_before &&
          memcmp(&bank, &bank_before, sizeof(bank)) == 0 &&
          memcmp(slots, slots_before, sizeof(slots)) == 0);
    CHECK(p4_remove_bank_ready(
              &bank, h4, (uintptr_t)0x404u,
              (int *)(void *)((unsigned char *)&ready + 1)) == -1);
    CHECK(memcmp(&bank, &bank_before, sizeof(bank)) == 0 &&
          memcmp(slots, slots_before, sizeof(slots)) == 0);

    /* Corrupt a field, prove full-table validation fails before output write,
     * then restore and continue. */
    slots[2].active = 2u;
    count = 0xfeedu;
    CHECK(p4_remove_bank_count_target(&bank, (uintptr_t)0xa00u, &count) == -1);
    CHECK(count == 0xfeedu);
    slots[2].active = 1u;
    slots[2].generation = 0u;
    CHECK(p4_remove_bank_count_target(&bank, (uintptr_t)0xa00u, &count) == -1);
    CHECK(count == 0xfeedu);
    slots[2].active = 0u;
    slots[2].generation = h3.generation;
    CHECK(p4_remove_bank_count_target(&bank, (uintptr_t)0xa00u, &count) == 1);
    CHECK(count == 1u);

    /* Invalid initialization ranges and capacities are rejected before
     * reading or writing either typed object. */
    {
        struct p4_remove_bank bad_bank = {0};
        struct p4_remove_bank_slot bad_slots[1] = {{0}};
        struct p4_remove_bank bad_before = bad_bank;
        struct p4_remove_bank_slot bad_slot_before = bad_slots[0];

        CHECK(p4_remove_bank_init(&bad_bank, bad_slots, 0u) == -1);
        CHECK(p4_remove_bank_init(&bad_bank, bad_slots,
                                  P4_REMOVE_BANK_MAX_SLOTS + 1u) == -1);
        CHECK(p4_remove_bank_init(
                  &bad_bank,
                  (struct p4_remove_bank_slot *)(void *)&bad_bank, 1u) == -1);
        CHECK(p4_remove_bank_init(
                  &bad_bank,
                  (struct p4_remove_bank_slot *)(void *)
                      ((unsigned char *)bad_slots + 1), 1u) == -1);
        CHECK(memcmp(&bad_bank, &bad_before, sizeof(bad_bank)) == 0 &&
              memcmp(&bad_slots[0], &bad_slot_before,
                     sizeof(bad_slots[0])) == 0);
        bad_slots[0].active = 1u;
        CHECK(p4_remove_bank_init(&bad_bank, bad_slots, 1u) == -1);
        CHECK(memcmp(&bad_bank, &bad_before, sizeof(bad_bank)) == 0);
    }

    /* The serial may reach UINT32_MAX once, but never wraps to zero/ABA. */
    {
        struct p4_remove_bank last_bank = {0};
        struct p4_remove_bank_slot last_slots[1] = {{0}};
        struct p4_remove_handle last = {55u, 66u};
        struct p4_remove_handle last_before;

        CHECK(p4_remove_bank_init(&last_bank, last_slots, 1u) == 1);
        last_bank.serial = UINT32_MAX - 1u;
        CHECK(p4_remove_bank_reserve(&last_bank, (uintptr_t)0x606u,
                                     (uintptr_t)0xc00u, &last) == 1);
        CHECK(last.generation == UINT32_MAX);
        CHECK(p4_remove_bank_drop(&last_bank, last, (uintptr_t)0x606u, 1,
                                  &reclaim) == 1);
        CHECK(reclaim == 0);
        CHECK(p4_remove_bank_ack(&last_bank, last, (uintptr_t)0xc00u,
                                 &notify, &reclaim) == 1);
        CHECK(notify == 0u && reclaim == 1);
        last_before = (struct p4_remove_handle){77u, 88u};
        last = last_before;
        CHECK(p4_remove_bank_reserve(&last_bank, (uintptr_t)0x707u,
                                     (uintptr_t)0xc00u, &last) == 0);
        CHECK(last.index == last_before.index &&
              last.generation == last_before.generation);
    }

    CHECK(p4_remove_bank_count_target(NULL, (uintptr_t)0xa00u, &count) == -1);
    CHECK(p4_remove_bank_next_target(&bank, (uintptr_t)0u, 0u, &h4) == -1);
    CHECK(p4_remove_bank_drop(&bank, h4, (uintptr_t)0x404u, 2,
                              &reclaim) == -1);
    CHECK(p4_remove_bank_ack(&bank, h4, (uintptr_t)0xa00u,
                             &notify, NULL) == -1);

    /* Requester retirement cancels every ticket under one gate, preserving
     * target cleanup ownership even across multiple target identities. */
    {
        struct p4_remove_bank cancel_bank = {0};
        struct p4_remove_bank_slot cancel_slots[3] = {{0}};
        struct p4_remove_bank_slot snapshot[3];
        struct p4_remove_handle ca, cb, cc;
        uint32_t dropped = 99u;

        CHECK(p4_remove_bank_init(&cancel_bank, cancel_slots, 3u) == 1);
        CHECK(p4_remove_bank_reserve(&cancel_bank, 11u, 21u, &ca) == 1);
        CHECK(p4_remove_bank_reserve(&cancel_bank, 11u, 22u, &cb) == 1);
        CHECK(p4_remove_bank_reserve(&cancel_bank, 12u, 21u, &cc) == 1);
        CHECK(p4_remove_bank_ack(&cancel_bank, cb, 22u, &notify, &reclaim) == 1);
        CHECK(notify == 11u && reclaim == 0);
        memcpy(snapshot, cancel_slots, sizeof(snapshot));
        CHECK(p4_remove_bank_cancel_requester(&cancel_bank, 11u,
            &cancel_slots[0].generation) == -1);
        CHECK(memcmp(snapshot, cancel_slots, sizeof(snapshot)) == 0);
        cancel_slots[2].active = 2u;
        memcpy(snapshot, cancel_slots, sizeof(snapshot));
        CHECK(p4_remove_bank_cancel_requester(&cancel_bank, 11u, &dropped) == -1);
        CHECK(dropped == 99u &&
            memcmp(snapshot, cancel_slots, sizeof(snapshot)) == 0);
        cancel_slots[2].active = 1u;
        CHECK(p4_remove_bank_cancel_requester(&cancel_bank, 11u, &dropped) == 1);
        CHECK(dropped == 2u && cancel_slots[1].active == 0u);
        CHECK(cancel_slots[0].active == 1u &&
            cancel_slots[0].completion.service_ref == 1u &&
            cancel_slots[0].completion.requester_ref == 0u);
        CHECK(cancel_slots[2].completion.requester_ref == 1u);
        CHECK(p4_remove_bank_cancel_requester(&cancel_bank, 11u, &dropped) == 1);
        CHECK(dropped == 0u);
        CHECK(p4_remove_bank_count_target(&cancel_bank, 21u, &count) == 1);
        CHECK(count == 2u);
        CHECK(p4_remove_bank_ack(&cancel_bank, ca, 21u, &notify, &reclaim) == 1);
        CHECK(notify == 0u && reclaim == 1);
        CHECK(p4_remove_bank_ack(&cancel_bank, cc, 21u, &notify, &reclaim) == 1);
        CHECK(notify == 12u && reclaim == 0);
    }

    printf("p4-remove-bank-test: PASS (%u checks)\n", checks);
    return 0;
}
