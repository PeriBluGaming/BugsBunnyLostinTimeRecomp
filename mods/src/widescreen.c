#include <stdint.h>

#include "cpu_state.h"
#include "mod_plugins.h"

/* Runtime (gpu.h): is the frame presented native-wide right now? The anchor
 * uses the same test; it is 0 for frames shown 4:3 even with the mod on. */
extern int ws_native_wide_active(void);

/* Native-wide 16:9: the runtime widens the draw area and display instead of
 * squashing the GTE projection, so characters keep their proportions. The
 * renderer's on-screen test is widened in game.toml ([widescreen.cull]). */
static void widescreen_activate(void)
{
    (void)psx_mod_set_fixed_display_aspect(16, 9);
}

/* HUD. Every HUD icon and digit is a small 3D object in the group list at
 * *0x80069508, drawn before the world by 0x800399C8: it projects the objects
 * of a group, then calls the RAM renderer once per object (jal 0x80010258 at
 * 0x80039B98, so $ra = 0x80039BA0). The renderer gets the object in $a0 and
 * the packet pointer *0x80076188 in $a1, writes contiguous tagged polygons
 * from $a1 and returns the new pointer, which the caller stores back. The next
 * renderer call (HUD or world, every caller passes *0x80076188) therefore
 * starts where the previous object's packets end, and that is where they are
 * finished. Menus, the level select and in-engine cutscenes draw no HUD group.
 *
 * An object's screen position is its GTE offset (context +0x14, 16.16 pixels
 * from the centre). Corner widgets sit at |OFX| >= 128; the health carrots,
 * the pause stats (|OFX| <= 96) and the pause "0%" (bottom left) stay put.
 * The anchor moves the corner widgets by the live reveal, only in frames shown
 * native-wide; while a frame is shown native-wide the packets also carry a
 * small fixed correction so both sides keep a 20 px gap to the wide edges (the
 * original gaps are 12 px left, 18 px right). Frames shown 4:3 keep the
 * original HUD. */
#define BB_RENDER_OBJECT      0x80010258u
#define BB_RENDER_OBJECT_ALT  0x80010204u
#define BB_HUD_RENDER_RA      0x80039BA0u
#define BB_PACKET_PTR         0x80076188u
#define BB_OBJ_OFX            0xA8u   /* object +0x94 context, +0x14 */
#define BB_OBJ_OFY            0xACu   /* object +0x94 context, +0x18 */
#define BB_SCREEN_WIDTH       512u
#define BB_HUD_EDGE_OFX       128
#define BB_HUD_SIDE_X         96      /* packet X is relative to the centre */
#define BB_HUD_RANGE_MAX      0x2000u
#define BB_HUD_TARGET_GAP     20
#define BB_HUD_LEFT_GAP       12
#define BB_HUD_RIGHT_GAP      18

/* An anchor tag stays fresh for two frames, but the HUD is built first and
 * the frame can be drawn several VBlanks later (30 fps; longer when the pause
 * opens), so each finished packet is re-tagged at every VBlank while its
 * words are unchanged; the correction it carries is matched to the current
 * presentation at the same time. A packet whose words changed (its buffer
 * reused) is dropped; one rebuilt at the same address replaces its entry. */
#define BB_HUD_LIST_MAX       1024u
#define BB_HUD_LIST_VBLANKS   60u

static uint32_t hud_start;   /* first packet of the pending HUD object, 0 = none */
static int hud_edge;
static struct { uint32_t prim, hash, born; int edge, dx; } hud_list[BB_HUD_LIST_MAX];
static unsigned hud_count;
static uint32_t hud_vblanks;

static uint32_t hud_hash(uint32_t prim, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 1; i <= len; ++i)
        h = (h ^ psx_mod_read_word(prim + 4u * i)) * 16777619u;
    return h;
}

/* Word offsets (from the tag) of the vertices of a renderer polygon; 0 when
 * the command or the tag length is not one the renderer emits. */
static unsigned hud_poly_vertices(uint32_t cmd, uint32_t len, unsigned v[4])
{
    static const struct { uint8_t len, n, v[4]; } shapes[8] = {
        { 4, 3, { 2, 3, 4 } },        /* 0x20 F3 */
        { 7, 3, { 2, 4, 6 } },        /* 0x24 FT3 */
        { 5, 4, { 2, 3, 4, 5 } },     /* 0x28 F4 */
        { 9, 4, { 2, 4, 6, 8 } },     /* 0x2C FT4 */
        { 6, 3, { 2, 4, 6 } },        /* 0x30 G3 */
        { 9, 3, { 2, 5, 8 } },        /* 0x34 GT3 */
        { 8, 4, { 2, 4, 6, 8 } },     /* 0x38 G4 */
        { 12, 4, { 2, 5, 8, 11 } },   /* 0x3C GT4 */
    };
    if ((cmd & 0xE0u) != 0x20u)
        return 0;
    unsigned s = (cmd >> 2) & 7u;
    if (shapes[s].len != len)
        return 0;
    for (unsigned i = 0; i < shapes[s].n; ++i)
        v[i] = shapes[s].v[i];
    return shapes[s].n;
}

/* Correction for an edge while the frame is presented native-wide, else 0. */
static int hud_correction(int edge)
{
    if (!ws_native_wide_active())
        return 0;
    return edge < 0 ? BB_HUD_TARGET_GAP - BB_HUD_LEFT_GAP
                    : BB_HUD_RIGHT_GAP - BB_HUD_TARGET_GAP;
}

static void hud_shift(uint32_t prim, uint32_t len, int32_t dx)
{
    unsigned v[4];
    unsigned n = hud_poly_vertices(psx_mod_read_byte(prim + 7u), len, v);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t a = prim + 4u * v[i];
        psx_mod_write_half(a, (uint16_t)((int16_t)psx_mod_read_half(a) + dx));
    }
}

/* Correct and anchor the packets of one HUD object. Stops at the first packet
 * that is not a renderer polygon on the object's side, so nothing written by
 * other code after the object can be moved. */
static void hud_finish(uint32_t start, uint32_t end, int edge)
{
    int dx = hud_correction(edge);
    uint32_t p = start;
    while (p < end) {
        uint32_t len = psx_mod_read_word(p) >> 24;
        unsigned v[4];
        unsigned n = hud_poly_vertices(psx_mod_read_byte(p + 7u), len, v);
        if (!n || p + 4u * (len + 1u) > end) {
            psx_mod_counter_add("bugsbunny.widescreen.hud.stop", 1u);
            return;
        }
        for (unsigned i = 0; i < n; ++i) {
            int32_t x = (int16_t)psx_mod_read_half(p + 4u * v[i]);
            if (edge < 0 ? x >= -BB_HUD_SIDE_X : x <= BB_HUD_SIDE_X) {
                psx_mod_counter_add("bugsbunny.widescreen.hud.side_stop", 1u);
                return;
            }
        }
        hud_shift(p, len, dx);
        psx_mod_anchor_hud_primitive(p, edge);
        unsigned slot = 0;
        while (slot < hud_count && hud_list[slot].prim != p)
            ++slot;
        if (slot == hud_count && hud_count < BB_HUD_LIST_MAX)
            ++hud_count;
        if (slot < hud_count) {
            hud_list[slot].prim = p;
            hud_list[slot].hash = hud_hash(p, len);
            hud_list[slot].born = hud_vblanks;
            hud_list[slot].edge = edge;
            hud_list[slot].dx = dx;
        } else {
            psx_mod_counter_add("bugsbunny.widescreen.hud.list_full", 1u);
        }
        psx_mod_counter_add("bugsbunny.widescreen.hud.prims", 1u);
        p += 4u * (len + 1u);
    }
}

/* Finish the pending object at $a1 of the next renderer call, or at a VBlank
 * once its caller has stored the returned pointer. Each range is finished
 * once: hud_start is cleared first. */
static void hud_close(uint32_t end)
{
    uint32_t start = hud_start;
    hud_start = 0;
    if (end >= start && end - start <= BB_HUD_RANGE_MAX) {
        if (hud_edge)
            hud_finish(start, end, hud_edge);
    } else {
        psx_mod_counter_add("bugsbunny.widescreen.hud.stale", 1u);
    }
}

static void widescreen_hud_vblank(void)
{
    ++hud_vblanks;
    if (hud_start) {
        uint32_t ptr = psx_mod_read_word(BB_PACKET_PTR);
        if (ptr > hud_start && ptr - hud_start <= BB_HUD_RANGE_MAX)
            hud_close(ptr);
    }
    unsigned kept = 0;
    for (unsigned i = 0; i < hud_count; ++i) {
        uint32_t p = hud_list[i].prim;
        if (hud_vblanks - hud_list[i].born > BB_HUD_LIST_VBLANKS)
            continue;
        uint32_t len = psx_mod_read_word(p) >> 24;
        if (hud_hash(p, len) != hud_list[i].hash) {
            psx_mod_counter_add("bugsbunny.widescreen.hud.changed", 1u);
            continue;
        }
        int dx = hud_correction(hud_list[i].edge);
        if (dx != hud_list[i].dx) {
            hud_shift(p, len, dx - hud_list[i].dx);
            hud_list[i].hash = hud_hash(p, len);
            hud_list[i].dx = dx;
            psx_mod_counter_add("bugsbunny.widescreen.hud.recorrected", 1u);
        }
        psx_mod_anchor_hud_primitive(p, hud_list[i].edge);
        hud_list[kept++] = hud_list[i];
    }
    hud_count = kept;
}

static int widescreen_hud_filter(struct CPUState* cpu, uint32_t address)
{
    (void)address;
    uint32_t a1 = cpu->gpr[5];
    if (hud_start)
        hud_close(a1);

    if (cpu->gpr[31] != BB_HUD_RENDER_RA ||
        psx_mod_widescreen_view_x_margin() <= 0 ||
        psx_mod_display_width() != BB_SCREEN_WIDTH)
        return 0;

    uint32_t obj = cpu->gpr[4];
    int32_t ofx = (int32_t)psx_mod_read_word(obj + BB_OBJ_OFX) >> 16;
    int32_t ofy = (int32_t)psx_mod_read_word(obj + BB_OBJ_OFY) >> 16;
    if (ofx <= -BB_HUD_EDGE_OFX && ofy < 0)
        hud_edge = -1;
    else if (ofx >= BB_HUD_EDGE_OFX)
        hud_edge = 1;
    else
        return 0;
    hud_start = a1;
    return 0;
}

static void widescreen_hud_reset(void)
{
    hud_start = 0;
    hud_count = 0;
}

PSX_MOD_CONSTRUCTOR(register_widescreen)
{
    (void)psx_mod_register_activation_plugin(
        "bugsbunny.widescreen",
        widescreen_activate
    );
    (void)psx_mod_register_function_filter_plugin(
        "bugsbunny.widescreen", BB_RENDER_OBJECT, widescreen_hud_filter);
    (void)psx_mod_register_function_filter_plugin(
        "bugsbunny.widescreen", BB_RENDER_OBJECT_ALT, widescreen_hud_filter);
    (void)psx_mod_register_vblank_plugin(
        "bugsbunny.widescreen", widescreen_hud_vblank);
    (void)psx_mod_register_session_reset_plugin(
        "bugsbunny.widescreen", widescreen_hud_reset);
    (void)psx_mod_register_savestate_plugin(
        "bugsbunny.widescreen", widescreen_hud_reset);
}
