#pragma once

// The overlay's side of Domination (domination.h): a marker over each spot in its owner's colour
// (yours blue, open white), the round's timer and count at the top, the spot list and standings on
// the right, the trick going on and what each landed trick did, and the results after the round.
// All of it on the background draw list, under ReSkate's own menus and chat, taking no input.
namespace dingosdk::overlay {
// Every presented frame: whether there is something to draw (so the overlay renders with the menu
// closed), then the drawing.
bool domination_pending();
void draw_domination();
}
