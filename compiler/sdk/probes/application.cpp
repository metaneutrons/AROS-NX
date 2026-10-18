#include <exec/lists.h>
#include <proto/alib.h>
#include <string>

volatile unsigned sdk_probe_anchor;

int main(int argc, char **argv)
{
    struct List list;
    NewList(&list);
    /* reserve/append require the normal driver's out-of-line C++ runtime. */
    std::string value = argc > 1 ? argv[1] : "AROS";
    value.reserve(value.size() + 16U);
    value.append("-SDK");
    sdk_probe_anchor = static_cast<unsigned>(value.size());
    volatile unsigned result = sdk_probe_anchor;
    result += IsListEmpty(&list) ? 1U : 0U;
    return result == 8U ? 0 : 1;
}
