/* Every PC system library. A new file in runtime/system/ adds its table here. */
#include "recomp_system.h"

extern const RecompSystemTable recomp_system_p4tp;

const RecompSystemTable *const recomp_system_tables[] = {
    &recomp_system_p4tp,
};
const uint64_t recomp_system_table_count = sizeof recomp_system_tables / sizeof recomp_system_tables[0];
