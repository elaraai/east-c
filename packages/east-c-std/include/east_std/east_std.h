#ifndef EAST_STD_H
#define EAST_STD_H

#include <east/platform.h>
#include <stddef.h>

// Individual module registration
void east_std_register_console(PlatformRegistry *reg);
void east_std_register_fs(PlatformRegistry *reg);
void east_std_register_path(PlatformRegistry *reg);
void east_std_register_crypto(PlatformRegistry *reg);
void east_std_register_time(PlatformRegistry *reg);
void east_std_register_random(PlatformRegistry *reg);
void east_std_register_fetch(PlatformRegistry *reg);
void east_std_register_parallel(PlatformRegistry *reg);
void east_std_register_test(PlatformRegistry *reg);

// Register all standard platform functions
void east_std_register_all(PlatformRegistry *reg);

#endif
