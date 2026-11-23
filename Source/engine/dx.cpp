/**
 * @file dx.cpp
 *
 * Implementation of functions setting up the graphics pipeline.
 */
#include "engine/dx.h"

#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "controls/control_mode.hpp"
#include "controls/plrctrls.h"
#include "engine/render/primitive_render.hpp"
#include "headless_mode.hpp"
#include "init.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/sdl_wrap.h"
#include "utils/static_vector.hpp"
#if defined(HELLSPAWN) && defined(__3DS__) /* HS3DS-WIDE */
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game_mode.hpp"
#include "control/control.hpp"
#include "player.h"
#include "tables/playerdat.hpp"
#include "doom.h" /* HS3DS-UITOP */
#include "gmenu.h"
#include "help.h"
#include "minitext.h"
#include "qol/chatlog.h"
#include "stores.h"
#endif

#ifndef USE_SDL1
#include "controls/touch/renderers.h"
#endif

#ifdef __3DS__
#include <3ds.h>
#endif

namespace devilution {

int refreshDelay;
SDL_Renderer *renderer;
#ifndef USE_SDL1
SDLTextureUniquePtr texture;
#endif

/** Currently active palette */
SDLPaletteUniquePtr Palette;

/** 24-bit renderer texture surface */
SDLSurfaceUniquePtr RendererTextureSurface;

/** 8-bit surface that we render to */
SDL_Surface *PalSurface;
namespace {
SDLSurfaceUniquePtr PinnedPalSurface;

#ifndef USE_SDL1
constexpr size_t MaxDirtyRects = 16;
StaticVector<SDL_Rect, MaxDirtyRects> DirtyRects;
bool ForceFullUpdate;
const void *LastPresentedPixels;
#endif
} // namespace

/** Whether we render directly to the screen surface, i.e. `PalSurface == GetOutputSurface()` */
bool RenderDirectlyToOutputSurface;

namespace {

bool CanRenderDirectlyToOutputSurface()
{
#ifdef USE_SDL1
#ifdef SDL1_FORCE_DIRECT_RENDER
	return true;
#else
	auto *outputSurface = GetOutputSurface();
	return ((outputSurface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF
	    && outputSurface->w == gnScreenWidth && outputSurface->h == gnScreenHeight
	    && outputSurface->format->BitsPerPixel == 8);
#endif
#else // !USE_SDL1
	if (renderer != nullptr) return false;
	SDL_Surface *outputSurface = GetOutputSurface();
	// Assumes double-buffering is available.
	return outputSurface->w == static_cast<int>(gnScreenWidth)
	    && outputSurface->h == static_cast<int>(gnScreenHeight)
	    && SDLC_SURFACE_BITSPERPIXEL(outputSurface) == 8;
#endif
}

/**
 * @brief Limit FPS to avoid high CPU load, use when v-sync isn't available
 */
void LimitFrameRate()
{
	if (*GetOptions().Graphics.frameRateControl != FrameRateControl::CPUSleep)
		return;
	static uint32_t frameDeadline;
	const uint32_t tc = SDL_GetTicks() * 1000;
	uint32_t v = 0;
	if (frameDeadline > tc) {
		v = tc % refreshDelay;
		SDL_Delay((v / 1000) + 1); // ceil
	}
	frameDeadline = tc + v + refreshDelay;
}

} // namespace

void dx_init()
{
#ifndef USE_SDL1
	SDL_RaiseWindow(ghMainWnd);
	SDL_ShowWindow(ghMainWnd);
#endif

	Palette = SDLWrap::AllocPalette();
	palette_init();
	CreateBackBuffer();
}

Surface GlobalBackBuffer()
{
	return Surface(PalSurface, SDL_Rect { 0, 0, gnScreenWidth, gnScreenHeight });
}

void dx_cleanup()
{
#ifndef USE_SDL1
	if (ghMainWnd != nullptr)
		SDL_HideWindow(ghMainWnd);
#endif

	PalSurface = nullptr;
	PinnedPalSurface = nullptr;
	Palette = nullptr;
	RendererTextureSurface = nullptr;
#ifndef USE_SDL1
	texture = nullptr;
	FreeVirtualGamepadTextures();
	if (*GetOptions().Graphics.upscale)
		SDL_DestroyRenderer(renderer);
#endif
	SDL_DestroyWindow(ghMainWnd);
}

void CreateBackBuffer()
{
	if (CanRenderDirectlyToOutputSurface()) {
		Log("{}", "Will render directly to the SDL output surface");
		PalSurface = GetOutputSurface();
		RenderDirectlyToOutputSurface = true;
	} else {
		PinnedPalSurface = SDLWrap::CreateRGBSurfaceWithFormat(
		    /*flags=*/0,
		    /*width=*/gnScreenWidth,
		    /*height=*/gnScreenHeight,
		    /*depth=*/8,
		    SDL_PIXELFORMAT_INDEX8);
		PalSurface = PinnedPalSurface.get();
	}

#if defined(USE_SDL3)
	if (!SDL_SetSurfacePalette(PalSurface, Palette.get())) ErrSdl();
#elif !defined(USE_SDL1)
	// In SDL2, `PalSurface` points to the global `palette`.
	if (SDL_SetSurfacePalette(PalSurface, Palette.get()) < 0)
		ErrSdl();
#else
	// In SDL1, `PalSurface` owns its palette and we must update it every
	// time the global `palette` is changed. No need to do anything here as
	// the global `palette` doesn't have any colors set yet.
#endif
}

void BltFast(SDL_Rect *srcRect, SDL_Rect *dstRect)
{
	if (RenderDirectlyToOutputSurface) {
#ifndef USE_SDL1
		// A null rect means the caller changed the entire surface.
		if (dstRect == nullptr || DirtyRects.size() == MaxDirtyRects)
			ForceFullUpdate = true;
		else
			DirtyRects.push_back(*dstRect);
#endif
		return;
	}
	Blit(PalSurface, srcRect, dstRect);
}

void Blit(SDL_Surface *src, SDL_Rect *srcRect, SDL_Rect *dstRect)
{
	if (HeadlessMode)
		return;

	SDL_Surface *dst = GetOutputSurface();
#if defined(USE_SDL3)
	if (!SDL_BlitSurface(src, srcRect, dst, dstRect)) ErrSdl();
#elif !defined(USE_SDL1)
	if (SDL_BlitSurface(src, srcRect, dst, dstRect) < 0)
		ErrSdl();
#else
	if (!OutputRequiresScaling()) {
		if (SDL_BlitSurface(src, srcRect, dst, dstRect) < 0)
			ErrSdl();
		return;
	}

	SDL_Rect scaledDstRect;
	if (dstRect != NULL) {
		scaledDstRect = *dstRect;
		ScaleOutputRect(&scaledDstRect);
		dstRect = &scaledDstRect;
	}

	// Same pixel format: We can call BlitScaled directly.
	if (SDLBackport_PixelFormatFormatEq(src->format, dst->format)) {
		if (SDL_BlitScaled(src, srcRect, dst, dstRect) < 0)
			ErrSdl();
		return;
	}

	// If the surface has a color key, we must stretch first and can then call BlitSurface.
	if (SDL_HasColorKey(src)) {
		SDLSurfaceUniquePtr stretched = SDLWrap::CreateRGBSurface(SDL_SWSURFACE, dstRect->w, dstRect->h, src->format->BitsPerPixel,
		    src->format->Rmask, src->format->Gmask, src->format->BitsPerPixel, src->format->Amask);
		SDL_SetColorKey(stretched.get(), SDL_SRCCOLORKEY, src->format->colorkey);
		if (src->format->palette != NULL)
			SDL_SetPalette(stretched.get(), SDL_LOGPAL, src->format->palette->colors, 0, src->format->palette->ncolors);
		SDL_Rect stretched_rect = { 0, 0, dstRect->w, dstRect->h };
		if (SDL_SoftStretch(src, srcRect, stretched.get(), &stretched_rect) < 0
		    || SDL_BlitSurface(stretched.get(), &stretched_rect, dst, dstRect) < 0) {
			ErrSdl();
		}
		return;
	}

	// A surface with a non-output pixel format but without a color key needs scaling.
	// We can convert the format and then call BlitScaled.
	SDLSurfaceUniquePtr converted = SDLWrap::ConvertSurface(src, dst->format, 0);
	if (SDL_BlitScaled(converted.get(), srcRect, dst, dstRect) < 0)
		ErrSdl();
#endif
}

#ifndef USE_SDL1
namespace {

void UpdateOutputSurface()
{

	// Only do partial blitting if the output buffer is the same as last frame.
	const SDL_Surface *outputSurface = GetOutputSurface();
	const void *pixels = outputSurface != nullptr ? outputSurface->pixels : nullptr;
	const void *lastPixels = LastPresentedPixels;
	LastPresentedPixels = pixels;

	if (!ForceFullUpdate && !DirtyRects.empty() && pixels == lastPixels) {
		const int numRects = static_cast<int>(DirtyRects.size());
#ifdef USE_SDL3
		const bool updated = SDL_UpdateWindowSurfaceRects(ghMainWnd, DirtyRects.data(), numRects);
#else
		const bool updated = SDL_UpdateWindowSurfaceRects(ghMainWnd, DirtyRects.data(), numRects) >= 0;
#endif
		DirtyRects.clear();
		if (!updated) ErrSdl();
		return;
	}
	DirtyRects.clear();
	ForceFullUpdate = false;

#ifdef USE_SDL3
	if (!SDL_UpdateWindowSurface(ghMainWnd)) ErrSdl();
#else
	if (SDL_UpdateWindowSurface(ghMainWnd) <= -1) ErrSdl();
#endif
}

} // namespace
#endif

#if defined(HELLSPAWN) && defined(__3DS__) /* HS3DS-WIDE: salida propia, arriba en modo ancho 800x240 */
/* HS3DS-WIDE2: se escribe en el formato que tenga cada pantalla (la SDL lo cambia al cambiar de modo) */
bool HsIsProgress(); /* HS3DS-PANT6 (interfac.cpp) */
#define HS3DS_LOGS2 0 /* HS3DS-SINLOG3: 1 = escribir hs_rendimiento.txt */
void HsLog(const char *fmt, ...); /* HS3DS-REGISTRO */
uint64_t HsPerfMiniSum = 0; /* HS3DS-MEDIR (lo suma scrollrt.cpp) */
bool HsMenuBottom = false; /* HS3DS-MENUS: el BestiarioDex usa la pantalla de abajo */
uint64_t HsPerfPresSum = 0;
uint64_t HsPerfFrameSum = 0;
uint64_t HsPerfLastFrame = 0;
int HsPerfN = 0;
namespace {
uint32_t HsLut[256];

uint32_t HsPack(GSPGPU_FramebufferFormat f, uint8_t r, uint8_t g, uint8_t b)
{
	switch (f) {
	case GSP_RGBA8_OES: return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(b) << 8) | 0xFFU;
	case GSP_BGR8_OES: return static_cast<uint32_t>(b) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(r) << 16);
	case GSP_RGB565_OES: return static_cast<uint32_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	case GSP_RGB5_A1_OES: return static_cast<uint32_t>(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
	default: return static_cast<uint32_t>(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | 0xF); /* RGBA4 */
	}
}

template <int BPP>
inline void HsPut(uint8_t *fb, int idx, uint32_t v)
{
	if constexpr (BPP == 4) {
		reinterpret_cast<uint32_t *>(fb)[idx] = v;
	} else if constexpr (BPP == 2) {
		reinterpret_cast<uint16_t *>(fb)[idx] = static_cast<uint16_t>(v);
	} else {
		uint8_t *q = fb + idx * 3;
		q[0] = static_cast<uint8_t>(v);
		q[1] = static_cast<uint8_t>(v >> 8);
		q[2] = static_cast<uint8_t>(v >> 16);
	}
}

/* Copia una zona de la imagen del juego (sx0,sy0,sw x sh) a una zona de la pantalla (dx0,dy0,dw x dh).
   Los framebuffers estan girados: cada columna de pantalla son 240 pixeles seguidos, de abajo arriba. */
template <int BPP>
void HsBlit(uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0, int sw, int sh, int dx0, int dy0, int dw, int dh)
{
	for (int x = 0; x < dw; x++) {
		const int sx = sx0 + x * sw / dw;
		const int base = (dx0 + x) * 240;
		for (int y = 0; y < dh; y++) {
			const int sy = sy0 + y * sh / dh;
			HsPut<BPP>(fb, base + 239 - (dy0 + y), HsLut[px[sy * pitch + sx]]);
		}
	}
}

int HsBpp(GSPGPU_FramebufferFormat f)
{
	return f == GSP_RGBA8_OES ? 4 : (f == GSP_BGR8_OES ? 3 : 2);
}

void HsBlitAny(GSPGPU_FramebufferFormat f, uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0, int sw, int sh, int dx0, int dy0, int dw, int dh)
{
	switch (HsBpp(f)) {
	case 4: HsBlit<4>(fb, px, pitch, sx0, sy0, sw, sh, dx0, dy0, dw, dh); break;
	case 3: HsBlit<3>(fb, px, pitch, sx0, sy0, sw, sh, dx0, dy0, dw, dh); break;
	default: HsBlit<2>(fb, px, pitch, sx0, sy0, sw, sh, dx0, dy0, dw, dh); break;
	}
}

void HsSetLut(const SDL_Palette *pal, GSPGPU_FramebufferFormat f)
{
	if (pal == nullptr)
		return;
	for (int i = 0; i < pal->ncolors && i < 256; i++) {
		const SDL_Color &c = pal->colors[i];
		HsLut[i] = HsPack(f, c.r, c.g, c.b);
	}
}

/* HS3DS-VISTA3: reduccion vertical a la mitad mezclando 2 filas (colores 0x00RRGGBB) */
uint32_t HsRgb[256];
void HsSetRgb(const SDL_Palette *pal)
{
	if (pal == nullptr)
		return;
	for (int i = 0; i < pal->ncolors && i < 256; i++)
		HsRgb[i] = (static_cast<uint32_t>(pal->colors[i].r) << 16) | (static_cast<uint32_t>(pal->colors[i].g) << 8) | pal->colors[i].b;
}

template <int BPP, GSPGPU_FramebufferFormat F>
void HsHalf(uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0, int w, int dx0)
{
	for (int x = 0; x < w; x++) {
		const uint8_t *src = px + sy0 * pitch + sx0 + x;
		const int base = (dx0 + x) * 240;
		for (int y = 0; y < 240; y++) {
			const uint32_t a = HsRgb[src[(2 * y) * pitch]];
			const uint32_t b = HsRgb[src[(2 * y + 1) * pitch]];
			const uint32_t m = ((a >> 1) & 0x7F7F7FU) + ((b >> 1) & 0x7F7F7FU);
			HsPut<BPP>(fb, base + 239 - y, HsPack(F, static_cast<uint8_t>(m >> 16), static_cast<uint8_t>(m >> 8), static_cast<uint8_t>(m)));
		}
	}
}

/* copia w columnas desde (sx0,sy0) de 480 filas a 240 filas, en la columna dx0 de la pantalla */
void HsHalfAny(GSPGPU_FramebufferFormat f, uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0, int w, int dx0)
{
	switch (f) {
	case GSP_RGBA8_OES: HsHalf<4, GSP_RGBA8_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	case GSP_BGR8_OES: HsHalf<3, GSP_BGR8_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	case GSP_RGB565_OES: HsHalf<2, GSP_RGB565_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	case GSP_RGB5_A1_OES: HsHalf<2, GSP_RGB5_A1_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	default: HsHalf<2, GSP_RGBA4_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	}
}

/* HS3DS-PANT8: reduccion 2x2 (640x480 -> 320x240) mezclando 4 pixeles */
template <int BPP, GSPGPU_FramebufferFormat F>
void HsDown2(uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0)
{
	for (int x = 0; x < 320; x++) {
		const uint8_t *src = px + sy0 * pitch + sx0 + 2 * x;
		const int base = x * 240;
		for (int y = 0; y < 240; y++) {
			const uint8_t *r0 = src + (2 * y) * pitch;
			const uint8_t *r1 = r0 + pitch;
			const uint32_t m = ((HsRgb[r0[0]] >> 2) & 0x3F3F3FU) + ((HsRgb[r0[1]] >> 2) & 0x3F3F3FU)
			    + ((HsRgb[r1[0]] >> 2) & 0x3F3F3FU) + ((HsRgb[r1[1]] >> 2) & 0x3F3F3FU);
			HsPut<BPP>(fb, base + 239 - y, HsPack(F, static_cast<uint8_t>(m >> 16), static_cast<uint8_t>(m >> 8), static_cast<uint8_t>(m)));
		}
	}
}

void HsDown2Any(GSPGPU_FramebufferFormat f, uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0)
{
	switch (f) {
	case GSP_RGBA8_OES: HsDown2<4, GSP_RGBA8_OES>(fb, px, pitch, sx0, sy0); break;
	case GSP_BGR8_OES: HsDown2<3, GSP_BGR8_OES>(fb, px, pitch, sx0, sy0); break;
	case GSP_RGB565_OES: HsDown2<2, GSP_RGB565_OES>(fb, px, pitch, sx0, sy0); break;
	case GSP_RGB5_A1_OES: HsDown2<2, GSP_RGB5_A1_OES>(fb, px, pitch, sx0, sy0); break;
	default: HsDown2<2, GSP_RGBA4_OES>(fb, px, pitch, sx0, sy0); break;
	}
}

/* HS3DS-RAPIDO: mezcla de colores ya empaquetados en el formato de la pantalla */
template <GSPGPU_FramebufferFormat F>
inline uint32_t HsAvg(uint32_t a, uint32_t b)
{
	if constexpr (F == GSP_RGB565_OES)
		return (((a ^ b) & 0xF7DEU) >> 1) + (a & b);
	else
		return ((a >> 1) & 0x7F7F7F7FU) + ((b >> 1) & 0x7F7F7F7FU);
}

/* w columnas, 480 filas -> 240 filas (mezclando 2 filas), en tiras de 16 columnas */
template <int BPP, GSPGPU_FramebufferFormat F>
void HsFastHalf(uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0, int w, int dx0)
{
	for (int xb = 0; xb < w; xb += 16) {
		const int tw = std::min(16, w - xb);
		for (int y = 0; y < 240; y++) {
			const uint8_t *r0 = px + (sy0 + 2 * y) * pitch + sx0 + xb;
			const uint8_t *r1 = r0 + pitch;
			int idx = (dx0 + xb) * 240 + 239 - y;
			for (int i = 0; i < tw; i++, idx += 240)
				HsPut<BPP>(fb, idx, HsAvg<F>(HsLut[r0[i]], HsLut[r1[i]]));
		}
	}
}

/* 640x480 -> 320x240 (mezclando 2x2), en tiras de 16 columnas */
template <int BPP, GSPGPU_FramebufferFormat F>
void HsFastDown2(uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0)
{
	for (int xb = 0; xb < 320; xb += 16) {
		for (int y = 0; y < 240; y++) {
			const uint8_t *r0 = px + (sy0 + 2 * y) * pitch + sx0 + 2 * xb;
			const uint8_t *r1 = r0 + pitch;
			int idx = xb * 240 + 239 - y;
			for (int i = 0; i < 16; i++, idx += 240) {
				const uint32_t a = HsAvg<F>(HsLut[r0[2 * i]], HsLut[r0[2 * i + 1]]);
				const uint32_t b = HsAvg<F>(HsLut[r1[2 * i]], HsLut[r1[2 * i + 1]]);
				HsPut<BPP>(fb, idx, HsAvg<F>(a, b));
			}
		}
	}
}

/* HsLut debe estar preparada para el formato f (HsSetLut) */
void HsFastHalfAny(GSPGPU_FramebufferFormat f, const SDL_Palette *pal, uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0, int w, int dx0)
{
	switch (f) {
	case GSP_RGB565_OES: HsSetLut(pal, f); HsFastHalf<2, GSP_RGB565_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	case GSP_RGBA8_OES: HsSetLut(pal, f); HsFastHalf<4, GSP_RGBA8_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	case GSP_BGR8_OES: HsSetLut(pal, f); HsFastHalf<3, GSP_BGR8_OES>(fb, px, pitch, sx0, sy0, w, dx0); break;
	default: HsSetRgb(pal); HsHalfAny(f, fb, px, pitch, sx0, sy0, w, dx0); break;
	}
}

void HsFastDown2Any(GSPGPU_FramebufferFormat f, const SDL_Palette *pal, uint8_t *fb, const uint8_t *px, int pitch, int sx0, int sy0)
{
	switch (f) {
	case GSP_RGB565_OES: HsSetLut(pal, f); HsFastDown2<2, GSP_RGB565_OES>(fb, px, pitch, sx0, sy0); break;
	case GSP_RGBA8_OES: HsSetLut(pal, f); HsFastDown2<4, GSP_RGBA8_OES>(fb, px, pitch, sx0, sy0); break;
	case GSP_BGR8_OES: HsSetLut(pal, f); HsFastDown2<3, GSP_BGR8_OES>(fb, px, pitch, sx0, sy0); break;
	default: HsSetRgb(pal); HsDown2Any(f, fb, px, pitch, sx0, sy0); break;
	}
}

/* HS3DS-CARGA: 640x480 -> 800x240 (estirada a lo ancho, 2 filas mezcladas) */
template <int BPP, GSPGPU_FramebufferFormat F>
void HsStretch(uint8_t *fb, const uint8_t *px, int pitch)
{
	for (int xb = 0; xb < 800; xb += 16) {
		for (int y = 0; y < 240; y++) {
			const uint8_t *r0 = px + (2 * y) * pitch; /* HS3DS-ZOOM: interfaz en x 0 */
			const uint8_t *r1 = r0 + pitch;
			int idx = xb * 240 + 239 - y;
			for (int i = 0; i < 16; i++, idx += 240) {
				const int sx = (xb + i) * 640 / 800;
				HsPut<BPP>(fb, idx, HsAvg<F>(HsLut[r0[sx]], HsLut[r1[sx]]));
			}
		}
	}
}

void HsStretchAny(GSPGPU_FramebufferFormat f, const SDL_Palette *pal, uint8_t *fb, const uint8_t *px, int pitch)
{
	switch (f) {
	case GSP_RGB565_OES: HsSetLut(pal, f); HsStretch<2, GSP_RGB565_OES>(fb, px, pitch); break;
	case GSP_RGBA8_OES: HsSetLut(pal, f); HsStretch<4, GSP_RGBA8_OES>(fb, px, pitch); break;
	case GSP_BGR8_OES: HsSetLut(pal, f); HsStretch<3, GSP_BGR8_OES>(fb, px, pitch); break;
	default: HsFastHalfAny(f, pal, fb, px, pitch, 0, 0, 640, 80); break;
	}
}

void HsWorldAny(GSPGPU_FramebufferFormat f, uint8_t *fb, const uint8_t *px, int pitch)
{
	HsHalfAny(f, fb, px, pitch, 0, 0, 800, 0);
}

/* HS3DS-SEGURO: interruptores para buscar el cuelgue (1 = activado) */
#ifndef HS3DS_USE_VBCB
#define HS3DS_USE_VBCB 0
#endif
#ifndef HS3DS_FORCE565
#define HS3DS_FORCE565 0
#endif
u64 HsLastSwap = 0; /* HS3DS-FPS */

/* ---------- HS3DS-VBLANK: contar refrescos reales de pantalla ---------- */
volatile u32 HsVBlankCount = 0;
u32 HsSwapVB = 0;
bool HsVBlankHooked = false;
void HsOnVBlank(void *)
{
	HsVBlankCount = HsVBlankCount + 1;
}
/* hay que esperar si desde el ultimo cambio de buffer no ha pasado ningun refresco */
/* HS3DS-VBRELOJ: refrescos calculados con el reloj del sistema */
u64 HsVbRef = 0;    /* momento exacto de un refresco real */
u64 HsSwapTick = 0; /* momento del ultimo cambio de buffer */
constexpr u64 HsVbPeriod = SYSCLOCK_ARM11 * 1000ULL / 59831ULL; /* 1 refresco = 1/59,831 s */
constexpr u64 HsVbMargin = SYSCLOCK_ARM11 / 1000;                /* 1 ms de margen */

void HsWaitVBlank()
{
	gspWaitForVBlank();
	HsVbRef = svcGetSystemTick();
}

/* hay que esperar si desde el ultimo cambio de buffer aun no ha pasado ningun refresco */
bool HsNeedWait()
{
	const u64 now = svcGetSystemTick();
	if (HsVbRef == 0 || now - HsVbRef > SYSCLOCK_ARM11 * 5ULL || now < HsVbRef + HsVbMargin || HsSwapTick < HsVbRef)
		return true; /* sin referencia, referencia vieja (resincronizar) o dudosa: esperar */
	const u64 iSwap = (HsSwapTick - HsVbRef) / HsVbPeriod;
	const u64 iNow = (now - HsVbMargin - HsVbRef) / HsVbPeriod;
	return iNow <= iSwap;
}

/* ---------- HS3DS-HILO: la conversion de la partida en el nucleo 2 ---------- */
uint8_t *HsSnap = nullptr; /* copia de la imagen del juego (640x960) */
SDL_Color HsSnapColors[256];
int HsSnapNColors = 0;
int HsSnapXp = -1;
int HsSnapMode = 0;     /* 0 HUD, 1 panel izquierdo, 2 panel derecho, 3 los dos */
bool HsSnapUiTop = false;
Thread HsWorker = nullptr;
bool HsWorkerTried = false;
LightEvent HsGo;
LightEvent HsDone;
volatile u64 HsWorkerTicks = 0;
uint64_t HsPerfWorkSum = 0;

volatile bool HsLcdRefresh = false; /* HS3DS-HOME: lo pone el aviso del sistema al volver */
aptHookCookie HsAptCookie;
void HsAptHook(APT_HookType hook, void *)
{
	if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
		HsLcdRefresh = true;
}
void HsLcdCheck(SDL_Surface *s)
{ /* HS3DS-LCD: luz de la pantalla de abajo encendida (al empezar y en cada cambio de modo) */
	static int hsLastW = -1, hsLastH = -1;
	static bool hsLastRun = false;
	static bool hsHooked = false;
	if (!hsHooked) {
		hsHooked = true;
		aptHook(&HsAptCookie, HsAptHook, nullptr); /* HS3DS-HOME */
	}
	if (HsLcdRefresh) { /* HS3DS-HOME: volver del menu HOME / despertar: encender otra vez */
		HsLcdRefresh = false;
		hsLastW = -1;
	}
	if (s->w != hsLastW || s->h != hsLastH || gbRunGame != hsLastRun) {
		hsLastW = s->w;
		hsLastH = s->h;
		hsLastRun = gbRunGame;
		if (R_SUCCEEDED(gspLcdInit())) {
			GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
			gspLcdExit();
		}
	}
	if (!HsVBlankHooked) {
		HsVBlankHooked = true;
#if HS3DS_USE_VBCB
		gspSetEventCallback(GSPGPU_EVENT_VBlank0, HsOnVBlank, nullptr, false);
#endif
	}
}

void HsPrepareScreens()
{
#if HS3DS_FORCE565
	if (gfxGetScreenFormat(GFX_TOP) != GSP_RGB565_OES) /* HS3DS-RAPIDO */
		gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
#endif
	if (!gfxIsWide())
		gfxSetWide(true);
	if (HsNeedWait()) /* HS3DS-VBLANK + HS3DS-VBRELOJ */
		HsWaitVBlank();
}

void HsFinishFrame()
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	HsLastSwap = osGetTime();
	HsSwapVB = HsVBlankCount;
	HsSwapTick = svcGetSystemTick(); /* HS3DS-VBRELOJ */
}

/* HS3DS-XPVERDE: relleno de la barra (0-307) o -1 si no hay que pintarla */
int HsXpFill()
{
	if (!*GetOptions().Gameplay.experienceBar || ChatFlag || MyPlayer == nullptr)
		return -1;
	const Player &pl = *MyPlayer;
	if (pl.isMaxCharacterLevel())
		return -1;
	const unsigned lvl = pl.getCharacterLevel();
	const uint64_t prev = GetNextExperienceThresholdForLevel(lvl - 1);
	if (pl._pExperience < prev)
		return -1;
	const uint64_t d = GetNextExperienceThresholdForLevel(lvl) - prev;
	if (d == 0)
		return -1;
	return static_cast<int>(307 * (pl._pExperience - prev) / d);
}

/* HS3DS-XP2: barra verde con tono azulado, 4 filas (235-238), desde la columna 84 */
void HsDrawXpBar(GSPGPU_FramebufferFormat f, uint8_t *bot, int fill)
{
	if (fill < 0)
		return;
	const uint32_t cols[4] = { HsPack(f, 150, 255, 200), HsPack(f, 90, 225, 170), HsPack(f, 50, 175, 155), HsPack(f, 30, 120, 115) };
	const int bpp = HsBpp(f);
	for (int x = 0; x < fill / 2 && 84 + x < 320; x++) {
		const int base = (84 + x) * 240;
		for (int r = 0; r < 4; r++) {
			const int idx = base + 239 - (235 + r);
			if (bpp == 4)
				HsPut<4>(bot, idx, cols[r]);
			else if (bpp == 3)
				HsPut<3>(bot, idx, cols[r]);
			else
				HsPut<2>(bot, idx, cols[r]);
		}
	}
}

/* HS3DS-ZOOM: mundo 640x384 -> 800x240 (columnas x0,8; 2 filas mezcladas cada 1,6 filas) */
template <int BPP, GSPGPU_FramebufferFormat F>
void HsWorldScale(uint8_t *fb, const uint8_t *px, int pitch)
{
	for (int xb = 0; xb < 800; xb += 16) {
		for (int y = 0; y < 240; y++) {
			const uint8_t *r0 = px + (y * 384 / 240) * pitch;
			const uint8_t *r1 = r0 + pitch;
			int idx = xb * 240 + 239 - y;
			for (int i = 0; i < 16; i++, idx += 240) {
				const int sx = (xb + i) * 4 / 5;
				HsPut<BPP>(fb, idx, HsAvg<F>(HsLut[r0[sx]], HsLut[r1[sx]]));
			}
		}
	}
}

void HsWorldScaleAny(GSPGPU_FramebufferFormat f, SDL_Palette *pal, uint8_t *fb, const uint8_t *px, int pitch)
{
	switch (f) {
	case GSP_RGB565_OES: HsSetLut(pal, f); HsWorldScale<2, GSP_RGB565_OES>(fb, px, pitch); break;
	case GSP_RGBA8_OES: HsSetLut(pal, f); HsWorldScale<4, GSP_RGBA8_OES>(fb, px, pitch); break;
	case GSP_BGR8_OES: HsSetLut(pal, f); HsWorldScale<3, GSP_BGR8_OES>(fb, px, pitch); break;
	default: HsSetLut(pal, f); HsBlitAny(f, fb, px, pitch, 0, 0, 640, 384, 0, 0, 800, 240); break;
	}
}

/* HS3DS-TRIPLE: se convierte en estos buffers y luego se copia a la pantalla */
alignas(32) uint8_t HsTopBuf[800 * 240 * 4];
alignas(32) uint8_t HsBotBuf[320 * 240 * 4];

void HsConvertGame(const uint8_t *px, int pitch, SDL_Palette *pal, int xp, int mode, bool uiTop)
{
#if HS3DS_FORCE565
	if (gfxGetScreenFormat(GFX_TOP) != GSP_RGB565_OES) /* HS3DS-RAPIDO */
		gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
#endif
	if (!gfxIsWide())
		gfxSetWide(true);
	const GSPGPU_FramebufferFormat ft = gfxGetScreenFormat(GFX_TOP);
	const GSPGPU_FramebufferFormat fbo = gfxGetScreenFormat(GFX_BOTTOM);
	/* arriba */
	if (uiTop) { /* HS3DS-UITOP: zona de interfaz entera (640x480) a 640x240 */
		std::memset(HsTopBuf, 0, 800 * 240 * HsBpp(ft));
		HsFastHalfAny(ft, pal, HsTopBuf, px, pitch, 0, 0, 640, 80);
	} else {
		HsWorldScaleAny(ft, pal, HsTopBuf, px, pitch);
	}
	/* abajo */
	if (mode == 0) {
		HsFastDown2Any(fbo, pal, HsBotBuf, px, pitch, 0, 480);
		HsDrawXpBar(fbo, HsBotBuf, xp);
	} else { /* HS3DS-PANELES */
		std::memset(HsBotBuf, 0, 320 * 240 * HsBpp(fbo));
		HsSetLut(pal, fbo);
		if (mode == 3)
			HsBlitAny(fbo, HsBotBuf, px, pitch, 0, 480, 640, 352, 0, 32, 320, 176);
		else
			HsBlitAny(fbo, HsBotBuf, px, pitch, mode == 1 ? 0 : 320, 480, 320, 352, 51, 0, 218, 240);
	}
	if (HsNeedWait()) /* HS3DS-VBLANK + HS3DS-VBRELOJ */
		HsWaitVBlank();
	std::memcpy(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr), HsTopBuf, 800 * 240 * HsBpp(ft));
	std::memcpy(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr), HsBotBuf, 320 * 240 * HsBpp(fbo));
	HsFinishFrame();
}

void HsWorkerMain(void *)
{
	for (;;) {
		LightEvent_Wait(&HsGo);
		const u64 t0 = svcGetSystemTick();
		SDL_Palette pal;
		pal.ncolors = HsSnapNColors;
		pal.colors = HsSnapColors;
		HsConvertGame(HsSnap, 640, &pal, HsSnapXp, HsSnapMode, HsSnapUiTop);
		HsWorkerTicks = svcGetSystemTick() - t0;
		LightEvent_Signal(&HsDone);
	}
}

void HsWaitWorkerIdle()
{
	if (HsWorker != nullptr) {
		LightEvent_Wait(&HsDone);
		LightEvent_Signal(&HsDone);
	}
}

int HsBottomMode()
{
	const bool l = IsLeftPanelOpen();
	const bool r = IsRightPanelOpen();
	return (l && r) ? 3 : (l ? 1 : (r ? 2 : 0));
}

bool HsUiOnTop()
{
	return false; /* HS3DS-UITOP2: todo cabe en las 384 filas del mundo */
}

#ifndef HS3DS_VOZ_AL_ABRIR
#define HS3DS_VOZ_AL_ABRIR 0 /* 0 = frase al cerrar la tapa, 1 = frase al abrirla */
#endif
/* HS3DS-TAPA: frases cortas del heroe al cerrar la tapa (no usa el azar del juego: no rompe el multijugador) */
void HsLidSay()
{
	static const HeroSpeech frases[] = {
		HeroSpeech::Yes, HeroSpeech::No, HeroSpeech::Yeah, HeroSpeech::Hmm, HeroSpeech::MmHmm, HeroSpeech::UhHuh,
		HeroSpeech::Ah, HeroSpeech::Oh, HeroSpeech::Wow, HeroSpeech::Phew, HeroSpeech::HeavyBreathing,
		HeroSpeech::NoWay, HeroSpeech::NotAChance, HeroSpeech::Die, HeroSpeech::WhatWasThat, HeroSpeech::ThankTheLight,
		HeroSpeech::TimeToDie, HeroSpeech::VengeanceIsMine, HeroSpeech::OhTooEasy, HeroSpeech::BackToTheGrave,
		HeroSpeech::ImNotImpressed, HeroSpeech::RestInPeaceMyFriend, HeroSpeech::ImNotThirsty,
	};
	constexpr int n = static_cast<int>(sizeof(frases) / sizeof(frases[0]));
	static int ultima = -1;
	int i = static_cast<int>((svcGetSystemTick() >> 4) % static_cast<u64>(n));
	if (i == ultima)
		i = (i + 1) % n; /* no repetir la misma dos veces seguidas */
	ultima = i;
	MyPlayer->Say(frases[i]);
}
bool HsLidCanSay()
{
	return gbRunGame && MyPlayer != nullptr && !HsIsProgress() && !MyPlayer->_pLvlChanging;
}
void HsLidCheck()
{
	static int estado = -1; /* -1 sin iniciar, 0 no disponible, 1 listo */
	static bool cerrada = false;
	static bool durmiendo = false;
	static u64 tCierre = 0;
	static int cuenta = 0;
	static bool alAbrir = false; /* HS3DS-TAPA4: frase pendiente para cuando se abra */
	if (estado < 0) {
		estado = R_SUCCEEDED(ptmuInit()) ? 1 : 0;
		if (estado == 1)
			aptSetSleepAllowed(false); /* el juego decide cuando dormir */
	}
	if (estado != 1)
		return;
	if (!cerrada && (++cuenta % 6) != 0)
		return; /* mirar la tapa cada 6 fotogramas (con la tapa cerrada, siempre) */
	u8 abierta = 1;
	if (R_FAILED(PTMU_GetShellState(&abierta)))
		return;
	if (abierta == 0 && !cerrada) { /* se acaba de cerrar */
		cerrada = true;
		durmiendo = false;
		tCierre = svcGetSystemTick();
		alAbrir = false;
		if (!HS3DS_VOZ_AL_ABRIR && HsLidCanSay()) { /* HS3DS-TAPA2 + HS3DS-TAPA6: siempre al cerrar (sin mirar auriculares) */
			HsLidSay();
			alAbrir = true; /* HS3DS-TAPA5: y otra frase al abrir */
		} else {
			alAbrir = HsLidCanSay(); /* sin auriculares no se oye: la frase al abrir */
			tCierre = 0; /* dormir ya */
		}
	}
	if (cerrada && abierta == 0 && !durmiendo && (tCierre == 0 || svcGetSystemTick() - tCierre >= SYSCLOCK_ARM11 * 22ULL / 10ULL)) {
		durmiendo = true;
		aptSetSleepAllowed(true); /* con la tapa cerrada, la consola entra en reposo */
	}
	if (abierta != 0 && cerrada) { /* se ha vuelto a abrir */
		cerrada = false;
		durmiendo = false;
		aptSetSleepAllowed(false);
		if ((HS3DS_VOZ_AL_ABRIR || alAbrir) && HsLidCanSay()) /* HS3DS-TAPA3 */
			HsLidSay();
		alAbrir = false;
	}
}

void HsCtrPresent(SDL_Surface *s)
{
	HsLcdCheck(s);
	HsLidCheck(); /* HS3DS-TAPA */
	{ /* HS3DS-REGISTRO: cada 60 fotogramas */
		static int hsLogN = 0;
		if (++hsLogN % 60 == 1) {
			HsLog("pantalla %dx%d partida=%d carga=%d", s->w, s->h, gbRunGame ? 1 : 0, HsIsProgress() ? 1 : 0);
		}
	}
	const u64 hsT0 = svcGetSystemTick(); /* HS3DS-MEDIR */
	if (SDL_MUSTLOCK(s))
		SDL_LockSurface(s);
	const uint8_t *px = static_cast<const uint8_t *>(s->pixels);
	const int pitch = s->pitch;
	const int sw = s->w;
	const int sh = s->h;
	const bool juego = gbRunGame && sw == 640 && sh == 960 && !HsIsProgress();
	if (juego && !HsWorkerTried) {
		HsWorkerTried = true;
		HsSnap = static_cast<uint8_t *>(std::malloc(640 * 960));
		LightEvent_Init(&HsGo, RESET_ONESHOT);
		LightEvent_Init(&HsDone, RESET_ONESHOT);
		LightEvent_Signal(&HsDone);
		if (HsSnap != nullptr)
			HsWorker = threadCreate(HsWorkerMain, nullptr, 64 * 1024, 0x18, 2, true); /* nucleo 2 (solo New 3DS) */
	}
	if (juego && HsWorker != nullptr) {
		LightEvent_Wait(&HsDone); /* el fotograma anterior ya esta en pantalla */
		HsPerfWorkSum += HsWorkerTicks;
		for (int y = 0; y < 960; y++)
			std::memcpy(HsSnap + y * 640, px + y * pitch, 640);
		const SDL_Palette *pal = s->format->palette;
		HsSnapNColors = pal != nullptr ? std::min(pal->ncolors, 256) : 0;
		if (HsSnapNColors > 0)
			std::memcpy(HsSnapColors, pal->colors, sizeof(SDL_Color) * HsSnapNColors);
		HsSnapXp = HsXpFill();
		HsSnapMode = HsBottomMode();
		HsSnapUiTop = HsUiOnTop();
		LightEvent_Signal(&HsGo);
	} else if (juego) {
		HsConvertGame(px, pitch, s->format->palette, HsXpFill(), HsBottomMode(), HsUiOnTop()); /* sin nucleo 2 */
	} else {
		HsWaitWorkerIdle();
		HsPrepareScreens();
		const GSPGPU_FramebufferFormat ft = gfxGetScreenFormat(GFX_TOP);
		const GSPGPU_FramebufferFormat fbo = gfxGetScreenFormat(GFX_BOTTOM);
		uint8_t *top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
		uint8_t *bot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
		std::memset(bot, 0, 320 * 240 * HsBpp(fbo));
		if (sw == 640 && sh == 960 && HsIsProgress()) {
			HsStretchAny(ft, s->format->palette, top, px, pitch); /* HS3DS-CARGA2: carga a todo el ancho */
		} else if (sw == 640 && sh == 960) {
			std::memset(top, 0, 800 * 240 * HsBpp(ft)); /* menus: 640x480 (x 0, y 0) a 640x240 */
			HsFastHalfAny(ft, s->format->palette, top, px, pitch, 0, 0, 640, 80);
			if (HsMenuBottom) /* HS3DS-MENUS2: lista del BestiarioDex (filas 480-959) */
				HsFastDown2Any(fbo, s->format->palette, bot, px, pitch, 0, 480);
		} else {
			/* videos: ajustado a 800x240 (en modo ancho cada pixel es la mitad de ancho) */
			int dw = sw * 240 * 2 / sh;
			int dh = 240;
			if (dw > 800) {
				dw = 800;
				dh = sh * 800 / (sw * 2);
			}
			if (dw < 800 || dh < 240)
				std::memset(top, 0, 800 * 240 * HsBpp(ft));
			HsSetLut(s->format->palette, ft);
			HsBlitAny(ft, top, px, pitch, 0, 0, sw, sh, (800 - dw) / 2, (240 - dh) / 2, dw, dh);
		}
		HsFinishFrame();
	}
	if (SDL_MUSTLOCK(s))
		SDL_UnlockSurface(s);
	HsPerfPresSum += svcGetSystemTick() - hsT0; /* HS3DS-MEDIR */
	if (HsPerfLastFrame != 0)
		HsPerfFrameSum += hsT0 - HsPerfLastFrame;
	HsPerfLastFrame = hsT0;
	if (++HsPerfN >= 300) {
		const double k = 1000.0 / SYSCLOCK_ARM11 / HsPerfN;
		const double frameMs = HsPerfFrameSum * k;
		FILE *f = HS3DS_LOGS2 ? std::fopen("sdmc:/3ds/devilutionx/hs_rendimiento.txt", "a") : nullptr; /* HS3DS-SINLOG2 */
		if (f != nullptr) {
			std::fprintf(f, "%s: fotograma %.1f ms (%.1f fps) | principal %.1f ms | nucleo2 %.1f ms (%s) | minimapa %.1f ms | refrescos %s\n",
			    gbRunGame ? "partida" : "menu", frameMs, frameMs > 0 ? 1000.0 / frameMs : 0.0, HsPerfPresSum * k, HsPerfWorkSum * k,
			    HsWorker != nullptr ? "hilo" : "sin hilo", HsPerfMiniSum * k, HsVbRef != 0 ? "reloj" : "sin reloj");
			std::fclose(f);
		}
		HsPerfN = 0;
		HsPerfFrameSum = 0;
		HsPerfPresSum = 0;
		HsPerfMiniSum = 0;
		HsPerfWorkSum = 0;
	}
}
} // namespace

void HsCtrWaitIdle() /* HS3DS-HILO: lo llama SetVideoMode (display.cpp) */
{
	HsWaitWorkerIdle();
}

#ifndef HS3DS_LOGS
#define HS3DS_LOGS 0 /* HS3DS-SINLOG: 1 = escribir hs_registro.txt y hs_rendimiento.txt */
#endif
void HsLog(const char *fmt, ...) /* HS3DS-REGISTRO */
{
	if (!HS3DS_LOGS)
		return;
	FILE *f = std::fopen("sdmc:/3ds/devilutionx/hs_registro.txt", "a");
	if (f == nullptr)
		return;
	std::fprintf(f, "%llu ", static_cast<unsigned long long>(osGetTime()));
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fclose(f);
}
#endif

void RenderPresent()
{
	if (HeadlessMode)
		return;

	SDL_Surface *surface = GetOutputSurface();

	if (!gbActive) {
#ifdef __EMSCRIPTEN__
		// Just yield to browser when inactive instead of blocking
		emscripten_sleep(1);
#else
		LimitFrameRate();
#endif
		return;
	}

#ifndef USE_SDL1
	if (renderer != nullptr) {
#ifdef USE_SDL3
		if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255)) ErrSdl();
		if (!SDL_RenderClear(renderer)) ErrSdl();
		if (!SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch)) ErrSdl();
		if (!SDL_RenderTexture(renderer, texture.get(), nullptr, nullptr)) ErrSdl();
#else
		if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) <= -1) ErrSdl();
		if (SDL_RenderClear(renderer) <= -1) ErrSdl();
		if (SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch) <= -1) ErrSdl();
		if (SDL_RenderCopy(renderer, texture.get(), nullptr, nullptr) <= -1) ErrSdl();
#endif

		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(renderer);
		}
		SDL_RenderPresent(renderer);

#ifdef __EMSCRIPTEN__
		// TODO: Refactor to use emscripten_set_main_loop or requestAnimationFrame instead.
		// For now, yield to browser to allow rendering via ASYNCIFY sleep.
		emscripten_sleep(1);
#endif

		if (*GetOptions().Graphics.frameRateControl != FrameRateControl::VerticalSync) {
			LimitFrameRate();
		}
	} else {
		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(surface);
		}

		UpdateOutputSurface();

		if (RenderDirectlyToOutputSurface)
			PalSurface = GetOutputSurface();
		LimitFrameRate();
	}
#else
#if defined(HELLSPAWN) && defined(__3DS__) /* HS3DS-WIDE */
	HsCtrPresent(surface);
#else
	if (SDL_Flip(surface) <= -1) {
		ErrSdl();
	}
#endif
	if (RenderDirectlyToOutputSurface)
		PalSurface = GetOutputSurface();
	LimitFrameRate();
#endif
}

} // namespace devilution
