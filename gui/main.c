/*
 * mcme GUI — the window, the wire under it, the viewport on top.
 *
 * D2 chose GTK4; D3 keeps the GUI a client of the code the CLI already
 * dispatches to. G2 made that client real: `./mcgui <pack-dir>` reads
 * the pack on a worker through the core's mcme_pack_open (the function
 * `mcme pack info` dispatches to), the wire wakes the loop, and the
 * visit — the only thread allowed to touch GTK (D6) — renders the same
 * four lines the command prints. G3 lays D6's other half: with a model
 * argument the same window also meshes it (mcme_mesh_build, the builder
 * the CLI's mesh verbs all end at, D32) on the worker after the pack's
 * answer lands, stages the delivered mesh on the visit, and the GL
 * area's render callback — the one place any gl* call happens — uploads
 * and draws it.
 *
 * Flags:
 *   --selftest   run the wire's checks (no display needed) and exit
 *   --smoke      open the window and quit: at 300 ms when no pack was
 *                given; with a pack, once both halves settle — the
 *                pack's answer AND the viewport's frame (when a model
 *                was asked) — under a 3 s never-hang backstop
 *   <pack-dir>   the first client call: summarise this pack into the
 *                window
 *   <model>      ns:path of the model to mesh into the viewport (the
 *                pack's argument to `mcme blockmesh`'s builder half)
 *
 * Smoke exit codes: 0 — the pack opened and (if asked) the model
 * meshed and a frame carried it; 1 — a refusal, or the mesh never
 * reached a frame; 2 — GL trouble (context, shader, upload, draw).
 */

#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "meshtask.h"
#include "packtask.h"
#include "selftest.h"
#include "viewport.h"
#include "wire.h"

static gboolean g_smoke;
static const char *g_pack_dir;
static const char *g_mesh_ns;    /* the model split at ':'            */
static const char *g_mesh_path;
static char g_mesh_ns_buf[256];
static McmeWire *g_wire;         /* the shell's standing wire (D6)    */
static GtkApplication *g_app;
static int g_exit_code;
static int g_tasks;              /* workers that may still post       */
static bool g_pack_settled, g_pack_ok;
static bool g_mesh_settled, g_mesh_ok;
static GtkWidget *g_viewport;
static GtkWidget *g_mesh_label;

/* GTK's process-lifetime caches (fontconfig, pango) are never freed by
 * design; with the debug build's ASan that is half a megabyte of noise
 * on every clean exit. The wire's own allocations are still asserted —
 * the selftest runs a recoverable leak check itself, where no GTK has
 * run. */
const char *__asan_default_options(void)
{
    return "detect_leaks=0";
}

/* The smoke settles when the pack's answer landed and — a model was
   asked — the viewport's frame carried it (or GL gave out). Decides
   the exit code from what actually happened, then quits. */
static void maybe_smoke_quit(void)
{
    if (!g_smoke || !g_pack_settled) return;
    if (!g_mesh_settled) return;
    if (g_mesh_ns != NULL && g_mesh_ok &&
        !mcme_viewport_drew(g_viewport) &&
        !mcme_viewport_gl_failed(g_viewport))
        return; /* a built mesh awaits its frame; the poll asks again */

    g_exit_code = 0;
    if (!g_pack_ok) {
        g_exit_code = 1;
    } else if (g_mesh_ns != NULL) {
        if (!g_mesh_ok) g_exit_code = 1;
        else if (mcme_viewport_gl_failed(g_viewport)) g_exit_code = 2;
        else if (!mcme_viewport_drew(g_viewport)) g_exit_code = 1;
    }
    fprintf(stderr,
            "mcgui: smoke: pack=%s mesh=%s drew=%d gl_failed=%d\n",
            g_pack_ok ? "ok" : "refused",
            g_mesh_ns == NULL ? "not-asked"
                              : (g_mesh_ok ? "ok" : "refused"),
            mcme_viewport_drew(g_viewport),
            mcme_viewport_gl_failed(g_viewport));
    g_application_quit(G_APPLICATION(g_app));
}

static gboolean smoke_poll(gpointer data)
{
    (void)data;
    maybe_smoke_quit();
    return G_SOURCE_CONTINUE;
}

static gboolean smoke_timeout(gpointer data)
{
    /* Neither half ever settled: the gate must not hang. */
    fprintf(stderr, "mcgui: smoke: 3s backstop pack_settled=%d "
            "mesh_settled=%d\n", g_pack_settled, g_mesh_settled);
    g_exit_code = 1;
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

static gboolean smoke_quit(gpointer data)
{
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

/* The loop thread — the wire's visit, and the only thread that may
 * touch a widget. */
static void on_mesh_result(void *userdata, McmeMeshResult *result);

static void on_pack_result(void *userdata, McmePackResult *result)
{
    GtkWidget *label = userdata;

    gtk_label_set_text(GTK_LABEL(label),
                       result->text ? result->text : "(no memory)");
    g_pack_ok = result->ok;
    g_pack_settled = true;
    if (result->ok && g_mesh_ns != NULL) {
        /* The mesh's worker starts only now: the core's error slot is
           one global, so at any time one worker calls core code and
           copies its answer out (the G2 discipline, kept). */
        if (mcme_meshtask_start(g_wire, g_pack_dir, g_mesh_ns,
                                g_mesh_path, on_mesh_result, NULL)) {
            g_tasks++;
        } else {
            g_mesh_settled = true;
            gtk_label_set_text(GTK_LABEL(g_mesh_label),
                               "mesh task: no memory");
        }
    } else {
        g_mesh_settled = true; /* no model asked, or the pack refused */
    }
    mcme_packresult_free(result);
    g_tasks--;
    maybe_smoke_quit();
}

static void on_mesh_result(void *userdata, McmeMeshResult *result)
{
    (void)userdata;
    g_mesh_ok = result->ok;
    g_mesh_settled = true;
    if (result->ok) {
        gtk_label_set_text(GTK_LABEL(g_mesh_label), g_pack_dir);
        mcme_viewport_set_mesh(g_viewport, result); /* takes it */
    } else {
        char buf[1024];

        snprintf(buf, sizeof buf, "no mesh: %s",
                 result->text ? result->text : "(no memory)");
        gtk_label_set_text(GTK_LABEL(g_mesh_label), buf);
        mcme_meshresult_free(result);
    }
    g_tasks--;
    maybe_smoke_quit();
}

static void on_activate(GtkApplication *app, gpointer user)
{
    GtkWidget *win = gtk_application_window_new(app);
    GtkWidget *bar = gtk_header_bar_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *label = gtk_label_new(g_pack_dir
                                     ? "reading the pack…"
                                     : "the shell is up — no world open");

    (void)user;
    g_viewport = mcme_viewport_new();
    gtk_widget_set_vexpand(g_viewport, TRUE);
    g_mesh_label = gtk_label_new(g_mesh_ns ? "meshing…" : "");

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
    gtk_box_append(GTK_BOX(box), g_viewport);
    gtk_box_append(GTK_BOX(box), g_mesh_label);
    gtk_box_append(GTK_BOX(box), label);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));

    if (g_pack_dir) {
        if (mcme_packtask_start(g_wire, g_pack_dir, on_pack_result,
                                label)) {
            g_tasks++;
        } else {
            g_pack_settled = true;
            gtk_label_set_text(GTK_LABEL(label), "pack task: no memory");
        }
    } else {
        g_pack_settled = true; /* nothing to wait for */
    }

    if (g_smoke) {
        if (g_pack_dir) {
            g_timeout_add(50, smoke_poll, app);
            g_timeout_add(3000, smoke_timeout, app);
        } else {
            g_timeout_add(300, smoke_quit, app);
        }
    }
}

int main(int argc, char **argv)
{
    GtkApplication *app;
    const char *model = NULL;
    const char *colon;
    int rc;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--selftest") == 0) {
            return mcme_gui_selftest();
        } else if (strcmp(argv[i], "--smoke") == 0) {
            g_smoke = TRUE;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: mcgui [--selftest|--smoke] "
                            "[pack-dir [ns:model]]\n");
            return 1;
        } else if (g_pack_dir == NULL) {
            g_pack_dir = argv[i];
        } else if (model == NULL) {
            model = argv[i];
        } else {
            fprintf(stderr, "mcgui: one pack directory and one model\n");
            return 1;
        }
    }

    if (model != NULL && g_pack_dir == NULL) {
        fprintf(stderr, "mcgui: a model needs a pack directory\n");
        return 1;
    }
    if (model != NULL) {
        size_t ns_len;

        colon = strchr(model, ':');
        if (colon == NULL || colon == model || colon[1] == '\0') {
            fprintf(stderr, "mcgui: the model is ns:path\n");
            return 1;
        }
        /* The same split the CLI makes (src/app/main.c): the bytes
           before the colon are the namespace, the rest the path. */
        ns_len = (size_t)(colon - model);
        if (ns_len >= sizeof g_mesh_ns_buf) {
            fprintf(stderr, "mcgui: the namespace is too long\n");
            return 1;
        }
        memcpy(g_mesh_ns_buf, model, ns_len);
        g_mesh_ns_buf[ns_len] = '\0';
        g_mesh_ns = g_mesh_ns_buf;
        g_mesh_path = colon + 1;
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
    while (g_tasks > 0)
        g_main_context_iteration(NULL, TRUE);

    g_object_unref(app);
    mcme_wire_free(g_wire);
    return g_exit_code ? g_exit_code : rc;
}
