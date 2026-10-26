#include <stdint.h>

#include "mod_plugins.h"

/* Native-wide 16:9: the runtime widens the draw area and display instead of
 * squashing the GTE projection, so characters keep their proportions. The
 * game's own object culling is still 4:3, and the HUD is not yet anchored to
 * the wide edges. */
static void widescreen_activate(void)
{
    (void)psx_mod_set_fixed_display_aspect(16, 9);
}

PSX_MOD_CONSTRUCTOR(register_widescreen)
{
    (void)psx_mod_register_activation_plugin(
        "bugsbunny.widescreen",
        widescreen_activate
    );
}
