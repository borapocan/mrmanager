/* mrmanager.c — Mr.Manager process viewer for MrRobotOS
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Merih Bora Poçan
 */

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <gtk/gtk.h>

#define RUNNING_COUNT  "ps --no-headers -eo stat | grep -c 'R'"
#define SLEEPING_COUNT "ps --no-headers -eo stat | grep -cE 'S|I'"
#define STOPPED_COUNT  "ps --no-headers -eo stat | grep -c 'T'"
#define ZOMBIE_COUNT   "ps --no-headers -eo stat | grep -c 'Z'"
#define MEM_INFO_CMD   "free -m | awk 'NR==2{printf \"Mem:       %.0f total  %.0f free  %.0f used  %.0f cache\",$2,$4,$3,$6}'"
#define SWAP_INFO_CMD  "free -m | awk 'NR==3{printf \"Swap:      %.0f total  %.0f free  %.0f used\",$2,$4,$3}'"
#define CPU_INFO_CMD   "awk '/^cpu /{printf \"CPU:       us=%d ni=%d sy=%d id=%d wa=%d\",$2,$3,$4,$5,$6}' /proc/stat"
#define PS_CMD \
	"ps -eo pid,ppid,user,pri,ni,vsz,bsdtime,s,cls,pcpu,pmem,comm --sort=+pid" \
	" | awk 'NR>1{printf \"\\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\"" \
	" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\"\\n\"," \
	"$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12}'"

enum {
	COL_PID=0, COL_PPID, COL_USER, COL_PRI, COL_NI,
	COL_VSZ, COL_TIME, COL_S, COL_CLS, COL_PCPU, COL_PMEM, COL_COMM,
	COL_SORT_CPU, COL_SORT_MEM,
	N_COLS
};

static const char *COL_NAMES[] = {
	"PID","PPID","USER","PRI","NI","VSZ","TIME","S","CLS","%CPU","%MEM","COMMAND",NULL,NULL
};
static const int COL_W[] = { 240,90,100,55,55,90,75,45,55,90,90,240 };

typedef struct Process {
	int   pid, ppid, vsz;
	float pcpu, pmem, max_cpu, max_mem;
	char *user, *pri, *ni, *bsdtime, *s, *cls, *comm;
} Process;

typedef struct Node {
	Process    *proc;
	struct Node *next, *child;
} Node;

typedef struct { GList *paths; } ExpandCtx;

/* ── globals ── */
static GtkWidget    *g_window       = NULL;
static GtkWidget    *g_search_entry = NULL;
static GtkWidget    *g_tree_view    = NULL;
static Node         *g_head         = NULL;
static int           g_save_y       = 0;
static GtkTreeStore *g_store        = NULL; /* persistent — never replaced */

/* ── prototypes ── */
static void          activate(GtkApplication*, gpointer);
static void          apply_css(void);
static Node         *build_tree(void);
static void          compute_subtree(Node*);
static float         subtree_max_cpu(Node*);
static float         subtree_max_mem(Node*);
static void          add_to_store(GtkTreeStore*, Node*, GtkTreeIter*, const char*);
static GtkTreeStore *create_store(void);
static void          populate_store(GtkTreeStore*, Node*, const char*);
static Process      *parse_line(const char*);
static void          insert_node(Node**, Node*);
static Node         *find_recursive(Node*, int);
static void          fill_stats(char*, size_t);
static gboolean      cb_update_stats(GtkLabel*);
static gboolean      cb_update_tree(gpointer);
static void          collect_expanded(GtkTreeView*, GtkTreePath*, gpointer);
static gboolean      do_scroll_restore(gpointer);
static void          on_row_activated(GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*, gpointer);
static void          show_process_dialog(GtkTreeView*, GtkTreePath*);
static void          on_resp_main(GtkDialog*, gint, gpointer);
static void          on_resp_confirm(GtkDialog*, gint, gpointer);
static void          on_search_changed(GtkSearchEntry*, gpointer);
static void          free_tree(Node*);
static int           run_int(const char*);
static void          run_str(const char*, char*, size_t);
static gboolean      node_matches(Node*, const char*);

/* ================================================================== */
int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(
		"org.mrrobotos.mrmanager", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int rc = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return rc;
}

/* ================================================================== */
static void apply_css(void)
{
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_string(p,
		"window { background: #f0f0f0; }"
		"* { font-family: monospace; font-size: 13px; color: #1a1a1a; }"
		"headerbar {"
		"  background: #ebebeb; border-bottom: 1px solid #d0d0d0;"
		"  border-radius: 0; box-shadow: none; min-height: 36px; padding: 0 8px;"
		"}"
		"headerbar label { font-size: 14px; font-weight: bold; color: #000; }"
		"headerbar windowcontrols { background: transparent; }"
		"headerbar windowcontrols button {"
		"  background: transparent; border: none; border-radius: 0;"
		"  box-shadow: none; outline: none; padding: 4px 6px; margin: 0;"
		"  color: #333; -gtk-icon-size: 14px;"
		"}"
		"headerbar windowcontrols button:hover { background: transparent; border: none; box-shadow: none; }"
		"headerbar windowcontrols button:active { background: transparent; }"
		".stats { font-size: 13px; color: #222; padding: 10px 14px 8px 14px; line-height: 1.7; }"
		".stats-frame { background: #fafafa; border: none; border-bottom: 1px solid #d8d8d8; border-radius: 0; margin: 0; }"
		".search-row { background: #f8f8f8; border-bottom: 1px solid #d0d0d0; padding: 6px 10px; }"
		"searchentry { background: #fff; color: #1a1a1a; border: 1px solid #c0c0c0; border-radius: 3px; padding: 5px 10px; font-size: 13px; }"
		"searchentry:focus { border-color: #5b9bd5; }"
		"treeview { background: #fff; color: #1a1a1a; border: none; }"
		"treeview:selected { background: #3879d9; color: #fff; }"
		"treeview:hover { background: #eef4ff; }"
		"treeview header { background: #e4e4e4; }"
		"treeview header button {"
		"  background: #e4e4e4; color: #333; border: none;"
		"  border-bottom: 2px solid #c0c0c0; border-right: 1px solid #d0d0d0;"
		"  border-radius: 0; box-shadow: none; padding: 6px 8px; font-weight: bold; font-size: 12px;"
		"}"
		"treeview header button:hover { background: #d8d8d8; color: #000; }"
		"treeview header button:checked { background: #d0d0d0; }"
		"scrollbar { background: #f0f0f0; border: none; }"
		"scrollbar slider { background: #b8b8b8; border-radius: 3px; min-width: 6px; min-height: 6px; }"
		"scrollbar slider:hover { background: #888; }"
		"button { background: #e8e8e8; color: #1a1a1a; border: 1px solid #c0c0c0; border-radius: 3px; padding: 5px 18px; }"
		"button:hover { background: #d8d8d8; }"
		"dialog, messagedialog { background: #f5f5f5; }"
		".message-area { padding: 20px 24px 20px 24px; }"
		"messagedialog label { color: #1a1a1a; }"
		".dialog-action-area { padding: 8px 16px 16px 16px; }"
	);
	gtk_style_context_add_provider_for_display(
		gdk_display_get_default(),
		GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
	g_object_unref(p);
}

/* ================================================================== */
static void activate(GtkApplication *app, gpointer ud)
{
	apply_css();

	g_window = gtk_application_window_new(app);
	gtk_window_set_default_size(GTK_WINDOW(g_window), 1240, 740);
	gtk_window_set_title(GTK_WINDOW(g_window), "Mr.Manager");
	gtk_window_set_icon_name(GTK_WINDOW(g_window), "mrmanager");

	GtkWidget *hbar = gtk_header_bar_new();
	gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hbar), TRUE);
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hbar), gtk_label_new("Mr.Manager"));
	gtk_window_set_titlebar(GTK_WINDOW(g_window), hbar);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(GTK_WINDOW(g_window), vbox);

	/* stats */
	GtkWidget *stats_frame = gtk_frame_new(NULL);
	gtk_widget_add_css_class(stats_frame, "stats-frame");
	char stats[700];
	fill_stats(stats, sizeof(stats));
	GtkWidget *stats_lbl = gtk_label_new(stats);
	gtk_widget_add_css_class(stats_lbl, "stats");
	gtk_label_set_xalign(GTK_LABEL(stats_lbl), 0.0);
	gtk_frame_set_child(GTK_FRAME(stats_frame), stats_lbl);
	gtk_box_append(GTK_BOX(vbox), stats_frame);

	/* search */
	GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(search_row, "search-row");
	g_search_entry = gtk_search_entry_new();
	gtk_widget_set_hexpand(g_search_entry, TRUE);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(g_search_entry),
		"Search processes by PID, user, command...");
	gtk_box_append(GTK_BOX(search_row), g_search_entry);
	gtk_box_append(GTK_BOX(vbox), search_row);

	/* tree — create persistent store once, never replace it */
	g_head = build_tree();
	compute_subtree(g_head);

	g_store = create_store();
	populate_store(g_store, g_head, "");
	gtk_tree_sortable_set_sort_column_id(
		GTK_TREE_SORTABLE(g_store), COL_PID, GTK_SORT_ASCENDING);

	g_tree_view = gtk_tree_view_new();
	gtk_tree_view_set_model(GTK_TREE_VIEW(g_tree_view), GTK_TREE_MODEL(g_store));
	gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(g_tree_view), TRUE);
	gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(g_tree_view), TRUE);

	for (int i = 0; i < N_COLS - 2; i++) {
		GtkCellRenderer   *r   = gtk_cell_renderer_text_new();
		GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
			COL_NAMES[i], r, "text", i, NULL);
		if (i==COL_PID || i==COL_PPID || i==COL_VSZ || i==COL_PCPU || i==COL_PMEM) {
			g_object_set(r, "xalign", 1.0f, NULL);
			gtk_tree_view_column_set_alignment(col, 1.0f);
		}
		int sort_id = i;
		if (i == COL_PCPU) sort_id = COL_SORT_CPU;
		if (i == COL_PMEM) sort_id = COL_SORT_MEM;
		gtk_tree_view_column_set_sort_column_id(col, sort_id);
		gtk_tree_view_column_set_sort_indicator(col, TRUE);
		gtk_tree_view_column_set_resizable(col, TRUE);
		gtk_tree_view_column_set_fixed_width(col, COL_W[i]);
		gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_append_column(GTK_TREE_VIEW(g_tree_view), col);
	}

	g_signal_connect(g_tree_view, "row-activated", G_CALLBACK(on_row_activated), NULL);
	g_signal_connect(g_search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
	gtk_widget_set_vexpand(g_tree_view, TRUE);

	GtkWidget *sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), g_tree_view);
	gtk_box_append(GTK_BOX(vbox), sw);

	gtk_window_present(GTK_WINDOW(g_window));
	gtk_tree_view_expand_all(GTK_TREE_VIEW(g_tree_view));

	g_timeout_add_seconds(3, (GSourceFunc)cb_update_stats, stats_lbl);
	g_timeout_add_seconds(3, (GSourceFunc)cb_update_tree, NULL);
}

/* ================================================================== */
/* Search                                                               */
/* ================================================================== */
static gboolean node_matches(Node *n, const char *q)
{
	if (!n || !q || !*q) return TRUE;
	Process *p = n->proc;
	char pid_str[32];
	snprintf(pid_str, sizeof(pid_str), "%d", p->pid);
	if (strstr(pid_str,            q)) return TRUE;
	if (p->user && strstr(p->user, q)) return TRUE;
	if (p->comm && strstr(p->comm, q)) return TRUE;
	if (p->s    && strstr(p->s,    q)) return TRUE;
	if (p->cls  && strstr(p->cls,  q)) return TRUE;
	for (Node *ch = n->child; ch; ch = ch->next)
		if (node_matches(ch, q)) return TRUE;
	return FALSE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer ud)
{
	const char *q = gtk_editable_get_text(GTK_EDITABLE(entry));
	GtkTreeView *tv = GTK_TREE_VIEW(g_tree_view);

	gint sort_col; GtkSortType sort_ord;
	gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(g_store),
		&sort_col, &sort_ord);

	gtk_tree_store_clear(g_store);
	populate_store(g_store, g_head, q);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(g_store),
		sort_col, sort_ord);
	gtk_tree_view_expand_all(tv);
}

/* ================================================================== */
/* Stats                                                                */
/* ================================================================== */
static int run_int(const char *cmd)
{
	FILE *f = popen(cmd, "r"); if (!f) return 0;
	int v = 0; fscanf(f, "%d", &v); pclose(f); return v;
}

static void run_str(const char *cmd, char *buf, size_t sz)
{
	FILE *f = popen(cmd, "r");
	if (!f) { buf[0]='\0'; return; }
	if (!fgets(buf, sz, f)) buf[0]='\0';
	pclose(f);
	size_t l = strlen(buf);
	if (l && buf[l-1]=='\n') buf[l-1]='\0';
}

static void fill_stats(char *buf, size_t sz)
{
	int run=run_int(RUNNING_COUNT), slp=run_int(SLEEPING_COUNT);
	int stp=run_int(STOPPED_COUNT), zmb=run_int(ZOMBIE_COUNT);
	int tot=run+slp+stp+zmb;
	char upt[160]="", cpu[160]="", mem[160]="", swp[160]="";
	run_str("uptime",      upt, sizeof(upt));
	run_str(CPU_INFO_CMD,  cpu, sizeof(cpu));
	run_str(MEM_INFO_CMD,  mem, sizeof(mem));
	run_str(SWAP_INFO_CMD, swp, sizeof(swp));
	char *u = upt; while (*u==' ') u++;
	snprintf(buf, sz,
		"Time:      %s\n"
		"Tasks:     %d total  %d running  %d sleeping  %d stopped  %d zombie\n"
		"%s\n%s\n%s",
		u, tot, run, slp, stp, zmb, cpu, mem, swp);
}

static gboolean cb_update_stats(GtkLabel *lbl)
{
	char buf[700];
	fill_stats(buf, sizeof(buf));
	gtk_label_set_text(lbl, buf);
	return G_SOURCE_CONTINUE;
}

/* ================================================================== */
/* Tree refresh — repopulate persistent store, never swap model        */
/* ================================================================== */
static void collect_expanded(GtkTreeView *tv, GtkTreePath *path, gpointer ud)
{
	ExpandCtx *ctx = ud;
	ctx->paths = g_list_prepend(ctx->paths, gtk_tree_path_copy(path));
}

static gboolean do_scroll_restore(gpointer ud)
{
	gtk_tree_view_scroll_to_point(GTK_TREE_VIEW(g_tree_view), -1, g_save_y);
	return G_SOURCE_REMOVE;
}

static gboolean cb_update_tree(gpointer ud)
{
	GtkTreeView *tv = GTK_TREE_VIEW(g_tree_view);

	/* save sort */
	gint sort_col; GtkSortType sort_ord;
	gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(g_store),
		&sort_col, &sort_ord);

	/* save scroll in tree coordinates */
	GdkRectangle vis;
	gtk_tree_view_get_visible_rect(tv, &vis);
	g_save_y = vis.y;

	/* save expanded paths */
	ExpandCtx ctx = {NULL};
	gtk_tree_view_map_expanded_rows(tv, collect_expanded, &ctx);
	gboolean had_expanded = (ctx.paths != NULL);

	const char *q = gtk_editable_get_text(GTK_EDITABLE(g_search_entry));

	/* rebuild process data */
	free_tree(g_head);
	g_head = build_tree();
	compute_subtree(g_head);

	/* repopulate the same store — model pointer never changes so
	 * GtkTreeView never resets its scroll adjustment to zero */
	gtk_tree_store_clear(g_store);
	populate_store(g_store, g_head, q);

	/* restore sort */
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(g_store),
		sort_col, sort_ord);

	/* restore expand */
	if (had_expanded) {
		for (GList *l = ctx.paths; l; l = l->next) {
			gtk_tree_view_expand_row(tv, (GtkTreePath*)l->data, TRUE);
			gtk_tree_path_free(l->data);
		}
		g_list_free(ctx.paths);
	} else {
		gtk_tree_view_expand_all(tv);
	}

	/* gtk_tree_store_clear resets vadjustment to 0 internally.
	 * Restore immediately and also after layout pass. */
	gtk_tree_view_scroll_to_point(tv, -1, g_save_y);
	g_idle_add_full(G_PRIORITY_HIGH_IDLE + 30, do_scroll_restore, NULL, NULL);

	return G_SOURCE_CONTINUE;
}

/* ================================================================== */
/* Process tree                                                         */
/* ================================================================== */
static Process *parse_line(const char *line)
{
	Process *p = calloc(1, sizeof(Process));
	sscanf(line,
		"\"%d\" \"%d\" \"%m[^\"]\" \"%m[^\"]\" \"%m[^\"]\" \"%d\""
		" \"%m[^\"]\" \"%m[^\"]\" \"%m[^\"]\" \"%f\" \"%f\" \"%m[^\"]\"",
		&p->pid, &p->ppid, &p->user, &p->pri, &p->ni, &p->vsz,
		&p->bsdtime, &p->s, &p->cls, &p->pcpu, &p->pmem, &p->comm);
	p->max_cpu = p->pcpu;
	p->max_mem = p->pmem;
	return p;
}

static Node *search_parent(Node *h, int ppid)
{
	if (!h) return NULL;
	if (h->proc->pid == ppid) return h;
	Node *r = search_parent(h->child, ppid);
	return r ? r : search_parent(h->next, ppid);
}

static void insert_node(Node **head, Node *n)
{
	Node *parent = search_parent(*head, n->proc->ppid);
	if (parent) {
		if (!parent->child) { parent->child = n; return; }
		Node *t = parent->child; while (t->next) t = t->next; t->next = n;
	} else { n->next = *head; *head = n; }
}

static Node *build_tree(void)
{
	Node *root = NULL;
	FILE *f = popen(PS_CMD, "r"); if (!f) return NULL;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		Process *p = parse_line(line);
		Node *n = calloc(1, sizeof(Node)); n->proc = p;
		if (p->ppid == 0) { n->next = root; root = n; }
		else insert_node(&root, n);
	}
	pclose(f); return root;
}

static float subtree_max_cpu(Node *n)
{
	if (!n) return 0;
	float m = n->proc->pcpu;
	for (Node *ch = n->child; ch; ch = ch->next) {
		float c = subtree_max_cpu(ch); if (c > m) m = c;
	}
	return m;
}

static float subtree_max_mem(Node *n)
{
	if (!n) return 0;
	float m = n->proc->pmem;
	for (Node *ch = n->child; ch; ch = ch->next) {
		float c = subtree_max_mem(ch); if (c > m) m = c;
	}
	return m;
}

static void compute_subtree(Node *n)
{
	for (; n; n = n->next) {
		n->proc->max_cpu = subtree_max_cpu(n);
		n->proc->max_mem = subtree_max_mem(n);
		compute_subtree(n->child);
	}
}

static void free_process(Process *p)
{
	if (!p) return;
	free(p->user); free(p->pri); free(p->ni);
	free(p->bsdtime); free(p->s); free(p->cls); free(p->comm);
	free(p);
}

static void free_tree(Node *n)
{
	if (!n) return;
	free_tree(n->child); free_tree(n->next);
	free_process(n->proc); free(n);
}

/* ================================================================== */
/* Tree model                                                           */
/* ================================================================== */
static void add_to_store(GtkTreeStore *store, Node *n,
                         GtkTreeIter *parent, const char *q)
{
	if (!node_matches(n, q)) return;
	GtkTreeIter it; Process *p = n->proc;
	gtk_tree_store_append(store, &it, parent);
	gtk_tree_store_set(store, &it,
		COL_PID,      p->pid,
		COL_PPID,     p->ppid,
		COL_USER,     p->user    ? p->user    : "",
		COL_PRI,      p->pri     ? p->pri     : "",
		COL_NI,       p->ni      ? p->ni      : "",
		COL_VSZ,      p->vsz,
		COL_TIME,     p->bsdtime ? p->bsdtime : "",
		COL_S,        p->s       ? p->s       : "",
		COL_CLS,      p->cls     ? p->cls     : "",
		COL_PCPU,     p->pcpu,
		COL_PMEM,     p->pmem,
		COL_COMM,     p->comm    ? p->comm    : "",
		COL_SORT_CPU, p->max_cpu,
		COL_SORT_MEM, p->max_mem,
		-1);
	for (Node *ch = n->child; ch; ch = ch->next)
		add_to_store(store, ch, &it, q);
}

static GtkTreeStore *create_store(void)
{
	return gtk_tree_store_new(N_COLS,
		G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_FLOAT, G_TYPE_FLOAT, G_TYPE_STRING,
		G_TYPE_FLOAT, G_TYPE_FLOAT);
}

static void populate_store(GtkTreeStore *store, Node *root, const char *q)
{
	for (Node *n = root; n; n = n->next)
		add_to_store(store, n, NULL, q ? q : "");
}

/* ================================================================== */
/* Node lookup                                                          */
/* ================================================================== */
static Node *find_recursive(Node *n, int pid)
{
	if (!n) return NULL;
	if (n->proc->pid == pid) return n;
	Node *r = find_recursive(n->child, pid);
	return r ? r : find_recursive(n->next, pid);
}

/* ================================================================== */
/* Row click / dialog                                                   */
/* ================================================================== */
static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                              GtkTreeViewColumn *col, gpointer ud)
{
	show_process_dialog(tv, path);
}

static void show_process_dialog(GtkTreeView *tv, GtkTreePath *path)
{
	GtkTreeModel *model = gtk_tree_view_get_model(tv);
	GtkTreeIter iter; int pid = 0; char *comm = NULL;
	if (gtk_tree_model_get_iter(model, &iter, path))
		gtk_tree_model_get(model, &iter, COL_PID, &pid, COL_COMM, &comm, -1);

	gchar *msg = g_strdup_printf("\nProcess:  %s\nPID:      %d",
		comm ? comm : "?", pid);
	g_free(comm);

	GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
		GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s", msg);
	gtk_window_set_title(GTK_WINDOW(dlg), "Process Options");
	gtk_dialog_add_button(GTK_DIALOG(dlg), "Terminate",    GTK_RESPONSE_APPLY);
	gtk_dialog_add_button(GTK_DIALOG(dlg), "Show Details", GTK_RESPONSE_OK);
	gtk_dialog_add_button(GTK_DIALOG(dlg), "Close",        GTK_RESPONSE_CANCEL);
	g_signal_connect(dlg, "response", G_CALLBACK(on_resp_main), GINT_TO_POINTER(pid));
	gtk_widget_set_visible(dlg, TRUE);
	g_free(msg);
}

static void on_resp_confirm(GtkDialog *dlg, gint resp, gpointer ud)
{
	if (resp == GTK_RESPONSE_YES) kill(GPOINTER_TO_INT(ud), SIGTERM);
	gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_resp_main(GtkDialog *dlg, gint resp, gpointer ud)
{
	int pid = GPOINTER_TO_INT(ud);
	if (resp == GTK_RESPONSE_CANCEL) {
		gtk_window_destroy(GTK_WINDOW(dlg)); return;
	}
	if (resp == GTK_RESPONSE_APPLY) {
		gchar *msg = g_strdup_printf("\nTerminate process PID %d?", pid);
		GtkWidget *c = gtk_message_dialog_new(GTK_WINDOW(dlg),
			GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg);
		gtk_window_set_title(GTK_WINDOW(c), "Confirm Terminate");
		g_signal_connect(c, "response", G_CALLBACK(on_resp_confirm), GINT_TO_POINTER(pid));
		gtk_widget_set_visible(c, TRUE);
		g_free(msg); return;
	}
	if (resp == GTK_RESPONSE_OK) {
		Node *n = find_recursive(g_head, pid); if (!n) return;
		Process *p = n->proc;
		gchar *det = g_strdup_printf(
			"\nPID:      %d\nPPID:     %d\nUSER:     %s\nCOMMAND:  %s\n"
			"PRI:      %s\nNI:       %s\nVSZ:      %d kB\nTIME:     %s\n"
			"STATE:    %s\nCLASS:    %s\n%%CPU:     %.1f\n%%MEM:     %.1f",
			p->pid, p->ppid,
			p->user    ? p->user    : "?", p->comm    ? p->comm    : "?",
			p->pri     ? p->pri     : "?", p->ni      ? p->ni      : "?",
			p->vsz,     p->bsdtime ? p->bsdtime : "?",
			p->s       ? p->s       : "?", p->cls     ? p->cls     : "?",
			p->pcpu, p->pmem);
		GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(dlg),
			GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", det);
		gtk_window_set_title(GTK_WINDOW(d), "Process Details");
		g_signal_connect_swapped(d, "response", G_CALLBACK(gtk_window_destroy), d);
		gtk_widget_set_visible(d, TRUE);
		g_free(det);
	}
}
