// text.h — letters.
//
// This lived in menu.c, under a comment that said: "the font lives here, not in the engine.
// Text is a capability every game eventually wants, and the moment a second one asks, it
// earns a move to src/. Until then an engine with a font in it is an engine with an opinion
// about letters."
//
// A game that ends asked. A run that ends has a distance, and a distance is a number, and a number has
// to be said out loud — so the rule fires exactly as written, and the font moves.
//
// It's still not an opinion about letters: five columns of bits and a blitter. There's no
// layout, no wrapping, no kerning and no font file. A game that wants those can have them
// when a second game wants them too.
#ifndef TEXT_H
#define TEXT_H
#include <stdint.h>

void text_put(int x, int y, int s, char c, uint8_t ci);   // one glyph, s pixels per bit
void text_draw(int x, int y, int s, const char *str, uint8_t ci);
int  text_width(const char *str, int s);                  // pixels, for centring
#endif
