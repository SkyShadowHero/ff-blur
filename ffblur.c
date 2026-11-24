// ffblur.c — make any Wayland client (e.g. Firefox) request background blur
// via the ext-background-effect-v1 protocol, without recompiling the client.
//
// Strategy: interpose on libwayland's request-marshalling entry point.
//   * discover wl_compositor + ext_background_effect_manager_v1 from a private
//     registry on the app's own wl_display (one roundtrip, at connect time)
//   * learn which wl_surface is an xdg_toplevel (xdg_wm_base.get_xdg_surface
//     followed by xdg_surface.get_toplevel)
//   * right before wl_surface.commit, create the effect object for that
//     surface and set the blur region (window geometry, or a huge rect that
//     the compositor clips to the surface size)
//
// The blur region is double-buffered state, so it must be set before commit.
//
// Env:
//   FFBLUR_DEBUG=1      log what we do to stderr
//   FFBLUR_RADIUS=<px>  subtract rounded corners of this radius from the
//                       blur region (matches a CSS border-radius)

#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "ext-background-effect-v1-client-protocol.h"

/* xdg-shell opcodes (xdg-shell.xml, stable v1..v7; these never change) */
#define OP_XDG_WM_BASE_GET_XDG_SURFACE 2
#define OP_XDG_SURFACE_DESTROY 0
#define OP_XDG_SURFACE_GET_TOPLEVEL 1
#define OP_XDG_SURFACE_SET_WINDOW_GEOMETRY 3
/* wl_surface opcodes */
#define OP_WL_SURFACE_DESTROY 0
#define OP_WL_SURFACE_COMMIT 6

#define FALLBACK_SIZE 16384

typedef struct wl_proxy *(*marshal_fn)(struct wl_proxy *, uint32_t,
				      const struct wl_interface *, uint32_t,
				      uint32_t, union wl_argument *);
typedef struct wl_display *(*connect_fn)(const char *);
typedef struct wl_display *(*connect_fd_fn)(int);

static marshal_fn real_marshal;
static connect_fn real_connect;
static connect_fd_fn real_connect_fd;
static int resolved;

static struct wl_display *g_display;
static struct wl_compositor *g_compositor;
static struct ext_background_effect_manager_v1 *g_mgr;
static int g_init;	  /* registry setup started		*/
static int g_ready;	  /* init finished			*/
static int g_capable;	  /* compositor says it can blur		*/
static int g_external;	  /* the client speaks the protocol itself */
static struct wl_proxy *g_own_registry;
static int g_debug;
static int64_t g_radius;  /* rounded-corner cutout, 0 = off	*/

static struct toplevel {
	struct wl_surface *surface;
	struct ext_background_effect_surface_v1 *effect;
	int32_t geom_w, geom_h;
	int32_t blur_w, blur_h;
	int have_geom;
	int dirty;
	int external;	/* effect object owned by the client itself */
} *g_tops;
static size_t g_ntops, g_ctops;

static struct xs_surf {
	struct wl_proxy *xs;
	struct wl_surface *surface;
} *g_xs;
static size_t g_nxs, g_cxs;

static void
dbg(const char *fmt, ...)
{
	va_list ap;

	if (!g_debug)
		return;
	va_start(ap, fmt);
	fputs("ffblur: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ------------------------------------------------------------------ lists */

static struct toplevel *
top_get(struct wl_surface *s)
{
	size_t i;

	for (i = 0; i < g_ntops; i++)
		if (g_tops[i].surface == s)
			return &g_tops[i];
	return NULL;
}

static struct toplevel *
top_add(struct wl_surface *s)
{
	if (g_ntops == g_ctops) {
		size_t n = g_ctops ? g_ctops * 2 : 8;
		void *p = realloc(g_tops, n * sizeof *g_tops);
		if (!p)
			return NULL;
		g_tops = p;
		g_ctops = n;
	}
	memset(&g_tops[g_ntops], 0, sizeof *g_tops);
	g_tops[g_ntops].surface = s;
	return &g_tops[g_ntops++];
}

static void
top_remove(struct wl_surface *s)
{
	size_t i;

	for (i = 0; i < g_ntops; i++) {
		if (g_tops[i].surface != s)
			continue;
		if (g_tops[i].effect)
			ext_background_effect_surface_v1_destroy(g_tops[i].effect);
		g_tops[i] = g_tops[--g_ntops];
		return;
	}
}

static struct wl_surface *
xs_get(struct wl_proxy *xs)
{
	size_t i;

	for (i = 0; i < g_nxs; i++)
		if (g_xs[i].xs == xs)
			return g_xs[i].surface;
	return NULL;
}

static void
xs_add(struct wl_proxy *xs, struct wl_surface *s)
{
	if (g_nxs == g_cxs) {
		size_t n = g_cxs ? g_cxs * 2 : 8;
		void *p = realloc(g_xs, n * sizeof *g_xs);
		if (!p)
			return;
		g_xs = p;
		g_cxs = n;
	}
	g_xs[g_nxs].xs = xs;
	g_xs[g_nxs].surface = s;
	g_nxs++;
}

static void
xs_remove(struct wl_proxy *xs)
{
	size_t i;

	for (i = 0; i < g_nxs; i++) {
		if (g_xs[i].xs == xs) {
			g_xs[i] = g_xs[--g_nxs];
			return;
		}
	}
}

/* --------------------------------------------------------------- protocol */

static void
mgr_capabilities(void *data, struct ext_background_effect_manager_v1 *m,
		 uint32_t flags)
{
	(void)data;
	(void)m;
	g_capable = (flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR) != 0;
	dbg("capabilities=0x%x blur=%d", flags, g_capable);
}

static const struct ext_background_effect_manager_v1_listener mgr_listener = {
	.capabilities = mgr_capabilities,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *iface, uint32_t version)
{
	(void)data;

	if (!strcmp(iface, "wl_compositor") && !g_compositor) {
		g_compositor = wl_registry_bind(registry, name,
						&wl_compositor_interface,
						version < 4 ? version : 4);
		dbg("bound wl_compositor v%u", version < 4 ? version : 4);
	} else if (!strcmp(iface, "ext_background_effect_manager_v1") &&
		   !g_mgr) {
		g_mgr = wl_registry_bind(registry, name,
					 &ext_background_effect_manager_v1_interface,
					 1);
		if (g_mgr)
			ext_background_effect_manager_v1_add_listener(g_mgr,
								     &mgr_listener,
								     NULL);
		dbg("bound ext_background_effect_manager_v1");
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
ensure_init(struct wl_display *dpy)
{
	if (g_init || !dpy)
		return;
	g_init = 1;
	g_display = dpy;

	struct wl_registry *reg = wl_display_get_registry(dpy);
	if (!reg) {
		dbg("get_registry failed");
		return;
	}
	g_own_registry = (struct wl_proxy *)reg;
	wl_registry_add_listener(reg, &registry_listener, NULL);
	if (wl_display_roundtrip(dpy) < 0) {
		dbg("roundtrip failed");
		return;
	}
	/* the registry itself is kept alive on purpose; no destroy */
	g_ready = 1;
	dbg("init done: compositor=%p mgr=%p", (void *)g_compositor,
	    (void *)g_mgr);
}

/* Build the blur region: (0,0,w,h) minus optional rounded corners. */
static struct wl_region *
build_region(int32_t w, int32_t h)
{
	struct wl_region *r = wl_compositor_create_region(g_compositor);
	int64_t rad = g_radius;

	if (!r)
		return NULL;
	if (rad <= 0 || rad * 2 > w || rad * 2 > h) {
		wl_region_add(r, 0, 0, w, h);
		return r;
	}
	wl_region_add(r, 0, 0, w, h);
	/* carve the four corners out of the rectangle; wl_region only knows
	 * rectangles, so approximate the arc with 1px horizontal strips */
	for (int64_t i = 0; i < rad; i++) {
		/* horizontal inset of the circle at row i (and mirrored) */
		double dy = (double)(rad - i) - 0.5;
		double dx = (double)rad -
			    sqrt((double)rad * rad - dy * dy);
		int32_t cut = (int32_t)(dx + 0.5);

		if (cut <= 0)
			continue;
		if (cut > rad)
			cut = rad;
		wl_region_subtract(r, 0, (int32_t)i, cut, 1);
		wl_region_subtract(r, w - cut, (int32_t)i, cut, 1);
		wl_region_subtract(r, 0, h - 1 - (int32_t)i, cut, 1);
		wl_region_subtract(r, w - cut, h - 1 - (int32_t)i, cut, 1);
	}
	return r;
}

static void
apply_region(struct toplevel *t)
{
	int32_t w = t->have_geom ? t->geom_w : FALLBACK_SIZE;
	int32_t h = t->have_geom ? t->geom_h : FALLBACK_SIZE;
	struct wl_region *r;
	union wl_argument a[1];

	if (w <= 0 || h <= 0)
		return;
	r = build_region(w, h);
	if (!r)
		return;

	memset(a, 0, sizeof a);
	a[0].o = (struct wl_object *)r;
	real_marshal((struct wl_proxy *)t->effect,
		     EXT_BACKGROUND_EFFECT_SURFACE_V1_SET_BLUR_REGION, NULL, 1, 0,
		     a);
	/* copy semantics: the server has the region now */
	wl_region_destroy(r);

	t->blur_w = w;
	t->blur_h = h;
	t->dirty = 0;
	dbg("blur region %dx%d on %p (r=%lld)", w, h, (void *)t->surface,
	    (long long)g_radius);
}

static void
on_commit(struct wl_surface *s)
{
	struct toplevel *t;

	if (!g_ready || g_external || !g_capable || !g_compositor || !g_mgr)
		return;
	t = top_get(s);
	if (!t || t->external)
		return;

	if (!t->effect) {
		union wl_argument a[2];

		memset(a, 0, sizeof a);
		a[1].o = (struct wl_object *)s; /* new_id stays NULL */
		struct wl_proxy *p = real_marshal(
			(struct wl_proxy *)g_mgr,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_GET_BACKGROUND_EFFECT,
			&ext_background_effect_surface_v1_interface, 1, 0, a);
		if (!p) {
			dbg("get_background_effect failed on %p", (void *)s);
			return;
		}
		t->effect = (struct ext_background_effect_surface_v1 *)p;
		t->dirty = 1;
	}

	if (t->dirty)
		apply_region(t);
}

/* ------------------------------------------------------------- interposer */

static void
resolve(void)
{
	if (resolved)
		return;
	resolved = 1;
	real_marshal = (marshal_fn)dlsym(RTLD_NEXT,
					 "wl_proxy_marshal_array_flags");
	real_connect = (connect_fn)dlsym(RTLD_NEXT, "wl_display_connect");
	real_connect_fd = (connect_fd_fn)dlsym(RTLD_NEXT,
					       "wl_display_connect_to_fd");
	g_debug = getenv("FFBLUR_DEBUG") != NULL;
	const char *r = getenv("FFBLUR_RADIUS");
	if (r && *r)
		g_radius = strtoll(r, NULL, 10);
}

struct wl_proxy *
wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
			     const struct wl_interface *interface,
			     uint32_t version, uint32_t flags,
			     union wl_argument *args)
{
	resolve();
	if (!real_marshal)
		return NULL;

	if (!g_ready) {
		struct wl_display *dpy = NULL;

		if (proxy)
			dpy = wl_proxy_get_display(proxy);
		if (dpy && (!g_display || g_display == dpy))
			ensure_init(dpy);
	}

	const struct wl_interface *iface =
		proxy ? wl_proxy_get_interface(proxy) : NULL;
	const char *name = iface ? iface->name : NULL;

	if (name && args) {
		/* the client itself binds the manager -> it knows about blur,
		 * stay out of its way entirely */
		if (!strcmp(name, "wl_registry") && opcode == 0 &&
		    args[1].s &&
		    !strcmp(args[1].s, "ext_background_effect_manager_v1") &&
		    proxy != g_own_registry) {
			g_external = 1;
			dbg("client binds the manager itself, standing down");
		}
		/* someone else already created an effect object for this
		 * surface: never create a second one (protocol error) */
		if (!strcmp(name, "ext_background_effect_manager_v1") &&
		    opcode ==
			    EXT_BACKGROUND_EFFECT_MANAGER_V1_GET_BACKGROUND_EFFECT) {
			struct wl_surface *s = (struct wl_surface *)args[1].o;
			struct toplevel *t = s ? top_get(s) : NULL;
			if (t) {
				t->external = 1;
				t->dirty = 0;
				dbg("surface %p already has an effect object",
				    (void *)s);
			}
		}
	}

	if (name && g_ready) {
		if (!strcmp(name, "wl_surface") &&
		    opcode == OP_WL_SURFACE_COMMIT) {
			on_commit((struct wl_surface *)proxy);
		} else if (!strcmp(name, "xdg_surface") &&
			   opcode == OP_XDG_SURFACE_SET_WINDOW_GEOMETRY &&
			   args != NULL) {
			struct wl_surface *s = xs_get(proxy);
			struct toplevel *t = s ? top_get(s) : NULL;
			if (t) {
				t->geom_w = args[2].i;
				t->geom_h = args[3].i;
				t->have_geom = t->geom_w > 0 && t->geom_h > 0;
				if (t->geom_w != t->blur_w ||
				    t->geom_h != t->blur_h)
					t->dirty = 1;
			}
		}
	}

	struct wl_proxy *ret =
		real_marshal(proxy, opcode, interface, version, flags, args);

	if (name && ret) {
		if (!strcmp(name, "xdg_wm_base") &&
		    opcode == OP_XDG_WM_BASE_GET_XDG_SURFACE && args != NULL) {
			struct wl_surface *s = (struct wl_surface *)args[1].o;
			if (s)
				xs_add(ret, s);
			dbg("xdg_surface %p -> wl_surface %p", (void *)ret,
			    (void *)s);
		} else if (!strcmp(name, "xdg_surface") &&
			   opcode == OP_XDG_SURFACE_GET_TOPLEVEL) {
			struct wl_surface *s = xs_get(proxy);
			if (s && !top_get(s)) {
				if (top_add(s))
					dbg("toplevel wl_surface %p",
					    (void *)s);
			}
		}
	}
	if (name && !strcmp(name, "xdg_surface") &&
	    opcode == OP_XDG_SURFACE_DESTROY)
		xs_remove(proxy);
	if (name && !strcmp(name, "wl_surface") &&
	    opcode == OP_WL_SURFACE_DESTROY)
		top_remove((struct wl_surface *)proxy);

	return ret;
}

struct wl_display *
wl_display_connect(const char *name)
{
	resolve();
	if (!real_connect)
		return NULL;
	struct wl_display *dpy = real_connect(name);
	if (dpy && !g_init)
		ensure_init(dpy);
	return dpy;
}

struct wl_display *
wl_display_connect_to_fd(int fd)
{
	resolve();
	if (!real_connect_fd)
		return NULL;
	struct wl_display *dpy = real_connect_fd(fd);
	if (dpy && !g_init)
		ensure_init(dpy);
	return dpy;
}
