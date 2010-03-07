/*
 *      filebrowser.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2007-2010 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2007-2010 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 *
 * $Id$
 */

/* Sidebar file browser plugin. */

#include "geanyplugin.h"
#include <string.h>

#include <gdk/gdkkeysyms.h>


GeanyPlugin		*geany_plugin;
GeanyData		*geany_data;
GeanyFunctions	*geany_functions;


PLUGIN_VERSION_CHECK(163)

PLUGIN_SET_INFO(_("File Browser"), _("Adds a file browser tab to the sidebar."), VERSION,
	_("The Geany developer team"))


/* Keybinding(s) */
enum
{
	KB_FOCUS_FILE_LIST,
	KB_FOCUS_PATH_ENTRY,
	KB_COUNT
};

PLUGIN_KEY_GROUP(file_browser, KB_COUNT)


enum
{
	FILEVIEW_COLUMN_ICON = 0,
	FILEVIEW_COLUMN_NAME,
	FILEVIEW_COLUMN_FILENAME, /* the full filename, including path for display as tooltip */
	FILEVIEW_N_COLUMNS
};

static gboolean fb_set_project_base_path = FALSE;
static gboolean fb_follow_path 	  = FALSE;
static gboolean show_hidden_files = FALSE;
static gboolean hide_object_files = TRUE;

static GtkWidget			*file_view_vbox;
static GtkWidget			*file_view;
static GtkListStore			*file_store;
static GtkTreeIter			*last_dir_iter = NULL;
static GtkEntryCompletion	*entry_completion = NULL;

static GtkWidget	*filter_entry;
static GtkWidget	*path_entry;
static gchar		*current_dir = NULL;	/* in locale-encoding */
static gchar		*open_cmd;				/* in locale-encoding */
static gchar		*config_file;
static gchar 		*filter = NULL;

static gint			 page_number = 0;

static struct
{
	GtkWidget *open;
	GtkWidget *open_external;
	GtkWidget *find_in_files;
} popup_items;


static void project_change_cb(GObject *obj, GKeyFile *config, gpointer data);

PluginCallback plugin_callbacks[] =
{
	{ "project-open", (GCallback) &project_change_cb, TRUE, NULL },
	{ "project-save", (GCallback) &project_change_cb, TRUE, NULL },
	{ NULL, NULL, FALSE, NULL }
};


/* Returns: whether name should be hidden. */
static gboolean check_hidden(const gchar *base_name)
{
	gsize len;

	if (! NZV(base_name))
		return FALSE;

	if (base_name[0] == '.')
		return TRUE;

	len = strlen(base_name);
	if (base_name[len - 1] == '~')
		return TRUE;

	if (hide_object_files)
	{
		const gchar *exts[] = {".o", ".obj", ".so", ".dll", ".a", ".lib"};
		guint i, exts_len;

		exts_len = G_N_ELEMENTS(exts);
		for (i = 0; i < exts_len; i++)
		{
			const gchar *ext = exts[i];

			if (g_str_has_suffix(base_name, ext))
				return TRUE;
		}
	}
	return FALSE;
}


/* Returns: whether filename should be removed. */
static gboolean check_filtered(const gchar *base_name)
{
	if (filter == NULL)
		return FALSE;

	if (! utils_str_equal(base_name, "*") && ! g_pattern_match_simple(filter, base_name))
	{
		return TRUE;
	}
	return FALSE;
}


/* name is in locale encoding */
static void add_item(const gchar *name)
{
	GtkTreeIter iter;
	gchar *fname, *utf8_name, *utf8_fullname, *sep;
	gboolean dir;

	if (! show_hidden_files && check_hidden(name))
		return;

	sep = (utils_str_equal(current_dir, "/")) ? "" : G_DIR_SEPARATOR_S;
	fname = g_strconcat(current_dir, sep, name, NULL);
	dir = g_file_test(fname, G_FILE_TEST_IS_DIR);
	utf8_fullname = utils_get_locale_from_utf8(fname);
	utf8_name = utils_get_utf8_from_locale(name);
	g_free(fname);

	if (dir)
	{
		if (last_dir_iter == NULL)
			gtk_list_store_prepend(file_store, &iter);
		else
		{
			gtk_list_store_insert_after(file_store, &iter, last_dir_iter);
			gtk_tree_iter_free(last_dir_iter);
		}
		last_dir_iter = gtk_tree_iter_copy(&iter);
	}
	else
	{
		if (check_filtered(utf8_name))
		{
			g_free(utf8_name);
			g_free(utf8_fullname);
			return;
		}
		gtk_list_store_append(file_store, &iter);
	}
	gtk_list_store_set(file_store, &iter,
		FILEVIEW_COLUMN_ICON, (dir) ? GTK_STOCK_DIRECTORY : GTK_STOCK_FILE,
		FILEVIEW_COLUMN_NAME, utf8_name,
		FILEVIEW_COLUMN_FILENAME, utf8_fullname,
		-1);
	g_free(utf8_name);
	g_free(utf8_fullname);
}


/* adds ".." to the start of the file list */
static void add_top_level_entry(void)
{
	GtkTreeIter iter;
	gchar *utf8_dir;

	if (! NZV(g_path_skip_root(current_dir)))
		return;	/* ignore 'C:\' or '/' */

	utf8_dir = g_path_get_dirname(current_dir);
	setptr(utf8_dir, utils_get_utf8_from_locale(utf8_dir));

	gtk_list_store_prepend(file_store, &iter);
	last_dir_iter = gtk_tree_iter_copy(&iter);

	gtk_list_store_set(file_store, &iter,
		FILEVIEW_COLUMN_ICON, GTK_STOCK_DIRECTORY,
		FILEVIEW_COLUMN_NAME, "..",
		FILEVIEW_COLUMN_FILENAME, utf8_dir,
		-1);
	g_free(utf8_dir);
}


static void clear(void)
{
	gtk_list_store_clear(file_store);

	/* reset the directory item pointer */
	if (last_dir_iter != NULL)
		gtk_tree_iter_free(last_dir_iter);
	last_dir_iter = NULL;
}


/* Reuses list to free each node, so list must be a variable */
#define foreach_slist_free(node, list) \
	for (node = list, list = NULL; g_slist_free_1(list), node != NULL; list = node, node = node->next)

/* recreate the tree model from current_dir. */
static void refresh(void)
{
	gchar *utf8_dir;
	GSList *list, *node;

	/* don't clear when the new path doesn't exist */
	if (! g_file_test(current_dir, G_FILE_TEST_EXISTS))
		return;

	clear();

	utf8_dir = utils_get_utf8_from_locale(current_dir);
	gtk_entry_set_text(GTK_ENTRY(path_entry), utf8_dir);
	g_free(utf8_dir);

	add_top_level_entry();	/* ".." item */

	list = utils_get_file_list(current_dir, NULL, NULL);
	if (list != NULL)
	{
		/* free filenames & nodes as we go through the list */
		foreach_slist_free(node, list)
		{
			gchar *fname = node->data;

			add_item(fname);
			g_free(fname);
		}
	}
	gtk_entry_completion_set_model(entry_completion, GTK_TREE_MODEL(file_store));
}


static void on_go_home(void)
{
	setptr(current_dir, g_strdup(g_get_home_dir()));
	refresh();
}


/* TODO: use utils_get_default_dir_utf8() */
static gchar *get_default_dir(void)
{
	const gchar *dir = NULL;
	GeanyProject *project = geany->app->project;

	if (project)
		dir = project->base_path;
	else
		dir = geany->prefs->default_open_path;

	if (NZV(dir))
		return utils_get_locale_from_utf8(dir);

	return g_get_current_dir();
}


static void on_current_path(void)
{
	gchar *fname;
	gchar *dir;
	GeanyDocument *doc = document_get_current();

	if (doc == NULL || doc->file_name == NULL || ! g_path_is_absolute(doc->file_name))
	{
		setptr(current_dir, get_default_dir());
		refresh();
		return;
	}
	fname = doc->file_name;
	fname = utils_get_locale_from_utf8(fname);
	dir = g_path_get_dirname(fname);
	g_free(fname);

	setptr(current_dir, dir);
	refresh();
}


static void on_go_up(void)
{
	/* remove the highest directory part (which becomes the basename of current_dir) */
	setptr(current_dir, g_path_get_dirname(current_dir));
	refresh();
}


static gboolean check_single_selection(GtkTreeSelection *treesel)
{
	if (gtk_tree_selection_count_selected_rows(treesel) == 1)
		return TRUE;

	ui_set_statusbar(FALSE, _("Too many items selected!"));
	return FALSE;
}


/* Returns: TRUE if at least one of selected_items is a folder. */
static gboolean is_folder_selected(GList *selected_items)
{
	GList *item;
	GtkTreeModel *model = GTK_TREE_MODEL(file_store);
	gboolean dir_found = FALSE;

	for (item = selected_items; item != NULL; item = g_list_next(item))
	{
		gchar *icon;
		GtkTreeIter iter;
		GtkTreePath *treepath;

		treepath = (GtkTreePath*) item->data;
		gtk_tree_model_get_iter(model, &iter, treepath);
		gtk_tree_model_get(model, &iter, FILEVIEW_COLUMN_ICON, &icon, -1);

		if (utils_str_equal(icon, GTK_STOCK_DIRECTORY))
		{
			dir_found = TRUE;
			g_free(icon);
			break;
		}
		g_free(icon);
	}
	return dir_found;
}


/* Returns: the full filename in locale encoding. */
static gchar *get_tree_path_filename(GtkTreePath *treepath)
{
	GtkTreeModel *model = GTK_TREE_MODEL(file_store);
	GtkTreeIter iter;
	gchar *name, *fname;

	gtk_tree_model_get_iter(model, &iter, treepath);
	gtk_tree_model_get(model, &iter, FILEVIEW_COLUMN_FILENAME, &name, -1);

	fname = utils_get_locale_from_utf8(name);
	g_free(name);

	return fname;
}


static void open_external(const gchar *fname, gboolean dir_found)
{
	gchar *cmd;
	gchar *locale_cmd;
	gchar *dir;
	GString *cmd_str = g_string_new(open_cmd);
	GError *error = NULL;

	if (! dir_found)
		dir = g_path_get_dirname(fname);
	else
		dir = g_strdup(fname);

	utils_string_replace_all(cmd_str, "%f", fname);
	utils_string_replace_all(cmd_str, "%d", dir);

	cmd = g_string_free(cmd_str, FALSE);
	locale_cmd = utils_get_locale_from_utf8(cmd);
	if (! g_spawn_command_line_async(locale_cmd, &error))
	{
		gchar *c = strchr(cmd, ' ');

		if (c != NULL)
			*c = '\0';
		ui_set_statusbar(TRUE,
			_("Could not execute configured external command '%s' (%s)."),
			cmd, error->message);
		g_error_free(error);
	}
	g_free(locale_cmd);
	g_free(cmd);
	g_free(dir);
}


static void on_external_open(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkTreeSelection *treesel;
	GtkTreeModel *model;
	GList *list;
	gboolean dir_found;

	treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_view));

	list = gtk_tree_selection_get_selected_rows(treesel, &model);
	dir_found = is_folder_selected(list);

	if (! dir_found || check_single_selection(treesel))
	{
		GList *item;

		for (item = list; item != NULL; item = g_list_next(item))
		{
			GtkTreePath *treepath = item->data;
			gchar *fname = get_tree_path_filename(treepath);

			open_external(fname, dir_found);
			g_free(fname);
		}
	}

	g_list_foreach(list, (GFunc) gtk_tree_path_free, NULL);
	g_list_free(list);
}


/* We use document_open_files() as it's more efficient. */
static void open_selected_files(GList *list)
{
	GSList *files = NULL;
	GList *item;
	GeanyDocument *doc;

	for (item = list; item != NULL; item = g_list_next(item))
	{
		GtkTreePath *treepath = item->data;
		gchar *fname = get_tree_path_filename(treepath);

		files = g_slist_append(files, fname);
	}
	document_open_files(files, FALSE, NULL, NULL);
	doc = document_get_current();
	if (doc != NULL)
		keybindings_send_command(GEANY_KEY_GROUP_FOCUS, GEANY_KEYS_FOCUS_EDITOR);

	g_slist_foreach(files, (GFunc) g_free, NULL);	/* free filenames */
	g_slist_free(files);
}


static void open_folder(GtkTreePath *treepath)
{
	gchar *fname = get_tree_path_filename(treepath);

	setptr(current_dir, fname);
	refresh();
}


static void on_open_clicked(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkTreeSelection *treesel;
	GtkTreeModel *model;
	GList *list;
	gboolean dir_found;

	treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_view));

	list = gtk_tree_selection_get_selected_rows(treesel, &model);
	dir_found = is_folder_selected(list);

	if (dir_found)
	{
		if (check_single_selection(treesel))
		{
			GtkTreePath *treepath = list->data;	/* first selected item */

			open_folder(treepath);
		}
	}
	else
		open_selected_files(list);

	g_list_foreach(list, (GFunc) gtk_tree_path_free, NULL);
	g_list_free(list);
}


static void on_find_in_files(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkTreeSelection *treesel;
	GtkTreeModel *model;
	GList *list;
	gchar *dir;
	gboolean is_dir = FALSE;

	treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_view));
	if (! check_single_selection(treesel))
		return;

	list = gtk_tree_selection_get_selected_rows(treesel, &model);
	is_dir = is_folder_selected(list);

	if (is_dir)
	{
		GtkTreePath *treepath = list->data;	/* first selected item */

		dir = get_tree_path_filename(treepath);
	}
	else
		dir = g_strdup(current_dir);

	g_list_foreach(list, (GFunc) gtk_tree_path_free, NULL);
	g_list_free(list);

	setptr(dir, utils_get_utf8_from_locale(dir));
	search_show_find_in_files_dialog(dir);
	g_free(dir);
}


static void on_hidden_files_clicked(GtkCheckMenuItem *item)
{
	show_hidden_files = gtk_check_menu_item_get_active(item);
	refresh();
}


static void on_hide_sidebar(void)
{
	keybindings_send_command(GEANY_KEY_GROUP_VIEW, GEANY_KEYS_VIEW_SIDEBAR);
}


static void on_show_preferences(void)
{
	plugin_show_configure(geany_plugin);
}


static GtkWidget *create_popup_menu(void)
{
	GtkWidget *item, *menu;

	menu = gtk_menu_new();

	item = gtk_image_menu_item_new_from_stock(GTK_STOCK_OPEN, NULL);
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_open_clicked), NULL);
	popup_items.open = item;

	item = ui_image_menu_item_new(GTK_STOCK_OPEN, _("Open _externally"));
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_external_open), NULL);
	popup_items.open_external = item;

	item = ui_image_menu_item_new(GTK_STOCK_FIND, _("_Find in Files"));
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_find_in_files), NULL);
	popup_items.find_in_files = item;

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = gtk_check_menu_item_new_with_mnemonic(_("Show _Hidden Files"));
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_hidden_files_clicked), NULL);

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES, NULL);
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_show_preferences), NULL);

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = ui_image_menu_item_new(GTK_STOCK_CLOSE, _("H_ide Sidebar"));
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_hide_sidebar), NULL);

	return menu;
}


static void on_tree_selection_changed(GtkTreeSelection *selection, gpointer data)
{
	gboolean have_sel = (gtk_tree_selection_count_selected_rows(selection) > 0);
	gboolean multi_sel = (gtk_tree_selection_count_selected_rows(selection) > 1);

	if (popup_items.open != NULL)
		gtk_widget_set_sensitive(popup_items.open, have_sel);
	if (popup_items.open_external != NULL)
		gtk_widget_set_sensitive(popup_items.open_external, have_sel);
	if (popup_items.find_in_files != NULL)
		gtk_widget_set_sensitive(popup_items.find_in_files, have_sel && ! multi_sel);
}


static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	if (event->button == 1 && event->type == GDK_2BUTTON_PRESS)
	{
		on_open_clicked(NULL, NULL);
		return TRUE;
	}
	else if (event->button == 3)
	{
		static GtkWidget *popup_menu = NULL;

		if (popup_menu == NULL)
			popup_menu = create_popup_menu();

		gtk_menu_popup(GTK_MENU(popup_menu), NULL, NULL, NULL, NULL, event->button, event->time);
		/* don't return TRUE here, unless the selection won't be changed */
	}
	return FALSE;
}


static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	if (event->keyval == GDK_Return
		|| event->keyval == GDK_ISO_Enter
		|| event->keyval == GDK_KP_Enter
		|| event->keyval == GDK_space)
	{
		on_open_clicked(NULL, NULL);
		return TRUE;
	}

	if ((event->keyval == GDK_Up ||
		event->keyval == GDK_KP_Up) &&
		(event->state & GDK_MOD1_MASK))	/* FIXME: Alt-Up doesn't seem to work! */
	{
		on_go_up();
		return TRUE;
	}

	if ((event->keyval == GDK_F10 && event->state & GDK_SHIFT_MASK) || event->keyval == GDK_Menu)
	{
		GdkEventButton button_event;

		button_event.time = event->time;
		button_event.button = 3;

		on_button_press(widget, &button_event, data);
		return TRUE;
	}

	return FALSE;
}


static void on_clear_filter(GtkEntry *entry, gpointer user_data)
{
	setptr(filter, NULL);

	gtk_entry_set_text(GTK_ENTRY(filter_entry), "");

	refresh();
}


static void on_path_entry_activate(GtkEntry *entry, gpointer user_data)
{
	gchar *new_dir = (gchar*) gtk_entry_get_text(entry);

	if (NZV(new_dir))
	{
		if (g_str_has_suffix(new_dir, ".."))
		{
			on_go_up();
			return;
		}
		else if (new_dir[0] == '~')
		{
			GString *str = g_string_new(new_dir);
			utils_string_replace_first(str, "~", g_get_home_dir());
			new_dir = g_string_free(str, FALSE);
		}
		else
			new_dir = utils_get_locale_from_utf8(new_dir);
	}
	else
		new_dir = g_strdup(g_get_home_dir());

	setptr(current_dir, new_dir);

	on_clear_filter(NULL, NULL);
}


static void on_filter_activate(GtkEntry *entry, gpointer user_data)
{
	setptr(filter, g_strdup(gtk_entry_get_text(entry)));

	if (! NZV(filter))
	{
		setptr(filter, g_strdup("*"));
	}

	refresh();
}


static void on_filter_clear(GtkEntry *entry, gint icon_pos,
							GdkEvent *event, gpointer data)
{
	setptr(filter, g_strdup("*"));

	refresh();
}


static void prepare_file_view(void)
{
	GtkCellRenderer *text_renderer, *icon_renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;

	file_store = gtk_list_store_new(FILEVIEW_N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

	gtk_tree_view_set_model(GTK_TREE_VIEW(file_view), GTK_TREE_MODEL(file_store));
	g_object_unref(file_store);

	icon_renderer = gtk_cell_renderer_pixbuf_new();
	text_renderer = gtk_cell_renderer_text_new();
	column = gtk_tree_view_column_new();
	gtk_tree_view_column_pack_start(column, icon_renderer, FALSE);
	gtk_tree_view_column_set_attributes(column, icon_renderer, "stock-id", FILEVIEW_COLUMN_ICON, NULL);
	gtk_tree_view_column_pack_start(column, text_renderer, TRUE);
	gtk_tree_view_column_set_attributes(column, text_renderer, "text", FILEVIEW_COLUMN_NAME, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(file_view), column);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(file_view), FALSE);

	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(file_view), TRUE);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(file_view), FILEVIEW_COLUMN_NAME);

	ui_widget_modify_font_from_string(file_view, geany->interface_prefs->tagbar_font);

	/* GTK 2.12 tooltips */
	if (gtk_check_version(2, 12, 0) == NULL)
		g_object_set(file_view, "has-tooltip", TRUE, "tooltip-column", FILEVIEW_COLUMN_FILENAME, NULL);

	/* selection handling */
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_view));
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

	g_signal_connect(file_view, "realize", G_CALLBACK(on_current_path), NULL);
	g_signal_connect(selection, "changed", G_CALLBACK(on_tree_selection_changed), NULL);
	g_signal_connect(file_view, "button-press-event", G_CALLBACK(on_button_press), NULL);
	g_signal_connect(file_view, "key-press-event", G_CALLBACK(on_key_press), NULL);
}


static GtkWidget *make_toolbar(void)
{
	GtkWidget *wid, *toolbar;

	toolbar = gtk_toolbar_new();
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_MENU);
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_GO_UP));
	ui_widget_set_tooltip_text(wid, _("Up"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_go_up), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_REFRESH));
	ui_widget_set_tooltip_text(wid, _("Refresh"));
	g_signal_connect(wid, "clicked", G_CALLBACK(refresh), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_HOME));
	ui_widget_set_tooltip_text(wid, _("Home"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_go_home), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_JUMP_TO));
	ui_widget_set_tooltip_text(wid, _("Set path from document"));
	g_signal_connect(wid, "clicked", G_CALLBACK(on_current_path), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);

	if (gtk_check_version(2, 15, 2) != NULL)
	{
		wid = GTK_WIDGET(gtk_separator_tool_item_new());
		gtk_container_add(GTK_CONTAINER(toolbar), wid);

		wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_CLEAR));
		ui_widget_set_tooltip_text(wid, _("Clear the filter"));
		g_signal_connect(wid, "clicked", G_CALLBACK(on_clear_filter), NULL);
		gtk_container_add(GTK_CONTAINER(toolbar), wid);
	}
	return toolbar;
}


static GtkWidget *make_filterbar(void)
{
	GtkWidget *label, *filterbar;

	filterbar = gtk_hbox_new(FALSE, 1);

	label = gtk_label_new(_("Filter:"));

	filter_entry = gtk_entry_new();

	if (gtk_check_version(2, 15, 2) == NULL)
	{
		ui_entry_add_clear_icon(GTK_ENTRY(filter_entry));
		g_signal_connect(filter_entry, "icon-release", G_CALLBACK(on_filter_clear), NULL);
	}
	ui_widget_set_tooltip_text(filter_entry,
		_("Filter your files with usual wildcards"));
	g_signal_connect(filter_entry, "activate", G_CALLBACK(on_filter_activate), NULL);

	gtk_box_pack_start(GTK_BOX(filterbar), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(filterbar), filter_entry, TRUE, TRUE, 0);

	return filterbar;
}


static gboolean completion_match_func(GtkEntryCompletion *completion, const gchar *key,
									  GtkTreeIter *iter, gpointer user_data)
{
	gchar *str, *icon;
	gboolean result = FALSE;

	gtk_tree_model_get(GTK_TREE_MODEL(file_store), iter,
		FILEVIEW_COLUMN_ICON, &icon, FILEVIEW_COLUMN_NAME, &str, -1);

	if (str != NULL && icon != NULL && utils_str_equal(icon, GTK_STOCK_DIRECTORY) &&
		! g_str_has_suffix(key, G_DIR_SEPARATOR_S))
	{
		/* key is something like "/tmp/te" and str is a filename like "test",
		 * so strip the path from key to make them comparable */
		gchar *base_name = g_path_get_basename(key);
		gchar *str_lowered = g_utf8_strdown(str, -1);
		result = g_str_has_prefix(str_lowered, base_name);
		g_free(base_name);
		g_free(str_lowered);
	}
	g_free(str);
	g_free(icon);

	return result;
}


static gboolean completion_match_selected(GtkEntryCompletion *widget, GtkTreeModel *model,
										  GtkTreeIter *iter, gpointer user_data)
{
	gchar *str;
	gtk_tree_model_get(model, iter, FILEVIEW_COLUMN_NAME, &str, -1);
	if (str != NULL)
	{
		gchar *text = g_strconcat(current_dir, G_DIR_SEPARATOR_S, str, NULL);
		gtk_entry_set_text(GTK_ENTRY(path_entry), text);
		gtk_editable_set_position(GTK_EDITABLE(path_entry), -1);
		/* force change of directory when completion is done */
		on_path_entry_activate(GTK_ENTRY(path_entry), NULL);
		g_free(text);
	}
	g_free(str);

	return TRUE;
}


static void completion_create(void)
{
	entry_completion = gtk_entry_completion_new();

	gtk_entry_completion_set_inline_completion(entry_completion, FALSE);
	gtk_entry_completion_set_popup_completion(entry_completion, TRUE);
	gtk_entry_completion_set_text_column(entry_completion, FILEVIEW_COLUMN_NAME);
	gtk_entry_completion_set_match_func(entry_completion, completion_match_func, NULL, NULL);

	g_signal_connect(entry_completion, "match-selected",
		G_CALLBACK(completion_match_selected), NULL);

	gtk_entry_set_completion(GTK_ENTRY(path_entry), entry_completion);
}


#define CHECK_READ_SETTING(var, error, tmp) \
	if ((error) != NULL) \
	{ \
		g_error_free((error)); \
		(error) = NULL; \
	} \
	else \
		(var) = (tmp);

static void load_settings(void)
{
	GKeyFile *config = g_key_file_new();
	GError *error = NULL;
	gboolean tmp;

	config_file = g_strconcat(geany->app->configdir, G_DIR_SEPARATOR_S, "plugins", G_DIR_SEPARATOR_S,
		"filebrowser", G_DIR_SEPARATOR_S, "filebrowser.conf", NULL);
	g_key_file_load_from_file(config, config_file, G_KEY_FILE_NONE, NULL);
	open_cmd = g_key_file_get_string(config, "filebrowser", "open_command", &error);
	if (error != NULL)
	{
		open_cmd = g_strdup("nautilus \"%d\"");
		g_error_free(error);
		error = NULL;
	}
	tmp = g_key_file_get_boolean(config, "filebrowser", "show_hidden_files", &error);
	CHECK_READ_SETTING(show_hidden_files, error, tmp);
	tmp = g_key_file_get_boolean(config, "filebrowser", "hide_object_files", &error);
	CHECK_READ_SETTING(hide_object_files, error, tmp);
	tmp = g_key_file_get_boolean(config, "filebrowser", "fb_follow_path", &error);
	CHECK_READ_SETTING(fb_follow_path, error, tmp);
	tmp = g_key_file_get_boolean(config, "filebrowser", "fb_set_project_base_path", &error);
	CHECK_READ_SETTING(fb_set_project_base_path, error, tmp);

	g_key_file_free(config);
}


static void project_change_cb(G_GNUC_UNUSED GObject *obj, G_GNUC_UNUSED GKeyFile *config,
							  G_GNUC_UNUSED gpointer data)
{
	gchar *new_dir;
	GeanyProject *project = geany->app->project;

	if (! fb_set_project_base_path || project == NULL || ! NZV(project->base_path))
		return;

	/* TODO this is a copy of project_get_base_path(), add it to the plugin API */
	if (g_path_is_absolute(project->base_path))
		new_dir = g_strdup(project->base_path);
	else
	{	/* build base_path out of project file name's dir and base_path */
		gchar *dir = g_path_get_dirname(project->file_name);

		new_dir = g_strconcat(dir, G_DIR_SEPARATOR_S, project->base_path, NULL);
		g_free(dir);
	}
	/* get it into locale encoding */
	setptr(new_dir, utils_get_locale_from_utf8(new_dir));

	if (! utils_str_equal(current_dir, new_dir))
	{
		setptr(current_dir, new_dir);
		refresh();
	}
	else
		g_free(new_dir);
}


static void document_activate_cb(G_GNUC_UNUSED GObject *obj, GeanyDocument *doc,
								 G_GNUC_UNUSED gpointer data)
{
	gchar *new_dir;

	if (! fb_follow_path || doc->file_name == NULL || ! g_path_is_absolute(doc->file_name))
		return;

	new_dir = g_path_get_dirname(doc->file_name);
	setptr(new_dir, utils_get_locale_from_utf8(new_dir));

	if (! utils_str_equal(current_dir, new_dir))
	{
		setptr(current_dir, new_dir);
		refresh();
	}
	else
		g_free(new_dir);
}


static void kb_activate(guint key_id)
{
	gtk_notebook_set_current_page(GTK_NOTEBOOK(geany->main_widgets->sidebar_notebook), page_number);
	switch (key_id)
	{
		case KB_FOCUS_FILE_LIST:
			gtk_widget_grab_focus(file_view);
			break;
		case KB_FOCUS_PATH_ENTRY:
			gtk_widget_grab_focus(path_entry);
			break;
	}
}


void plugin_init(GeanyData *data)
{
	GtkWidget *scrollwin, *toolbar, *filterbar;

	filter = NULL;

	file_view_vbox = gtk_vbox_new(FALSE, 0);
	toolbar = make_toolbar();
	gtk_box_pack_start(GTK_BOX(file_view_vbox), toolbar, FALSE, FALSE, 0);

	filterbar = make_filterbar();
	gtk_box_pack_start(GTK_BOX(file_view_vbox), filterbar, FALSE, FALSE, 0);

	path_entry = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(file_view_vbox), path_entry, FALSE, FALSE, 2);
	g_signal_connect(path_entry, "activate", G_CALLBACK(on_path_entry_activate), NULL);

	file_view = gtk_tree_view_new();
	prepare_file_view();
	completion_create();

	popup_items.open = popup_items.open_external = popup_items.find_in_files = NULL;

	scrollwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(
		GTK_SCROLLED_WINDOW(scrollwin),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scrollwin), file_view);
	gtk_container_add(GTK_CONTAINER(file_view_vbox), scrollwin);

	gtk_widget_show_all(file_view_vbox);
	page_number = gtk_notebook_append_page(GTK_NOTEBOOK(geany->main_widgets->sidebar_notebook),
		file_view_vbox, gtk_label_new(_("Files")));

	load_settings();

	/* setup keybindings */
	keybindings_set_item(plugin_key_group, KB_FOCUS_FILE_LIST, kb_activate,
		0, 0, "focus_file_list", _("Focus File List"), NULL);
	keybindings_set_item(plugin_key_group, KB_FOCUS_PATH_ENTRY, kb_activate,
		0, 0, "focus_path_entry", _("Focus Path Entry"), NULL);

	plugin_signal_connect(geany_plugin, NULL, "document-activate", TRUE,
		(GCallback) &document_activate_cb, NULL);
}


static struct
{
	GtkWidget *open_cmd_entry;
	GtkWidget *show_hidden_checkbox;
	GtkWidget *hide_objects_checkbox;
	GtkWidget *follow_path_checkbox;
	GtkWidget *set_project_base_path_checkbox;
}
pref_widgets;

static void
on_configure_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY)
	{
		GKeyFile *config = g_key_file_new();
		gchar *data;
		gchar *config_dir = g_path_get_dirname(config_file);

		g_free(open_cmd);
		open_cmd = g_strdup(gtk_entry_get_text(GTK_ENTRY(pref_widgets.open_cmd_entry)));
		show_hidden_files = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pref_widgets.show_hidden_checkbox));
		hide_object_files = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pref_widgets.hide_objects_checkbox));
		fb_follow_path = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pref_widgets.follow_path_checkbox));
		fb_set_project_base_path = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
			pref_widgets.set_project_base_path_checkbox));

		g_key_file_load_from_file(config, config_file, G_KEY_FILE_NONE, NULL);

		g_key_file_set_string(config, "filebrowser", "open_command", open_cmd);
		g_key_file_set_boolean(config, "filebrowser", "show_hidden_files", show_hidden_files);
		g_key_file_set_boolean(config, "filebrowser", "hide_object_files", hide_object_files);
		g_key_file_set_boolean(config, "filebrowser", "fb_follow_path", fb_follow_path);
		g_key_file_set_boolean(config, "filebrowser", "fb_set_project_base_path",
			fb_set_project_base_path);

		if (! g_file_test(config_dir, G_FILE_TEST_IS_DIR) && utils_mkdir(config_dir, TRUE) != 0)
		{
			dialogs_show_msgbox(GTK_MESSAGE_ERROR,
				_("Plugin configuration directory could not be created."));
		}
		else
		{
			/* write config to file */
			data = g_key_file_to_data(config, NULL, NULL);
			utils_write_file(config_file, data);
			g_free(data);
		}

		/* apply the changes */
		refresh();

		g_free(config_dir);
		g_key_file_free(config);
	}
}


GtkWidget *plugin_configure(GtkDialog *dialog)
{
	GtkWidget *label, *entry, *checkbox_of, *checkbox_hf, *checkbox_fp, *checkbox_pb, *vbox;
	GtkWidget *box;

	vbox = gtk_vbox_new(FALSE, 6);
	box = gtk_vbox_new(FALSE, 3);

	label = gtk_label_new(_("External open command:"));
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

	entry = gtk_entry_new();
	gtk_widget_show(entry);
	if (open_cmd != NULL)
		gtk_entry_set_text(GTK_ENTRY(entry), open_cmd);
	ui_widget_set_tooltip_text(entry,
		_("The command to execute when using \"Open with\". You can use %f and %d wildcards.\n"
		  "%f will be replaced with the filename including full path\n"
		  "%d will be replaced with the path name of the selected file without the filename"));
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	pref_widgets.open_cmd_entry = entry;

	gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, FALSE, 6);

	checkbox_hf = gtk_check_button_new_with_label(_("Show hidden files"));
	gtk_button_set_focus_on_click(GTK_BUTTON(checkbox_hf), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox_hf), show_hidden_files);
	gtk_box_pack_start(GTK_BOX(vbox), checkbox_hf, FALSE, FALSE, 0);
	pref_widgets.show_hidden_checkbox = checkbox_hf;

	checkbox_of = gtk_check_button_new_with_label(_("Hide object files"));
	gtk_button_set_focus_on_click(GTK_BUTTON(checkbox_of), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox_of), hide_object_files);
	ui_widget_set_tooltip_text(checkbox_of,
		_("Don't show generated object files in the file browser, this includes "
		  "*.o, *.obj. *.so, *.dll, *.a, *.lib"));
	gtk_box_pack_start(GTK_BOX(vbox), checkbox_of, FALSE, FALSE, 0);
	pref_widgets.hide_objects_checkbox = checkbox_of;

	checkbox_fp = gtk_check_button_new_with_label(_("Follow the path of the current file"));
	gtk_button_set_focus_on_click(GTK_BUTTON(checkbox_fp), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox_fp), fb_follow_path);
	gtk_box_pack_start(GTK_BOX(vbox), checkbox_fp, FALSE, FALSE, 0);
	pref_widgets.follow_path_checkbox = checkbox_fp;

	checkbox_pb = gtk_check_button_new_with_label(_("Use the project's base directory"));
	gtk_button_set_focus_on_click(GTK_BUTTON(checkbox_pb), FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox_pb), fb_set_project_base_path);
	ui_widget_set_tooltip_text(checkbox_pb,
		_("Change the directory to the base directory of the currently opened project"));
	gtk_box_pack_start(GTK_BOX(vbox), checkbox_pb, FALSE, FALSE, 0);
	pref_widgets.set_project_base_path_checkbox = checkbox_pb;

	gtk_widget_show_all(vbox);

	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), NULL);
	return vbox;
}


void plugin_cleanup(void)
{
	g_free(config_file);
	g_free(open_cmd);
	g_free(filter);
	gtk_widget_destroy(file_view_vbox);
	g_object_unref(G_OBJECT(entry_completion));
}
