// The title scene's view as its world passes saw it. Host code.
//
// The boot module's title draw (0x1eb410) runs its world passes with the title's camera, then in its main menu (game mode 3)
// the menu's draw (0x2196b8) sets the menu's own camera (0x276128 at (256, 256, 64)) and view (0x1f6e48) for the menu's mobys:
// after the update the game's view globals are the menu's. The PS2 drew the passes' chain as they built it; the host's
// readers of the view after the update (the MRP world's capture and its GL projection, render_view.h) need the passes' view.
// So the view block the passes read (0x166c80..0x167000 and 0x16ccc0..0x16cd00, Veldin's addresses: 0x1f6e48's outputs, the
// camera 0x166dc0 and its rows 0x166fd0..) is copied when the passes end (title_draw calls mrp_title_view_note) and swapped
// into EE memory around the host's prepare and draw of that frame, then the game's own bytes put back: the game never sees it.
#ifndef RAC1_GAME_RENDER_MRP_TITLE_VIEW_H
#define RAC1_GAME_RENDER_MRP_TITLE_VIEW_H

namespace rac1 {

// orig: (host) the view block copied (the boot's title draw, after its world passes)
void mrp_title_view_note();
// orig: (host) the noted view swapped into EE memory (true when there was one); _out puts the game's bytes back and forgets
// the note
bool mrp_title_view_swap_in();
void mrp_title_view_swap_out();
// orig: (host) the boot's title draw (0x1eb410) of the last update drew its main menu (game mode 3: title_menu_draw 0x2196b8):
// its mobys are then the menu's boxes, seen from the menu's camera and placed under the menu's 2D (their 4:3 screen boxes,
// menu_box_project 0x299f40), so the host draws them in the game's own 4:3 projection, not the title scene's widened one
// (set by title_draw each frame)
void mrp_title_view_note_menu(bool menu);
bool mrp_title_menu_drawn();

}  // namespace rac1

#endif
