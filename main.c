#define TB_IMPL
#include "termbox2.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mimetypes.h"

// Configuration Constants
#define COL_WIDTH_CATEGORIES 22
#define COL_WIDTH_APPS 40
#define X_OFF_CATEGORIES 2
#define X_OFF_APPS (X_OFF_CATEGORIES + COL_WIDTH_CATEGORIES)
#define X_OFF_FILE (X_OFF_APPS + COL_WIDTH_APPS + 2)
#define Y_OFF_START 3
#define Y_OFF_TITLES 1
#define Y_OFF_FOOTER 2

// Color Scheme
#define COLOR_TITLE (TB_YELLOW | TB_BOLD)
#define COLOR_SELECTED TB_BLUE
#define COLOR_DEFAULT TB_WHITE
#define COLOR_BG TB_BLACK
#define COLOR_DIM TB_WHITE
#define COLOR_SUCCESS TB_GREEN
#define COLOR_ERROR TB_RED

typedef struct {
	int category_idx;
	int app_idx;
	int category_offset;
	int app_offset;
	int col; // 0 for category, 1 for app
	char message[512];
	GList *cached_apps;
	int is_dev_mode;
	int dev_count;
} State;

int get_category_count(State *state) {
	if (state->is_dev_mode) {
		return state->dev_count;
	}
	int count = 0;
	while (categories[count].name)
		count++;
	return count;
}

const char *get_category_name(State *state, int idx) {
	if (state->is_dev_mode) {
		static char mock_name[64];
		snprintf(mock_name, sizeof(mock_name), "Dev Category %d", idx + 1);
		return mock_name;
	}
	return categories[idx].name;
}

GList *get_apps_for_category(State *state, int category_idx) {
	GList *apps = NULL;
	for (int m = 0; categories[category_idx].mimetypes[m] != NULL; ++m) {
		GList *type_apps = g_app_info_get_all_for_type(categories[category_idx].mimetypes[m]);
		for (GList *l = type_apps; l != NULL; l = l->next) {
			GAppInfo *app = (GAppInfo *)l->data;
			int found = 0;
			for (GList *a = apps; a != NULL; a = a->next) {
				if (g_app_info_equal(app, (GAppInfo *)a->data)) {
					found = 1;
					break;
				}
			}
			if (!found) {
				apps = g_list_append(apps, g_object_ref(app));
			}
		}
		g_list_free_full(type_apps, g_object_unref);
	}
	return apps;
}

void update_cached_apps(State *state) {
	if (state->cached_apps && !state->is_dev_mode) {
		g_list_free_full(state->cached_apps, g_object_unref);
	} else if (state->cached_apps && state->is_dev_mode) {
		g_list_free_full(state->cached_apps, g_free);
	}
	state->cached_apps = NULL;

	if (state->is_dev_mode) {
		for (int i = 0; i < state->dev_count; ++i) {
			char *mock_app = g_strdup_printf("Dev Application %d.%d", state->category_idx + 1, i + 1);
			state->cached_apps = g_list_append(state->cached_apps, mock_app);
		}
	} else {
		state->cached_apps = get_apps_for_category(state, state->category_idx);
	}
	state->app_idx = 0;
}

void draw_titles() {
	tb_print(X_OFF_CATEGORIES, Y_OFF_TITLES, COLOR_TITLE, COLOR_BG, "CATEGORIES");
	tb_print(X_OFF_APPS, Y_OFF_TITLES, COLOR_TITLE, COLOR_BG, "APPLICATIONS");
	tb_print(X_OFF_FILE, Y_OFF_TITLES, COLOR_TITLE, COLOR_BG, "FILE");
}

void draw_categories(State *state) {
	int count = get_category_count(state);
	int height = tb_height() - Y_OFF_START - Y_OFF_FOOTER;
	for (int i = 0; i < height && (i + state->category_offset) < count; ++i) {
		int idx = i + state->category_offset;
		uint16_t fg = COLOR_DEFAULT;
		uint16_t bg = COLOR_BG;
		if (state->col == 0 && state->category_idx == idx) {
			bg = COLOR_SELECTED;
		} else if (state->category_idx == idx) {
			fg = COLOR_SELECTED;
			bg = COLOR_DEFAULT;
		}
		tb_print(X_OFF_CATEGORIES, Y_OFF_START + i, fg, bg, get_category_name(state, idx));
	}
}

void draw_apps_list(State *state) {
	int height = tb_height() - Y_OFF_START - Y_OFF_FOOTER;
	if (state->is_dev_mode) {
		int i = 0;
		GList *l = g_list_nth(state->cached_apps, state->app_offset);
		for (; l != NULL && i < height; l = l->next, ++i) {
			int idx = i + state->app_offset;
			char *app_name = (char *)l->data;
			uint16_t fg = COLOR_DEFAULT;
			uint16_t bg = COLOR_BG;
			if (state->col == 1 && state->app_idx == idx) {
				bg = COLOR_SELECTED;
			}
			char name[256];
			snprintf(name, sizeof(name), "  %s", app_name);
			tb_print(X_OFF_APPS, Y_OFF_START + i, fg, bg, name);
		}
		return;
	}
	GList *defaults = NULL;
	for (int m = 0; categories[state->category_idx].mimetypes[m] != NULL; ++m) {
		GAppInfo *d = g_app_info_get_default_for_type(categories[state->category_idx].mimetypes[m], FALSE);
		if (d) {
			int found = 0;
			for (GList *l = defaults; l != NULL; l = l->next) {
				if (g_app_info_equal(d, (GAppInfo *)l->data)) {
					found = 1;
					break;
				}
			}
			if (!found) {
				defaults = g_list_append(defaults, d);
			} else {
				g_object_unref(d);
			}
		}
	}

	int i = 0;
	GList *l = g_list_nth(state->cached_apps, state->app_offset);
	for (; l != NULL && i < height; l = l->next, ++i) {
		int idx = i + state->app_offset;
		GAppInfo *app = (GAppInfo *)l->data;
		uint16_t fg = COLOR_DEFAULT;
		uint16_t bg = COLOR_BG;
		if (state->col == 1 && state->app_idx == idx) {
			bg = COLOR_SELECTED;
		}

		int is_default = 0;
		for (GList *d = defaults; d != NULL; d = d->next) {
			if (g_app_info_equal(app, (GAppInfo *)d->data)) {
				is_default = 1;
				break;
			}
		}

		char name[256];
		snprintf(name, sizeof(name), "%s %s", is_default ? "*" : " ", g_app_info_get_name(app));
		tb_print(X_OFF_APPS, Y_OFF_START + i, fg, bg, name);

		if (G_IS_DESKTOP_APP_INFO(app)) {
			const char *filename = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
			if (filename) {
				tb_print(X_OFF_FILE, Y_OFF_START + i, COLOR_DIM, bg, filename);
			}
		}
	}
	g_list_free_full(defaults, g_object_unref);
}

void draw(State *state) {
	tb_clear();
	draw_titles();
	draw_categories(state);
	draw_apps_list(state);

	if (state->message[0] != '\0' || state->is_dev_mode) {
		uint16_t msg_col = COLOR_SUCCESS;
		const char *msg = state->message;
		if (state->message[0] == '\0' && state->is_dev_mode) {
			msg = "Developer Mode";
		}
		if (strncmp(msg, "Failed", 6) == 0) {
			msg_col = COLOR_ERROR;
		}
		tb_print(X_OFF_CATEGORIES, tb_height() - 1, msg_col, COLOR_BG, msg);
	}

	tb_present();
}

int main() {
	if (tb_init() != 0) {
		return 1;
	}
	tb_set_clear_attrs(COLOR_DEFAULT, COLOR_BG);

	State state = {0, 0, 0, 0, 0, {0}, NULL, 0, 0};
	char *dev_env = getenv("XDGCTL_DEV");
	if (dev_env) {
		state.is_dev_mode = 1;
		state.dev_count = atoi(dev_env);
		if (state.dev_count <= 0)
			state.dev_count = 10;
	}

	update_cached_apps(&state);

	struct tb_event ev;
	while (1) {
		int visible_height = tb_height() - Y_OFF_START - Y_OFF_FOOTER;
		if (state.category_idx < state.category_offset) {
			state.category_offset = state.category_idx;
		} else if (state.category_idx >= state.category_offset + visible_height) {
			state.category_offset = state.category_idx - visible_height + 1;
		}
		if (state.app_idx < state.app_offset) {
			state.app_offset = state.app_idx;
		} else if (state.app_idx >= state.app_offset + visible_height) {
			state.app_offset = state.app_idx - visible_height + 1;
		}

		draw(&state);
		tb_poll_event(&ev);

		if (ev.type == TB_EVENT_KEY) {
			if (ev.key == TB_KEY_ESC || ev.ch == 'q') {
				break;
			}

			if (ev.key == TB_KEY_ARROW_UP) {
				if (state.col == 0) {
					if (state.category_idx > 0) {
						state.category_idx--;
						update_cached_apps(&state);
						state.app_offset = 0;
						state.message[0] = '\0';
					}
				} else {
					if (state.app_idx > 0) {
						state.app_idx--;
						state.message[0] = '\0';
					}
				}
			} else if (ev.key == TB_KEY_ARROW_DOWN) {
				if (state.col == 0) {
					int count = get_category_count(&state);
					if (state.category_idx < count - 1) {
						state.category_idx++;
						update_cached_apps(&state);
						state.app_offset = 0;
						state.message[0] = '\0';
					}
				} else {
					int count = g_list_length(state.cached_apps);
					if (state.app_idx < count - 1) {
						state.app_idx++;
						state.message[0] = '\0';
					}
				}
			} else if (ev.key == TB_KEY_ARROW_RIGHT || ev.key == TB_KEY_TAB) {
				if (state.col == 0) {
					state.col = 1;
					state.app_idx = 0;
					state.app_offset = 0;
				}
			} else if (ev.key == TB_KEY_ARROW_LEFT) {
				if (state.col == 1) {
					state.col = 0;
				}
			} else if (ev.key == TB_KEY_ENTER) {
				if (state.col == 1) {
					if (state.is_dev_mode) {
						char *selected_app = (char *)g_list_nth_data(state.cached_apps, state.app_idx);
						if (selected_app) {
							snprintf(state.message, sizeof(state.message),
									 "Mock: %s is now default for %s",
									 selected_app, get_category_name(&state, state.category_idx));
						}
					} else {
						GAppInfo *selected_app = (GAppInfo *)g_list_nth_data(state.cached_apps, state.app_idx);
						if (selected_app) {
							snprintf(state.message, sizeof(state.message),
									 "%s is now default %s",
									 g_app_info_get_name(selected_app), categories[state.category_idx].name);
							for (int m = 0; categories[state.category_idx].mimetypes[m] != NULL; ++m) {
								GError *error = NULL;
								g_app_info_set_as_default_for_type(selected_app, categories[state.category_idx].mimetypes[m], &error);
								if (error) {
									snprintf(state.message, sizeof(state.message), "Failed to set default application");
									g_error_free(error);
									break;
								}
							}
						}
					}
				}
			}
		}
	}

	if (state.cached_apps && !state.is_dev_mode) {
		g_list_free_full(state.cached_apps, g_object_unref);
	} else if (state.cached_apps && state.is_dev_mode) {
		g_list_free_full(state.cached_apps, g_free);
	}
	tb_shutdown();
	return 0;
}
