#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifndef NEED_RUNLEVELS_DEF
#define NEED_RUNLEVELS_DEF
#endif
#include "listing.h"


int map_has_runlevels(void)
{
    return RUNLEVELS;
}

char map_runlevel_to_key(const int runlevel)
{
    if (runlevel >= RUNLEVELS) {
        error("Wrong runlevel %d\n", runlevel);
    }
    return runlevel_locations[runlevel].key;
}

ushort map_key_to_lvl(const char key)
{
    int runlevel;
    const char uckey = toupper(key);
    for (runlevel = 0; runlevel < RUNLEVELS; runlevel++) {
        if (uckey == runlevel_locations[runlevel].key)
            return runlevel_locations[runlevel].lvl;
    }
    error("Wrong runlevel key '%c'\n", uckey);
    return 0;
}

const char *map_runlevel_to_location(const int runlevel)
{
    if (runlevel >= RUNLEVELS) {
        error("Wrong runlevel %d\n", runlevel);
    }
    return runlevel_locations[runlevel].location;
}

ushort map_runlevel_to_lvl(const int runlevel)
{
    if (runlevel >= RUNLEVELS) {
        error("Wrong runlevel %d\n", runlevel);
    }
    return runlevel_locations[runlevel].lvl;
}

ushort map_runlevel_to_seek(const int runlevel)
{
    return runlevel_locations[runlevel].seek;
}

