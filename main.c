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
#define Y_OFF_START 4
#define Y_OFF_TITLES 2
#define Y_OFF_FOOTER 2

// Color Scheme
#define COLOR_TITLE (TB_YELLOW | TB_BOLD)
#define COLOR_SELECTED TB_BLUE
#define COLOR_DEFAULT TB_WHITE
#define COLOR_BG TB_BLACK
#define COLOR_DIM TB_WHITE
#define COLOR_SUCCESS TB_GREEN
#define COLOR_ERROR TB_RED

typedef enum {
	MODE_CATEGORIES,
	MODE_MIMETYPES
} AppMode;

typedef struct {
	char *name;
	char **mimetypes;
} DynamicCategory;

typedef struct {
	int category_idx;
	int app_idx;
	int category_offset;
	int app_offset;
	int col; // 0 for category/mimetype, 1 for app
	char message[512];
	GList *cached_apps;
	int is_dev_mode;
	int dev_count;

	AppMode mode;
	char search_query[128];
	int is_searching;
	int is_adding;
	char input_buffer[128];

	GPtrArray *categories; // Array of DynamicCategory*
	GPtrArray *all_mimetypes; // Array of char* (for MODE_MIMETYPES)

	GPtrArray *filtered_list; // Array of char* or DynamicCategory* depending on mode
	GHashTable *mime_to_ext; // char* mime -> char* ext
} State;

void load_mime_types_map(State *state) {
	state->mime_to_ext = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	const char *paths[] = {"/etc/mime.types", "/usr/share/mime/globs2", "/usr/share/mime/globs", NULL};
	for (int p = 0; paths[p]; p++) {
		FILE *f = fopen(paths[p], "r");
		if (!f) continue;

		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			if (line[0] == '#') continue;
			line[strcspn(line, "\n")] = 0;

			if (strcmp(paths[p], "/etc/mime.types") == 0) {
				char *mime = strtok(line, " \t");
				if (!mime) continue;
				char *ext = strtok(NULL, " \t");
				if (ext && !g_hash_table_contains(state->mime_to_ext, mime)) {
					g_hash_table_insert(state->mime_to_ext, g_strdup(mime), g_strdup(ext));
				}
			} else if (strcmp(paths[p], "/usr/share/mime/globs2") == 0) {
				// priority:mimetype:*.ext
				char *prio = strtok(line, ":");
				char *mime = strtok(NULL, ":");
				char *pattern = strtok(NULL, ":");
				if (mime && pattern && pattern[0] == '*' && pattern[1] == '.') {
					if (!g_hash_table_contains(state->mime_to_ext, mime)) {
						g_hash_table_insert(state->mime_to_ext, g_strdup(mime), g_strdup(pattern + 2));
					}
				}
			} else if (strcmp(paths[p], "/usr/share/mime/globs") == 0) {
				// mimetype:*.ext
				char *mime = strtok(line, ":");
				char *pattern = strtok(NULL, ":");
				if (mime && pattern && pattern[0] == '*' && pattern[1] == '.') {
					if (!g_hash_table_contains(state->mime_to_ext, mime)) {
						g_hash_table_insert(state->mime_to_ext, g_strdup(mime), g_strdup(pattern + 2));
					}
				}
			}
		}
		fclose(f);
	}

	// Add some manual ones if not found or for schemes
	struct { const char *m; const char *e; } manual[] = {
		{"x-scheme-handler/http", "http"},
		{"x-scheme-handler/https", "https"},
		{"x-scheme-handler/mailto", "mailto"},
		{"x-scheme-handler/magnet", "magnet"},
		{"inode/directory", "folder"},
		{NULL, NULL}
	};
	for (int i = 0; manual[i].m; i++) {
		if (!g_hash_table_contains(state->mime_to_ext, manual[i].m)) {
			g_hash_table_insert(state->mime_to_ext, g_strdup(manual[i].m), g_strdup(manual[i].e));
		}
	}
}

const char *get_display_name_for_mime(State *state, const char *mime) {
	const char *ext = g_hash_table_lookup(state->mime_to_ext, mime);
	if (ext) return ext;
	return mime;
}

void free_dynamic_category(DynamicCategory *cat) {
	if (!cat) return;
	g_free(cat->name);
	if (cat->mimetypes) {
		for (int i = 0; cat->mimetypes[i] != NULL; i++) {
			g_free(cat->mimetypes[i]);
		}
		g_free(cat->mimetypes);
	}
	g_free(cat);
}

char *get_config_path() {
	const char *config_home = g_get_user_config_dir();
	return g_build_filename(config_home, "xdgctl", "mimetypes.ini", NULL);
}

void save_config(State *state) {
	GKeyFile *kf = g_key_file_new();
	for (int i = 0; i < state->categories->len; i++) {
		DynamicCategory *cat = g_ptr_array_index(state->categories, i);
		g_key_file_set_string_list(kf, "Categories", cat->name, (const char *const *)cat->mimetypes, g_strv_length(cat->mimetypes));
	}

	// Also save the flat list of mimetypes that are NOT in any category, if we want.
	// For now, let's just save all_mimetypes as a separate section to preserve them.
	g_key_file_set_string_list(kf, "Extra", "mimetypes", (const char *const *)state->all_mimetypes->pdata, state->all_mimetypes->len);

	char *path = get_config_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);

	GError *error = NULL;
	g_key_file_save_to_file(kf, path, &error);
	if (error) {
		snprintf(state->message, sizeof(state->message), "Failed to save config: %s", error->message);
		g_error_free(error);
	}

	g_free(dir);
	g_free(path);
	g_key_file_unref(kf);
}

void load_config(State *state) {
	char *path = get_config_path();
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_free(path);
		return;
	}

	GKeyFile *kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		// Clear defaults if config exists? Or merge? 
		// User said "ability to add/remove", so config should probably replace or be the source of truth.
		g_ptr_array_set_size(state->categories, 0);
		g_ptr_array_set_size(state->all_mimetypes, 0);

		gsize n_cats;
		char **cat_names = g_key_file_get_keys(kf, "Categories", &n_cats, NULL);
		if (cat_names) {
			for (gsize i = 0; i < n_cats; i++) {
				DynamicCategory *cat = g_new0(DynamicCategory, 1);
				cat->name = g_strdup(cat_names[i]);
				cat->mimetypes = g_key_file_get_string_list(kf, "Categories", cat_names[i], NULL, NULL);
				g_ptr_array_add(state->categories, cat);

				for (int j = 0; cat->mimetypes[j]; j++) {
					int found = 0;
					for (int k = 0; k < state->all_mimetypes->len; k++) {
						if (strcmp(cat->mimetypes[j], (char *)g_ptr_array_index(state->all_mimetypes, k)) == 0) {
							found = 1;
							break;
						}
					}
					if (!found) g_ptr_array_add(state->all_mimetypes, g_strdup(cat->mimetypes[j]));
				}
			}
			g_strfreev(cat_names);
		}

		char **extra = g_key_file_get_string_list(kf, "Extra", "mimetypes", NULL, NULL);
		if (extra) {
			for (int i = 0; extra[i]; i++) {
				int found = 0;
				for (int k = 0; k < state->all_mimetypes->len; k++) {
					if (strcmp(extra[i], (char *)g_ptr_array_index(state->all_mimetypes, k)) == 0) {
						found = 1;
						break;
					}
				}
				if (!found) g_ptr_array_add(state->all_mimetypes, g_strdup(extra[i]));
			}
			g_strfreev(extra);
		}
	}

	g_ptr_array_sort(state->all_mimetypes, (GCompareFunc)strcmp);
	g_key_file_unref(kf);
	g_free(path);
}

void init_data(State *state) {
	load_mime_types_map(state);
	state->categories = g_ptr_array_new_with_free_func((GDestroyNotify)free_dynamic_category);
	state->all_mimetypes = g_ptr_array_new_with_free_func(g_free);
	state->filtered_list = g_ptr_array_new();

	// Load defaults first
	for (int i = 0; categories[i].name != NULL; i++) {
		DynamicCategory *cat = g_new0(DynamicCategory, 1);
		cat->name = g_strdup(categories[i].name);

		int m_count = 0;
		while (categories[i].mimetypes[m_count]) m_count++;

		cat->mimetypes = g_new0(char *, m_count + 1);
		for (int j = 0; j < m_count; j++) {
			cat->mimetypes[j] = g_strdup(categories[i].mimetypes[j]);

			int found = 0;
			for (int k = 0; k < state->all_mimetypes->len; k++) {
				if (strcmp(cat->mimetypes[j], (char *)g_ptr_array_index(state->all_mimetypes, k)) == 0) {
					found = 1;
					break;
				}
			}
			if (!found) g_ptr_array_add(state->all_mimetypes, g_strdup(cat->mimetypes[j]));
		}
		g_ptr_array_add(state->categories, cat);
	}

	load_config(state);
	g_ptr_array_sort(state->all_mimetypes, (GCompareFunc)strcmp);
}

void update_filtered_list(State *state) {
	g_ptr_array_set_size(state->filtered_list, 0);

	if (state->mode == MODE_CATEGORIES) {
		for (int i = 0; i < state->categories->len; i++) {
			DynamicCategory *cat = g_ptr_array_index(state->categories, i);
			if (state->search_query[0] == '\0' || 
					g_str_has_prefix(g_utf8_casefold(cat->name, -1), g_utf8_casefold(state->search_query, -1))) {
				g_ptr_array_add(state->filtered_list, cat);
			}
		}
	} else {
		for (int i = 0; i < state->all_mimetypes->len; i++) {
			char *mime = g_ptr_array_index(state->all_mimetypes, i);
			const char *display = get_display_name_for_mime(state, mime);
			if (state->search_query[0] == '\0' || 
					strstr(g_utf8_casefold(display, -1), g_utf8_casefold(state->search_query, -1)) ||
					strstr(g_utf8_casefold(mime, -1), g_utf8_casefold(state->search_query, -1))) {
				g_ptr_array_add(state->filtered_list, mime);
			}
		}
	}
}

int get_category_count(State *state) {
	if (state->is_dev_mode) {
		return state->dev_count;
	}
	return state->filtered_list->len;
}

const char *get_category_name(State *state, int idx) {
	if (state->is_dev_mode) {
		static char mock_name[64];
		snprintf(mock_name, sizeof(mock_name), "Dev Category %d", idx + 1);
		return mock_name;
	}
	if (idx >= state->filtered_list->len) return "";

	if (state->mode == MODE_CATEGORIES) {
		DynamicCategory *cat = g_ptr_array_index(state->filtered_list, idx);
		return cat->name;
	} else {
		return get_display_name_for_mime(state, (const char *)g_ptr_array_index(state->filtered_list, idx));
	}
}

GList *get_apps_for_category(State *state, int category_idx) {
	if (category_idx >= state->filtered_list->len) return NULL;
	GList *apps = NULL;

	if (state->mode == MODE_CATEGORIES) {
		DynamicCategory *cat = g_ptr_array_index(state->filtered_list, category_idx);
		for (int m = 0; cat->mimetypes[m] != NULL; ++m) {
			GList *type_apps = g_app_info_get_all_for_type(cat->mimetypes[m]);
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
	} else {
		const char *mime = g_ptr_array_index(state->filtered_list, category_idx);
		GList *type_apps = g_app_info_get_all_for_type(mime);
		for (GList *l = type_apps; l != NULL; l = l->next) {
			apps = g_list_append(apps, g_object_ref((GAppInfo *)l->data));
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

void draw_titles(State *state) {
	const char *left_title = (state->mode == MODE_CATEGORIES) ? "CATEGORIES (1)" : "EXTENSIONS (2)";
	tb_print(X_OFF_CATEGORIES, Y_OFF_TITLES, COLOR_TITLE, COLOR_BG, left_title);
	tb_print(X_OFF_APPS, Y_OFF_TITLES, COLOR_TITLE, COLOR_BG, "APPLICATIONS");
	tb_print(X_OFF_FILE, Y_OFF_TITLES, COLOR_TITLE, COLOR_BG, "FILE");

	if (state->is_searching) {
		char search_str[160];
		snprintf(search_str, sizeof(search_str), "Search: %s_", state->search_query);
		tb_print(X_OFF_CATEGORIES, 0, TB_CYAN | TB_BOLD, COLOR_BG, search_str);
	} else if (state->is_adding) {
		char add_str[160];
		snprintf(add_str, sizeof(add_str), "Add %s: %s_", 
				(state->mode == MODE_CATEGORIES) ? "Mime/Ext to Category" : "Mimetype or Extension", 
				state->input_buffer);
		tb_print(X_OFF_CATEGORIES, 0, TB_MAGENTA | TB_BOLD, COLOR_BG, add_str);
	} else {
		tb_print(X_OFF_CATEGORIES, 0, COLOR_DIM, COLOR_BG, "1:Categories 2:Ext /:Search a:Add d:Delete");
	}
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

	if (state->category_idx >= state->filtered_list->len) return;

	GList *defaults = NULL;
	if (state->mode == MODE_CATEGORIES) {
		DynamicCategory *cat = g_ptr_array_index(state->filtered_list, state->category_idx);
		for (int m = 0; cat->mimetypes[m] != NULL; ++m) {
			GAppInfo *d = g_app_info_get_default_for_type(cat->mimetypes[m], FALSE);
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
	} else {
		const char *mime = g_ptr_array_index(state->filtered_list, state->category_idx);
		GAppInfo *d = g_app_info_get_default_for_type(mime, FALSE);
		if (d) defaults = g_list_append(defaults, d);
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
	draw_titles(state);
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

	State state = {0};
	state.mode = MODE_CATEGORIES;
	init_data(&state);
	update_filtered_list(&state);

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
			if (state.is_searching || state.is_adding) {
				char *buf = state.is_searching ? state.search_query : state.input_buffer;
				int buf_size = state.is_searching ? sizeof(state.search_query) : sizeof(state.input_buffer);

				if (ev.key == TB_KEY_ESC) {
					state.is_searching = state.is_adding = 0;
				} else if (ev.key == TB_KEY_ENTER) {
					if (state.is_adding && state.input_buffer[0] != '\0') {
						char *resolved_mime = NULL;
						if (strchr(state.input_buffer, '/')) {
							resolved_mime = g_strdup(state.input_buffer);
						} else {
							// Try to resolve extension
							char *dot_ext = state.input_buffer[0] == '.' ? g_strdup(state.input_buffer) : g_strdup_printf(".%s", state.input_buffer);
							char *content_type = g_content_type_guess(dot_ext, NULL, 0, NULL);
							if (content_type) {
								resolved_mime = g_content_type_get_mime_type(content_type);
								g_free(content_type);
							}
							g_free(dot_ext);
						}

						if (!resolved_mime) resolved_mime = g_strdup(state.input_buffer);

						if (state.mode == MODE_CATEGORIES) {
							DynamicCategory *cat = g_ptr_array_index(state.filtered_list, state.category_idx);
							int len = g_strv_length(cat->mimetypes);
							cat->mimetypes = g_realloc_n(cat->mimetypes, len + 2, sizeof(char *));
							cat->mimetypes[len] = resolved_mime;
							cat->mimetypes[len + 1] = NULL;
						} else {
							int found = 0;
							for (int i = 0; i < state.all_mimetypes->len; i++) {
								if (strcmp(resolved_mime, (char *)g_ptr_array_index(state.all_mimetypes, i)) == 0) {
									found = 1;
									break;
								}
							}
							if (!found) {
								g_ptr_array_add(state.all_mimetypes, resolved_mime);
								g_ptr_array_sort(state.all_mimetypes, (GCompareFunc)strcmp);
							} else {
								g_free(resolved_mime);
							}
						}
						snprintf(state.message, sizeof(state.message), "Added: %s", state.input_buffer);
						save_config(&state);
						update_filtered_list(&state);
						update_cached_apps(&state);
					}
					state.is_searching = state.is_adding = 0;
				} else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
					int len = strlen(buf);
					if (len > 0) buf[len - 1] = '\0';
					if (state.is_searching) {
						update_filtered_list(&state);
						state.category_idx = 0;
						update_cached_apps(&state);
					}
				} else if (ev.ch) {
					int len = strlen(buf);
					if (len < buf_size - 1) {
						buf[len] = ev.ch;
						buf[len + 1] = '\0';
						if (state.is_searching) {
							update_filtered_list(&state);
							state.category_idx = 0;
							update_cached_apps(&state);
						}
					}
				}
				continue;
			}

			if (ev.key == TB_KEY_ESC || ev.ch == 'q') {
				break;
			}

			if (ev.ch == '1') {
				state.mode = MODE_CATEGORIES;
				state.category_idx = 0;
				state.search_query[0] = '\0';
				update_filtered_list(&state);
				update_cached_apps(&state);
			} else if (ev.ch == '2') {
				state.mode = MODE_MIMETYPES;
				state.category_idx = 0;
				state.search_query[0] = '\0';
				update_filtered_list(&state);
				update_cached_apps(&state);
			} else if (ev.ch == '/') {
				state.is_searching = 1;
				state.search_query[0] = '\0';
			} else if (ev.ch == 'a') {
				state.is_adding = 1;
				state.input_buffer[0] = '\0';
			} else if (ev.ch == 'd') {
				if (state.category_idx < state.filtered_list->len) {
					if (state.mode == MODE_CATEGORIES) {
						// For categories, "Delete" could mean delete the whole category or we need a way to delete a mimetype.
						// Given the request "ability to add/remove more specific mime entries", 
						// maybe we need a way to select a mimetype within a category to delete it.
						// BUT, for now, let's keep it simple: in MODE_CATEGORIES, 'd' deletes the category?
						// Actually, the user wants to add/remove specific mime entries.
						// In MODE_MIMETYPES, 'd' will remove it from all_mimetypes.
						// Let's implement that first.
					} else {
						char *mime = g_ptr_array_index(state.filtered_list, state.category_idx);
						snprintf(state.message, sizeof(state.message), "Deleted: %s", mime);
						// Find it in all_mimetypes and remove it
						for (int i = 0; i < state.all_mimetypes->len; i++) {
							if (strcmp(mime, (char *)g_ptr_array_index(state.all_mimetypes, i)) == 0) {
								g_ptr_array_remove_index(state.all_mimetypes, i);
								break;
							}
						}
						save_config(&state);
						update_filtered_list(&state);
						if (state.category_idx >= state.filtered_list->len && state.category_idx > 0) {
							state.category_idx--;
						}
						update_cached_apps(&state);
					}
				}
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
							if (state.mode == MODE_CATEGORIES) {
								DynamicCategory *cat = g_ptr_array_index(state.filtered_list, state.category_idx);
								snprintf(state.message, sizeof(state.message),
										"%s is now default for %s",
										g_app_info_get_name(selected_app), cat->name);
								for (int m = 0; cat->mimetypes[m] != NULL; ++m) {
									GError *error = NULL;
									g_app_info_set_as_default_for_type(selected_app, cat->mimetypes[m], &error);
									if (error) {
										snprintf(state.message, sizeof(state.message), "Failed to set default application");
										g_error_free(error);
										break;
									}
								}
							} else {
								const char *mime = g_ptr_array_index(state.filtered_list, state.category_idx);
								snprintf(state.message, sizeof(state.message),
										"%s is now default for %s",
										g_app_info_get_name(selected_app), mime);
								GError *error = NULL;
								g_app_info_set_as_default_for_type(selected_app, mime, &error);
								if (error) {
									snprintf(state.message, sizeof(state.message), "Failed to set default application");
									g_error_free(error);
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

	g_ptr_array_unref(state.categories);
	g_ptr_array_unref(state.all_mimetypes);
	g_ptr_array_unref(state.filtered_list);
	g_hash_table_unref(state.mime_to_ext);

	tb_shutdown();
	return 0;
}
