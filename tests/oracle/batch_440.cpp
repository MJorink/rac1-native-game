// Oracle batch 440 (issue #24): the main loop's pass in game mode 1, a movie playing. The PS2 runs the whole movie
// inside the mode's update 0x2999a8, in one pass of the main loop 0x245c28; the pass then goes on at 0x245fc8: the
// frames-in-mode count 0x15f5c8, and, the movie's end having cut the frame short (0x15f5d8), the DMA wait 0x2a09d8 and
// 0x2a07f8 before the loop's next pass (0x245ee0). The frame's end (0x2460d4..: the cards' machines, the frame counter
// 0x15f3f8, the autosave tick 0x284c50 that counts the save's time played 0x15ee24) is not reached. The host runs the
// movie one picture a host frame (overlay_movie_pass, overlay_entry.cpp); before this batch's fix each of those host
// frames ran a whole pass (its start and its frame's end): the time played counted the movie's length.
//
// The original side runs 0x245c28 from the update's call (0x245fc0, the pass's mode in $s0 = 1) to the loop's next
// pass (0x245ee0, a hook that returns); the port side calls overlay_movie_pass until the pass is over. The movie is one
// of Veldin's own (the level header's table, 0x137b80 + 0x1808: sector, size), played to its end on both sides through
// the host's player (as batch 96's stand-ins), or a movie that cannot be opened (one host frame); the talk that played it
// stepped on at the end (its next record may start another movie). The counters the frame's end would
// change are random. Run with build/tests/ovl_oracle --batch 440 (level 0, needs the ISO; no dumps).

#include "ovl_oracle.h"

#include <game/lib/libcdvd.h>
#include <game/lib/libmc.h>
#include <game/lib/movie_player.h>
#include <game/ovl/arrival.h>
#include <game/ovl/cutscene.h>
#include <game/ovl/level_change.h>
#include <game/ovl/memory_card.h>
#include <game/ovl/movie_mode.h>
#include <game/ovl/movies.h>
#include <game/ovl/ovl_reloc.h>
#include <game/ovl/veldin_mobys.h>
#include <runtime/ipu/movie_hw.h>
#include <runtime/kernel/disc.h>

namespace rac1 {
bool overlay_movie_pass(bool& done);  // overlay_entry.cpp: the host's frames of the main loop's pass in game mode 1
}

namespace rac1::oracle {

void batch_440() {
    if (g_only_batch != 440) return;
    g_batch = 440;
    g_ignored_words = vcross_lanes();
    g_ignored_words.push_back(OV(0x00160188));  // the particle update's saved return address
    g_ignored_words.push_back(OV(0x0016018c));
    const bool null_ram = g_ee_null_is_ram;
    g_ee_null_is_ram = true;
    g_host_clock_fixed = 1759658400;
    g_extra_hooks = {{0x00121780, [](EeCpu& c) { c.set32(2, cd_read_clock(c.r32(4))); }},
                     {0x0012d6d8, [](EeCpu& c) { clock_to_local(c.r32(4)); }},
                     {0x00123d68, [](EeCpu& c) { c.set32(2, mc_get_info(static_cast<int32_t>(c.r32(4)), static_cast<int32_t>(c.r32(5)), c.r32(6), c.r32(7), c.r32(8))); }},
                     {0x00123c30, [](EeCpu& c) { c.set32(2, mc_sync(static_cast<int32_t>(c.r32(4)), c.r32(5), c.r32(6))); }},
                     {OVF(0x00266040), [](EeCpu& c) { c.set32(2, disc_read_wait(c.r32(4), c.r32(5), c.r32(6))); }},
                     {0x00121450, [](EeCpu& c) { c.set32(2, cd_read(c.r32(4), c.r32(5), c.r32(6), c.r32(7))); }},
                     {0x00121630, [](EeCpu& c) { c.set32(2, cd_get_error()); }},
                     {0x001216c8, [](EeCpu& c) { c.set32(2, cd_break()); }},
                     {OVF(0x00267898), [](EeCpu&) { pad_update(); }},
                     // the GS memory read back (sceGsExecStoreImage: the DMA from the VIF1 FIFO, not modelled by the interpreter)
                     {0x001227d8, [](EeCpu& c) { c.set32(2, gs_exec_store_image(c.r32(4), c.r32(5))); }},
                     // the movie player's entry points: the host's stand-ins (batch 96's)
                     {OVF(0x002ee130), [](EeCpu& c) { c.set32(2, movie_player_open(c.r32(4), c.r32(5), static_cast<int32_t>(c.r32(6))) ? 1 : 0); }},
                     {OVF(0x002eddd0), [](EeCpu& c) {
                          movie_player_loop_start(c.r32(4), c.r32(5), c.r32(6));
                          while (!movie_player_loop_frame()) {
                          }
                          c.set32(2, 0);
                      }},
                     {OVF(0x002ee3d8), [](EeCpu&) { movie_player_close(); }},
                     // the movie player as the port runs it (0x2999a8's callee): the host's player to the movie's end
                     {OVF(0x002edd28), [](EeCpu& c) {
                          movie_stream_start(c.r32(4), c.r32(5), c.r32(6), c.r32(7), static_cast<int32_t>(c.r32(8)));
                          while (!movie_stream_frame()) {
                          }
                          c.set32(2, 0);
                      }},
                     // the main loop's next pass: the original side's end
                     {OVF(0x00245ee0), [](EeCpu& c) { c.set32(31, static_cast<int32_t>(0xfffffff0u)); }}};  // ($ra: the interpreter call's return sentinel)
    // the callees left TODO_PORT: the kernel's vsync wait, thread priority and vsync callback, the DMA kick, the screen
    // fade, the level change request, the effects of other levels' scenes (batch 96's)
    g_extra_noop_hooks = {0x00122298, 0x001188e0, 0x001188a0, 0x00122e68, OVF(0x002a08c8), OVF(0x001f8540), OVF(0x0023af30),
                          0x001187d0, 0x001190f8, OVF(0x00243d38), OVF(0x002900a0), OVF(0x0023e008), OVF(0x0026ced0), OVF(0x0024d430)};
    auto result = [](uint32_t id, uint32_t a0) -> uint32_t {
        const uint32_t h = (id * 0x9e3779b1u) ^ (a0 * 0x85ebca6bu);
        return ((h >> 7) % 4 == 0) ? 0u : ((h >> 3) | 1u);
    };
    sif_rpc_register(0x123456, [result](uint32_t fno, uint32_t send, uint32_t, uint32_t recv, uint32_t) {
        if (fno == 0x4d) {
            const uint32_t n = ee_read<uint32_t>(send);
            uint32_t p = send + 4;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t id = ee_read<uint16_t>(p), size = ee_read<uint16_t>(p + 2);
                ee_write<uint32_t>(recv + 4 + i * 4, result(id, size != 0 ? ee_read<uint32_t>(p + 4) : 0u));
                if (id == 0x38) {
                    disc_read_ee(ee_read<uint32_t>(p + 0xc), ee_read<uint32_t>(p + 4), ee_read<uint32_t>(p + 8));
                    ee_write<uint32_t>(0x00137b10, 0);
                    ee_write<uint32_t>(0x00137b00, 0);
                }
                p += 4 + ((size + 3) & ~3u);
            }
            ee_write<uint32_t>(recv, 0xffffffffu);
            ee_write<uint32_t>(recv + 4 + n * 4, 0xffffffffu);
            return;
        }
        ee_write<uint32_t>(recv + 4, result(fno, send != 0 ? ee_read<uint32_t>(send) : 0u));
        ee_write<uint32_t>(recv, 0xffffffffu);
        ee_write<uint32_t>(recv + 8, 0xffffffffu);
    });
    // the pass's mode in $s0 (0x245f54: the game mode read before the switch)
    g_pre_call = [](EeInterpreter& ee) { ee.cpu().set32(16, 1); };
    static const uint32_t S = 0x001516d0, P = 0x0013f350, TK = OV(0x00179100);
    static std::vector<uint8_t> snap;
    static uint32_t ship = 0;  // the Veldin ship moby (class 834, the talker whose script has the movie)
    static std::vector<std::pair<uint32_t, uint32_t>> movies;  // Veldin's movies (sector, size), the shortest first
    auto level = [] {
        uint8_t* ram = ee_get_ram_base();
        if (snap.empty()) {
            veldin_boot();
            level_setup_body(1, 0);
            ee_write<uint16_t>(0x001518d0, 512);
            ee_write<uint16_t>(0x001518d2, 416);
            particle_table_init();
            hud_init();
            w32(OV(0x0016c060), 0xf);
            w32(0x0015ec8c, 1);
            w32(0x00137b00, 0);
            sub_00264bf8();
            sub_002a07f8();
            snap.assign(ram, ram + 0x02000000);
            for (uint32_t m = ee_read<uint32_t>(OV(0x0015ffd8)); ee_read<uint8_t>(m + 0x20) != 0xff; m += 0x100) {
                const uint32_t fn = ee_read<uint32_t>(m + 0x74);
                if (ee_read<int16_t>(m + 0xa6) == 834) ship = m;
                if (fn != 0 && !ovl_moby_fn_ported(fn)) g_extra_noop_hooks.push_back(fn);
            }
            for (uint32_t n = 0; n < 0x40; ++n) {  // (movie_play 0x299460's table)
                const uint32_t sector = ee_read<uint32_t>(0x00137b80 + 0x1808 + n * 8), size = ee_read<uint32_t>(0x00137b80 + 0x180c + n * 8);
                if (sector != 0 && size != 0 && size < 0x4000000) movies.push_back({sector, size});
            }
            std::sort(movies.begin(), movies.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            std::printf("batch 440: %zu movies in Veldin's table, the shortest %u bytes at sector %u\n", movies.size(),
                        movies.empty() ? 0u : movies[0].second, movies.empty() ? 0u : movies[0].first);
        } else {
            std::memcpy(ram, snap.data(), snap.size());
        }
    };
    // the movie's state as movie_start (0x2995d0) leaves it (batch 96's), on one of the level's movies (`real`) or on a
    // sector no movie starts at; the counters of the frame's end random
    auto movie_state = [](bool real) {
        w32(0x0015ed80, 0);
        w32(0x0015ed88, u(ri(0, 5)));
        if (real && !movies.empty()) {
            const auto& mv = movies[static_cast<size_t>(ri(0, std::min<int>(1, static_cast<int>(movies.size()) - 1)))];
            w32(OV(0x00161048), mv.first);
            w32(OV(0x0016104c), mv.second);
        } else {
            w32(OV(0x00161048), u(ri(500000, 700000)));
            w32(OV(0x0016104c), u(ri(1, 0x1000000)));
        }
        w32(OV(0x00161050), u(ri(0, 5)));
        w32(OV(0x00161054), NOT_ADDR(0x00200000u) + u(ri(0, 0x100000)));
        w32(OV(0x00161058), ee_read<uint32_t>(OV(0x0015f40c)) + u(ri(0, 0x10000)));
        w32(0x0015eed8, ri(0, 2) == 0 ? 2u : coin() ? 0xffffffffu : u(ri(0, 1)));
        w16(S + 0x38, R());
        w8(0x0013e5bb, R());
        w32(OV(0x0015f5c4), 1);
        w32(OV(0x0015f5d8), 0);  // (the pass's start, 0x245f24)
        w32(OV(0x0015f5c8), u(ri(0, 3)));
        w32(OV(0x0015f3f8), R());
        w32(0x0015ee24, R());
        w32(0x0015ee28, R());
        w32(0x0015eea4, R());
        w32(OV(0x0015f5ac), u(ri(0, 1)));
        w32(0x0013d290 + 0xd4, 2);  // the card idle
        w32(0x0013d290 + 0xdc, 0xffffffffu);
    };
    // the talk that played the movie (batch 96's, its records none or movies): the end steps it on, which may start the
    // next record's movie at once (game mode 1 again: the frames-in-mode count goes on)
    auto talk_state = [](int32_t n) {
        wrand(M3, 0x10);
        for (int32_t r = 0; r < n; ++r) {
            const uint32_t e = T + u(r) * 0x1c;
            wrand(e, 7);
            static const uint32_t texts[] = {0, 0x4ee2, 0x4ee5, 0x4f1d};
            w32(e, texts[ri(0, 3)]);
            const int k = ri(0, 3);
            w16(e + 4, k == 0 ? 0xffffu : (0x4000u | u(ri(0, 3))));  // (no scene: batch 96 has the end's scenes)
            w16(e + 6, ri(0, 4) == 0 ? u(r) : u(ri(r, n - 1)));
            w16(e + 8, u(ri(0, 7)));
            w16(e + 0xa, u(ri(0, 6)));
            w16(e + 0xc, u(ri(r, n - 1)));
            w16(e + 0xe, u(ri(r, n - 1)));
            w16(e + 0x10, R() & 0xfu);
        }
        const int32_t i = talker_index(ship);
        if (i >= 0) w32(OV(0x001b1678) + u(i) * 4, T);
        w32(0x0015ed98, u(ri(0, 4000)));
        for (int32_t k = 0; k < 0x30; ++k) w8(0x0013d4c0 + u(k), R() & 1);
        for (int32_t k = 0; k < 0x30; ++k) w8(0x0013d4e8 + u(k), R() & 1);
        for (int32_t k = 0; k < 0x10; ++k) w8(0x0013d388 + u(k), R() & 1);
        w32(OV(0x00160130), coin() ? 0xffffffffu : u(ri(0, 3)));
        talk_init(ship, M3);
        w16(M3 + 0x36, u(ri(0, n - 1)));
        w32(TK + 8, ship);
        w32(TK + 0xc, M3);
        w16(S + 0x5a, 3);
        w32(S + 0x50, (R() & 0x7ffffffeu) | 1u);
        w16(S + 0x5c, 0);
        w32(S + 0x1c, 0xffffffffu);
        w32(P + 0x22a8, ri(0, 8) == 0 ? 0u : 1u);
        w32(OV(0x0015f5cc), u(ri(0, 100)));
    };
    auto hook_new_mobys = [] {
        for (uint32_t m = ee_read<uint32_t>(OV(0x0015ffd8)); ee_read<uint8_t>(m + 0x20) != 0xff; m += 0x100) {
            const uint32_t fn = ee_read<uint32_t>(m + 0x74);
            if (fn != 0 && !ovl_moby_fn_ported(fn) && std::find(g_extra_noop_hooks.begin(), g_extra_noop_hooks.end(), fn) == g_extra_noop_hooks.end())
                g_extra_noop_hooks.push_back(fn);
        }
    };
    auto pass = [] {
        mc_reset();
        bool done = false;
        int frames = 0;
        while (!done && frames < 100000) {
            overlay_movie_pass(done);
            ++frames;
        }
        return frames;
    };
    static int max_frames = 0;
    auto run = [&](const char* name, bool real, int n) {
        movie_hw_enable(real);
        max_frames = 0;
        check(name, OVF(0x00245fc0), n, Ret::None, [level, movie_state, talk_state, hook_new_mobys, real] {
            level();
            mc_reset();
            movie_state(real);
            if (coin()) {
                talk_state(ri(1, 6));
                hook_new_mobys();
            }
            return Args{};
        }, [pass](A) {
            max_frames = std::max(max_frames, pass());
            return uint64_t{0};
        });
        std::printf("  (%s: the longest movie took %d host frames)\n", name, max_frames);
    };
    run("movie_pass", true, 4);
    run("movie_pass_unopened", false, 30);

    movie_hw_enable(true);
    mc_reset();
    g_pre_call = nullptr;
    g_host_clock_fixed = 0;
    g_extra_noop_hooks.clear();
    g_extra_hooks.clear();
    g_ee_null_is_ram = null_ram;
}

}  // namespace rac1::oracle
