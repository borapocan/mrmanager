#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <gtk/gtk.h>

#define PROCESS_COUNT "ps -e --no-headers | wc -l"
#define RUNNING_COUNT "ps --no-headers -eo stat | grep 'R' | wc -l"
#define SLEEPING_COUNT "idle=$(ps --no-headers -eo stat | grep -o 'I' | wc -l);sleeping=$(ps --no-headers -eo stat | grep -o 'S' | wc -l); total=$(($idle+$sleeping)); echo $total"
#define STOPPED_COUNT "ps --no-headers -eo stat | grep 'T' | wc -l"
#define ZOMBIE_COUNT "ps --no-headers -eo stat | grep 'Z' | wc -l"
#define MEM_INFO_COMMAND "free -m | awk 'NR==2 {printf \"MiB Mem :  %.1f total,  %.1f free,   %.1f used,   %.1f buff/cache\", $2, $4, $3, $6}'"
#define SWAP_INFO_COMMAND "free -m | awk 'NR==3 {printf \"MiB Swap:   %.1f total,   %.1f free,      %.1f used.  %.1f avail Mem\", $2, $4, $3, $7}'"
#define CPU_INFO_COMMAND "awk '/^cpu / {printf \"%%Cpu: us %d - ni %d - sy %d - id %d - wa %d - hi %d - si %d - st %d - g %d - gn %d\", $2, $3, $4, $5, $6, $7, $8, $9, $10, $11}' /proc/stat"
#define PS_AWK_COMMAND "ps -eo pid,ppid,user,pri,ni,vsz,bsdtime,s,cls,pcpu,pmem,comm --sort +pcpu | awk '{ printf(\"\\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\\\" \\\"%s\", $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12); for(i=13;i<=NF;i++) printf(\" %s\", $i); printf(\"\\\"\\n\") }' | tail -n +2"

typedef struct Process Process;
typedef struct ProcessNode ProcessNode;
typedef struct Label TreeItem;

enum { PID_COLUMN = 0, PPID_COLUMN, USER_COLUMN, PRI_COLUMN, NI_COLUMN, VSZ_COLUMN,
	BSDTIME_COLUMN, S_COLUMN, CLS_COLUMN, PCPU_COLUMN, PMEM_COLUMN, COMM_COLUMN,
	NUMB_COLUMNS };

struct Process {
	int pid;
	int ppid;
	char *user;
	char *pri;
	char *ni;
	int vsz;
	char *bsdtime;
	char *s;
	char *cls;
	float pcpu;
	float pmem;
	char *comm;
};

struct ProcessNode {
	Process *process;
	ProcessNode *next;
	ProcessNode *child;
};

static void activate(GtkApplication* app, gpointer user_data);
static void add_process_to_tree_model(GtkTreeStore *model, ProcessNode *node, GtkTreeIter *parent_iter);
static Process* create_process_from_cmdline(const char *line);
static ProcessNode* find_child_process_by_pid(int pid);
static ProcessNode* find_child_process_recursive(ProcessNode *parent, gint pid);
static ProcessNode* find_child_process_recursive_chain(ProcessNode *parent, gint pid);
static void get_cpu_info(char *cpu_info_str, size_t size);
static void get_mem_info(char *mem_info_str, size_t size);
static ProcessNode *get_process_node_by_pid(ProcessNode *head, int pid);
static void get_swap_info(char *swap_info_str, size_t size);
static void get_uptime(char *uptime_str, size_t size);
static void on_back_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data);
static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);
static void on_show_details_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data);
static void on_terminate_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data);
static void on_yes_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data);
static void print_list(ProcessNode *head, int depth);
static void print_list_with_depth(ProcessNode *head, int depth);
static void push_process_node(ProcessNode **head, ProcessNode *newProcess);
static ProcessNode *search_process_node(ProcessNode *head, ProcessNode *process);
static GtkTreeModel* setup_tree(ProcessNode *head);
static void show_dialog(GtkTreeView *tree_view, GtkTreePath *path, int row_number);
static gboolean update_label_by_second(GtkLabel *label);
static void update_label_text(GtkLabel *label);
static gboolean update_tree_view(GtkWidget *tree_view);

static const char *labels[] = { "PID", "PPID", "USER", "PRI", "NI", "VSZ", "BSDTIME", "S", "CLS", "PCPU", "PMEM", "COMM", NULL };
GtkWidget *window = NULL;
ProcessNode *head = NULL;

int main(int argc, char **argv) {
	GtkApplication *app;
	int status;
	app = gtk_application_new("org.gtk.example", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}

static void activate(GtkApplication* app, gpointer user_data) {
	GtkWidget *tree_view, *vbox, *hbox;
	GtkWidget *scrolled_window, *controls;
	GtkTreeModel *model;

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "Mr.Manager");
	gtk_window_set_default_size(GTK_WINDOW(window), 600, 400);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
	controls = gtk_window_controls_new(GTK_PACK_END);
	gtk_widget_set_margin_start(vbox, 5);
	gtk_widget_set_margin_end(vbox, 5);
	gtk_widget_set_margin_top(vbox, 5);
	gtk_widget_set_margin_bottom(vbox, 5);
	gtk_window_set_child(GTK_WINDOW(window), vbox);
	GtkWidget *frame = gtk_frame_new(NULL);

	char text[1024];
	char uptime[128];
	char cpu_info[256];
	char mem_info[256];
	char swap_info[256];
	FILE *psOutput;

	int total_count, running_count, sleeping_count, stopped_count, zombie_count;
	psOutput = popen(RUNNING_COUNT, "r");
	fscanf(psOutput, "%d", &running_count);
	pclose(psOutput);

	psOutput = popen(SLEEPING_COUNT, "r");
	fscanf(psOutput, "%d", &sleeping_count);
	pclose(psOutput);

	psOutput = popen(STOPPED_COUNT, "r");
	fscanf(psOutput, "%d", &stopped_count);
	pclose(psOutput);

	psOutput = popen(ZOMBIE_COUNT, "r");
	fscanf(psOutput, "%d", &zombie_count);
	pclose(psOutput);

	get_uptime(uptime, sizeof(uptime));
	get_cpu_info(cpu_info, sizeof(cpu_info));
	get_mem_info(mem_info, sizeof(mem_info));
	get_swap_info(swap_info, sizeof(swap_info));

	total_count = running_count + sleeping_count + stopped_count + zombie_count;
	sprintf(text, "Mr.Manager: %sProcesses: Total %d - Running %d - Sleeping %d - Stopped %d - Zombie %d\n%s\n%s\n%s\n",
		uptime, total_count, running_count, sleeping_count, stopped_count, zombie_count, cpu_info, mem_info, swap_info);

	GtkWidget *label = gtk_label_new(text);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_frame_set_child(GTK_FRAME(frame), label);
	gtk_box_append(GTK_BOX(vbox), controls);
	gtk_box_append(GTK_BOX(vbox), frame);

	psOutput = popen(PS_AWK_COMMAND, "r");
	if (psOutput == NULL) {
		perror("Error opening PS_AWK_COMMAND");
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), psOutput) != NULL) {
		Process *newProcess = create_process_from_cmdline(line);
		ProcessNode *newProcessNode = (ProcessNode *)malloc(sizeof(ProcessNode));
		newProcessNode->process = newProcess;
		newProcessNode->next = NULL;
		newProcessNode->child = NULL;
		if (newProcess->ppid == 0) {
			newProcessNode->next = head;
			head = newProcessNode;
		} else {
			push_process_node(&head, newProcessNode);
		}
	}

	if (pclose(psOutput) == -1) {
		perror("Error closing PS_AWK_COMMAND");
		return;
	}

	tree_view = gtk_tree_view_new();
	model = setup_tree(head);
	gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), model);

	for (int i = 0; i < NUMB_COLUMNS; i++) {
		GtkTreeViewColumn *column = gtk_tree_view_column_new();
		GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
		gtk_tree_view_column_pack_start(column, renderer, TRUE);
		gtk_tree_view_column_set_attributes(column, renderer, "text", i, NULL);
		gtk_tree_view_column_set_title(column, *(labels + i));
		gtk_tree_view_column_set_sort_indicator(column, TRUE);
		gtk_tree_view_column_set_sort_column_id(column, i);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
	}

	g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_row_activated), NULL);
	gtk_widget_set_vexpand(tree_view, TRUE);
	scrolled_window = gtk_scrolled_window_new();
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled_window), TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), tree_view);
	gtk_box_append(GTK_BOX(vbox), scrolled_window);
	gtk_window_present(GTK_WINDOW(window));
	gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_view));

	g_timeout_add_seconds(3, (GSourceFunc)update_label_by_second, label);
	g_timeout_add_seconds(3, (GSourceFunc)update_tree_view, tree_view);
}

static gboolean update_tree_view(GtkWidget *tree_view) {
	GtkTreeModel *existing_model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
	GtkTreeSortable *sortable = GTK_TREE_SORTABLE(existing_model);
	gint sort_column_id;
	GtkSortType order;
	gtk_tree_sortable_get_sort_column_id(sortable, &sort_column_id, &order);

	ProcessNode *new_head = NULL;
	FILE *psOutput = popen(PS_AWK_COMMAND, "r");
	if (psOutput == NULL) {
		perror("Error opening PS_AWK_COMMAND");
		return G_SOURCE_CONTINUE;
	}
	char line[1024];
	while (fgets(line, sizeof(line), psOutput) != NULL) {
		Process *newProcess = create_process_from_cmdline(line);
		ProcessNode *newProcessNode = (ProcessNode *)malloc(sizeof(ProcessNode));
		newProcessNode->process = newProcess;
		newProcessNode->next = NULL;
		newProcessNode->child = NULL;
		if (newProcess->ppid == 0) {
			newProcessNode->next = new_head;
			new_head = newProcessNode;
		} else {
			push_process_node(&new_head, newProcessNode);
		}
	}

	if (pclose(psOutput) == -1) {
		perror("Error closing PS_AWK_COMMAND");
		return G_SOURCE_CONTINUE;
	}

	GtkTreeModel *new_model = setup_tree(new_head);
	gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), new_model);
	if (existing_model != NULL)
		g_object_unref(existing_model);
	head = new_head;
	gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_view));
	gtk_tree_sortable_set_sort_column_id(sortable, sort_column_id, order);
	gtk_widget_set_visible(tree_view, TRUE);
	return G_SOURCE_CONTINUE;
}

static GtkTreeModel *setup_tree(ProcessNode *head) {
	GtkTreeStore *model;
	model = gtk_tree_store_new(NUMB_COLUMNS,
		G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_FLOAT, G_TYPE_FLOAT, G_TYPE_STRING);
	ProcessNode *current = head;
	while (current != NULL) {
		add_process_to_tree_model(model, current, NULL);
		current = current->next;
	}
	return GTK_TREE_MODEL(model);
}

static void add_process_to_tree_model(GtkTreeStore *model, ProcessNode *node, GtkTreeIter *parent_iter) {
	GtkTreeIter iter;
	Process *process = node->process;
	gtk_tree_store_append(model, &iter, parent_iter);
	gtk_tree_store_set(model, &iter,
		PID_COLUMN, process->pid, PPID_COLUMN, process->ppid,
		USER_COLUMN, process->user, PRI_COLUMN, process->pri,
		NI_COLUMN, process->ni, VSZ_COLUMN, process->vsz,
		BSDTIME_COLUMN, process->bsdtime, S_COLUMN, process->s,
		CLS_COLUMN, process->cls, PCPU_COLUMN, process->pcpu,
		PMEM_COLUMN, process->pmem, COMM_COLUMN, process->comm, -1);
	if (node->child != NULL) {
		ProcessNode *child = node->child;
		while (child != NULL) {
			add_process_to_tree_model(model, child, &iter);
			child = child->next;
		}
	}
}

static Process* create_process_from_cmdline(const char *line) {
	Process *process = (Process*)malloc(sizeof(Process));
	process->user = NULL; process->pri = NULL; process->ni = NULL;
	process->bsdtime = NULL; process->s = NULL; process->cls = NULL;
	process->comm = NULL;
	sscanf(line, "\"%d\" \"%d\" \"%m[^\"]\" \"%m[^\"]\" \"%m[^\"]\" \"%d\" \"%m[^\"]\" \"%m[^\"]\" \"%m[^\"]\" \"%f\" \"%f\" \"%m[^\"]\"",
		&process->pid, &process->ppid, &process->user, &process->pri,
		&process->ni, &process->vsz, &process->bsdtime, &process->s,
		&process->cls, &process->pcpu, &process->pmem, &process->comm);
	return process;
}

static void print_list(ProcessNode *head, int depth) {
	print_list_with_depth(head, depth);
}

static void print_list_with_depth(ProcessNode *head, int depth) {
	if (head != NULL) {
		for (int i = 0; i < depth; i++) printf("  ");
		printf("PID: %d, PPID: %d, USER: %s\n", head->process->pid, head->process->ppid, head->process->user);
		print_list_with_depth(head->child, depth + 1);
		print_list_with_depth(head->next, depth);
	}
}

static void push_process_node(ProcessNode **head, ProcessNode *newProcess) {
	if (newProcess == NULL || head == NULL) return;
	ProcessNode *parent = search_process_node(*head, newProcess);
	if (parent != NULL) {
		if (parent->child == NULL) {
			parent->child = newProcess;
		} else {
			ProcessNode *temp = parent->child;
			while (temp->next != NULL) temp = temp->next;
			temp->next = newProcess;
		}
	} else {
		newProcess->next = *head;
		*head = newProcess;
	}
}

static ProcessNode *search_process_node(ProcessNode *head, ProcessNode *process) {
	if (head == NULL || process == NULL) return NULL;
	if (head->process->pid == process->process->ppid) return head;
	ProcessNode *childResult = search_process_node(head->child, process);
	if (childResult != NULL) return childResult;
	return search_process_node(head->next, process);
}

static void get_cpu_info(char *cpu_info_str, size_t size) {
	FILE *f = popen(CPU_INFO_COMMAND, "r");
	if (!f) { perror("Error"); exit(EXIT_FAILURE); }
	fgets(cpu_info_str, size, f);
	pclose(f);
}

static ProcessNode *get_process_node_by_pid(ProcessNode *head, int pid) {
	ProcessNode *current = head;
	while (current != NULL) {
		if (current->process != NULL && current->process->pid == pid) return current;
		current = current->next;
	}
	return NULL;
}

static void get_mem_info(char *mem_info_str, size_t size) {
	FILE *f = popen(MEM_INFO_COMMAND, "r");
	if (!f) { perror("Error"); exit(EXIT_FAILURE); }
	fgets(mem_info_str, size, f);
	pclose(f);
}

static void get_swap_info(char *swap_info_str, size_t size) {
	FILE *f = popen(SWAP_INFO_COMMAND, "r");
	if (!f) { perror("Error"); exit(EXIT_FAILURE); }
	fgets(swap_info_str, size, f);
	pclose(f);
}

static void get_uptime(char *uptime_str, size_t size) {
	FILE *f = popen("uptime", "r");
	if (!f) { perror("Error"); exit(EXIT_FAILURE); }
	fgets(uptime_str, size, f);
	pclose(f);
}

static void update_label_text(GtkLabel *label) {
	char text[1024], uptime[128], cpu_info[256], mem_info[256], swap_info[256];
	FILE *psOutput;
	int total_count, running_count, sleeping_count, stopped_count, zombie_count;
	psOutput = popen(RUNNING_COUNT, "r"); fscanf(psOutput, "%d", &running_count); pclose(psOutput);
	psOutput = popen(SLEEPING_COUNT, "r"); fscanf(psOutput, "%d", &sleeping_count); pclose(psOutput);
	psOutput = popen(STOPPED_COUNT, "r"); fscanf(psOutput, "%d", &stopped_count); pclose(psOutput);
	psOutput = popen(ZOMBIE_COUNT, "r"); fscanf(psOutput, "%d", &zombie_count); pclose(psOutput);
	get_uptime(uptime, sizeof(uptime));
	get_mem_info(mem_info, sizeof(mem_info));
	get_cpu_info(cpu_info, sizeof(cpu_info));
	get_swap_info(swap_info, sizeof(swap_info));
	total_count = running_count + sleeping_count + stopped_count + zombie_count;
	sprintf(text, "Mr.Manager: %sProcesses: Total %d - Running %d - Sleeping %d - Stopped %d - Zombie %d\n%s\n%s\n%s\n",
		uptime, total_count, running_count, sleeping_count, stopped_count, zombie_count, cpu_info, mem_info, swap_info);
	gtk_label_set_text(label, text);
}

static gboolean update_label_by_second(GtkLabel *label) {
	update_label_text(label);
	return G_SOURCE_CONTINUE;
}

static ProcessNode* find_child_process_by_pid(int pid) {
	ProcessNode *current = head;
	while (current != NULL) {
		if (current->child != NULL) {
			ProcessNode *child = current->child;
			while (child != NULL) {
				if (child->process->pid == pid) return child;
				child = child->next;
			}
		}
		current = current->next;
	}
	return NULL;
}

static void on_yes_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data) {
	if (response_id == GTK_RESPONSE_YES) {
		gint pid = GPOINTER_TO_INT(user_data);
		g_print("Yes button clicked %d\n", pid);
		kill(pid, SIGTERM);
	}
	gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_terminate_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data) {
	if (response_id == GTK_RESPONSE_APPLY) {
		gint pid = GPOINTER_TO_INT(user_data);
		GtkWidget *terminate_dialog;
		gchar *terminate_text = g_strdup_printf("Are you sure you want to terminate the process with PID: %d?", pid);
		terminate_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog),
			GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
			"%s", terminate_text);
		gtk_window_set_title(GTK_WINDOW(terminate_dialog), "Terminate Process");
		g_signal_connect_swapped(terminate_dialog, "response", G_CALLBACK(gtk_window_destroy), terminate_dialog);
		g_signal_connect(terminate_dialog, "response", G_CALLBACK(on_yes_button_clicked), GINT_TO_POINTER(pid));
		gtk_widget_set_visible(terminate_dialog, TRUE);
		g_free(terminate_text);
	}
}

static void on_back_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data) {
	if (response_id == GTK_RESPONSE_CANCEL)
		gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_show_details_button_clicked(GtkDialog *dialog, gint response_id, gpointer user_data) {
	if (response_id == GTK_RESPONSE_OK) {
		gint pid = GPOINTER_TO_INT(user_data);
		GtkWidget *details_dialog;
		ProcessNode *node = get_process_node_by_pid(head, pid);
		if (node == NULL) node = find_child_process_recursive_chain(head, pid);
		if (node != NULL) {
			gchar *details_text = g_strdup_printf("Details for process:\nPID: %d\nPPID: %d\nUSER: %s\n",
				node->process->pid, node->process->ppid, node->process->user);
			details_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog),
				GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_CLOSE,
				"%s", details_text);
			gtk_window_set_title(GTK_WINDOW(details_dialog), "Process Details");
			g_signal_connect_swapped(details_dialog, "response", G_CALLBACK(gtk_window_destroy), details_dialog);
			gtk_widget_set_visible(details_dialog, TRUE);
			g_free(details_text);
		}
	}
}

static void show_dialog(GtkTreeView *tree_view, GtkTreePath *path, int row_number) {
	GtkWidget *dialog;
	gchar *text, *pid_text;
	GtkTreeModel *model;
	GtkTreeIter iter;
	model = gtk_tree_view_get_model(tree_view);
	int pid = 0;
	if (gtk_tree_model_get_iter(model, &iter, path)) {
		gtk_tree_model_get(model, &iter, PID_COLUMN, &pid, -1);
		pid_text = g_strdup_printf("PID: %d\n", pid);
	} else {
		pid_text = g_strdup("");
	}
	text = g_strdup_printf("Details for Row %d:\n%s", row_number, pid_text);
	dialog = gtk_message_dialog_new(GTK_WINDOW(window),
		GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_NONE,
		"%s", text);
	gtk_window_set_title(GTK_WINDOW(dialog), "Process Options");
	gtk_dialog_add_button(GTK_DIALOG(dialog), "Terminate", GTK_RESPONSE_APPLY);
	gtk_dialog_add_button(GTK_DIALOG(dialog), "Show Details", GTK_RESPONSE_OK);
	gtk_dialog_add_button(GTK_DIALOG(dialog), "Back", GTK_RESPONSE_CANCEL);
	g_signal_connect(dialog, "response", G_CALLBACK(on_terminate_button_clicked), GINT_TO_POINTER(pid));
	g_signal_connect(dialog, "response", G_CALLBACK(on_show_details_button_clicked), GINT_TO_POINTER(pid));
	g_signal_connect(dialog, "response", G_CALLBACK(on_back_button_clicked), NULL);
	gtk_widget_set_visible(dialog, TRUE);
	g_free(text);
	g_free(pid_text);
}

static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
	gint *indices = gtk_tree_path_get_indices(path);
	show_dialog(tree_view, path, indices[0]);
}

static ProcessNode* find_child_process_recursive(ProcessNode *parent, gint pid) {
	if (parent == NULL) return NULL;
	if (parent->process->pid == pid) return parent;
	ProcessNode *child = parent->child;
	while (child != NULL) {
		ProcessNode *result = find_child_process_recursive(child, pid);
		if (result != NULL) return result;
		child = child->next;
	}
	return NULL;
}

static ProcessNode* find_child_process_recursive_chain(ProcessNode *parent, gint pid) {
	ProcessNode *node = find_child_process_recursive(parent, pid);
	while (node == NULL && parent != NULL) {
		parent = parent->next;
		if (parent != NULL) node = find_child_process_recursive(parent, pid);
	}
	return node;
}
