#ifndef INPUT_H
#define INPUT_H

#include <xkbcommon/xkbcommon.h>
#include "velo.h"

void input_handle_keypress(struct velo *velo, xkb_keycode_t keycode);
void input_scroll_up(struct velo *velo);
void input_scroll_down(struct velo *velo);
void input_select_result(struct velo *velo, uint32_t index);
void input_refresh_results(struct velo *velo);

#endif /* INPUT_H */
