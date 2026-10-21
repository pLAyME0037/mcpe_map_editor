/*
 * mcme GUI — the window, the wire under it, the viewport on top.
 *
 * D2 chose GTK4; D3 keeps the GUI a client of the code the CLI already
 * dispatches to. G2 made that client real: `./mcgui <pack-dir>` reads
 * the pack on a worker through the core's mcme_pack_open (the function
 * `mcme pack info` dispatches to), the wire wakes the loop, and the
 * visit — the only thread allowed to touch GTK (D6) — renders the same
 * four lines the command prints. G3 laid D6's other half: with a model
 * argument the same window also meshes it (mcme_mesh_build, the builder
 * the CLI's mesh verbs all end at, D32) on the worker after the pack's
 * answer lands, stages the delivered mesh on the visit, and the GL
 * area's render callback — the one place any gl* call happens — uploads
 * and draws it. G5 ends the command line's grip: the header's Open…
 * button asks a GtkFileDialog for a directory, and the answer lands in
 * open_pack — the one function every opener calls (argv at startup,
 * the dialog, the smoke gate's --open) — which owns the session's
 * directory and re-runs the chain against it. A pack switched
 * mid-flight cannot cross its wires: every task carries the session
 * generation and stale answers are dropped whole. G6 puts the pack
 * itself on screen: the pack's answer carries every model its index
 * holds (D53's enumeration, "ns:path" in the index's own walk order)
 * into a dropdown, and a pick is a mesh request with its own
 * generation — the last pick wins, stale meshes fall like stale
 * packs. G7 is the twin one layer down: the blockstate index answers
 * its own walk (D53 extended), the pack answer carries "ns:name"
 * rows beside the models, and a block pick walks the blockmesh
 * layer's total answer (mcme_mesh_for_block, D33) instead — a bare
 * name, no states, the missing block standing for the unknown.
 *
 * Flags:
 *   --selftest   run the wire's checks (no display needed) and exit
 *   --smoke      open the window and quit: at 300 ms when no pack was
 *                given; with a pack, once both halves settle — the
 *                pack's answer AND the viewport's frame (when a model
 *                was asked) — under a 3 s never-hang backstop
 *   --open DIR   the dialog's answer without the dialog (smoke gates):
 *                open_pack runs at startup exactly where the callback
 *                would land it
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
static const char *g_pack_arg;    /* argv's directory, borrowed       */
static const char *g_open_dir;    /* --open: the dialog without it    */
static char *g_pack_dir;          /* the session's directory, owned   */
static char *g_mesh_ns;           /* the model split at ':', owned    */
static char *g_mesh_path;
static McmeWire *g_wire;         /* the shell's standing wire (D6)    */
static GtkApplication *g_app;
static int g_exit_code;
static int g_tasks;              /* workers that may still post       */
static int g_pack_gen;           /* session generation: stale falls   */
static int g_mesh_gen;           /* selection generation: stale falls */
static bool g_pack_settled, g_pack_ok;
static bool g_mesh_settled, g_mesh_ok;
static bool g_picker_sync;       /* populate must not fire the pick  */
static bool g_mesh_by_block;     /* the chain re-runs through which  */
static size_t g_models_seen;     /* the pack answer's model count    */
static size_t g_blocks_seen;     /* the pack answer's block count    */
static GtkWidget *g_viewport;
static GtkWidget *g_mesh_label;
static GtkWidget *g_pack_label;
static GtkWidget *g_picker;
static GtkStringList *g_models;
static GtkWidget *g_block_picker;
static GtkStringList *g_blocks;

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
    {
        float cyaw = 0, cpitch = 0, cdist = 0;

        mcme_viewport_camera(g_viewport, &cyaw, &cpitch, &cdist);
        fprintf(stderr,
                "mcgui: smoke: pack=%s models=%zu blocks=%zu mesh=%s "
                "drew=%d tex=%d gl_failed=%d cam=%.2f,%.2f,%.2f\n",
                g_pack_ok ? "ok" : "refused",
                g_models_seen,
                g_blocks_seen,
                g_mesh_ns == NULL ? "not-asked"
                                  : (g_mesh_ok ? "ok" : "refused"),
                mcme_viewport_drew(g_viewport),
                mcme_viewport_textured(g_viewport),
                mcme_viewport_gl_failed(g_viewport),
                cyaw, cpitch, cdist);
    }
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
 * touch a widget. Both callbacks carry the session generation as
 * their userdata: an answer from before the last open belongs to a
 * pack no longer on screen and is dropped whole. */
static void on_mesh_result(void *userdata, McmeMeshResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);

    if (gen != g_mesh_gen) {
        g_tasks--; /* a post is a post: the count follows workers */
        mcme_meshresult_free(result);
        return;
    }
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

/* One list's rows and one shown selection — the populate never fires
 * a pick (the sync guard), and the shown row is the model the chain
 * is about to mesh, in the list its mode asks. */
static void picker_fill(GtkStringList *list, GtkWidget *dd,
                        char **rows, size_t n)
{
    gtk_string_list_splice(list, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(list)),
                           NULL);
    for (size_t i = 0; i < n; ++i)
        gtk_string_list_append(list, rows[i]);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                               GTK_INVALID_LIST_POSITION);
}

static void picker_mirror(GtkStringList *list, GtkWidget *dd,
                          const char *want)
{
    for (guint i = 0;
         i < g_list_model_get_n_items(G_LIST_MODEL(list)); ++i) {
        if (strcmp(gtk_string_list_get_string(list, i), want) == 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
            break;
        }
    }
}

static void on_pack_result(void *userdata, McmePackResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);

    if (gen != g_pack_gen) {
        g_tasks--; /* a post is a post: the count follows workers */
        mcme_packresult_free(result);
        return;
    }
    gtk_label_set_text(GTK_LABEL(g_pack_label),
                       result->text ? result->text : "(no memory)");
    g_pack_ok = result->ok;
    g_pack_settled = true;
    /* The pickers' rows: the pack's own models and blocks, each
       index's walk order (D53, G7). */
    g_models_seen = result->ok ? result->model_count : 0;
    g_blocks_seen = result->ok ? result->block_count : 0;
    g_picker_sync = true;
    picker_fill(g_models, g_picker, result->models, result->model_count);
    picker_fill(g_blocks, g_block_picker, result->blocks,
                result->block_count);
    if (result->ok && g_mesh_ns != NULL) {
        char want[512];

        snprintf(want, sizeof want, "%s:%s", g_mesh_ns, g_mesh_path);
        picker_mirror(g_mesh_by_block ? g_blocks : g_models,
                      g_mesh_by_block ? g_block_picker : g_picker,
                      want);
    }
    g_picker_sync = false;
    if (result->ok && g_mesh_ns != NULL) {
        /* The mesh's worker starts only now: the core's error slot is
           one global, so at any time one worker calls core code and
           copies its answer out (the G2 discipline, kept). */
        bool started = g_mesh_by_block
            ? mcme_meshtask_start_block(g_wire, g_pack_dir, g_mesh_ns,
                                        g_mesh_path, on_mesh_result,
                                        GINT_TO_POINTER(g_mesh_gen))
            : mcme_meshtask_start(g_wire, g_pack_dir, g_mesh_ns,
                                  g_mesh_path, on_mesh_result,
                                  GINT_TO_POINTER(g_mesh_gen));
        if (started) {
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

/* ns:path into owned pieces — the CLI's split rule: the bytes before
 * the colon are the namespace (G3's discipline), and every caller
 * lands here (argv's model, the picker's row). */
static bool split_model(const char *s, char **ns_out, char **path_out)
{
    const char *colon = strchr(s, ':');
    size_t ns_len;
    char *ns, *path;

    if (colon == NULL || colon == s || colon[1] == '\0') return false;
    ns_len = (size_t)(colon - s);
    if (ns_len >= 256) return false;
    ns = g_strndup(s, ns_len);
    path = g_strdup(colon + 1);
    if (ns == NULL || path == NULL) {
        g_free(ns);
        g_free(path);
        return false;
    }
    g_free(*ns_out);
    g_free(*path_out);
    *ns_out = ns;
    *path_out = path;
    return true;
}

/* One pick, either list: the row is "ns:path" (a model) or
 * "ns:name" (a block — the same colon shape), the split is the
 * CLI's, and the request carries the mesh generation: the last pick
 * wins and stale meshes fall like stale packs. */
static void picked_mesh(GtkStringList *list, guint pos, bool by_block)
{
    if (g_picker_sync || pos == GTK_INVALID_LIST_POSITION) return;
    if (!split_model(gtk_string_list_get_string(list, pos),
                     &g_mesh_ns, &g_mesh_path))
        return; /* a row without a colon: nothing to mesh */

    g_mesh_by_block = by_block;
    g_mesh_settled = false;
    g_mesh_ok = false;
    gtk_label_set_text(GTK_LABEL(g_mesh_label), "meshing…");
    if ((by_block
         ? mcme_meshtask_start_block(g_wire, g_pack_dir, g_mesh_ns,
                                     g_mesh_path, on_mesh_result,
                                     GINT_TO_POINTER(++g_mesh_gen))
         : mcme_meshtask_start(g_wire, g_pack_dir, g_mesh_ns,
                               g_mesh_path, on_mesh_result,
                               GINT_TO_POINTER(++g_mesh_gen)))) {
        g_tasks++;
    } else {
        g_mesh_settled = true;
        gtk_label_set_text(GTK_LABEL(g_mesh_label),
                           "mesh task: no memory");
    }
}

static void on_model_picked(GtkDropDown *dd, GParamSpec *ps,
                            gpointer user)
{
    (void)ps;
    (void)user;
    picked_mesh(g_models, gtk_drop_down_get_selected(dd), false);
}

static void on_block_picked(GtkDropDown *dd, GParamSpec *ps,
                            gpointer user)
{
    (void)ps;
    (void)user;
    picked_mesh(g_blocks, gtk_drop_down_get_selected(dd), true);
}

/* The one pack session: every opener lands here — argv at startup,
 * the dialog's answer, the smoke gate's --open. The directory is
 * owned, the chain restarts from the pack, and the generation bump
 * retires any answer still on the road from the previous pack — its
 * callback frees it, still counting the post. The viewport keeps the
 * last good block until the new chain settles. */
static void open_pack(const char *dir)
{
    int gen;

    g_free(g_pack_dir);
    g_pack_dir = g_strdup(dir);
    gen = ++g_pack_gen;
    ++g_mesh_gen; /* a pack switch retires in-flight meshes too */
    g_pack_settled = false;
    g_pack_ok = false;
    g_mesh_settled = false;
    g_mesh_ok = false;
    gtk_label_set_text(GTK_LABEL(g_pack_label), "reading the pack…");
    if (g_mesh_ns != NULL)
        gtk_label_set_text(GTK_LABEL(g_mesh_label), "meshing…");
    if (mcme_packtask_start(g_wire, g_pack_dir, on_pack_result,
                            GINT_TO_POINTER(gen))) {
        g_tasks++;
    } else {
        g_pack_settled = true;
        g_mesh_settled = true;
        gtk_label_set_text(GTK_LABEL(g_pack_label),
                           "pack task: no memory");
    }
}

/* GtkFileDialog's answer, on the loop thread: the chosen directory
 * lands in open_pack exactly where argv and --open land theirs. */
static void on_folder_picked(GObject *source, GAsyncResult *res,
                             gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(dialog, res, &err);

    (void)user_data;
    if (file != NULL) {
        char *path = g_file_get_path(file);

        if (path != NULL) {
            open_pack(path);
            g_free(path);
        }
        g_object_unref(file);
    }
    g_clear_error(&err);
    g_object_unref(dialog);
}

static void on_open_clicked(GtkButton *btn, gpointer user_data)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();

    (void)btn;
    gtk_file_dialog_set_title(dialog, "Open a pack directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(user_data), NULL,
                                  on_folder_picked, NULL);
}

static void on_activate(GtkApplication *app, gpointer user)
{
    GtkWidget *win = gtk_application_window_new(app);
    GtkWidget *bar = gtk_header_bar_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *open = gtk_button_new_with_label("Open…");

    (void)user;
    g_viewport = mcme_viewport_new();
    gtk_widget_set_vexpand(g_viewport, TRUE);
    g_mesh_label = gtk_label_new(g_mesh_ns ? "meshing…" : "");
    g_pack_label = gtk_label_new((g_pack_arg || g_open_dir)
                                 ? "reading the pack…"
                                 : "the shell is up — no world open");
    g_models = gtk_string_list_new(NULL);
    g_picker = gtk_drop_down_new(G_LIST_MODEL(g_models), NULL);
    g_blocks = gtk_string_list_new(NULL);
    g_block_picker = gtk_drop_down_new(G_LIST_MODEL(g_blocks), NULL);
    g_signal_connect(g_picker, "notify::selected",
                     G_CALLBACK(on_model_picked), NULL);
    g_signal_connect(g_block_picker, "notify::selected",
                     G_CALLBACK(on_block_picked), NULL);

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(bar),
                                    gtk_label_new("mcme"));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), open);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_clicked), win);
    gtk_window_set_titlebar(GTK_WINDOW(win), bar);
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    gtk_widget_set_halign(g_pack_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(g_pack_label, GTK_ALIGN_CENTER);
    {
        /* No gtk_label_set_monospace in this GTK4; the Pango way. */
        PangoAttrList *attrs = pango_attr_list_new();

        pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(g_pack_label), attrs);
        pango_attr_list_unref(attrs);
    }
    gtk_box_append(GTK_BOX(box), g_picker);
    gtk_box_append(GTK_BOX(box), g_viewport);
    gtk_box_append(GTK_BOX(box), g_mesh_label);
    gtk_box_append(GTK_BOX(box), g_pack_label);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));

    if (g_open_dir != NULL) {
        open_pack(g_open_dir); /* the dialog's answer, no dialog */
    } else if (g_pack_arg != NULL) {
        open_pack(g_pack_arg);
    } else {
        g_pack_settled = true; /* nothing to wait for */
        g_mesh_settled = true;
    }

    if (g_smoke) {
        /* G8: the camera takes the hand — the smoke drives the same
           road the gestures do, so every frame this run draws
           through the dragged view. */
        mcme_viewport_drag(g_viewport, 80.0, 40.0);
        mcme_viewport_zoom(g_viewport, 2);
        if (g_pack_arg || g_open_dir) {
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
    const char *pos[2];
    int npos = 0;
    int rc;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--selftest") == 0) {
            return mcme_gui_selftest();
        } else if (strcmp(argv[i], "--smoke") == 0) {
            g_smoke = TRUE;
        } else if (strcmp(argv[i], "--open") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mcgui: --open wants a directory\n");
                return 1;
            }
            g_open_dir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: mcgui [--selftest|--smoke] "
                            "[--open dir] [pack-dir [ns:model]]\n");
            return 1;
        } else if (npos < 2) {
            pos[npos++] = argv[i];
        } else {
            fprintf(stderr, "mcgui: one pack directory and one model\n");
            return 1;
        }
    }

    if (g_open_dir != NULL && !g_smoke) {
        fprintf(stderr, "mcgui: --open is the smoke gate's dialog\n");
        return 1;
    }
    /* With --open the dialog stands in for the directory, so the one
       positional left is the model; without it they are pack then
       model (the G3 order, kept). A model needs a pack either way —
       the binding makes that structural. */
    if (g_open_dir != NULL) {
        if (npos > 1) {
            fprintf(stderr, "mcgui: --open stands in for the pack "
                            "directory\n");
            return 1;
        }
        model = npos > 0 ? pos[0] : NULL;
    } else {
        g_pack_arg = npos > 0 ? pos[0] : NULL;
        model = npos > 1 ? pos[1] : NULL;
    }
    if (model != NULL) {
        const char *colon = strchr(model, ':');
        size_t ns_len;

        if (colon == NULL || colon == model || colon[1] == '\0') {
            fprintf(stderr, "mcgui: the model is ns:path\n");
            return 1;
        }
        /* The same split the CLI makes (src/app/main.c): the bytes
           before the colon are the namespace, the rest the path. */
        ns_len = (size_t)(colon - model);
        if (ns_len >= 256) {
            fprintf(stderr, "mcgui: the namespace is too long\n");
            return 1;
        }
        if (!split_model(model, &g_mesh_ns, &g_mesh_path)) {
            fprintf(stderr, "mcgui: the model is ns:path\n");
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
    while (g_tasks > 0)
        g_main_context_iteration(NULL, TRUE);

    g_object_unref(app);
    mcme_wire_free(g_wire);
    g_free(g_pack_dir);
    g_free(g_mesh_ns);
    g_free(g_mesh_path);
    return g_exit_code ? g_exit_code : rc;
}
