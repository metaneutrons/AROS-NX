#include <stdio.h>
#include <string.h>
#include "../p4_remove_completion.c"
static unsigned int checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "failure at line %d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    struct p4_remove_completion t = {0}, before;
    int notify = -7, reclaim = -9;
    CHECK(p4_remove_completion_init(&t));
    CHECK(!p4_remove_completion_init(&t));
    CHECK(!p4_remove_completion_ready(&t));
    before = t;
    CHECK(!p4_remove_completion_drop(&t, 0, &reclaim));
    CHECK(!memcmp(&before, &t, sizeof(t)) && reclaim == -9);
    CHECK(p4_remove_completion_ack(&t, &notify, &reclaim));
    CHECK(notify == 1 && reclaim == 0 && p4_remove_completion_ready(&t));
    CHECK(!p4_remove_completion_ack(&t, &notify, &reclaim));
    CHECK(p4_remove_completion_drop(&t, 0, &reclaim) && reclaim == 1);
    CHECK(!p4_remove_completion_drop(&t, 0, &reclaim));
    CHECK(!p4_remove_completion_ready(&t));
    memset(&t, 0, sizeof(t));
    CHECK(p4_remove_completion_init(&t));
    CHECK(p4_remove_completion_drop(&t, 1, &reclaim) && reclaim == 0);
    CHECK(t.service_ref == 1 && !p4_remove_completion_ready(&t));
    CHECK(!p4_remove_completion_drop(&t, 1, &reclaim));
    CHECK(p4_remove_completion_ack(&t, &notify, &reclaim));
    CHECK(notify == 0 && reclaim == 1);
    for (unsigned int requester = 0; requester < 3; ++requester)
        for (unsigned int service = 0; service < 3; ++service)
            for (unsigned int done = 0; done < 3; ++done)
            {
                int valid = requester <= 1 && service <= 1 && done <= 1 &&
                            service == 1u - done;
                t = (struct p4_remove_completion){requester, service, done};
                before = t;
                notify = -7; reclaim = -9;
                int accepted = p4_remove_completion_ack(&t, &notify, &reclaim);
                CHECK(accepted == (valid && service == 1));
                if (!accepted)
                    CHECK(!memcmp(&before, &t, sizeof(t)) && notify == -7 && reclaim == -9);
                else
                    CHECK(t.completed == 1 && t.service_ref == 0 &&
                          notify == (requester != 0) && reclaim == (requester == 0));
                for (int cancel = 0; cancel < 3; ++cancel)
                {
                    t = before; reclaim = -9;
                    accepted = p4_remove_completion_drop(&t, cancel, &reclaim);
                    CHECK(accepted == (valid && requester == 1 && cancel <= 1 &&
                                       (cancel || done)));
                    if (!accepted)
                        CHECK(!memcmp(&before, &t, sizeof(t)) && reclaim == -9);
                    else
                        CHECK(t.requester_ref == 0 && t.service_ref == service &&
                              reclaim == (service == 0));
                }
            }
    for (unsigned int offset = 0; offset < sizeof(t); offset += sizeof(uint32_t))
    {
        int *alias = (int *)(void *)((unsigned char *)&t + offset);
        t = (struct p4_remove_completion){1, 1, 0};
        before = t; notify = -7; reclaim = -9;
        CHECK(!p4_remove_completion_ack(&t, alias, &reclaim));
        CHECK(!memcmp(&before, &t, sizeof(t)) && reclaim == -9);
        CHECK(!p4_remove_completion_ack(&t, &notify, alias));
        CHECK(!memcmp(&before, &t, sizeof(t)) && notify == -7);
        CHECK(!p4_remove_completion_drop(&t, 1, alias));
        CHECK(!memcmp(&before, &t, sizeof(t)));
    }
    CHECK(!p4_remove_completion_init(NULL));
    CHECK(!p4_remove_completion_ready(NULL));
    CHECK(!p4_remove_completion_ack(NULL, &notify, &reclaim));
    CHECK(!p4_remove_completion_drop(NULL, 1, &reclaim));
    printf("p4-remove-completion-test: PASS (%u checks)\n", checks);
    return 0;
}
