#pragma once

#include "AppFont.h"
#include "AppTypes.h"
#include "Gfx.h"

#include <SDL.h>

#include <string>

// Comic Reader's look: a Golden Age comic book. Yellowed newsprint with
// halftone dots, black ink outlines with a hard drop shadow, the print colors
// (red, yellow, blue), Bangers lettering and Comic Neue for small text.
// Covers sit in inked panel frames; the selection gets a starburst; hints go
// in a yellow caption box and messages in speech bubbles.
//
// Everything draws in logical units on the 720 x 1280 portrait canvas.
namespace Theme {

// --- colors ------------------------------------------------------------------
constexpr SDL_Color Paper     {244, 232, 200, 255};  // newsprint
constexpr SDL_Color PaperDot  {226, 204, 160, 255};  // its halftone dots
constexpr SDL_Color White     {255, 251, 240, 255};  // panel / bubble fill
constexpr SDL_Color Ink       {24, 20, 18, 255};
constexpr SDL_Color Red       {214, 36, 42, 255};
constexpr SDL_Color Yellow    {255, 212, 38, 255};
constexpr SDL_Color Blue      {30, 86, 168, 255};
constexpr SDL_Color Text      {24, 20, 18, 255};     // ink
constexpr SDL_Color Muted     {112, 92, 66, 255};    // faded ink
constexpr SDL_Color Night     {20, 18, 22, 255};     // behind a page while reading

// --- type sizes (logical px) -------------------------------------------------
namespace Type {
constexpr float Hero = 104.0f;
constexpr float Title = 72.0f;
constexpr float Heading = 46.0f;
constexpr float Body = 30.0f;
constexpr float Small = 24.0f;
constexpr float Caption = 20.0f;
}  // namespace Type

constexpr float kMargin = 36.0f;
constexpr float kFooterTop = 1170.0f;
constexpr float kInk = 4.0f;      // outline width
constexpr float kShadow = 6.0f;   // hard drop shadow offset

// Bakes the paper and the starburst. Call once after Gfx::init.
void init(SDL_Renderer* r, float canvasScale);
void shutdown();

// --- surfaces ----------------------------------------------------------------
void paper(SDL_Renderer* r);  // the whole canvas
// A flat box with a black outline and a hard shadow down-right.
void inkBox(SDL_Renderer* r, const FRect& rc, SDL_Color fill, float ink = kInk, float shadow = kShadow);
// A POW starburst centered at (cx, cy), turned by degrees: black edge, the fill,
// and an inner star in `inner` (alpha 0 for none).
void burst(SDL_Renderer* r, float cx, float cy, float radius, double degrees, SDL_Color fill, SDL_Color inner);
// A speech bubble: rounded white box, black outline, a tail under it at tailX.
void bubble(SDL_Renderer* r, const FRect& rc, float tailX);
// Progress: black outline, white, filled red to f (0..1).
void progress(SDL_Renderer* r, const FRect& rc, float f, SDL_Color fill = Red);

// --- lettering ---------------------------------------------------------------
float width(SDL_Renderer* r, const std::string& s, float size, AppFont::Face face = AppFont::Face::Body);
// Comic lettering: the text in `fill` with a black outline all round and a
// black drop shadow (Display face).
void letter(SDL_Renderer* r, const std::string& s, float x, float y, float size, SDL_Color fill, float outline = 3.0f);
void letterCentered(SDL_Renderer* r, const std::string& s, float cx, float y, float size, SDL_Color fill, float outline = 3.0f);
std::string upper(std::string s);
std::string ellipsize(SDL_Renderer* r, std::string s, float maxW, float size, AppFont::Face face = AppFont::Face::Body);
float fitSize(SDL_Renderer* r, const std::string& s, float maxW, float size, float minSize,
              AppFont::Face face = AppFont::Face::Body);

// --- pieces ------------------------------------------------------------------
// The red masthead, "COMIC READER!" in yellow.
void masthead(SDL_Renderer* r);
// The yellow caption box along the bottom with button hints:
// "A Read   LB/RB Shelves   B Exit" (key, then label, groups split by 3 spaces).
void footerHints(SDL_Renderer* r, const std::string& line);
// A question in a speech bubble with two answers; selected 0 or 1.
void confirmDialog(SDL_Renderer* r, const std::string& question, const std::string& cancelLabel,
                   const std::string& confirmLabel, int selected);
void emptyState(SDL_Renderer* r, const std::string& big, const std::string& hint);
// A short message in a speech bubble near the bottom, fading with alpha (0..1).
void toast(SDL_Renderer* r, const std::string& text, float alpha);

}  // namespace Theme
