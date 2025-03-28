#include <scenefx/types/wlr_scene.h>

#include "toplevel.h"

#include "config.h"
#include "ipc.h"
#include "layout.h"
#include "mwc.h"
#include "popup.h"
#include "rendering.h"
#include "something.h"
#include "workspace.h"
#include "output.h"
#include "helpers.h"
#include "layer_surface.h"
#include "pointer.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/edges.h>

extern struct mwc_server server;

void
server_handle_new_toplevel(struct wl_listener *listener, void *data) {
  /* this event is raised when a client creates a new toplevel */
  struct wlr_xdg_toplevel *xdg_toplevel = data;
  /* allocate an mwc_toplevel for this surface */
  struct mwc_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  toplevel->xdg_toplevel = xdg_toplevel;

  toplevel->something.type = MWC_TOPLEVEL;
  toplevel->something.toplevel = toplevel;

  toplevel->active_opacity = server.config->active_opacity;
  toplevel->inactive_opacity = server.config->inactive_opacity;

  toplevel->workspace = server.active_workspace;

  wlr_fractional_scale_v1_notify_scale(toplevel->xdg_toplevel->base->surface,
                                       toplevel->workspace->output->wlr_output->scale);
  wlr_surface_set_preferred_buffer_scale(toplevel->xdg_toplevel->base->surface,
                                         ceil(toplevel->workspace->output->wlr_output->scale));
  /* add foreign toplevel handler */
  toplevel->foreign_toplevel_handle =
    wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);

  /* listen to the various events it can emit */
  toplevel->map.notify = toplevel_handle_map;
  wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);

  toplevel->unmap.notify = toplevel_handle_unmap;
  wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);

  toplevel->commit.notify = toplevel_handle_commit;
  wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

  toplevel->destroy.notify = toplevel_handle_destroy;
  wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

  toplevel->request_move.notify = toplevel_handle_request_move;
  wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);

  toplevel->request_resize.notify = toplevel_handle_request_resize;
  wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);

  toplevel->request_maximize.notify = toplevel_handle_request_maximize;
  wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);

  toplevel->request_fullscreen.notify = toplevel_handle_request_fullscreen;
  wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

  toplevel->set_app_id.notify = toplevel_handle_set_app_id;
  wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

  toplevel->set_title.notify = toplevel_handle_set_title;
  wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
}

void
toplevel_handle_initial_commit(struct mwc_toplevel *toplevel) {
  /* when an xdg_surface performs an initial commit, the compositor must
   * reply with a configure so the client can map the surface. */
  toplevel->floating = toplevel_should_float(toplevel);

  uint32_t width, height;
  if(toplevel->floating) {
    /* we lookup window rules and send a configure */
    toplevel_floating_size(toplevel, &width, &height);
  } else {
    struct mwc_output *output = toplevel->workspace->output;

    uint32_t master_count = wl_list_length(&toplevel->workspace->masters);
    uint32_t slave_count = wl_list_length(&toplevel->workspace->slaves);
    if(master_count < server.config->master_count) {
      calculate_masters_dimensions(output, master_count + 1, slave_count, &width, &height);
    } else {
      calculate_slaves_dimensions(output, slave_count + 1, &width, &height);
    }
  }

  wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
  /* we lie that its maximized so it behaves better */
  wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
  wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, WLR_EDGE_TOP & WLR_EDGE_RIGHT
                             & WLR_EDGE_BOTTOM & WLR_EDGE_LEFT);
}

void
toplevel_handle_commit(struct wl_listener *listener, void *data) {
  /* called when a new surface state is committed */
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

  if(!toplevel->xdg_toplevel->base->initialized) return;

  if(toplevel->xdg_toplevel->base->initial_commit) {
    toplevel_handle_initial_commit(toplevel);
    return;
  }

  if(toplevel->resizing) {
    toplevel_commit(toplevel);
    return;
  }

  uint32_t serial = toplevel->xdg_toplevel->base->current.configure_serial;
  if(!toplevel->dirty || serial < toplevel->configure_serial) return;

  if(toplevel->floating && !toplevel->fullscreen) {
    if(toplevel->pending.width == 0) {
      struct wlr_box geometry = toplevel_get_geometry(toplevel);
      toplevel->pending.width = geometry.width;
      toplevel->pending.height = geometry.height;
    }

    if(toplevel->pending.x == UINT32_MAX) {
      struct wlr_box output_box = toplevel->workspace->output->usable_area;
      toplevel->pending.x = output_box.x + (output_box.width - toplevel->pending.width) / 2;
      toplevel->pending.y = output_box.y + (output_box.height - toplevel->pending.height) / 2;
    }
  }

  toplevel_commit(toplevel);
}

void
toplevel_handle_map(struct wl_listener *listener, void *data) {
  /* called when the surface is mapped, or ready to display on-screen. */
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, map);

  if(toplevel->floating) {
    wl_list_insert(&toplevel->workspace->floating_toplevels, &toplevel->link);
    toplevel->scene_tree = wlr_scene_xdg_surface_create(server.floating_tree,
                                                        toplevel->xdg_toplevel->base);
  } else {
    if(wl_list_length(&toplevel->workspace->masters) < server.config->master_count) {
      wl_list_insert(toplevel->workspace->masters.prev, &toplevel->link);
    } else {
      wl_list_insert(toplevel->workspace->slaves.prev, &toplevel->link);
    }

    toplevel->scene_tree = wlr_scene_xdg_surface_create(server.tiled_tree,
                                                        toplevel->xdg_toplevel->base);
    layout_set_pending_state(toplevel->workspace);
  }

  /* output at 0, 0 would get this toplevel flashed if its on some other output,
   * so we move it to its own, which will cause it to send frame event which
   * will place it where it belongs */
  wlr_scene_node_set_position(&toplevel->scene_tree->node,
                              toplevel->workspace->output->usable_area.x,
                              toplevel->workspace->output->usable_area.y);
  
  /* this often breaks the toplevel, but what can i do about it? */
  if(toplevel->workspace->fullscreen_toplevel != NULL) {
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
  }

  /* we are keeping toplevels scene_tree in this free user data field, it is used in 
   * assigning parents to popups */
  toplevel->xdg_toplevel->base->data = toplevel->scene_tree;

  /* in the node we want to keep information what that node represents. we do that
   * be keeping mwc_something in user data field, which is a union of all possible
   * 'things' we can have on the screen */
  toplevel->scene_tree->node.data = &toplevel->something;

  focus_toplevel(toplevel);

  if(toplevel->floating) {
    if(toplevel->pending.width == 0) {
      struct wlr_box geometry = toplevel_get_geometry(toplevel);
      toplevel->pending.width = geometry.width;
      toplevel->pending.height = geometry.height;
    }

    struct wlr_box output_box = toplevel->workspace->output->usable_area;
    toplevel->pending.x = output_box.x + (output_box.width - toplevel->pending.width) / 2;
    toplevel->pending.y = output_box.y + (output_box.height - toplevel->pending.height) / 2;
  } 

  /* we patch its startup animation */
  if(server.config->animations) {
    toplevel->animation.should_animate = true;
    toplevel->animation.initial = (struct wlr_box){
      .x = toplevel->pending.x + toplevel->pending.width / 2,
      .y = toplevel->pending.y + toplevel->pending.height / 2,
      .width = 1,
      .height = 1,
    };
  } else {
    toplevel->animation.should_animate = false;
  }

  toplevel_commit(toplevel);
}

void
toplevel_handle_unmap(struct wl_listener *listener, void *data) {
  /* called when the surface is unmapped, and should no longer be shown. */
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

  struct mwc_workspace *workspace = toplevel->workspace;

  /* reset the cursor mode if the grabbed toplevel was unmapped. */
  /* if its the one focus should be returned to, remove it */
  if(toplevel == server.prev_focused) {
    server.prev_focused = NULL;
  }

  if(toplevel == server.grabbed_toplevel) {
    server_reset_cursor_mode();
    
    if(toplevel->floating && !wl_list_empty(&workspace->floating_toplevels)) {
      struct mwc_toplevel *t = wl_container_of(workspace->floating_toplevels.next, t, link);
      focus_toplevel(t);
    } else if(!wl_list_empty(&workspace->masters)) {
      struct mwc_toplevel *t = wl_container_of(workspace->masters.next, t, link);
      focus_toplevel(t);
    } else {
      server.focused_toplevel = NULL;
      ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
    }

    return;
  }


  if(toplevel == workspace->fullscreen_toplevel) {
    workspace->fullscreen_toplevel = NULL;
    layers_under_fullscreen_set_enabled(workspace->output, true);
    struct mwc_toplevel *t;
    wl_list_for_each(t, &workspace->masters, link) {
      if(t == toplevel) continue;
      wlr_scene_node_set_enabled(&t->scene_tree->node, true);
    }
    wl_list_for_each(t, &workspace->slaves, link) {
      if(t == toplevel) continue;
      wlr_scene_node_set_enabled(&t->scene_tree->node, true);
    }
    wl_list_for_each(t, &workspace->floating_toplevels, link) {
      if(t == toplevel) continue;
      wlr_scene_node_set_enabled(&t->scene_tree->node, true);
    }
  }

  if(toplevel->floating) {
    if(server.focused_toplevel == toplevel) {
      /* try to find other floating toplevels to give focus to */
      struct wl_list *focus_next = toplevel->link.next;
      if(focus_next == &workspace->floating_toplevels) {
        focus_next = toplevel->link.prev;
        if(focus_next == &workspace->floating_toplevels) {
          focus_next = workspace->masters.next;
          if(focus_next == &workspace->masters) {
            focus_next = NULL;
          }
        }
      }

      if(focus_next != NULL) {
        struct mwc_toplevel *t = wl_container_of(focus_next, t, link);
        focus_toplevel(t);
      } else {
        server.focused_toplevel = NULL;
        ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
      }
    }

    wl_list_remove(&toplevel->link);
    return;
  }

  if(toplevel_is_master(toplevel)) {
    /* we find a new master to replace him if possible */
    if(!wl_list_empty(&workspace->slaves)) {
      struct mwc_toplevel *s = wl_container_of(workspace->slaves.prev, s, link);
      wl_list_remove(&s->link);
      wl_list_insert(workspace->masters.prev, &s->link);
    }
    if(toplevel == server.focused_toplevel) {
      /* we want to give focus to some other toplevel */
      struct wl_list *focus_next = toplevel->link.next;
      if(focus_next == &workspace->masters) {
        focus_next = toplevel->link.prev;
        if(focus_next == &workspace->masters) {
          focus_next = workspace->floating_toplevels.next;
          if(focus_next == &workspace->floating_toplevels) {
            focus_next = NULL;
          }
        }
      }

      if(focus_next != NULL) {
        struct mwc_toplevel *t = wl_container_of(focus_next, t, link);
        focus_toplevel(t);
      } else {
        server.focused_toplevel = NULL;
        ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
      }
    }

    /* we finally remove him from the list */
    wl_list_remove(&toplevel->link);
  } else {
    if(toplevel == server.focused_toplevel) {
      /* we want to give focus to some other toplevel */
      struct wl_list *focus_next = toplevel->link.next;
      if(focus_next == &workspace->slaves) {
        focus_next = toplevel->link.prev;
        if(focus_next == &workspace->slaves) {
          /* take the last master */
          focus_next = workspace->masters.prev;
        }
      }
      /* here its not possible to have no other toplevel to give focus,
       * there are always master_count masters available */
      struct mwc_toplevel *t = wl_container_of(focus_next, t, link);
      focus_toplevel(t);
    }

    wl_list_remove(&toplevel->link);
  }

  layout_set_pending_state(toplevel->workspace);
}

void
toplevel_handle_destroy(struct wl_listener *listener, void *data) {
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

  wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_toplevel_handle);

  wl_list_remove(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_remove(&toplevel->commit.link);
  wl_list_remove(&toplevel->destroy.link);
  wl_list_remove(&toplevel->request_move.link);
  wl_list_remove(&toplevel->request_resize.link);
  wl_list_remove(&toplevel->request_maximize.link);
  wl_list_remove(&toplevel->request_fullscreen.link);

  free(toplevel);
}

struct wlr_box
toplevel_get_geometry(struct mwc_toplevel *toplevel) {
  struct wlr_box geometry;
  wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geometry);
  return geometry;
}

void
toplevel_start_move(struct mwc_toplevel *toplevel) {
  if(server.grabbed_toplevel != NULL) return;

  server.grabbed_toplevel = toplevel;
  server.cursor_mode = MWC_CURSOR_MOVE;

  server.grab_x = server.cursor->x;
  server.grab_y = server.cursor->y;

  server.grabbed_toplevel_initial_box = (struct wlr_box){
    .x = X(toplevel),
    .y = Y(toplevel),
    .width = toplevel->current.width,
    .height = toplevel->current.height,
  };

  if(toplevel->floating) {
    wl_list_remove(&toplevel->link);
  } else {
    bool is_master = toplevel_is_master(toplevel);
    wl_list_remove(&toplevel->link);
    if(is_master && !wl_list_empty(&toplevel->workspace->slaves)) {
      struct mwc_toplevel *last = wl_container_of(toplevel->workspace->slaves.prev, last, link);
      wl_list_remove(&last->link);
      wl_list_insert(toplevel->workspace->masters.prev, &last->link);
    }

    layout_set_pending_state(toplevel->workspace);
  }
}

void
toplevel_start_resize(struct mwc_toplevel *toplevel, uint32_t edges) {
  if(server.grabbed_toplevel != NULL) return;

  server.grabbed_toplevel = toplevel;
  server.cursor_mode = MWC_CURSOR_RESIZE;

  server.grab_x = server.cursor->x;
  server.grab_y = server.cursor->y;

  server.grabbed_toplevel_initial_box = toplevel->current;
  server.resize_edges = edges;
}

void
toplevel_handle_request_move(struct wl_listener *listener, void *data) {
  /* This event is raised when a client would like to begin an interactive
   * move, typically because the user clicked on their client-side
   * decorations. Note that a more sophisticated compositor should check the
   * provided serial against a list of button press serials sent to this
   * client, to prevent the client from requesting this whenever they want. */
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
  if(toplevel != get_pointer_focused_toplevel()) return;

  server.client_driven_move_resize = true;
  toplevel_start_move(toplevel);
}

void
toplevel_handle_request_resize(struct wl_listener *listener, void *data) {
  /* This event is raised when a client would like to begin an interactive
   * resize, typically because the user clicked on their client-side
   * decorations. Note that a more sophisticated compositor should check the
   * provided serial against a list of button press serials sent to this
   * client, to prevent the client from requesting this whenever they want. */
  struct wlr_xdg_toplevel_resize_event *event = data;

  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
  if(!toplevel->floating || toplevel != get_pointer_focused_toplevel()) return;

  server.client_driven_move_resize = true;
  toplevel_start_resize(toplevel, event->edges);
}

void
toplevel_handle_request_maximize(struct wl_listener *listener, void *data) {
  /* This event is raised when a client would like to maximize itself,
   * typically because the user clicked on the maximize button on client-side
   * decorations. mwc doesn't support maximization, but to conform to
   * xdg-shell protocol we still must send a configure.
   * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
   * However, if the request was sent before an initial commit, we don't do
   * anything and let the client finish the initial surface setup. */
  struct mwc_toplevel *toplevel =
    wl_container_of(listener, toplevel, request_maximize);
  if(toplevel->xdg_toplevel->base->initialized) {
    wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
  }
}

void
toplevel_handle_request_fullscreen(struct wl_listener *listener, void *data) {
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);

  if(toplevel->xdg_toplevel->requested.fullscreen) {
    toplevel_set_fullscreen(toplevel);
  } else {
    toplevel_unset_fullscreen(toplevel);
  }
}

void
toplevel_recheck_opacity_rules(struct mwc_toplevel *toplevel) {
  /* check if it satisfies some window rule */
  struct window_rule_opacity *w;
  bool set = false;
  wl_list_for_each(w, &server.config->window_rules.opacity, link) {
    if(toplevel_matches_window_rule(toplevel, &w->condition)) {
      toplevel->inactive_opacity = w->inactive_value;
      toplevel->active_opacity = w->active_value;
      set = true;
      break;
    }
  }

  if(!set) {
    toplevel->inactive_opacity = server.config->inactive_opacity;
    toplevel->active_opacity = server.config->active_opacity;
  }
}

void
toplevel_handle_set_app_id(struct wl_listener *listener, void *data) {
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, set_app_id);

  toplevel_recheck_opacity_rules(toplevel);

  wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->foreign_toplevel_handle,
                                            toplevel->xdg_toplevel->app_id);

  if(toplevel == server.focused_toplevel) {
    ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
  }
}

void
toplevel_handle_set_title(struct wl_listener *listener, void *data) {
  struct mwc_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);

  toplevel_recheck_opacity_rules(toplevel);

  wlr_foreign_toplevel_handle_v1_set_title(toplevel->foreign_toplevel_handle,
                                           toplevel->xdg_toplevel->title);

  if(toplevel == server.focused_toplevel) {
    ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
  }
}

bool
toplevel_matches_window_rule(struct mwc_toplevel *toplevel,
                             struct window_rule_regex *condition) {
  char *app_id = toplevel->xdg_toplevel->app_id;
  char *title = toplevel->xdg_toplevel->title;

  bool matches_app_id;
  if(condition->has_app_id_regex) {
    if(app_id == NULL) {
      matches_app_id = false;
    } else {
      matches_app_id = regexec(&condition->app_id_regex, app_id, 0, NULL, 0) == 0;
    }
  } else {
    matches_app_id = true;
  }

  bool matches_title;
  if(condition->has_title_regex) {
    if(title == NULL) {
      matches_title = false;
    } else {
      matches_title = regexec(&condition->title_regex, title, 0, NULL, 0) == 0;
    }
  } else {
    matches_title = true;
  }

  return matches_app_id && matches_title;
}

void
toplevel_floating_size(struct mwc_toplevel *toplevel, uint32_t *width, uint32_t *height) {
  struct window_rule_size *w;
  wl_list_for_each(w, &server.config->window_rules.size, link) {
    if(toplevel_matches_window_rule(toplevel, &w->condition)) {
      if(w->relative_width) {
        *width = toplevel->workspace->output->usable_area.width * w->width / 100;
      } else {
        *width = w->width;
      }

      if(w->relative_height) {
        *height = toplevel->workspace->output->usable_area.height * w->height / 100;
      } else {
        *height = w->height;
      }

      return;
    }
  }

  *width = 0;
  *height = 0;
}

bool
toplevel_should_float(struct mwc_toplevel *toplevel) {
  /* we make toplevels float if they have fixed size
   * or are children of another toplevel */
  bool b =
    (toplevel->xdg_toplevel->current.max_height &&
    toplevel->xdg_toplevel->current.max_height
    == toplevel->xdg_toplevel->current.min_height)
    || (toplevel->xdg_toplevel->current.max_width &&
    toplevel->xdg_toplevel->current.max_width
    == toplevel->xdg_toplevel->current.min_width)
    || toplevel->xdg_toplevel->parent != NULL;
  if(b) return true;

  struct window_rule_float *w;
  wl_list_for_each(w, &server.config->window_rules.floating, link) {
    if(toplevel_matches_window_rule(toplevel, &w->condition)) {
      return true;
    }
  }

  return false;
}

struct mwc_toplevel *
get_pointer_focused_toplevel(void) {
  struct wlr_surface *focused_surface = server.seat->pointer_state.focused_surface;
  if(focused_surface == NULL) {
    return NULL;
  }

  struct mwc_something *something = root_parent_of_surface(focused_surface);
  if(something->type == MWC_TOPLEVEL) {
    return something->toplevel;
  }

  return NULL;
}

void
cursor_jump_focused_toplevel(void) {
  struct mwc_toplevel *toplevel = server.focused_toplevel;
  if(toplevel == NULL) return;

  struct wlr_box geo_box = toplevel_get_geometry(toplevel);
  wlr_cursor_warp(server.cursor, NULL,
                  toplevel->scene_tree->node.x + geo_box.x + toplevel->current.width / 2.0,
                  toplevel->scene_tree->node.y + geo_box.y + toplevel->current.height / 2.0);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  pointer_handle_focus(now.tv_sec * 1000 + now.tv_nsec / 1000, false);
}

void
toplevel_set_pending_state(struct mwc_toplevel *toplevel, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height) {
  struct wlr_box pending = {
    .x = x,
    .y = y,
    .width = width,
    .height = height,
  };

  toplevel->pending = pending;

  if(!server.config->animations || toplevel == server.grabbed_toplevel
     || wlr_box_equal(&toplevel->current, &pending)) {
    toplevel->animation.should_animate = false;
  } else {
    toplevel->animation.should_animate = true;
    toplevel->animation.initial = toplevel->current;
  }

  if(toplevel->current.width == toplevel->pending.width
     && toplevel->current.height == toplevel->pending.height) {
    toplevel_commit(toplevel);
    return;
  };

  toplevel->configure_serial = wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                                                         width, height);
  toplevel->dirty = true;
}

void
toplevel_commit(struct mwc_toplevel *toplevel) {
  toplevel->dirty = false;
  toplevel->current = toplevel->pending;

  if(toplevel->animation.should_animate) {
    if(toplevel->animation.running) {
      /* if there is already an animation running, we start this one from the current state */
      toplevel->animation.initial = toplevel->animation.current;
    }
    toplevel->animation.passed_frames = 0;
    toplevel->animation.total_frames = server.config->animation_duration
      / output_frame_duration_ms(toplevel->workspace->output);

    toplevel->animation.running = true;
    toplevel->animation.should_animate = false;
  }

  wlr_output_schedule_frame(toplevel->workspace->output->wlr_output);
}

void
toplevel_set_fullscreen(struct mwc_toplevel *toplevel) {
  if(!toplevel->xdg_toplevel->base->surface->mapped) return;

  if(toplevel->workspace->fullscreen_toplevel != NULL) return;
  if(toplevel == server.grabbed_toplevel) return;

  struct mwc_workspace *workspace = toplevel->workspace;
  struct mwc_output *output = workspace->output;

  struct wlr_box output_box;
  wlr_output_layout_get_box(server.output_layout,
                            output->wlr_output, &output_box);

  toplevel->prev_geometry = toplevel->current;

  workspace->fullscreen_toplevel = toplevel;
  toplevel->fullscreen = true;

  wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
  toplevel_set_pending_state(toplevel, output_box.x, output_box.y,
                             output_box.width, output_box.height);
  wlr_scene_node_reparent(&toplevel->scene_tree->node, server.fullscreen_tree);

  /* we disable all the other toplevels so they are not seen if there is transparency */
  struct mwc_toplevel *t;
  wl_list_for_each(t, &workspace->masters, link) {
    if(t == toplevel) continue;
    wlr_scene_node_set_enabled(&t->scene_tree->node, false);
  }
  wl_list_for_each(t, &workspace->slaves, link) {
    if(t == toplevel) continue;
    wlr_scene_node_set_enabled(&t->scene_tree->node, false);
  }
  wl_list_for_each(t, &workspace->floating_toplevels, link) {
    if(t == toplevel) continue;
    wlr_scene_node_set_enabled(&t->scene_tree->node, false);
  }

  /* we also disable bottom and top layer surfaces, and leave only the backgorund */
  layers_under_fullscreen_set_enabled(workspace->output, false);

  wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign_toplevel_handle, true);
}

void
toplevel_unset_fullscreen(struct mwc_toplevel *toplevel) {
  if(toplevel->workspace->fullscreen_toplevel != toplevel) return;

  struct mwc_workspace *workspace = toplevel->workspace;

  workspace->fullscreen_toplevel = NULL;
  toplevel->fullscreen = false;

  wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);

  if(toplevel->floating) {
    toplevel_set_pending_state(toplevel,
                               toplevel->prev_geometry.x, toplevel->prev_geometry.y,
                               toplevel->prev_geometry.width, toplevel->prev_geometry.height);
    wlr_scene_node_reparent(&toplevel->scene_tree->node, server.floating_tree);
  } else {
    wlr_scene_node_reparent(&toplevel->scene_tree->node, server.tiled_tree);
  }

  /* reenable the scene nodes */
  struct mwc_toplevel *t;
  wl_list_for_each(t, &workspace->masters, link) {
    if(t == toplevel) continue;
    wlr_scene_node_set_enabled(&t->scene_tree->node, true);
  }
  wl_list_for_each(t, &workspace->slaves, link) {
    if(t == toplevel) continue;
    wlr_scene_node_set_enabled(&t->scene_tree->node, true);
  }
  wl_list_for_each(t, &workspace->floating_toplevels, link) {
    if(t == toplevel) continue;
    wlr_scene_node_set_enabled(&t->scene_tree->node, true);
  }

  layers_under_fullscreen_set_enabled(workspace->output, true);
  layout_set_pending_state(workspace);
  wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign_toplevel_handle, false);
}

void
toplevel_move(void) {
  /* move the grabbed toplevel to the new position */
  struct mwc_toplevel *toplevel = server.grabbed_toplevel;

  int32_t new_x = server.grabbed_toplevel_initial_box.x + (server.cursor->x - server.grab_x);
  int32_t new_y = server.grabbed_toplevel_initial_box.y + (server.cursor->y - server.grab_y);

  toplevel_set_pending_state(toplevel, new_x, new_y,
                             toplevel->current.width, toplevel->current.height);
}

void
toplevel_resize(void) {
  struct mwc_toplevel *toplevel = server.grabbed_toplevel;

  toplevel->resizing = true;

  int start_x = server.grabbed_toplevel_initial_box.x;
  int start_y = server.grabbed_toplevel_initial_box.y;
  int start_width = server.grabbed_toplevel_initial_box.width;
  int start_height = server.grabbed_toplevel_initial_box.height;

  int new_x = server.grabbed_toplevel_initial_box.x;
  int new_y = server.grabbed_toplevel_initial_box.y;
  int new_width = server.grabbed_toplevel_initial_box.width;
  int new_height = server.grabbed_toplevel_initial_box.height;

  int min_width = max(toplevel->xdg_toplevel->current.min_width,
                      server.config->min_toplevel_size);
  int min_height = max(toplevel->xdg_toplevel->current.min_height,
                       server.config->min_toplevel_size);

  if(server.resize_edges & WLR_EDGE_TOP) {
    new_y = start_y + (server.cursor->y - server.grab_y);
    new_height = start_height - (server.cursor->y - server.grab_y);
    if(new_height <= min_height) {
      new_y = start_y + start_height - min_height;
      new_height = min_height;
    }
  } else if(server.resize_edges & WLR_EDGE_BOTTOM) {
    new_y = start_y;
    new_height = start_height + (server.cursor->y - server.grab_y);
    if(new_height <= min_height) {
      new_height = min_height;
    }
  }
  if(server.resize_edges & WLR_EDGE_LEFT) {
    new_x = start_x + (server.cursor->x - server.grab_x);
    new_width = start_width - (server.cursor->x - server.grab_x);
    if(new_width <= min_width) {
      new_x = start_x + start_width - min_width;
      new_width = min_width;
    }
  } else if(server.resize_edges & WLR_EDGE_RIGHT) {
    new_x = start_x;
    new_width = start_width + (server.cursor->x - server.grab_x);
    if(new_width <= min_width) {
      new_width = min_width;
    }
  }

  toplevel_set_pending_state(toplevel, new_x, new_y, new_width, new_height);
}

void
unfocus_focused_toplevel(void) {
  struct mwc_toplevel *toplevel = server.focused_toplevel;
  if(toplevel == NULL) return;

  server.focused_toplevel = NULL;
  /* deactivate the surface */
  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, false);
  /* clear all focus on the keyboard, focusing new should set new toplevel focus */
  wlr_seat_keyboard_clear_focus(server.seat);
  wlr_seat_pointer_clear_focus(server.seat);

  ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
  wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_toplevel_handle, false);

  /* we schedule a frame in order for borders to be redrawn */
  wlr_output_schedule_frame(toplevel->workspace->output->wlr_output);
}

void
focus_toplevel(struct mwc_toplevel *toplevel) {
  /* there has been an issue with some electron apps that do not
   * want to map the surface, and neither want to destroy themselfs */
  if(server.lock != NULL) return;
  if(server.exclusive) return;
  if(server.grabbed_toplevel != NULL) return;
  if(toplevel->workspace->fullscreen_toplevel != NULL
     && toplevel != toplevel->workspace->fullscreen_toplevel) return;

  struct mwc_toplevel *prev_toplevel = server.focused_toplevel;
  if(prev_toplevel == toplevel) return;

  if(prev_toplevel != NULL) {
    wlr_xdg_toplevel_set_activated(prev_toplevel->xdg_toplevel, false);
    wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_toplevel_handle, false);
  }

  server.focused_toplevel = toplevel;

  if(toplevel->floating) {
    wl_list_remove(&toplevel->link);
    wl_list_insert(&toplevel->workspace->floating_toplevels, &toplevel->link);
  }

	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
  wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

  struct wlr_seat *seat = server.seat;
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  if(keyboard != NULL) {
    wlr_seat_keyboard_notify_enter(seat, toplevel->xdg_toplevel->base->surface,
                                   keyboard->keycodes, keyboard->num_keycodes,
                                   &keyboard->modifiers);
  }

  ipc_broadcast_message(IPC_ACTIVE_TOPLEVEL);
  wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_toplevel_handle, true);

  /* we schedule a frame in order for borders to be redrawn */
  wlr_output_schedule_frame(toplevel->workspace->output->wlr_output);
}


struct mwc_toplevel *
toplevel_find_closest_floating_on_workspace(struct mwc_toplevel *toplevel,
                                            enum mwc_direction direction) {
  assert(toplevel->floating);
  struct mwc_workspace *workspace = toplevel->workspace;

  struct mwc_toplevel *min = NULL;
  uint32_t min_val = UINT32_MAX;

  struct mwc_toplevel *t;
  switch(direction) {
    case MWC_UP: {
      wl_list_for_each(t, &workspace->floating_toplevels, link) {
        if(t == toplevel || Y(t) > Y(toplevel)) continue;

        uint32_t dy = abs((int)Y(toplevel) - Y(t));
        if(dy < min_val) {
          min = t;
          min_val = dy;
        }
      }
      return min;
    }
    case MWC_DOWN: {
      wl_list_for_each(t, &workspace->floating_toplevels, link) {
        if(t == toplevel || Y(t) < Y(toplevel)) continue;

        uint32_t dy = abs((int)Y(toplevel) - Y(t));
        if(dy < min_val) {
          min = t;
          min_val = dy;
        }
      }
      return min;
    }
    case MWC_LEFT: {
      wl_list_for_each(t, &workspace->floating_toplevels, link) {
        if(t == toplevel || X(t) > X(toplevel)) continue;

        uint32_t dx = abs((int)X(toplevel) - X(t));
        if(dx < min_val) {
          min = t;
          min_val = dx;
        }
      }
      return min;
    }
    case MWC_RIGHT: {
      wl_list_for_each(t, &workspace->floating_toplevels, link) {
        if(t == toplevel || X(t) < X(toplevel)) continue;

        uint32_t dx = abs((int)X(toplevel) - X(t));
        if(dx < min_val) {
          min = t;
          min_val = dx;
        }
      }
      return min;
    }
  }
}

struct mwc_output *
toplevel_get_primary_output(struct mwc_toplevel *toplevel) {
  struct wlr_box intersection_box;
  struct wlr_box output_box;
  uint32_t max_area = 0;
  struct mwc_output *max_area_output = NULL;

  struct mwc_output *o;
  wl_list_for_each(o, &server.outputs, link) {
    wlr_output_layout_get_box(server.output_layout, o->wlr_output, &output_box);
    bool intersects =
      wlr_box_intersection(&intersection_box, &toplevel->current, &output_box);
    if(intersects && box_area(&intersection_box) > max_area) {
      max_area = box_area(&intersection_box);
      max_area_output = o;
    }
  }

  return max_area_output;
}

void
toplevel_get_actual_size(struct mwc_toplevel *toplevel, uint32_t *width, uint32_t *height) {
  *width = toplevel->animation.running
    ? toplevel->animation.current.width
    : toplevel->current.width;

  *height = toplevel->animation.running
    ? toplevel->animation.current.height
    : toplevel->current.height;
}

uint32_t
toplevel_get_closest_corner(struct wlr_cursor *cursor,
                            struct mwc_toplevel *toplevel) {
  uint32_t toplevel_x = X(toplevel);
  uint32_t toplevel_y = Y(toplevel);

  uint32_t left_dist = cursor->x - toplevel_x;
  uint32_t right_dist = toplevel->current.width - left_dist;
  uint32_t top_dist = cursor->y - toplevel_y;
  uint32_t bottom_dist = toplevel->current.height - top_dist;

  uint32_t edges = 0;
  if(left_dist <= right_dist) {
    edges |= WLR_EDGE_LEFT;
  } else {
    edges |= WLR_EDGE_RIGHT;
  }

  if(top_dist <= bottom_dist) {
    edges |= WLR_EDGE_TOP;
  } else {
    edges |= WLR_EDGE_BOTTOM;
  }

  return edges;
}

void
toplevel_tiled_insert_into_layout(struct mwc_toplevel *toplevel, uint32_t x, uint32_t y) {
  struct mwc_workspace *workspace = server.active_workspace;

  toplevel->workspace = workspace;

  struct mwc_toplevel *under_cursor = layout_toplevel_at(workspace, x, y);

  if(under_cursor == NULL) {
    if(wl_list_length(&workspace->masters) < server.config->master_count) {
      wl_list_insert(workspace->masters.prev, &toplevel->link);
    } else {
      wl_list_insert(workspace->slaves.prev, &toplevel->link);
    }
  } else {
    bool on_left_side = x <= under_cursor->current.x + under_cursor->current.width / 2;
    bool on_top_side = y <= under_cursor->current.y + under_cursor->current.height / 2;
    bool under_cursor_is_master = toplevel_is_master(under_cursor);

    /* we insert it before under_cursor if either:
     *   - its last master and there are some slaves
     *   - cursor is on left (top) */
    if((under_cursor_is_master && &under_cursor->link == workspace->masters.prev
       && wl_list_length(&workspace->slaves) > 0)
       || (under_cursor_is_master && on_left_side)
       || (!under_cursor_is_master && on_top_side)) {
      wl_list_insert(under_cursor->link.prev, &toplevel->link);
    } else {
      wl_list_insert(&under_cursor->link, &toplevel->link);
    }

    if(wl_list_length(&workspace->masters) > server.config->master_count) {
      struct mwc_toplevel *last = wl_container_of(workspace->masters.prev, last, link);
      wl_list_remove(&last->link);
      wl_list_insert(workspace->slaves.prev, &last->link);
    }
  }
}

void
xdg_activation_handle_token_destroy(struct wl_listener *listener, void *data) {
	struct mwc_token *token_data = wl_container_of(listener, token_data, destroy);
	wl_list_remove(&token_data->destroy.link);

	free(token_data);
}

void
xdg_activation_handle_new_token(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_token_v1 *wlr_token = data;
  if(wlr_token->surface == NULL || wlr_token->seat == NULL) return;

	struct mwc_token *token = calloc(1, sizeof(*token));
  token->wlr_token = wlr_token;
	wlr_token->data = token;

	token->destroy.notify = xdg_activation_handle_token_destroy;
	wl_signal_add(&wlr_token->events.destroy, &token->destroy);
}

void
xdg_activation_handle_request(struct wl_listener *listener, void *data) {
	const struct wlr_xdg_activation_v1_request_activate_event *event = data;

	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(event->surface);
	if(xdg_surface == NULL) return;

	struct wlr_scene_tree *tree = xdg_surface->data;
  /* this happens if the toplevel has not been mapped yet. anyway it does not make sense to
   * request that i activate this surface that is not on the screen */
  if(tree == NULL) return;

  struct mwc_something *something = tree->node.data;
  if(something == NULL) return;

  if(something->type == MWC_POPUP) {
    something = popup_get_root_parent(something->popup);
  }

  if(something->type != MWC_TOPLEVEL) return;

  struct mwc_toplevel *toplevel = something->toplevel;

  focus_toplevel(toplevel);
}
