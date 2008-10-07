static bool prompt_yesno(const char *prompt);
#define SIZEOF_REV	41	/* Holds a SHA-1 and an ending NUL. */
#define SIZEOF_ARG	32	/* Default argument array size. */
#define GIT_CONFIG "config"
#define TIG_MAIN_BASE \
	"git log --no-color --pretty=raw --parents --topo-order"

	TIG_MAIN_BASE " %s 2>/dev/null"
	REQ_(STATUS_CHECKOUT,	"Checkout file"), \
static int opt_author_cols		= AUTHOR_COLS-1;
static enum request
	enum request request = REQ_VIEW_MAIN;
		return REQ_VIEW_PAGER;
		return REQ_VIEW_MAIN;
		return REQ_VIEW_STATUS;
		return REQ_VIEW_BLAME;
		request = REQ_VIEW_DIFF;
		request = subcommand[0] == 'l' ? REQ_VIEW_LOG : REQ_VIEW_DIFF;
		string_copy(opt_cmd, TIG_MAIN_BASE);
			return REQ_NONE;
			return REQ_NONE;
	return request;
get_line_type(const char *line)
get_line_info(const char *name)
	{ '!',		REQ_STATUS_CHECKOUT },
add_run_request(enum keymap keymap, int key, int argc, const char **argv)
		const char *argv[1];
option_color_command(int argc, const char *argv[])
static int
parse_int(const char *s, int default_value, int min, int max)
{
	int value = atoi(s);

	return (value < min || value > max) ? default_value : value;
}

option_set_command(int argc, const char *argv[])
		opt_num_interval = parse_int(argv[2], opt_num_interval, 1, 1024);
		return OK;
	}

	if (!strcmp(argv[0], "author-width")) {
		opt_author_cols = parse_int(argv[2], opt_author_cols, 0, 1024);
		opt_tab_size = parse_int(argv[2], opt_tab_size, 1, 1024);
		const char *arg = argv[2];
		int arglen = strlen(arg);
		switch (arg[0]) {
			if (arglen == 1 || arg[arglen - 1] != arg[0]) {
				config_msg = "Unmatched quotation";
				return ERR;
			}
			arg += 1; arglen -= 2;
option_bind_command(int argc, const char *argv[])
	const char *argv[SIZEOF_ARG];
end_update(struct view *view, bool force)
	while (!view->ops->read(view, NULL))
		if (!force)
			return;
begin_update(struct view *view, bool refresh)
	} else if (!refresh) {
	if (ferror(view->pipe) && errno != 0) {
		end_update(view, TRUE);
		end_update(view, FALSE);
	end_update(view, TRUE);
	OPEN_NOMAXIMIZE = 8,	/* Do not maximize the current view. */
	OPEN_REFRESH = 16,	/* Refresh view using previous command. */
	bool reload = !!(flags & (OPEN_RELOAD | OPEN_REFRESH));
	if (view->pipe)
		end_update(view, TRUE);

		   !begin_update(view, flags & OPEN_REFRESH)) {
static bool
run_confirm(const char *cmd, const char *prompt)
{
	bool confirmation = prompt_yesno(prompt);

	if (confirmation)
		system(cmd);

	return confirmation;
}

		    view == VIEW(REQ_VIEW_MAIN) ||
			end_update(view, TRUE);
	    draw_field(view, LINE_MAIN_AUTHOR, author, opt_author_cols, TRUE))
/* This should work even for the "On branch" line. */
static inline bool
status_has_none(struct view *view, struct line *line)
{
	return line < view->line + view->lines && !line[1].data;
}

	"git ls-files -z --others --exclude-standard"
	"git ls-files -z --cached --exclude-standard"
	system("git update-index -q --refresh >/dev/null 2>/dev/null");
	if (opt_no_head &&
	    !status_run(view, STATUS_LIST_NO_HEAD_CMD, 'A', LINE_STAT_STAGED))
		return FALSE;
	else if (!status_run(view, STATUS_DIFF_INDEX_CMD, 0, LINE_STAT_STAGED))
	if (!status_run(view, STATUS_DIFF_FILES_CMD, 0, LINE_STAT_UNSTAGED) ||
	    !status_run(view, STATUS_LIST_OTHER_CMD, '?', LINE_STAT_UNTRACKED))
static bool
status_checkout(struct status *status, enum line_type type, bool has_none)
{
	if (!status || type != LINE_STAT_UNSTAGED) {
		if (type == LINE_STAT_STAGED) {
			report("Cannot checkout staged files");
		} else if (type == LINE_STAT_UNTRACKED) {
			report("Cannot checkout untracked files");
		} else if (has_none) {
			report("Nothing to checkout");
		} else {
			report("Cannot checkout multiple files");
		}
		return FALSE;

	} else {
		char cmd[SIZEOF_STR];
		char file_sq[SIZEOF_STR];

		if (sq_quote(file_sq, 0, status->old.name) >= sizeof(file_sq) ||
		    !string_format(cmd, "git checkout %s%s", opt_cdup, file_sq))
			return FALSE;

		return run_confirm(cmd, "Are you sure you want to overwrite any changes?");
	}
}

	case REQ_STATUS_CHECKOUT:
		if (!status_checkout(status, line->type, status_has_none(view, line)))
			return REQ_NONE;
		break;

	case REQ_STATUS_CHECKOUT:
		if (!status_checkout(stage_status.status ? &stage_status : NULL,
				     stage_line_type, FALSE))
			return REQ_NONE;
		break;

	open_view(view, REQ_VIEW_STAGE, OPEN_REFRESH | OPEN_NOMAXIMIZE);
static void
clear_rev_graph(struct rev_graph *graph)
{
	graph->boundary = 0;
	graph->size = graph->pos = 0;
	graph->commit = NULL;
	memset(graph->parents, 0, sizeof(*graph->parents));
}

	clear_rev_graph(graph);
static int load_refs(void);

	    draw_field(view, LINE_MAIN_AUTHOR, commit->author, opt_author_cols, TRUE))
		int i;

		if (view->lines > 0) {
			commit = view->line[view->lines - 1].data;
			if (!*commit->author) {
				view->lines--;
				free(commit);
				graph->commit = NULL;
			}
		}

		for (i = 0; i < ARRAY_SIZE(graph_stacks); i++)
			clear_rev_graph(&graph_stacks[i]);
	switch (request) {
	case REQ_ENTER:
		break;
	case REQ_REFRESH:
		load_refs();
		open_view(view, REQ_VIEW_MAIN, OPEN_REFRESH);
		break;
	default:
	}
static bool
prompt_yesno(const char *prompt)
{
	enum { WAIT, STOP, CANCEL  } status = WAIT;
	bool answer = FALSE;

	while (status == WAIT) {
		struct view *view;
		int i, key;

		input_mode = TRUE;

		foreach_view (view, i)
			update_view(view);

		input_mode = FALSE;

		mvwprintw(status_win, 0, 0, "%s [Yy]/[Nn]", prompt);
		wclrtoeol(status_win);

		/* Refresh, accept single keystroke of input */
		key = wgetch(status_win);
		switch (key) {
		case ERR:
			break;

		case 'y':
		case 'Y':
			answer = TRUE;
			status = STOP;
			break;

		case KEY_ESC:
		case KEY_RETURN:
		case KEY_ENTER:
		case KEY_BACKSPACE:
		case 'n':
		case 'N':
		case '\n':
		default:
			answer = FALSE;
			status = CANCEL;
		}
	}

	/* Clear the status window */
	status_empty = FALSE;
	report("");

	return answer;
}

	if (!*opt_git_dir)
		return OK;

	while (refs_size > 0)
		free(refs[--refs_size].name);
	while (id_refs_size > 0)
		free(id_refs[--id_refs_size]);

	return read_properties(popen("git " GIT_CONFIG " --list", "r"),
	request = parse_options(argc, argv);
	if (request == REQ_NONE)
	if (!opt_git_dir[0] && request != REQ_VIEW_PAGER)
	if (load_refs() == ERR)
					request = REQ_VIEW_DIFF;
					request = REQ_VIEW_PAGER;

				/* Always reload^Wrerun commands from the prompt. */
				open_view(view, request, OPEN_RELOAD);