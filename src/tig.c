/* Copyright (c) 2006-2014 Jonas Fonseca <jonas.fonseca@gmail.com>
#include "tig/tig.h"
#include "tig/types.h"
#include "tig/util.h"
#include "tig/parse.h"
#include "tig/io.h"
#include "tig/argv.h"
#include "tig/refs.h"
#include "tig/graph.h"
#include "tig/git.h"
#include "tig/request.h"
#include "tig/line.h"
#include "tig/keys.h"
#include "tig/view.h"
#include "tig/repo.h"
#include "tig/options.h"
#include "tig/draw.h"
#include "tig/display.h"
#include "tig/grep.h"
forward_request_to_child(struct view *child, enum request request)
	return displayed_views() == 2 && view_is_displayed(child) &&
		!strcmp(child->vid, child->ops->id);
static enum request
view_request(struct view *view, enum request request)
	if (!view || !view->lines)
		return request;
	if (request == REQ_ENTER && !opt_focus_child &&
	    view_has_flags(view, VIEW_SEND_CHILD_ENTER)) {
		struct view *child = display[1];
	    	if (forward_request_to_child(child, request)) {
			view_request(child, request);
			return REQ_NONE;
	if (request == REQ_REFRESH && view->unrefreshable) {
		report("This view can not be refreshed");
		return REQ_NONE;
	return view->ops->request(view, request, &view->line[view->pos.lineno]);
 * Option management
#define VIEW_FLAG_RESET_DISPLAY	((enum view_flag) -1)
#define TOGGLE_MENU_INFO(_) \
	_(LINENO,    '.', "line numbers",      &opt_show_line_numbers, NULL, VIEW_NO_FLAGS), \
	_(DATE,      'D', "dates",             &opt_show_date, date_map, VIEW_NO_FLAGS), \
	_(AUTHOR,    'A', "author",            &opt_show_author, author_map, VIEW_NO_FLAGS), \
	_(GRAPHIC,   '~', "graphics",          &opt_line_graphics, graphic_map, VIEW_NO_FLAGS), \
	_(REV_GRAPH, 'g', "revision graph",    &opt_show_rev_graph, NULL, VIEW_LOG_LIKE), \
	_(FILENAME,  '#', "file names",        &opt_show_filename, filename_map, VIEW_NO_FLAGS), \
	_(FILE_SIZE, '*', "file sizes",        &opt_show_file_size, file_size_map, VIEW_NO_FLAGS), \
	_(IGNORE_SPACE, 'W', "space changes",  &opt_ignore_space, ignore_space_map, VIEW_DIFF_LIKE), \
	_(COMMIT_ORDER, 'l', "commit order",   &opt_commit_order, commit_order_map, VIEW_LOG_LIKE), \
	_(REFS,      'F', "reference display", &opt_show_refs, NULL, VIEW_NO_FLAGS), \
	_(CHANGES,   'C', "local change display", &opt_show_changes, NULL, VIEW_NO_FLAGS), \
	_(ID,        'X', "commit ID display", &opt_show_id, NULL, VIEW_NO_FLAGS), \
	_(FILES,     '%', "file filtering",    &opt_file_filter, NULL, VIEW_DIFF_LIKE | VIEW_LOG_LIKE), \
	_(TITLE_OVERFLOW, '$', "commit title overflow display", &opt_title_overflow, NULL, VIEW_NO_FLAGS), \
	_(UNTRACKED_DIRS, 'd', "untracked directory info", &opt_status_untracked_dirs, NULL, VIEW_STATUS_LIKE), \
	_(VERTICAL_SPLIT, '|', "view split",   &opt_vertical_split, vertical_split_map, VIEW_FLAG_RESET_DISPLAY), \
static enum view_flag
toggle_option(struct view *view, enum request request, char msg[SIZEOF_STR])
	const struct {
		enum request request;
		const struct enum_map *map;
		enum view_flag reload_flags;
	} data[] = {
#define DEFINE_TOGGLE_DATA(id, key, help, value, map, vflags) { REQ_TOGGLE_ ## id, map, vflags  }
		TOGGLE_MENU_INFO(DEFINE_TOGGLE_DATA)
	};
	const struct menu_item menu[] = {
#define DEFINE_TOGGLE_MENU(id, key, help, value, map, vflags) { key, help, value }
		TOGGLE_MENU_INFO(DEFINE_TOGGLE_MENU)
		{ 0 }
	};
	int i = 0;
	if (request == REQ_OPTIONS) {
		if (!prompt_menu("Toggle option", menu, &i))
			return VIEW_NO_FLAGS;
	} else {
		while (i < ARRAY_SIZE(data) && data[i].request != request)
			i++;
		if (i >= ARRAY_SIZE(data))
			die("Invalid request (%d)", request);
	if (data[i].map != NULL) {
		unsigned int *opt = menu[i].data;

		*opt = (*opt + 1) % data[i].map->size;
		if (data[i].map == ignore_space_map) {
			update_ignore_space_arg();
			string_format_size(msg, SIZEOF_STR,
				"Ignoring %s %s", enum_name(data[i].map->entries[*opt]), menu[i].text);
		} else if (data[i].map == commit_order_map) {
			update_commit_order_arg();
			string_format_size(msg, SIZEOF_STR,
				"Using %s %s", enum_name(data[i].map->entries[*opt]), menu[i].text);
			string_format_size(msg, SIZEOF_STR,
				"Displaying %s %s", enum_name(data[i].map->entries[*opt]), menu[i].text);
	} else if (menu[i].data == &opt_title_overflow) {
		int *option = menu[i].data;
		*option = *option ? -*option : 50;
		string_format_size(msg, SIZEOF_STR,
			"%sabling %s", *option > 0 ? "En" : "Dis", menu[i].text);
		bool *option = menu[i].data;
		*option = !*option;
		string_format_size(msg, SIZEOF_STR,
			"%sabling %s", *option ? "En" : "Dis", menu[i].text);
	return data[i].reload_flags;
/*
 * View opening
 */
static enum request run_prompt_command(struct view *view, char *cmd);
static enum request
open_run_request(struct view *view, enum request request)
	struct run_request *req = get_run_request(request);
	const char **argv = NULL;
	bool confirmed = FALSE;
	request = REQ_NONE;
	if (!req) {
		report("Unknown run request");
		return request;
	}
	if (argv_format(view->env, &argv, req->argv, FALSE, TRUE)) {
		if (req->internal) {
			char cmd[SIZEOF_STR];
			if (argv_to_string(argv, cmd, sizeof(cmd), " ")) {
				request = run_prompt_command(view, cmd);
		else {
			confirmed = !req->confirm;
			if (req->confirm) {
				char cmd[SIZEOF_STR], prompt[SIZEOF_STR];
				const char *and_exit = req->exit ? " and exit" : "";
				if (argv_to_string(argv, cmd, sizeof(cmd), " ") &&
				    string_format(prompt, "Run `%s`%s?", cmd, and_exit) &&
				    prompt_yesno(prompt)) {
					confirmed = TRUE;
			}
			if (confirmed && argv_remove_quotes(argv)) {
				if (req->silent)
					io_run_bg(argv);
				else
					open_external_viewer(argv, NULL, !req->exit, "");
			}
		}
	}
	if (argv)
		argv_free(argv);
	free(argv);
	if (request == REQ_NONE) {
		if (req->confirm && !confirmed)
			request = REQ_NONE;
		else if (req->exit)
			request = REQ_QUIT;
		else if (view_has_flags(view, VIEW_REFRESH) && !view->unrefreshable)
			request = REQ_REFRESH;
	return request;
/*
 * User request switch noodle
 */
static int
view_driver(struct view *view, enum request request)
	if (request == REQ_NONE)
		return TRUE;
	if (request >= REQ_RUN_REQUESTS) {
		request = open_run_request(view, request);
		// exit quickly rather than going through view_request and back
		if (request == REQ_QUIT)
			return FALSE;
	request = view_request(view, request);
	if (request == REQ_NONE)
		return TRUE;
	switch (request) {
	case REQ_MOVE_UP:
	case REQ_MOVE_DOWN:
	case REQ_MOVE_PAGE_UP:
	case REQ_MOVE_PAGE_DOWN:
	case REQ_MOVE_FIRST_LINE:
	case REQ_MOVE_LAST_LINE:
		move_view(view, request);
		break;
	case REQ_SCROLL_FIRST_COL:
	case REQ_SCROLL_LEFT:
	case REQ_SCROLL_RIGHT:
	case REQ_SCROLL_LINE_DOWN:
	case REQ_SCROLL_LINE_UP:
	case REQ_SCROLL_PAGE_DOWN:
	case REQ_SCROLL_PAGE_UP:
	case REQ_SCROLL_WHEEL_DOWN:
	case REQ_SCROLL_WHEEL_UP:
		scroll_view(view, request);
		break;
	case REQ_VIEW_GREP:
		open_grep_view(view);
		break;
	case REQ_VIEW_MAIN:
	case REQ_VIEW_DIFF:
	case REQ_VIEW_LOG:
	case REQ_VIEW_TREE:
	case REQ_VIEW_HELP:
	case REQ_VIEW_BRANCH:
	case REQ_VIEW_BLAME:
	case REQ_VIEW_BLOB:
	case REQ_VIEW_STATUS:
	case REQ_VIEW_STAGE:
	case REQ_VIEW_PAGER:
	case REQ_VIEW_STASH:
		open_view(view, request, OPEN_DEFAULT);
		break;
	case REQ_NEXT:
	case REQ_PREVIOUS:
		if (view->parent) {
			int line;
			view = view->parent;
			line = view->pos.lineno;
			view_request(view, request);
			move_view(view, request);
			if (view_is_displayed(view))
				update_view_title(view);
			if (line != view->pos.lineno)
				view_request(view, REQ_ENTER);
		} else {
			move_view(view, request);
		}
		break;
	case REQ_VIEW_NEXT:
	{
		int nviews = displayed_views();
		int next_view = (current_view + 1) % nviews;
		if (next_view == current_view) {
			report("Only one view is displayed");
			break;
		}
		current_view = next_view;
		/* Blur out the title of the previous view. */
		update_view_title(view);
		report_clear();
		break;
	}
	case REQ_REFRESH:
		report("Refreshing is not supported by the %s view", view->name);
		break;
	case REQ_PARENT:
		report("Moving to parent is not supported by the the %s view", view->name);
		break;
	case REQ_BACK:
		report("Going back is not supported for by %s view", view->name);
		break;
	case REQ_MAXIMIZE:
		if (displayed_views() == 2)
			maximize_view(view, TRUE);
		break;
	case REQ_OPTIONS:
	case REQ_TOGGLE_LINENO:
	case REQ_TOGGLE_DATE:
	case REQ_TOGGLE_AUTHOR:
	case REQ_TOGGLE_FILENAME:
	case REQ_TOGGLE_GRAPHIC:
	case REQ_TOGGLE_REV_GRAPH:
	case REQ_TOGGLE_REFS:
	case REQ_TOGGLE_CHANGES:
	case REQ_TOGGLE_IGNORE_SPACE:
	case REQ_TOGGLE_ID:
	case REQ_TOGGLE_FILES:
	case REQ_TOGGLE_TITLE_OVERFLOW:
	case REQ_TOGGLE_FILE_SIZE:
	case REQ_TOGGLE_UNTRACKED_DIRS:
	case REQ_TOGGLE_VERTICAL_SPLIT:
		{
			char action[SIZEOF_STR] = "";
			enum view_flag flags = toggle_option(view, request, action);
	
			if (flags == VIEW_FLAG_RESET_DISPLAY) {
				resize_display();
				redraw_display(TRUE);
			} else {
				foreach_displayed_view(view, i) {
					if (view_has_flags(view, flags) && !view->unrefreshable)
						reload_view(view);
					else
						redraw_view(view);
				}
			}
			if (*action)
				report("%s", action);
		}
		break;
	case REQ_TOGGLE_SORT_FIELD:
	case REQ_TOGGLE_SORT_ORDER:
		report("Sorting is not yet supported for the %s view", view->name);
		break;
	case REQ_DIFF_CONTEXT_UP:
	case REQ_DIFF_CONTEXT_DOWN:
		report("Changing the diff context is not yet supported for the %s view", view->name);
		break;
	case REQ_SEARCH:
	case REQ_SEARCH_BACK:
		search_view(view, request);
		break;
	case REQ_FIND_NEXT:
	case REQ_FIND_PREV:
		find_next(view, request);
		break;
	case REQ_STOP_LOADING:
		foreach_view(view, i) {
			if (view->pipe)
				report("Stopped loading the %s view", view->name),
			end_update(view, TRUE);
		}
		break;
	case REQ_SHOW_VERSION:
		report("tig-%s (built %s)", TIG_VERSION, __DATE__);
		return TRUE;
	case REQ_SCREEN_REDRAW:
		redraw_display(TRUE);
		break;
	case REQ_EDIT:
		report("Nothing to edit");
		break;
	case REQ_ENTER:
		report("Nothing to enter");
		break;
	case REQ_VIEW_CLOSE:
		/* XXX: Mark closed views by letting view->prev point to the
		 * view itself. Parents to closed view should never be
		 * followed. */
		if (view->prev && view->prev != view) {
			maximize_view(view->prev, TRUE);
			view->prev = view;
			break;
		/* Fall-through */
	case REQ_QUIT:
		return FALSE;
	default:
		report("Unknown key, press %s for help",
		       get_view_key(view, REQ_VIEW_HELP));
		return TRUE;
	}
	return TRUE;
static const char usage_string[] =
"   or: tig grep   [options] [pattern]\n"
void
usage(const char *message)
	die("%s\n\n%s", message, usage_string);
filter_options(const char *argv[], bool rev_parse)
	update_options_from_argv(argv);
	if (!rev_parse) {
		opt_cmdline_argv = argv;
		return;
		opt_cmdline_argv = flags;
	bool rev_parse = TRUE;
	} else if (!strcmp(subcommand, "grep")) {
		request = REQ_VIEW_GREP;
		rev_parse = FALSE;

				exit(EXIT_SUCCESS);
				printf("%s\n", usage_string);
				exit(EXIT_SUCCESS);
				int lineno = atoi(opt + 1);

				argv_env.lineno = lineno > 0 ? lineno - 1 : 0;
		filter_options(filter_argv, rev_parse);
		string_ncopy(view->env->search, cmd, strlen(cmd));
		} else if (!argv_format(view->env, &next->argv, argv, FALSE, TRUE)) {
	if (!repo.git_dir[0] && request != REQ_VIEW_PAGER)
				string_ncopy(argv_env.search, search, strlen(search));
			else if (*argv_env.search)
	exit(EXIT_SUCCESS);