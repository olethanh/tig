/* Copyright (c) 2006-2009 Jonas Fonseca <fonseca@diku.dk>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
static struct ref **get_refs(const char *id);

enum format_flags {
	FORMAT_ALL,		/* Perform replacement in all arguments. */
	FORMAT_DASH,		/* Perform replacement up until "--". */
	FORMAT_NONE		/* No replacement should be performed. */
};

static bool format_argv(const char *dst[], const char *src[], enum format_flags flags);
#define prefixcmp(str1, str2) \
	strncmp(str1, str2, STRING_SIZE(str2))

static inline int
suffixcmp(const char *str, int slen, const char *suffix)
{
	size_t len = slen >= 0 ? slen : strlen(str);
	size_t suffixlen = strlen(suffix);

	return suffixlen < len ? strcmp(str + len - suffixlen, suffix) : -1;
}


static bool
argv_from_string(const char *argv[SIZEOF_ARG], int *argc, char *cmd)
{
	int valuelen;

	while (*cmd && *argc < SIZEOF_ARG && (valuelen = strcspn(cmd, " \t"))) {
		bool advance = cmd[valuelen] != 0;

		cmd[valuelen] = 0;
		argv[(*argc)++] = chomp_string(cmd);
		cmd += valuelen + advance;
	}

	if (*argc < SIZEOF_ARG)
		argv[*argc] = NULL;
	return *argc < SIZEOF_ARG;
}

static void
argv_from_env(const char **argv, const char *name)
{
	char *env = argv ? getenv(name) : NULL;
	int argc = 0;

	if (env && *env)
		env = strdup(env);
	if (env && !argv_from_string(argv, &argc, env))
		die("Too many arguments in the `%s` environment variable", name);
}


/*
 * Executing external commands.
enum io_type {
	IO_FD,			/* File descriptor based IO. */
	IO_BG,			/* Execute command in the background. */
	IO_FG,			/* Execute command with same std{in,out,err}. */
	IO_RD,			/* Read only fork+exec IO. */
	IO_WR,			/* Write only fork+exec IO. */
	IO_AP,			/* Append fork+exec output to file. */
};

struct io {
	enum io_type type;	/* The requested type of pipe. */
	const char *dir;	/* Directory from which to execute. */
	pid_t pid;		/* Pipe for reading or writing. */
	int pipe;		/* Pipe end for reading or writing. */
	int error;		/* Error status. */
	const char *argv[SIZEOF_ARG];	/* Shell command arguments. */
	char *buf;		/* Read buffer. */
	size_t bufalloc;	/* Allocated buffer size. */
	size_t bufsize;		/* Buffer content size. */
	char *bufpos;		/* Current buffer position. */
	unsigned int eof:1;	/* Has end of file been reached. */
};

static void
reset_io(struct io *io)
{
	io->pipe = -1;
	io->pid = 0;
	io->buf = io->bufpos = NULL;
	io->bufalloc = io->bufsize = 0;
	io->error = 0;
	io->eof = 0;
}

static void
init_io(struct io *io, const char *dir, enum io_type type)
	reset_io(io);
	io->type = type;
	io->dir = dir;
}
static bool
init_io_rd(struct io *io, const char *argv[], const char *dir,
		enum format_flags flags)
{
	init_io(io, dir, IO_RD);
	return format_argv(io->argv, argv, flags);
}
static bool
io_open(struct io *io, const char *name)
{
	init_io(io, NULL, IO_FD);
	io->pipe = *name ? open(name, O_RDONLY) : STDIN_FILENO;
	return io->pipe != -1;
}

static bool
kill_io(struct io *io)
{
	return kill(io->pid, SIGKILL) != -1;
}

static bool
done_io(struct io *io)
{
	pid_t pid = io->pid;

	if (io->pipe != -1)
		close(io->pipe);
	free(io->buf);
	reset_io(io);

	while (pid > 0) {
		int status;
		pid_t waiting = waitpid(pid, &status, 0);

		if (waiting < 0) {
			if (errno == EINTR)
				continue;
			report("waitpid failed (%s)", strerror(errno));
			return FALSE;
		}

		return waiting == pid &&
		       !WIFSIGNALED(status) &&
		       WIFEXITED(status) &&
		       !WEXITSTATUS(status);
	}

	return TRUE;
}

static bool
start_io(struct io *io)
{
	int pipefds[2] = { -1, -1 };

	if (io->type == IO_FD)
		return TRUE;

	if ((io->type == IO_RD || io->type == IO_WR) &&
	    pipe(pipefds) < 0)
		return FALSE;
	else if (io->type == IO_AP)
		pipefds[1] = io->pipe;

	if ((io->pid = fork())) {
		if (pipefds[!(io->type == IO_WR)] != -1)
			close(pipefds[!(io->type == IO_WR)]);
		if (io->pid != -1) {
			io->pipe = pipefds[!!(io->type == IO_WR)];
			return TRUE;
		}

	} else {
		if (io->type != IO_FG) {
			int devnull = open("/dev/null", O_RDWR);
			int readfd  = io->type == IO_WR ? pipefds[0] : devnull;
			int writefd = (io->type == IO_RD || io->type == IO_AP)
							? pipefds[1] : devnull;

			dup2(readfd,  STDIN_FILENO);
			dup2(writefd, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);

			close(devnull);
			if (pipefds[0] != -1)
				close(pipefds[0]);
			if (pipefds[1] != -1)
				close(pipefds[1]);
		}

		if (io->dir && *io->dir && chdir(io->dir) == -1)
			die("Failed to change directory: %s", strerror(errno));

		execvp(io->argv[0], (char *const*) io->argv);
		die("Failed to execute program: %s", strerror(errno));
	}

	if (pipefds[!!(io->type == IO_WR)] != -1)
		close(pipefds[!!(io->type == IO_WR)]);
	return FALSE;
}

static bool
run_io(struct io *io, const char **argv, const char *dir, enum io_type type)
{
	init_io(io, dir, type);
	if (!format_argv(io->argv, argv, FORMAT_NONE))
		return FALSE;
	return start_io(io);
}

static int
run_io_do(struct io *io)
{
	return start_io(io) && done_io(io);
}

static int
run_io_bg(const char **argv)
{
	struct io io = {};

	init_io(&io, NULL, IO_BG);
	if (!format_argv(io.argv, argv, FORMAT_NONE))
		return FALSE;
	return run_io_do(&io);
}

static bool
run_io_fg(const char **argv, const char *dir)
{
	struct io io = {};

	init_io(&io, dir, IO_FG);
	if (!format_argv(io.argv, argv, FORMAT_NONE))
		return FALSE;
	return run_io_do(&io);
}

static bool
run_io_append(const char **argv, enum format_flags flags, int fd)
{
	struct io io = {};

	init_io(&io, NULL, IO_AP);
	io.pipe = fd;
	if (format_argv(io.argv, argv, flags))
		return run_io_do(&io);
	close(fd);
	return FALSE;
}

static bool
run_io_rd(struct io *io, const char **argv, enum format_flags flags)
{
	return init_io_rd(io, argv, NULL, flags) && start_io(io);
}

static bool
io_eof(struct io *io)
{
	return io->eof;
}

static int
io_error(struct io *io)
{
	return io->error;
}

static bool
io_strerror(struct io *io)
{
	return strerror(io->error);
}

static bool
io_can_read(struct io *io)
{
	struct timeval tv = { 0, 500 };
	fd_set fds;

	FD_ZERO(&fds);
	FD_SET(io->pipe, &fds);

	return select(io->pipe + 1, &fds, NULL, NULL, &tv) > 0;
}

static ssize_t
io_read(struct io *io, void *buf, size_t bufsize)
{
	do {
		ssize_t readsize = read(io->pipe, buf, bufsize);

		if (readsize < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (readsize == -1)
			io->error = errno;
		else if (readsize == 0)
			io->eof = 1;
		return readsize;
	} while (1);
}

static char *
io_get(struct io *io, int c, bool can_read)
{
	char *eol;
	ssize_t readsize;

	if (!io->buf) {
		io->buf = io->bufpos = malloc(BUFSIZ);
		if (!io->buf)
			return NULL;
		io->bufalloc = BUFSIZ;
		io->bufsize = 0;
	}

	while (TRUE) {
		if (io->bufsize > 0) {
			eol = memchr(io->bufpos, c, io->bufsize);
			if (eol) {
				char *line = io->bufpos;

				*eol = 0;
				io->bufpos = eol + 1;
				io->bufsize -= io->bufpos - line;
				return line;
			}

		if (io_eof(io)) {
			if (io->bufsize) {
				io->bufpos[io->bufsize] = 0;
				io->bufsize = 0;
				return io->bufpos;
			}
			return NULL;
		}

		if (!can_read)
			return NULL;

		if (io->bufsize > 0 && io->bufpos > io->buf)
			memmove(io->buf, io->bufpos, io->bufsize);

		io->bufpos = io->buf;
		readsize = io_read(io, io->buf + io->bufsize, io->bufalloc - io->bufsize);
		if (io_error(io))
			return NULL;
		io->bufsize += readsize;
}

static bool
io_write(struct io *io, const void *buf, size_t bufsize)
{
	size_t written = 0;
	while (!io_error(io) && written < bufsize) {
		ssize_t size;
		size = write(io->pipe, buf + written, bufsize - written);
		if (size < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (size == -1)
			io->error = errno;
		else
			written += size;
	}

	return written == bufsize;
static bool
run_io_buf(const char **argv, char buf[], size_t bufsize)
{
	struct io io = {};
	bool error;

	if (!run_io_rd(&io, argv, FORMAT_NONE))
		return FALSE;

	io.buf = io.bufpos = buf;
	io.bufalloc = bufsize;
	error = !io_get(&io, '\n', TRUE) && io_error(&io);
	io.buf = NULL;

	return done_io(&io) || error;
}

static int read_properties(struct io *io, const char *separators, int (*read)(char *, size_t, char *, size_t));
	REQ_(STATUS_REVERT,	"Revert file changes"), \
	const char *name;
	const char *help;
static char opt_head_rev[SIZEOF_REV]	= "";
static FILE *opt_tty			= NULL;

#define is_initial_commit()	(!*opt_head_rev)
#define is_head_commit(rev)	(!strcmp((rev), "HEAD") || !strcmp(opt_head_rev, (rev)))
parse_options(int argc, const char *argv[], const char ***run_argv)
	const char *subcommand;
	/* XXX: This is vulnerable to the user overriding options
	 * required for the main view parser. */
	static const char *custom_argv[SIZEOF_ARG] = {
		"git", "log", "--no-color", "--pretty=raw", "--parents",
			"--topo-order", NULL
	};
	int i, j = 6;
	if (!isatty(STDIN_FILENO))
	if (subcommand) {
		custom_argv[1] = subcommand;
		j = 2;
	}
		const char *opt = argv[i];
		custom_argv[j++] = opt;
		if (j >= ARRAY_SIZE(custom_argv))
	custom_argv[j] = NULL;
	*run_argv = custom_argv;
	unsigned int cleareol:1;
	{ '!',		REQ_STATUS_REVERT },
struct keybinding_table {
	struct keybinding *data;
	size_t size;
};

static struct keybinding_table keybindings[ARRAY_SIZE(keymap_table)];
	struct keybinding_table *table = &keybindings[keymap];
	table->data = realloc(table->data, (table->size + 1) * sizeof(*table->data));
	if (!table->data)
	table->data[table->size].alias = key;
	table->data[table->size++].request = request;
	size_t i;
	for (i = 0; i < keybindings[keymap].size; i++)
		if (keybindings[keymap].data[i].alias == key)
			return keybindings[keymap].data[i].request;
	for (i = 0; i < keybindings[KEYMAP_GENERIC].size; i++)
		if (keybindings[KEYMAP_GENERIC].data[i].alias == key)
			return keybindings[KEYMAP_GENERIC].data[i].request;
	const char *name;
static const char *
	const char *seq = NULL;
static const char *
	const char *argv[SIZEOF_ARG];
	if (argc >= ARRAY_SIZE(req->argv) - 1)
		return REQ_NONE;
	req = &run_request[run_requests];
	req->argv[0] = NULL;
	if (!format_argv(req->argv, argv, FORMAT_NONE))
		return REQ_NONE;

	return REQ_NONE + ++run_requests;
	const char *cherry_pick[] = { "git", "cherry-pick", "%(commit)", NULL };
	const char *gc[] = { "git", "gc", NULL };
		int argc;
		const char **argv;
		{ KEYMAP_MAIN,	  'C', ARRAY_SIZE(cherry_pick) - 1, cherry_pick },
		{ KEYMAP_GENERIC, 'G', ARRAY_SIZE(gc) - 1, gc },
		req = add_run_request(reqs[i].keymap, reqs[i].key, reqs[i].argc, reqs[i].argv);
static const char *config_msg;
		const char *obsolete[] = { "cherry-pick", "screen-resize" };
set_option(const char *opt, char *value)
	if (!argv_from_string(argv, &argc, value)) {
		config_msg = "Too many option arguments";
		return ERR;
	struct io io = {};
	if (!io_open(&io, path))
	if (read_properties(&io, " \t", read_option) == ERR ||
	const char *home = getenv("HOME");
	const char *tigrc_user = getenv("TIGRC_USER");
	const char *tigrc_system = getenv("TIGRC_SYSTEM");
	struct io io;
	struct io *pipe;
	time_t update_secs;
	/* Default command arguments. */
	const char **argv;
static struct view_ops diff_ops;
#define VIEW_STR(name, env, ref, ops, map, git) \
	{ name, #env, ref, ops, map, git }
	VIEW_STR(name, TIG_##id##_CMD, ref, ops, KEYMAP_##id, git)
	VIEW_(DIFF,   "diff",   &diff_ops,   TRUE,  ref_commit),
draw_field(struct view *view, enum line_type type, const char *text, int len, bool trim)
	if (line->cleareol)
		wclrtoeol(view->win);
	line->dirty = line->cleareol = 0;
		if (view->offset + lineno >= view->lines)
			break;
		if (!view->line[view->offset + lineno].dirty)
	werase(view->win);
	if (view != VIEW(REQ_VIEW_STATUS) && view->lines) {
		string_format_from(state, &statelen, " - %s %d of %d (%d%%)",
	}
	if (view->pipe) {
		time_t secs = time(NULL) - view->start_time;

		/* Three git seconds are a long time ... */
		if (secs > 2)
			string_format_from(state, &statelen, " loading %lds", secs);
		string_format_from(buf, &bufpos, "%s", state);
redraw_display(bool clear)
		if (clear)
			wclear(view->win);
static void
toggle_view_option(bool *option, const char *help)
{
	*option = !*option;
	redraw_display(FALSE);
	report("%sabling %s", *option ? "En" : "Dis", help);
}

static void
select_view_line(struct view *view, unsigned long lineno)
		if (view_is_displayed(view))
			redraw_view(view);
		if (view_is_displayed(view)) {
			draw_view_line(view, old_lineno);
			draw_view_line(view, view->lineno - view->offset);
			redrawwin(view->win);
			wrefresh(view->win);
		} else {
			view->ops->select(view, &view->line[view->lineno]);
		}
		if (view->ops->grep(view, &view->line[lineno])) {
			select_view_line(view, lineno);
			report("Line %ld matches '%s'", lineno + 1, view->grep);
		}
	view->update_secs = 0;
}

static void
free_argv(const char *argv[])
{
	int argc;

	for (argc = 0; argv[argc]; argc++)
		free((void *) argv[argc]);
}

static bool
format_argv(const char *dst_argv[], const char *src_argv[], enum format_flags flags)
{
	char buf[SIZEOF_STR];
	int argc;
	bool noreplace = flags == FORMAT_NONE;

	free_argv(dst_argv);

	for (argc = 0; src_argv[argc]; argc++) {
		const char *arg = src_argv[argc];
		size_t bufpos = 0;

		while (arg) {
			char *next = strstr(arg, "%(");
			int len = next - arg;
			const char *value;

			if (!next || noreplace) {
				if (flags == FORMAT_DASH && !strcmp(arg, "--"))
					noreplace = TRUE;
				len = strlen(arg);
				value = "";

			} else if (!prefixcmp(next, "%(directory)")) {
				value = opt_path;

			} else if (!prefixcmp(next, "%(file)")) {
				value = opt_file;

			} else if (!prefixcmp(next, "%(ref)")) {
				value = *opt_ref ? opt_ref : "HEAD";

			} else if (!prefixcmp(next, "%(head)")) {
				value = ref_head;

			} else if (!prefixcmp(next, "%(commit)")) {
				value = ref_commit;

			} else if (!prefixcmp(next, "%(blob)")) {
				value = ref_blob;

			} else {
				report("Unknown replacement: `%s`", next);
				return FALSE;
			}

			if (!string_format_from(buf, &bufpos, "%.*s%s", len, arg, value))
				return FALSE;

			arg = next && !noreplace ? strchr(next, ')') + 1 : NULL;
		}

		dst_argv[argc] = strdup(buf);
		if (!dst_argv[argc])
			break;
	}

	dst_argv[argc] = NULL;

	return src_argv[argc] == NULL;
	if (force)
		kill_io(view->pipe);
	done_io(view->pipe);
static void
setup_update(struct view *view, const char *vid)
{
	set_nonblocking_input(TRUE);
	reset_view(view);
	string_copy_rev(view->vid, vid);
	view->pipe = &view->io;
	view->start_time = time(NULL);
}

prepare_update(struct view *view, const char *argv[], const char *dir,
	       enum format_flags flags)
	if (view->pipe)
		end_update(view, TRUE);
	return init_io_rd(&view->io, argv, dir, flags);
}
static bool
prepare_update_file(struct view *view, const char *name)
{
	if (view->pipe)
		end_update(view, TRUE);
	return io_open(&view->io, name);
}
static bool
begin_update(struct view *view, bool refresh)
{
	if (view->pipe)
		end_update(view, TRUE);
	if (refresh) {
		if (!start_io(&view->io))
	} else {
		if (view == VIEW(REQ_VIEW_TREE) && strcmp(view->vid, view->id))
			opt_path[0] = 0;
		if (!run_io_rd(&view->io, view->ops->argv, FORMAT_ALL))
	setup_update(view, view->id);
	/* Clear the view and redraw everything since the tree sorting
	 * might have rearranged things. */
	bool redraw = view->lines == 0;
	bool can_read = TRUE;
	if (!io_can_read(view->pipe)) {
		if (view->lines == 0) {
			time_t secs = time(NULL) - view->start_time;
			if (secs > view->update_secs) {
				if (view->update_secs == 0)
					redraw_view(view);
				update_view_title(view);
				view->update_secs = secs;
			}
		}
		return TRUE;
	}
	for (; (line = io_get(view->pipe, '\n', can_read)); can_read = FALSE) {
			size_t inlen = strlen(line) + 1;
			if (ret != (size_t) -1)
		unsigned long lines = view->lines;
			if (opt_line_number || view == VIEW(REQ_VIEW_BLAME))
				redraw = TRUE;
	if (io_error(view->pipe)) {
		report("Failed to read: %s", io_strerror(view->pipe));
		end_update(view, TRUE);
	} else if (io_eof(view->pipe)) {
		report("");
		end_update(view, FALSE);
	if (!view_is_displayed(view))
		return TRUE;

	if (redraw)
		redraw_view_from(view, 0);
	else
	struct line *line;
	if (!realloc_lines(view, view->lines + 1))
		return NULL;

	line = &view->line[view->lines++];
	line->dirty = 1;
add_line_text(struct view *view, const char *text, enum line_type type)
	char *data = text ? strdup(text) : NULL;
static struct line *
add_line_format(struct view *view, enum line_type type, const char *fmt, ...)
{
	char buf[SIZEOF_STR];
	va_list args;

	va_start(args, fmt);
	if (vsnprintf(buf, sizeof(buf), fmt, args) >= sizeof(buf))
		buf[0] = 0;
	va_end(args);

	return buf[0] ? add_line_text(view, buf, type) : NULL;
}
	OPEN_PREPARED = 32,	/* Open already prepared command. */
	bool reload = !!(flags & (OPEN_RELOAD | OPEN_REFRESH | OPEN_PREPARED));
		   !begin_update(view, flags & (OPEN_REFRESH | OPEN_PREPARED))) {
	} else if (view_is_displayed(view)) {
open_external_viewer(const char *argv[], const char *dir)
	run_io_fg(argv, dir);
	getc(opt_tty);
	redraw_display(TRUE);
	const char *mergetool_argv[] = { "git", "mergetool", file, NULL };
	open_external_viewer(mergetool_argv, opt_cdup);
	const char *editor_argv[] = { "vi", file, NULL };
	const char *editor;
	editor_argv[0] = editor;
	open_external_viewer(editor_argv, from_root ? opt_cdup : NULL);
	const char *argv[ARRAY_SIZE(req->argv)] = { NULL };
	if (format_argv(argv, req->argv, FORMAT_ALL))
		open_external_viewer(argv, NULL);
	free_argv(argv);
		if (!VIEW(REQ_VIEW_PAGER)->pipe && !VIEW(REQ_VIEW_PAGER)->lines) {
		toggle_view_option(&opt_line_number, "line numbers");
		toggle_view_option(&opt_date, "date display");
		toggle_view_option(&opt_author, "author display");
		toggle_view_option(&opt_rev_graph, "revision graph display");
		toggle_view_option(&opt_show_refs, "reference display");
		redraw_display(TRUE);
			redraw_display(FALSE);
add_describe_ref(char *buf, size_t *bufpos, const char *commit_id, const char *sep)
	const char *describe_argv[] = { "git", "describe", commit_id, NULL };
	if (run_io_buf(describe_argv, refbuf, sizeof(refbuf)))
		ref = chomp_string(refbuf);
		const char *fmt = ref->tag    ? "%s[%s]" :
		                  ref->remote ? "%s<%s>" : "%s%s";
	NULL,
static const char *log_argv[SIZEOF_ARG] = {
	"git", "log", "--no-color", "--cc", "--stat", "-n100", "%(head)", NULL
};

	log_argv,
static const char *diff_argv[SIZEOF_ARG] = {
	"git", "show", "--pretty=fuller", "--no-color", "--root",
		"--patch-with-stat", "--find-copies-harder", "-C", "%(commit)", NULL
};

static struct view_ops diff_ops = {
	"line",
	diff_argv,
	NULL,
	pager_read,
	pager_draw,
	pager_request,
	pager_grep,
	pager_select,
};
		const char *key;
		add_line_format(view, LINE_DEFAULT, "    %-25s %s",
				key, req_info[i].help);
		const char *key;
		char cmd[SIZEOF_STR];
		size_t bufpos;
		int argc;
		for (bufpos = 0, argc = 0; req->argv[argc]; argc++)
			if (!string_format_from(cmd, &bufpos, "%s%s",
					        argc ? " " : "", req->argv[argc]))
				return REQ_NONE;
		add_line_format(view, LINE_DEFAULT, "    %-10s %-14s `%s`",
				keymap_table[req->keymap].name, key, cmd);
	NULL,
push_tree_stack_entry(const char *name, unsigned long lineno)
static const char *
tree_path(struct line *line)
	const char *path = line->data;
	return path + SIZEOF_TREE_ATTR;
static int
tree_compare_entry(struct line *line1, struct line *line2)
	if (line1->type != line2->type)
		return line1->type == LINE_TREE_DIR ? -1 : 1;
	return strcmp(tree_path(line1), tree_path(line2));
	struct line *entry, *line;
	if (view->lines == 0 &&
	    !add_line_format(view, LINE_DEFAULT, "Directory path /%s", opt_path))
		return FALSE;

		/* Insert "link" to parent directory. */
		if (view->lines == 1 &&
		    !add_line_format(view, LINE_TREE_DIR, TREE_UP_FORMAT, view->ref))
			return FALSE;
	entry = add_line_text(view, text, type);
	if (!entry)
		return FALSE;
	text = entry->data;
	/* Skip "Directory ..." and ".." line. */
	for (line = &view->line[1 + !!*opt_path]; line < entry; line++) {
		if (tree_compare_entry(line, entry) <= 0)
		memmove(line + 1, line, (entry - line) * sizeof(*entry));
		for (; line <= entry; line++)
			line->dirty = line->cleareol = 1;
static void
open_blob_editor()
{
	char file[SIZEOF_STR] = "/tmp/tigblob.XXXXXX";
	int fd = mkstemp(file);

	if (fd == -1)
		report("Failed to create temporary file");
	else if (!run_io_append(blob_ops.argv, FORMAT_ALL, fd))
		report("Failed to save blob data to file");
	else
		open_editor(FALSE, file);
	if (fd != -1)
		unlink(file);
}

	switch (request) {
	case REQ_VIEW_BLAME:
		if (line->type != LINE_TREE_FILE) {
			report("Blame only supported for files");

	case REQ_EDIT:
		if (line->type != LINE_TREE_FILE) {
			report("Edit only supported for files");
		} else if (!is_head_commit(view->vid)) {
			open_blob_editor();
			open_editor(TRUE, opt_file);
		}
		return REQ_NONE;

	case REQ_TREE_PARENT:
		if (!*opt_path) {
		/* fake 'cd  ..' */
		line = &view->line[1];
		break;

	case REQ_ENTER:
		break;

	default:
	}
			const char *basename = tree_path(line);
		string_format(opt_file, "%s%s", opt_path, tree_path(line));
static const char *tree_argv[SIZEOF_ARG] = {
	"git", "ls-tree", "%(commit)", "%(directory)", NULL
};

	tree_argv,
static enum request
blob_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_EDIT:
		open_blob_editor();
		return REQ_NONE;
	default:
		return pager_request(view, request, line);
	}
}

static const char *blob_argv[SIZEOF_ARG] = {
	"git", "cat-file", "blob", "%(blob)", NULL
};

	blob_argv,
	blob_request,
static const char *blame_head_argv[] = {
	"git", "blame", "--incremental", "--", "%(file)", NULL
};

static const char *blame_ref_argv[] = {
	"git", "blame", "--incremental", "%(ref)", "--", "%(file)", NULL
};

static const char *blame_cat_file_argv[] = {
	"git", "cat-file", "blob", "%(ref):%(file)", NULL
};

	if (*opt_ref || !io_open(&view->io, opt_file)) {
		if (!run_io_rd(&view->io, blame_cat_file_argv, FORMAT_ALL))
	setup_update(view, opt_file);
parse_number(const char **posref, size_t *number, size_t min, size_t max)
	const char *pos = *posref;
parse_blame_commit(struct view *view, const char *text, int *blamed)
	const char *pos = text + SIZEOF_REV - 1;
blame_read_file(struct view *view, const char *line, bool *read_file)
		const char **argv = *opt_ref ? blame_ref_argv : blame_head_argv;
		struct io io = {};
		if (view->lines == 0 && !view->parent)

		if (view->lines == 0 || !run_io_rd(&io, argv, FORMAT_ALL)) {
		done_io(view->pipe);
		view->io = io;
		*read_file = FALSE;
	static bool read_file = TRUE;
	if (read_file)
		return blame_read_file(view, line, &read_file);
		read_file = TRUE;
	const char *id = NULL, *author = NULL;
static bool
check_blame_commit(struct blame *blame)
{
	if (!blame->commit)
		report("Commit data not loaded yet");
	else if (!strcmp(blame->commit->id, NULL_ID))
		report("No commit exist for the selected line");
	else
		return TRUE;
	return FALSE;
}

	case REQ_VIEW_BLAME:
		if (check_blame_commit(blame)) {
			string_copy(opt_ref, blame->commit->id);
			open_view(view, REQ_VIEW_BLAME, OPEN_REFRESH);
		}
		break;

		if (view_is_displayed(VIEW(REQ_VIEW_DIFF)) &&
		    !strcmp(blame->commit->id, VIEW(REQ_VIEW_DIFF)->ref))
			break;
		if (!strcmp(blame->commit->id, NULL_ID)) {
			struct view *diff = VIEW(REQ_VIEW_DIFF);
			const char *diff_index_argv[] = {
				"git", "diff-index", "--root", "--cached",
					"--patch-with-stat", "-C", "-M",
					"HEAD", "--", view->vid, NULL
			};

			if (!prepare_update(diff, diff_index_argv, NULL, FORMAT_DASH)) {
				report("Failed to allocate diff command");
			}
			flags |= OPEN_PREPARED;
	NULL,
status_get_diff(struct status *file, const char *buf, size_t bufsize)
	const char *old_mode = buf +  1;
	const char *new_mode = buf +  8;
	const char *old_rev  = buf + 15;
	const char *new_rev  = buf + 56;
	const char *status   = buf + 97;
	if (bufsize < 98 ||
status_run(struct view *view, const char *argv[], char status, enum line_type type)
	char *buf;
	struct io io = {};
	if (!run_io(&io, argv, NULL, IO_RD))
	while ((buf = io_get(&io, 0, TRUE))) {
		if (!file) {
			file = calloc(1, sizeof(*file));
			if (!file || !add_line_data(view, file, type))
				goto error_out;
		}
		/* Parse diff info part. */
		if (status) {
			file->status = status;
			if (status == 'A')
				string_copy(file->old.rev, NULL_ID);
		} else if (!file->status) {
			if (!status_get_diff(file, buf, strlen(buf)))
				goto error_out;
			buf = io_get(&io, 0, TRUE);
			if (!buf)
				break;
			/* Collapse all 'M'odified entries that follow a
			 * associated 'U'nmerged entry. */
			if (file->status == 'U') {
				unmerged = file;
			} else if (unmerged) {
				int collapse = !strcmp(buf, unmerged->new.name);
				unmerged = NULL;
				if (collapse) {
					free(file);
					view->lines--;
					continue;
		}
		/* Grab the old name for rename/copy. */
		if (!*file->old.name &&
		    (file->status == 'R' || file->status == 'C')) {
			string_ncopy(file->old.name, buf, strlen(buf));
			buf = io_get(&io, 0, TRUE);
			if (!buf)
				break;

		/* git-ls-files just delivers a NUL separated list of
		 * file names similar to the second half of the
		 * git-diff-* output. */
		string_ncopy(file->new.name, buf, strlen(buf));
		if (!*file->old.name)
			string_copy(file->old.name, file->new.name);
		file = NULL;
	if (io_error(&io)) {
		done_io(&io);
	done_io(&io);
static const char *status_diff_index_argv[] = {
	"git", "diff-index", "-z", "--diff-filter=ACDMRTXB",
			     "--cached", "-M", "HEAD", NULL
};

static const char *status_diff_files_argv[] = {
	"git", "diff-files", "-z", NULL
};
static const char *status_list_other_argv[] = {
	"git", "ls-files", "-z", "--others", "--exclude-standard", NULL
};
static const char *status_list_no_head_argv[] = {
	"git", "ls-files", "-z", "--cached", "--exclude-standard", NULL
};
static const char *update_index_argv[] = {
	"git", "update-index", "-q", "--unmerged", "--refresh", NULL
};
	if (is_initial_commit())
	run_io_bg(update_index_argv);
	if (is_initial_commit()) {
		if (!status_run(view, status_list_no_head_argv, 'A', LINE_STAT_STAGED))
	} else if (!status_run(view, status_diff_index_argv, 0, LINE_STAT_STAGED)) {
	if (!status_run(view, status_diff_files_argv, 0, LINE_STAT_UNSTAGED) ||
	    !status_run(view, status_list_other_argv, '?', LINE_STAT_UNTRACKED))
	const char *text;
	const char *oldpath = status ? status->old.name : NULL;
	/* Diffs for unmerged entries are empty when passing the new
	 * path, so leave it empty. */
	const char *newpath = status && status->status != 'U' ? status->new.name : NULL;
	const char *info;
	struct view *stage = VIEW(REQ_VIEW_STAGE);
		if (is_initial_commit()) {
			const char *no_head_diff_argv[] = {
				"git", "diff", "--no-color", "--patch-with-stat",
					"--", "/dev/null", newpath, NULL
			};

			if (!prepare_update(stage, no_head_diff_argv, opt_cdup, FORMAT_DASH))
			const char *index_show_argv[] = {
				"git", "diff-index", "--root", "--patch-with-stat",
					"-C", "-M", "--cached", "HEAD", "--",
					oldpath, newpath, NULL
			};

			if (!prepare_update(stage, index_show_argv, opt_cdup, FORMAT_DASH))
	{
		const char *files_show_argv[] = {
			"git", "diff-files", "--root", "--patch-with-stat",
				"-C", "-M", "--", oldpath, newpath, NULL
		};

		if (!prepare_update(stage, files_show_argv, opt_cdup, FORMAT_DASH))
	}
		if (!newpath) {
	    	if (!suffixcmp(status->new.name, -1, "/")) {
			report("Cannot display a directory");
			return REQ_NONE;
		}

		if (!prepare_update_file(stage, newpath))
			return REQ_QUIT;
	open_view(view, REQ_VIEW_STAGE, OPEN_REFRESH | split);
	unsigned long lineno;
	for (lineno = 0; lineno < view->lines; lineno++) {
		struct line *line = &view->line[lineno];
		if (line->type != type)
			continue;
		if (!pos && (!status || !status->status) && line[1].data) {
			select_view_line(view, lineno);
		}
		if (pos && !strcmp(status->new.name, pos->new.name)) {
			select_view_line(view, lineno);
			return TRUE;
		}
static bool
status_update_prepare(struct io *io, enum line_type type)
	const char *staged_argv[] = {
		"git", "update-index", "-z", "--index-info", NULL
	};
	const char *others_argv[] = {
		"git", "update-index", "-z", "--add", "--remove", "--stdin", NULL
	};
		return run_io(io, staged_argv, opt_cdup, IO_WR);
		return run_io(io, others_argv, opt_cdup, IO_WR);

		return run_io(io, others_argv, NULL, IO_WR);
		return FALSE;
status_update_write(struct io *io, struct status *status, enum line_type type)
	return io_write(io, buf, bufsize);
	struct io io = {};
	if (!status_update_prepare(&io, type))
	result = status_update_write(&io, status, type);
	done_io(&io);
	struct io io = {};
	if (!status_update_prepare(&io, line->type))
		result = status_update_write(&io, line->data, line->type);
	done_io(&io);
status_revert(struct status *status, enum line_type type, bool has_none)
			report("Cannot revert changes to staged files");
			report("Cannot revert changes to untracked files");
			report("Nothing to revert");
			report("Cannot revert changes to multiple files");
		const char *checkout_argv[] = {
			"git", "checkout", "--", status->old.name, NULL
		};
		if (!prompt_yesno("Are you sure you want to overwrite any changes?"))
		return run_io_fg(checkout_argv, opt_cdup);
	case REQ_STATUS_REVERT:
		if (!status_revert(status, line->type, status_has_none(view, line)))
		if (status->status == 'D') {
			report("File has been deleted.");
			return REQ_NONE;
		}
	const char *text;
	const char *key;
		const char *text;
	NULL,
stage_diff_write(struct io *io, struct line *line, struct line *end)
		if (!io_write(io, line->data, strlen(line->data)) ||
		    !io_write(io, "\n", 1))
		line++;
stage_apply_chunk(struct view *view, struct line *chunk, bool revert)
	const char *apply_argv[SIZEOF_ARG] = {
		"git", "apply", "--whitespace=nowarn", NULL
	};
	struct io io = {};
	int argc = 3;
	if (!revert)
		apply_argv[argc++] = "--cached";
	if (revert || stage_line_type == LINE_STAT_STAGED)
		apply_argv[argc++] = "-R";
	apply_argv[argc++] = "-";
	apply_argv[argc++] = NULL;
	if (!run_io(&io, apply_argv, opt_cdup, IO_WR))
	if (!stage_diff_write(&io, diff_hdr, chunk) ||
	    !stage_diff_write(&io, chunk, view->line + view->lines))
	done_io(&io);
	run_io_bg(update_index_argv);
	if (!is_initial_commit() && stage_line_type != LINE_STAT_UNTRACKED)
		if (!stage_apply_chunk(view, chunk, FALSE)) {
static bool
stage_revert(struct view *view, struct line *line)
{
	struct line *chunk = NULL;

	if (!is_initial_commit() && stage_line_type == LINE_STAT_UNSTAGED)
		chunk = stage_diff_find(view, line, LINE_DIFF_CHUNK);

	if (chunk) {
		if (!prompt_yesno("Are you sure you want to revert changes?"))
			return FALSE;

		if (!stage_apply_chunk(view, chunk, TRUE)) {
			report("Failed to revert chunk");
			return FALSE;
		}
		return TRUE;

	} else {
		return status_revert(stage_status.status ? &stage_status : NULL,
				     stage_line_type, FALSE);
	}
}


	case REQ_STATUS_REVERT:
		if (!stage_revert(view, line))
		if (stage_status.status == 'D') {
			report("File has been deleted.");
			return REQ_NONE;
		}
	if (stage_line_type == LINE_STAT_UNTRACKED) {
	    	if (!suffixcmp(stage_status.new.name, -1, "/")) {
			report("Cannot display a directory");
			return REQ_NONE;
		}

		if (!prepare_update_file(view, stage_status.new.name)) {
			report("Failed to open file: %s", strerror(errno));
			return REQ_NONE;
		}
	}
	NULL,
push_rev_graph(struct rev_graph *graph, const char *parent)
update_rev_graph(struct view *view, struct rev_graph *graph)
	if (view->lines > 2)
		view->line[view->lines - 3].dirty = 1;
	if (view->lines > 1)
		view->line[view->lines - 2].dirty = 1;
static const char *main_argv[SIZEOF_ARG] = {
	"git", "log", "--no-color", "--pretty=raw", "--parents",
		      "--topo-order", "%(head)", NULL
};

			view->line[view->lines - 1].dirty = 1;
		update_rev_graph(view, graph);
		update_rev_graph(view, graph);
		view->line[view->lines - 1].dirty = 1;
		view->line[view->lines - 1].dirty = 1;
	main_argv,
		opt_tty = stdin;
		opt_tty = fopen("/dev/tty", "r+");
		if (!opt_tty)
		cursed = !!newterm(NULL, opt_tty, opt_tty);
static int
get_input(bool prompting)
	struct view *view;
	int i, key;
	if (prompting)
	while (true) {
		/* Refresh, accept single keystroke of input */
		key = wgetch(status_win);

		/* wgetch() with nodelay() enabled returns ERR when
		 * there's no input. */
		if (key == ERR) {
			doupdate();

		} else if (key == KEY_RESIZE) {
			int height, width;

			getmaxyx(stdscr, height, width);

			/* Resize the status view and let the view driver take
			 * care of resizing the displayed views. */
			resize_display();
			redraw_display(TRUE);
			wresize(status_win, 1, width);
			mvwin(status_win, height - 1, 0);
			wrefresh(status_win);

		} else {
			input_mode = FALSE;
			return key;
		}
	}
}

static bool
prompt_yesno(const char *prompt)
{
	enum { WAIT, STOP, CANCEL  } status = WAIT;
	bool answer = FALSE;

	while (status == WAIT) {
		int key;
		key = get_input(TRUE);
	static char buf[SIZEOF_STR];
		int key;
		key = get_input(TRUE);
 * Repository properties
static int
git_properties(const char **argv, const char *separators,
	       int (*read_property)(char *, size_t, char *, size_t))
{
	struct io io = {};

	if (init_io_rd(&io, argv, NULL, FORMAT_NONE))
		return read_properties(&io, separators, read_property);
	return ERR;
}

static int
compare_refs(const void *ref1_, const void *ref2_)
{
	const struct ref *ref1 = *(const struct ref **)ref1_;
	const struct ref *ref2 = *(const struct ref **)ref2_;

	if (ref1->tag != ref2->tag)
		return ref2->tag - ref1->tag;
	if (ref1->ltag != ref2->ltag)
		return ref2->ltag - ref2->ltag;
	if (ref1->head != ref2->head)
		return ref2->head - ref1->head;
	if (ref1->tracked != ref2->tracked)
		return ref2->tracked - ref1->tracked;
	if (ref1->remote != ref2->remote)
		return ref2->remote - ref1->remote;
	return strcmp(ref1->name, ref2->name);
}

get_refs(const char *id)
		ref_list[ref_list_size]->next = 1;

	if (ref_list) {
		qsort(ref_list, ref_list_size, sizeof(*ref_list), compare_refs);
		ref_list[ref_list_size - 1]->next = 0;
	}
	if (!prefixcmp(name, "refs/tags/")) {
		if (!suffixcmp(name, namelen, "^{}")) {
	} else if (!prefixcmp(name, "refs/remotes/")) {
	} else if (!prefixcmp(name, "refs/heads/")) {
		string_ncopy(opt_head_rev, id, idlen);
		 * the commit id of an annotated tag right before the commit id
	static const char *ls_remote_argv[SIZEOF_ARG] = {
		"git", "ls-remote", ".", NULL
	};
	static bool init = FALSE;

	if (!init) {
		argv_from_env(ls_remote_argv, "TIG_LS_REMOTE");
		init = TRUE;
	}
	return git_properties(ls_remote_argv, "\t", read_ref);
		if (!prefixcmp(value, "refs/heads/")) {
	const char *config_list_argv[] = { "git", GIT_CONFIG, "--list", NULL };

	return git_properties(config_list_argv, "=", read_repo_config_option);
		string_ncopy(opt_cdup, name, namelen);
	const char *head_argv[] = {
		"git", "symbolic-ref", "HEAD", NULL
	};
	const char *rev_parse_argv[] = {
		"git", "rev-parse", "--git-dir", "--is-inside-work-tree",
			"--show-cdup", NULL
	};
	if (run_io_buf(head_argv, opt_head, sizeof(opt_head))) {
		chomp_string(opt_head);
		if (!prefixcmp(opt_head, "refs/heads/")) {
			char *offset = opt_head + STRING_SIZE("refs/heads/");
			memmove(opt_head, offset, strlen(offset) + 1);
		}
	}
	return git_properties(rev_parse_argv, "=", read_repo_info);
read_properties(struct io *io, const char *separators,
	if (!start_io(io))
	while (state == OK && (name = io_get(io, '\n', TRUE))) {
	if (state != ERR && io_error(io))
	done_io(io);
main(int argc, const char *argv[])
	const char **run_argv = NULL;
	request = parse_options(argc, argv, &run_argv);
	foreach_view (view, i)
		argv_from_env(view->ops->argv, view->cmd_env);
	if (request == REQ_VIEW_PAGER || run_argv) {
		if (request == REQ_VIEW_PAGER)
			io_open(&VIEW(request)->io, "");
		else if (!prepare_update(VIEW(request), run_argv, NULL, FORMAT_NONE))
			die("Failed to format arguments");
		open_view(NULL, request, OPEN_PREPARED);
		request = REQ_NONE;
	}
	while (view_driver(display[current_view], request)) {
		int key = get_input(FALSE);
		view = display[current_view];
		request = get_keybinding(view->keymap, key);
			if (cmd) {
				struct view *next = VIEW(REQ_VIEW_PAGER);
				const char *argv[SIZEOF_ARG] = { "git" };
				int argc = 1;

				/* When running random commands, initially show the
				 * command in the title. However, it maybe later be
				 * overwritten if a commit line is selected. */
				string_ncopy(next->ref, cmd, strlen(cmd));

				if (!argv_from_string(argv, &argc, cmd)) {
					report("Too many arguments");
				} else if (!prepare_update(next, argv, NULL, FORMAT_DASH)) {
					report("Failed to format command");
					open_view(view, REQ_VIEW_PAGER, OPEN_PREPARED);
			const char *prompt = request == REQ_SEARCH ? "/" : "?";