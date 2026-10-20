/*
 * mcme GUI — the window, and the wire under it.
 *
 * D2 chose GTK4; D3 keeps the GUI a client of the code the CLI already
 * dispatches to, so this file stays a shell — the application, one
 * window, and the core's event queue (D6) wired to the GTK main loop
 * through gui/wire.c: workers post, the loop wakes, the loop drains.
 * G1 opens the window and proves the wire headless; the client calls
 * come with the features that need them.
 *
 * Flags:
 *   --selftest   run the wire's checks (no display needed) and exit
 *   --smoke      open the window, quit it after 300 ms — the gate a
 *                scripted run (broadwayd, a CI box) can assert on
 */

#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "selftest.h"
#include "wire.h"

static gboolean g_smoke;
static McmeWire *g_wire; /* the shell's standing wire (D6) */

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

static void on_activate(GtkApplication *app, gpointer user)
{
    GtkWidget *win = gtk_application_window_new(app);
    GtkWidget *bar = gtk_header_bar_new();
    GtkWidget *label = gtk_label_new("the shell is up — no world open");

    (void)user;
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(bar),
                                    gtk_label_new("mcme"));
    gtk_window_set_titlebar(GTK_WINDOW(win), bar);
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_window_set_child(GTK_WINDOW(win), label);
    gtk_window_present(GTK_WINDOW(win));

    if (g_smoke) g_timeout_add(300, smoke_quit, app);
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
        } else {
            fprintf(stderr, "usage: mcgui [--selftest|--smoke]\n");
            return 1;
        }
    }

    g_wire = mcme_wire_new();
    if (g_wire == NULL) {
        fprintf(stderr, "mcgui: no event queue\n");
        return 6; /* internal, the CLI's convention carried over */
    }

    app = gtk_application_new("mcme.mapedit", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    /* The flags above are ours; GApplication gets a clean command line. */
    rc = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    mcme_wire_free(g_wire);
    return rc;
}
