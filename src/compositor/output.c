#include "internal.h"
#include "macros.h"
#include "output.h"
#include "visibility.h"

#include "compositor.h"
#include "callback.h"
#include "surface.h"
#include "buffer.h"
#include "view.h"

#include "seat/seat.h"
#include "seat/pointer.h"

#include "platform/backend/backend.h"
#include "platform/context/context.h"
#include "platform/render/render.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <time.h>

#include <wayland-server.h>

static struct wlc_space*
wlc_space_new(struct wlc_output *output)
{
   assert(output);

   struct wlc_space *space;
   if (!(space = calloc(1, sizeof(struct wlc_space))))
      return NULL;

   space->output = output;
   wl_list_init(&space->views);
   wl_list_insert(output->spaces.prev, &space->link);
   return space;
}

static void
wlc_space_free(struct wlc_space *space)
{
   assert(space);

   if (space->output->space == space)
      space->output->space = (wl_list_empty(&space->output->spaces) ? NULL : wlc_space_from_link(space->link.prev));

   free(space);
}

static void
wl_cb_output_resource_destructor(struct wl_resource *resource)
{
   if (wl_resource_get_user_data(resource))
      wl_list_remove(wl_resource_get_link(resource));
}

static void
wl_output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *resource;
   if (!(resource = wl_resource_create(client, &wl_output_interface, MIN(version, 2) , id)))
      goto fail;

   struct wlc_output *output = data;
   wl_resource_set_implementation(resource, NULL, output, &wl_cb_output_resource_destructor);
   wl_list_insert(&output->resources, wl_resource_get_link(resource));

   wl_output_send_geometry(resource, output->information.x, output->information.y,
       output->information.physical_width, output->information.physical_height, output->information.subpixel,
       (output->information.make.data ? output->information.make.data : "unknown"),
       (output->information.model.data ? output->information.model.data : "model"),
       output->information.transform);

   if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
      wl_output_send_scale(resource, output->information.scale);

   uint32_t m = 0;
   output->mode = UINT_MAX;
   struct wlc_output_mode *mode;
   wl_array_for_each(mode, &output->information.modes) {
      wl_output_send_mode(resource, mode->flags, mode->width, mode->height, mode->refresh);

      if (mode->flags & WL_OUTPUT_MODE_CURRENT || (output->mode == UINT_MAX && (mode->flags & WL_OUTPUT_MODE_PREFERRED)))
         output->mode = m;

      ++m;
   }

   assert(output->mode != UINT_MAX && "output should have at least one current mode!");

   if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
      wl_output_send_done(resource);

   return;

fail:
   wl_client_post_no_memory(client);
}

static bool
should_render(struct wlc_output *output)
{
   return (wlc_get_active() && !output->pending && output->context && output->render);
}

static bool
is_transparent_top_of_background(struct wlc_output *output, struct wlc_view *view)
{
   struct wlc_view *v;
   wl_list_for_each(v, &output->space->views, link) {
      if (!v->surface->opaque)
         continue;

      if (wlc_geometry_contains(&v->commit.geometry, &view->commit.geometry))
         return false;
   }
   return true;
}

static bool
is_visible(struct wlc_output *output)
{
   struct wlc_view *v;
   struct wlc_geometry g = { { INT_MAX, INT_MAX }, { 0, 0 } }, root = { { 0, 0 }, output->resolution };
   wl_list_for_each(v, &output->space->views, link) {
      if (!v->surface->opaque) {
         if (is_transparent_top_of_background(output, v))
            return true;
         continue;
      }

      struct wlc_size size = {
         v->commit.geometry.origin.x + v->commit.geometry.size.w,
         v->commit.geometry.origin.y + v->commit.geometry.size.h
      };

      wlc_origin_min(&g.origin, &v->commit.geometry.origin, &g.origin);
      wlc_size_max(&g.size, &size, &g.size);
   }
   return !wlc_geometry_contains(&g, &root);
}

static bool
repaint(struct wlc_output *output)
{
   assert(output);

   if (!should_render(output) || !wlc_render_bind(output->render, output)) {
      wlc_dlog(WLC_DBG_RENDER, "-> Skipped repaint");
      output->activity = output->scheduled = false;
      return false;
   }

   wlc_render_time(output->render, output->frame_time);

   if (output->compositor->options.enable_bg && !output->background_visible && is_visible(output)) {
      wlc_dlog(WLC_DBG_RENDER, "-> Background visible");
      output->background_visible = true;
   }

   if (output->background_visible) {
      wlc_render_background(output->render);
   } else if (!output->compositor->options.enable_bg) {
      wlc_render_clear(output->render);
   }

   struct wl_list callbacks;
   wl_list_init(&callbacks);

   struct wlc_view *view;
   wl_list_for_each(view, &output->space->views, link) {
      if (!view->created || !view->surface->commit.attached)
         continue;

      wlc_view_commit_state(view, &view->pending, &view->commit);
      wlc_render_view_paint(output->render, view);

      wl_list_insert_list(&callbacks, &view->surface->commit.frame_cb_list);
      wl_list_init(&view->surface->commit.frame_cb_list);
   }

   if (output->compositor->output == output) // XXX: Make this option instead, and give each output current cursor coords
      wlc_pointer_paint(output->compositor->seat->pointer, output->render);

   {
      void *rgba;
      struct wlc_geometry g = { { 0, 0 }, output->resolution };
      if (output->task.pixels && (rgba = calloc(1, g.size.w * g.size.h * 4))) {
         wlc_render_read_pixels(output->render, &g, rgba);
         output->task.pixels(&g.size, rgba);
         output->task.pixels = NULL;
         free(rgba);
      }
   }

   output->pending = true;
   wlc_render_swap(output->render);

   struct wlc_callback *cb, *cbn;
   wl_list_for_each_safe(cb, cbn, &callbacks, link) {
      wl_callback_send_done(cb->resource, output->frame_time);
      wlc_callback_free(cb);
   }

   wlc_dlog(WLC_DBG_RENDER, "-> Repaint");
   return true;
}

static int
cb_idle_timer(void *data)
{
   repaint(data);
   return 1;
}

void
wlc_output_finish_frame(struct wlc_output *output, const struct timespec *ts)
{
   output->pending = false;

   // XXX: uint32_t holds mostly for 50 days before overflowing
   //      is this tied to wayland somewhere, or should we increase precision?
   const uint32_t last = output->frame_time;
   output->frame_time = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
   const uint32_t ms = output->frame_time - last;

   // TODO: handle presentation feedback here

   if (output->compositor->options.enable_bg && output->background_visible && !is_visible(output)) {
      wlc_dlog(WLC_DBG_RENDER, "-> Background not visible");
      output->background_visible = false;
   }

   if ((output->background_visible || output->activity) && !output->terminating) {
      output->ims = fmin(fmax(output->ims * (output->activity ? 0.9 : 1.1), 1), 41);
      wlc_dlog(WLC_DBG_RENDER, "-> Interpolated idle time %f (%u : %d)", output->ims, ms, output->activity);
      wl_event_source_timer_update(output->idle_timer, output->ims);
      output->scheduled = true;
      output->activity = false;
   } else {
      output->scheduled = false;
   }

   wlc_dlog(WLC_DBG_RENDER, "-> Finished frame");

   if (output->terminating) {
      struct wlc_output_event ev = { output, WLC_OUTPUT_EVENT_REMOVE };
      wl_signal_emit(&wlc_system_signals()->output, &ev);
      output->terminating = false;
   }
}

bool
wlc_output_information_add_mode(struct wlc_output_information *info, struct wlc_output_mode *mode)
{
   assert(info && mode);

   struct wlc_output_mode *copied;
   if (!(copied = wl_array_add(&info->modes, sizeof(struct wlc_output_mode))))
      return false;

   memcpy(copied, mode, sizeof(struct wlc_output_mode));
   return true;
}

void
wlc_output_surface_destroy(struct wlc_output *output, struct wlc_surface *surface)
{
   assert(output && surface);

   // XXX: Code smell, another case of resource management.
   if (output->compositor->seat->pointer->surface == surface)
      output->compositor->seat->pointer->surface = NULL;

   wlc_render_surface_destroy(output->render, surface);
   surface->output = NULL;

   wlc_output_schedule_repaint(output);
}

bool
wlc_output_surface_attach(struct wlc_output *output, struct wlc_surface *surface, struct wlc_buffer *buffer)
{
   assert(output && surface);

   if (surface->output && surface->output != output)
      wlc_output_surface_destroy(surface->output, surface);

   if (!wlc_render_surface_attach(output->render, surface, buffer))
      return false;

   wlc_dlog(WLC_DBG_RENDER, "-> Attached surface (%p) to output (%p)", surface, output);
   wlc_output_schedule_repaint(output);
   return true;
}

void
wlc_output_schedule_repaint(struct wlc_output *output)
{
   assert(output);

   if (!output->activity)
      wlc_dlog(WLC_DBG_RENDER, "-> Activity marked");

   output->activity = true;

   if (output->scheduled)
      return;

   output->scheduled = true;
   wl_event_source_timer_update(output->idle_timer, 1);
   wlc_dlog(WLC_DBG_RENDER, "-> Repaint scheduled");
}

bool
wlc_output_set_surface(struct wlc_output *output, struct wlc_backend_surface *surface)
{
   if (output->surface == surface)
      return true;

   if (output->surface) {
      if (output->render) {
         wlc_render_free(output->render);
         output->render = NULL;
      }

      if (output->context) {
         wlc_context_free(output->context);
         output->context = NULL;
      }

      if (output->surface) {
         wlc_backend_surface_free(output->surface);
         output->surface = NULL;
      }
   }

   if ((output->surface = surface)) {
      if (!(output->context = wlc_context_new(surface)))
         goto fail;

      if (!(output->render = wlc_render_new(output->context)))
         goto fail;
   }

   return true;

fail:
   wlc_output_set_surface(output, NULL);
   return false;
}

void
wlc_output_terminate(struct wlc_output *output)
{
   assert(output);
   output->terminating = true;
   wlc_output_schedule_repaint(output);
}

void
wlc_output_free(struct wlc_output *output)
{
   assert(output);

   if (output->idle_timer)
      wl_event_source_remove(output->idle_timer);

   struct wl_resource *r, *rn;
   wl_resource_for_each_safe(r, rn, &output->resources)
      wl_resource_destroy(r);

   struct wlc_space *s, *sn;
   wl_list_for_each_safe(s, sn, &output->spaces, link)
      wlc_space_free(s);

   wlc_output_set_surface(output, NULL);

   wlc_string_release(&output->information.make);
   wlc_string_release(&output->information.model);
   wl_array_release(&output->information.modes);

   if (output->global)
      wl_global_destroy(output->global);

   free(output);
}

struct wlc_output*
wlc_output_new(struct wlc_compositor *compositor, struct wlc_backend_surface *surface, struct wlc_output_information *info)
{
   struct wlc_output *output;
   if (!(output = calloc(1, sizeof(struct wlc_output))))
      goto fail;

   if (!(output->idle_timer = wl_event_loop_add_timer(wlc_event_loop(), cb_idle_timer, output)))
      goto fail;

   if (!(output->global = wl_global_create(wlc_display(), &wl_output_interface, 2, output, &wl_output_bind)))
      goto fail;

   memcpy(&output->information, info, sizeof(output->information));
   wl_list_init(&output->resources);
   wl_list_init(&output->spaces);

   output->ims = 41;
   output->compositor = compositor;

   if (!(output->space = wlc_space_new(output)))
      goto fail;

   if (!wlc_output_set_surface(output, surface))
      goto fail;

   wlc_context_bind_to_wl_display(output->context, wlc_display());

   struct wlc_output_mode *mode = output->information.modes.data + (output->mode * sizeof(struct wlc_output_mode));
   wlc_output_set_resolution(output, &(struct wlc_size){ mode->width, mode->height });
   return output;

fail:
   if (output)
      wlc_output_free(output);
   return NULL;
}

WLC_API void
wlc_output_get_pixels(struct wlc_output *output, void (*async)(const struct wlc_size *size, uint8_t *rgba))
{
   assert(output && async);

   if (output->task.pixels)
      return;

   output->task.pixels = async;
   wlc_output_schedule_repaint(output);
}

WLC_API void
wlc_output_set_resolution(struct wlc_output *output, const struct wlc_size *resolution)
{
   if (wlc_size_equals(resolution, &output->resolution))
      return;

   output->resolution = *resolution;

   WLC_INTERFACE_EMIT(output.resolution, output->compositor, output, resolution);

   wlc_output_schedule_repaint(output);
}

WLC_API const struct wlc_size*
wlc_output_get_resolution(struct wlc_output *output)
{
   assert(output);
   return &output->resolution;
}

WLC_API struct wlc_space*
wlc_output_get_active_space(struct wlc_output *output)
{
   assert(output);
   return output->space;
}

WLC_API struct wl_list*
wlc_output_get_spaces(struct wlc_output *output)
{
   assert(output);
   return &output->spaces;
}

WLC_API struct wl_list*
wlc_output_get_link(struct wlc_output *output)
{
   assert(output);
   return &output->link;
}

WLC_API struct wlc_output*
wlc_output_from_link(struct wl_list *output_link)
{
   assert(output_link);
   struct wlc_output *output;
   return wl_container_of(output_link, output, link);
}

WLC_API void
wlc_output_set_userdata(struct wlc_output *output, void *userdata)
{
   assert(output);
   output->userdata = userdata;
}

WLC_API void*
wlc_output_get_userdata(struct wlc_output *output)
{
   assert(output);
   return output->userdata;
}

WLC_API void
wlc_output_focus_space(struct wlc_output *output, struct wlc_space *space)
{
   assert(output);

   if (output->space == space)
      return;

   output->space = space;

   WLC_INTERFACE_EMIT(space.activated, output->compositor, space);

   wlc_output_schedule_repaint(output);
}

WLC_API struct wlc_output*
wlc_space_get_output(struct wlc_space *space)
{
   assert(space);
   return space->output;
}

WLC_API struct wl_list*
wlc_space_get_views(struct wlc_space *space)
{
   assert(space);
   return &space->views;
}

WLC_API struct wl_list*
wlc_space_get_link(struct wlc_space *space)
{
   assert(space);
   return &space->link;
}

WLC_API struct wlc_space*
wlc_space_from_link(struct wl_list *space_link)
{
   assert(space_link);
   struct wlc_space *space;
   return wl_container_of(space_link, space, link);
}

WLC_API void
wlc_space_set_userdata(struct wlc_space *space, void *userdata)
{
   assert(space);
   space->userdata = userdata;
}

WLC_API void*
wlc_space_get_userdata(struct wlc_space *space)
{
   assert(space);
   return space->userdata;
}

WLC_API struct wlc_space*
wlc_space_add(struct wlc_output *output)
{
   assert(output);
   return wlc_space_new(output);
}

WLC_API void
wlc_space_remove(struct wlc_space *space)
{
   assert(0 && "not fully implemented");
   assert(space);
   return wlc_space_free(space);
}
