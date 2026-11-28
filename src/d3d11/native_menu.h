#pragma once
#include "../common/native_menu.h"

// Frame generation (fix.frame_generation, Lock): EDVR's menu on S. B's door (native_sharpen.cpp) finishes S before the runtime draws the menu on B, so the menu is drawn on the
// finished S here, in place, the way the runtime's treat (native_menu.cpp treat) will draw it on B: the same panel, alpha, frustum and bounds, from the pose the runtime
// published for this frame, with S's eye in place of B's (B's eye carried by the head's step S was built with, x_S = rot x_B + t). A head-locked panel rides the head, so it
// sits on S's pixels exactly where it sits on B's; an anchored one is seen from S's eye. Same thread and device as the treat. Returns kFrameGenFinishDone (drawn, or the menu
// shows nothing on this eye, as B would get nothing) or kFrameGenFinishMenu (the menu shows and cannot be drawn: no pose published, another thread or device).
uint32_t nativeMenuFrameGenDraw(ID3D11DeviceContext* ctx, uint32_t eye, ID3D11Texture2D* target, const float box[4], const float rot[9], const float t[3]);
