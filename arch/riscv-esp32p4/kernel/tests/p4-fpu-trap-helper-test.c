/* Host regression for the production P4 entry-spill helper. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_trap_frame.h"

static unsigned int checks;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL p4 fpu trap helper: %s:%d: %s\n", \
                __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void fill_image(unsigned char *image, unsigned int seed)
{
    size_t i;

    for (i = 0; i < P4_TRAP_FPU_SPILL_BYTES; i++)
        image[i] = (unsigned char)(seed + (unsigned int)(i * 37));
}

int main(void)
{
    struct P4TrapFrameLayout outer;
    struct P4TrapFrameLayout nested;
    unsigned char task_image[P4_TRAP_FPU_SPILL_BYTES];
    unsigned char nested_task_image[P4_TRAP_FPU_SPILL_BYTES];
    unsigned char entry_image[P4_TRAP_FPU_SPILL_BYTES];
    unsigned char handler_live_state[P4_TRAP_FPU_SPILL_BYTES];
    unsigned char clean_saved_image[P4_TRAP_FPU_SPILL_BYTES];
    unsigned long fs_states[] = {
        0,
        0x00002000UL,
        0x00004000UL,
        0x00006000UL
    };
    size_t i;

    fill_image(handler_live_state, 0x11);
    memcpy(outer.fpu_spill, handler_live_state, sizeof(outer.fpu_spill));
    memcpy(entry_image, handler_live_state, sizeof(entry_image));

    /* Model arbitrary FP use by C after entry captured the spill. */
    fill_image(handler_live_state, 0xA5);
    CHECK(memcmp(entry_image, handler_live_state, sizeof(entry_image)) != 0);

    memset(task_image, 0xCC, sizeof(task_image));
    p4_trap_fpu_copy_entry_spill(task_image, outer.fpu_spill);
    CHECK(memcmp(task_image, entry_image, sizeof(task_image)) == 0);
    CHECK(memcmp(task_image, handler_live_state, sizeof(task_image)) != 0);

    /* A nested trap owns a distinct stack image and cannot overwrite outer. */
    fill_image(nested.fpu_spill, 0x62);
    memset(nested_task_image, 0xD3, sizeof(nested_task_image));
    p4_trap_fpu_copy_entry_spill(nested_task_image, nested.fpu_spill);
    CHECK(memcmp(nested_task_image, nested.fpu_spill,
                 sizeof(nested_task_image)) == 0);
    CHECK(memcmp(outer.fpu_spill, entry_image, sizeof(entry_image)) == 0);

    /* Only FS=DIRTY persists to a task image; INITIAL/CLEAN/OFF do not. */
    memset(clean_saved_image, 0x7B, sizeof(clean_saved_image));
    for (i = 0; i < sizeof(fs_states) / sizeof(fs_states[0]); i++)
    {
        int should_save = p4_trap_fpu_state_is_dirty(fs_states[i]);

        CHECK(should_save == (i == 3));
        if (should_save)
            p4_trap_fpu_copy_entry_spill(clean_saved_image, outer.fpu_spill);
        else
            CHECK(clean_saved_image[0] == 0x7B);
    }
    CHECK(memcmp(clean_saved_image, entry_image, sizeof(entry_image)) == 0);

    printf("PASS P4 FPU trap helper (%u checks)\n", checks);
    return 0;
}
