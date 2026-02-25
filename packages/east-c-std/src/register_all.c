/*
 * Register all standard platform functions.
 */
#include <east_std/east_std.h>

void east_std_register_all(PlatformRegistry *reg)
{
    east_std_register_console(reg);
    east_std_register_fs(reg);
    east_std_register_path(reg);
    east_std_register_crypto(reg);
    east_std_register_time(reg);
    east_std_register_random(reg);
    east_std_register_fetch(reg);
    east_std_register_parallel(reg);
    east_std_register_test(reg);
}
