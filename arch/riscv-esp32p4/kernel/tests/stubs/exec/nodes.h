/* Host-only subset needed to compile the private console resource ABI. */
#ifndef P4_TEST_EXEC_NODES_H
#define P4_TEST_EXEC_NODES_H

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    unsigned char ln_Type;
    signed char ln_Pri;
    char *ln_Name;
};

#define NT_RESOURCE 8

#endif
