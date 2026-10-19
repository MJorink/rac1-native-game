// The title scene's view as its world passes saw it (see mrp_title_view.h). Host code.
#include <game/render/mrp_title_view.h>

#include <game/ovl/ovl_reloc.h>
#include <runtime/kernel/ee_memory.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace rac1 {

namespace {
struct Window {
    uint32_t addr = 0, size = 0;
};
// the two windows (their EE addresses translated when noted: the boot module's image)
std::array<Window, 2> g_win;
std::array<std::array<uint8_t, 0x380>, 2> g_noted, g_game;
bool g_have = false, g_in = false;
bool g_menu = false;
}  // namespace

// orig: (host) see mrp_title_view.h
void mrp_title_view_note() {
    g_win[0] = {OV(0x00166c80u), 0x380};
    g_win[1] = {OV(0x0016ccc0u), 0x40};
    for (size_t k = 0; k < g_win.size(); ++k) {
        const void* p = ee_ram_ptr_read(g_win[k].addr);
        if (p == nullptr) return;
        std::memcpy(g_noted[k].data(), p, g_win[k].size);
    }
    g_have = true;
}

// orig: (host) see mrp_title_view.h
bool mrp_title_view_swap_in() {
    if (!g_have || g_in) return false;
    for (size_t k = 0; k < g_win.size(); ++k) {
        uint8_t* p = static_cast<uint8_t*>(ee_ram_ptr(g_win[k].addr));
        if (p == nullptr) return false;
        std::memcpy(g_game[k].data(), p, g_win[k].size);
        std::memcpy(p, g_noted[k].data(), g_win[k].size);
    }
    g_in = true;
    return true;
}

// orig: (host) see mrp_title_view.h
void mrp_title_view_swap_out() {
    if (g_in)
        for (size_t k = 0; k < g_win.size(); ++k) std::memcpy(ee_ram_ptr(g_win[k].addr), g_game[k].data(), g_win[k].size);
    g_in = false;
    g_have = false;
}

// orig: (host) see mrp_title_view.h
void mrp_title_view_note_menu(bool menu) { g_menu = menu; }
// orig: (host) see mrp_title_view.h
bool mrp_title_menu_drawn() { return g_menu; }

}  // namespace rac1
