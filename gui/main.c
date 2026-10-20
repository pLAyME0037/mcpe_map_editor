/*
 * mcme GUI — the window, and the wire under it.
 *
 * D2 chose GTK4; D3 keeps the GUI a client of the code the CLI already
 * dispatches to. G2 makes that client real: `./mcgui <pack-dir>` reads
 * the pack on a worker through the core's mcme_pack_open (the function
 * `mcme pack info` dispatches to), the wire wakes the loop, and the
 * visit — the only thread allowed to touch GTK (D6) — renders the same
 * four lines the command prints.
 *
 * Flags:
 *   --selftest   run the wire's checks (no display needed) and exit
 *   --smoke      open the window and quit: at 300 ms when no pack was
 *                given, or the moment the pack's answer lands (the
 *                exit code carries it) with a 3 s never-hang backstop
 *   <pack-dir>   the first client call: summarise this pack into the
 *                window
 */

#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "packtask.h"
#include "selftest.h"
#include "wire.h"

static gboolean g_smoke;
static const char *g_pack_dir;
static McmeWire *g_wire;   /* the shell's standing wire (D6) */
static GtkApplication *g_app;
static int g_exit_code;
static bool g_task_running; /* a pack worker may still post */

/* GTK's process-lifetime caches (fontconfig, pango) are never freed by
 * design; with the debug build's ASan that is half a megabyte of noise
 * on every clean exit. The wire's own allocations are still asserted —
 * the selftest runs a recoverable leak check itself, where no GTK has
 * run. */
const char *__asan_default_options(void)
{
    return "detect_leaks=0";
}

static gboolean smoke_quit(gpointer data)
{
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

static gboolean smoke_timeout(gpointer data)
{
    /* The pack's answer never arrived: the gate must not hang. */
    g_exit_code = 1;
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

/* The loop thread — the wire's visit, and the only thread that may
 * touch a widget. */
static void on_pack_result(void *userdata, McmePackResult *result)
{
    GtkWidget *label = userdata;

    gtk_label_set_text(GTK_LABEL(label),
                       result->text ? result->text : "(no memory)");
    g_task_running = false;
    if (g_smoke) {
        g_exit_code = result->ok ? 0 : 1;
        g_application_quit(G_APPLICATION(g_app));
    }
    mcme_packresult_free(result);
}

static void on_activate(GtkApplication *app, gpointer user)
{
    GtkWidget *win = gtk_application_window_new(app);
    GtkWidget *bar = gtk_header_bar_new();
    GtkWidget *label = gtk_label_new(g_pack_dir
                                     ? "reading the pack…"
                                     : "the shell is up — no world open");

    (void)user;
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(bar),
                                    gtk_label_new("mcme"));
    gtk_window_set_titlebar(GTK_WINDOW(win), bar);
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    {
        /* No gtk_label_set_monospace in this GTK4; the Pango way. */
        PangoAttrList *attrs = pango_attr_list_new();

        pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(label), attrs);
        pango_attr_list_unref(attrs);
    }
    gtk_window_set_child(GTK_WINDOW(win), label);
    gtk_window_present(GTK_WINDOW(win));

    if (g_pack_dir) {
        g_task_running = mcme_packtask_start(g_wire, g_pack_dir,
                                             on_pack_result, label);
        if (!g_task_running)
            gtk_label_set_text(GTK_LABEL(label), "pack task: no memory");
    }

    if (g_smoke) {
        g_timeout_add(g_pack_dir ? 3000 : 300,
                      g_pack_dir ? smoke_timeout : smoke_quit, app);
    }
}

int main(int argc, char **argv)
{
    GtkApplication *app;
    int rc;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--selftest") == 0) {
            return mcme_gui_selftest();
        } else if (strcmp(argv[i], "--smoke") == 0) {
            g_smoke = TRUE;
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                    "usage: mcgui [--selftest|--smoke] [pack-dir]\n");
            return 1;
        } else if (g_pack_dir == NULL) {
            g_pack_dir = argv[i];
        } else {
            fprintf(stderr, "mcgui: one pack directory at most\n");
            return 1;
        }
    }

    g_wire = mcme_wire_new();
    if (g_wire == NULL) {
        fprintf(stderr, "mcgui: no event queue\n");
        return 6; /* internal, the CLI's convention carried over */
    }

    app = gtk_application_new("mcme.mapedit", G_APPLICATION_DEFAULT_FLAGS);
    g_app = app;
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    /* The flags above are ours; GApplication gets a clean command line. */
    rc = g_application_run(G_APPLICATION(app), 0, NULL);

    /* The window may close while a worker still reads the pack; the
     * wire's free contract waits for every producer (D6 teardown). */
    while (g_task_running)
        g_main_context_iteration(NULL, TRUE);

    g_object_unref(app);
    mcme_wire_free(g_wire);
    return g_exit_code ? g_exit_code : rc;
}
