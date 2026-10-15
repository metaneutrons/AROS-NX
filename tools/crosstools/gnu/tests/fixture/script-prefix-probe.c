#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the actual libiberty implementation supplied by a Binutils build. */
extern char *make_relative_prefix(const char *, const char *, const char *);

int main(int argc, char **argv)
{
    char *directory;
    char *script;
    size_t length;
    int readable;

    if (argc != 4)
        return 2;
    directory = make_relative_prefix(argv[1], argv[2], argv[3]);
    if (directory == NULL)
        return 3;
    length = strlen(directory) + sizeof("/ldscripts/probe.x");
    script = malloc(length);
    if (script == NULL) {
        free(directory);
        return 4;
    }
    snprintf(script, length, "%s/ldscripts/probe.x", directory);
    readable = access(script, R_OK) == 0;
    free(script);
    free(directory);
    return readable ? 0 : 1;
}
