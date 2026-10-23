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
 *   --world DIR --at X Y Z [--dimension <name|n>] [--section]
 *                ask the world for one block — or, with --section,
 *                the whole sub-chunk the coordinates stand in (G11):
 *                after the pack's answer lands, the worker composes
 *                the sub-chunk grid the builder walks and meshes
 *                every cell the cull leaves faces, merged at the
 *                sub-chunk's own origin — and reads the spec the
 *                store spells at those coordinates, reads the six
 *                neighbours standing beside it, and meshes it culled
 *                to them (G10) into
 *                the viewport — a buried block lands as the empty
 *                mesh the world itself would draw, and both halves
 *                of the face count (kept/total) ride the settle
 *                line. The dimension spells the CLI's names here
 *                too since G20 — overworld, nether, end, or a
 *                number, one vocabulary in core read by the bar's
 *                entries and this flag alike — and the three flags
 *                come as one question: together or refused. A refusal's own
 *                words land in the world's label, beside the
 *                pack's. Since G13 these flags are the bar's
 *                answers without the bar, as --open is the
 *                dialog's: the header's World… button picks the
 *                directory, the row of entries (X, Y, Z, dim,
 *                section, Go) presses the same lane the chain
 *                rides — one ask on the road at a time — and the
 *                world's answer lands in its own label: "spec —
 *                kept/total faces, cells" or the refusal's words.
 *                Since G14 the picture itself answers a point: a
 *                still tap on the viewport raycasts the frame's
 *                own matrix (mcme_camera_mvp over the read-back
 *                orbit, the render's own width/height) onto the
 *                staged world mesh — the cell under the struck
 *                face — fills the bar with those world
 *                coordinates and asks through the lane; a push
 *                over four pixels stays with the camera (G8's
 *                road), and the smoke's settle line answers its
 *                center pixel's pick as pick=x,y,z or
 *                pick=none.
 *   --block ns:block
 *                mesh this pack's block instead of a model (G16): the
 *                picker's other row without the bar — the blockstate
 *                index's own walk (mcme_mesh_for_block, D33) — and
 *                the settle line names the door as
 *                ask=block|model|none beside mesh=. One question
 *                with the model positional: give one.
 *   --prune      delete the chunk column the --at cell stands in
 *                (G18) — the lane's second writer, and the
 *                destructive one: mcme_prune behind one door for
 *                both kinds (Bedrock tombstones the column's keys
 *                under the store's LOCK, Java zeroes the chunk's
 *                slot in its region file), and the report's words —
 *                what that world lost, in its own unit — land in
 *                the world's label before the re-ask replaces them
 *                with the world's new answer: the refusal that
 *                says the chunk is gone, the delete's own proof.
 *                No argument: the cell is --at's, the same one
 *                --fill writes, and the two are one question —
 *                give one. The bar's Prune button asks before it
 *                deletes (D65); this flag skips the dialog because
 *                the typed verb is its own confirmation, the CLI's
 *                `chunk prune` never asking either. The settle
 *                line names the door as prune=ok|refused|none
 *                beside fill=.
 *   --convert    turn the open world into the other format (G19):
 *                Bedrock in, Java 1.20 out, written BESIDE the
 *                input as <dir trimmed of trailing slashes>_java —
 *                a sibling by construction, never inside, and
 *                never over anything: mcme_convert_bedrock_java
 *                refuses an output that exists and removes a
 *                failed one whole, so an output world is finished
 *                or absent (convert.h, D12). The whole report
 *                rides the answer — the label spells the headline
 *                and the drops through mcme_convert_drops_text,
 *                the sentence the CLI's line prints — and unlike
 *                the two cell writers it stands: convert never
 *                touches the open world, so there is nothing for
 *                D62's re-ask to prove and the label keeps the
 *                report (a pressed Go still gets its turn). Only
 *                Bedrock input converts today; a Java input is
 *                refused in cmd_convert's own words. No argument,
 *                no dialog (D66): this writer creates, so nothing
 *                asks back — the tooltip names the pattern, the
 *                answer names the directory. Composes with
 *                --fill/--prune in the drains' order (fill,
 *                prune, convert). The settle line names the door
 *                as convert=ok|refused|none.
 *   --stats      count what the open world holds (G21): the CLI's
 *                info/chunks engine read through the door — the
 *                Bedrock store walked key by key (the inventory
 *                promoted to core when this door became its
 *                second client), Java's regions from the probe
 *                that already counted them — the label spelling
 *                the sentence: identity, the chunks, and the
 *                dimensions spelled through mcme_dimension_name
 *                (G20's vocabulary, the number bare when the
 *                name doesn't know it). This door only reads: no
 *                cell — --stats is the one world run --at is not
 *                asked for, it counts the world, not a place in
 *                it, and with no cell given no ask runs behind
 *                it either: the bar's defaults are display, not
 *                a question — no dialog (nothing to ask back: the
 *                readers' family under D66), and its sentence
 *                stands (nothing changed, so nothing re-asks; a
 *                pressed Go still gets its turn). The window's
 *                standing binding holds anyway: a world run
 *                still brings a pack (G9). Composes with
 *                --fill/--prune/--convert through the same lane,
 *                behind every world traffic (writers, the ask,
 *                then the count). The settle line names the
 *                door as stats=ok|refused|none.
 *   <pack-dir>   the first client call: summarise this pack into the
 *                window
 *   <model>      ns:path of the model to mesh into the viewport (the
 *                pack's argument to `mcme blockmesh`'s builder half)
 *
 * Smoke exit codes: 0 — the pack opened and (if asked) the model
 * or block meshed and a frame carried it, or the world answered
 * with faces to show — or, since G18, a landed prune answered
 * with the refusal that says the chunk is gone, the delete's own
 * proof; 1 — a refusal, or a refused --convert or --stats (each
 * its own verdict, whatever the world's ask then answers), or the
 * mesh never reached a frame (a world answer whose cull kept zero
 * faces settles without one — the world said nothing stands
 * there);
 * 2 — GL trouble (context, shader, upload, draw).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "mcme/core.h"  /* mcme_last_error — the flag's refusal words */
#include "mcme/world.h" /* mcme_dimension_parse — the one vocabulary  */

#include "camera.h"
#include "chunktask.h"
#include "converttask.h"
#include "filltask.h"
#include "meshtask.h"
#include "packtask.h"
#include "pick.h"
#include "prunetask.h"
#include "statstask.h"
#include "selftest.h"
#include "viewport.h"
#include "wire.h"
#include "worldbar.h"

static gboolean g_smoke;
static const char *g_pack_arg;    /* argv's directory, borrowed       */
static const char *g_open_dir;    /* --open: the dialog without it    */
static const char *g_fill_arg;    /* --fill: the block spec, borrowed */
static const char *g_block_arg;   /* --block: the block row, borrowed
                                     (G16) */
static gboolean g_prune_arg;      /* --prune: the cell's chunk column,
                                     deleted (G18) */
static gboolean g_convert_arg;    /* --convert: the world to Java
                                     beside itself (G19)            */
static gboolean g_stats_arg;      /* --stats: count what the world
                                     holds (G21)                    */
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
static char *g_world_dir;          /* --world or World…: owned (G13)  */
static int32_t g_world_dim;        /* --dimension: the number the CLI
                                      parses, the window takes it raw */
static long g_wx, g_wy, g_wz;      /* --at: where to stand            */
static bool g_chunk_settled, g_chunk_ok;
static bool g_ask_seen;           /* an ask ever started: the settle's
                                      world= and the exit's world
                                      clauses speak only for runs
                                      that asked (G21 — a stats-only
                                      world run never does)         */
static bool g_at_seen;            /* a cell was ever GIVEN — the
                                      --at flag, the bar's parsed Go
                                      or a cell door (G21): without
                                      one there is no question to
                                      ask, and the bar's defaults
                                      are not a question            */
static bool g_fill_seen, g_fill_ok;  /* the writer's answer (G15):
                                        the settle line's fill=      */
static bool g_prune_seen, g_prune_ok; /* the destructive writer's
                                         answer (G18): the settle
                                         line's prune=              */
static bool g_convert_seen, g_convert_ok; /* the creating writer's
                                             answer (G19): the settle
                                             line's convert=        */
static bool g_stats_seen, g_stats_ok; /* the reader's answer (G21):
                                         the settle line's stats=   */
static char *g_spec_seen;          /* the world's answer, its words   */
static size_t g_chunk_faces;       /* the cull's kept faces (G10)     */
static size_t g_chunk_faces_total; /* the mesh's own, before it       */
static size_t g_chunk_cells;       /* the cells whose faces stand
                                       (G11; the block ask's one)     */
static bool g_section;             /* --section: the whole sub-chunk  */
static bool g_chunk_flight;        /* the lane: one ask or fill up   */
static bool g_chunk_pending;       /* a Go pressed while one is up    */
static bool g_fill_pending;        /* a Fill pressed while one is up  */
static bool g_prune_pending;       /* a Prune pressed while one is up */
static bool g_convert_pending;     /* a Convert pressed while up      */
static bool g_stats_pending;       /* a Stats pressed while up (G21)  */
static GtkWidget *g_world_label;   /* the world's answer, its words   */
static GtkWidget *g_wx_entry;      /* the bar's own --at: X, Y, Z     */
static GtkWidget *g_wy_entry;
static GtkWidget *g_wz_entry;
static GtkWidget *g_dim_entry;     /* the bar's own --dimension       */
static GtkWidget *g_block_entry;   /* the bar's own fill block (G15)  */
static GtkWidget *g_section_check; /* the bar's own --section         */
static float *g_pick_verts;        /* the world's mesh staged for the
                                      point's question (G14): the
                                      frame uploads, the pick reads   */
static uint32_t *g_pick_idx;
static size_t g_pick_nverts, g_pick_nidx;
static bool g_pick_section;        /* which frame the mesh came in   */
static double g_press_x, g_press_y; /* the tap's own down-point      */

/* GTK's process-lifetime caches (fontconfig, pango) are never freed by
 * design; with the debug build's ASan that is half a megabyte of noise
 * on every clean exit. The wire's own allocations are still asserted —
 * the selftest runs a recoverable leak check itself, where no GTK has
 * run. */
const char *__asan_default_options(void)
{
    return "detect_leaks=0";
}

/* The tap's road (G14) — the settle line drives it too. */
static bool pick_world(double px, double py, long out[3]);

/* The smoke settles when the pack's answer landed and — a model was
   asked — the viewport's frame carried it (or GL gave out). Decides
   the exit code from what actually happened, then quits. */
static void maybe_smoke_quit(void)
{
    if (!g_smoke || !g_pack_settled) return;
    if (!g_mesh_settled) return;
    if (g_world_dir != NULL && !g_chunk_settled) return;
    /* A built mesh awaits its frame — unless the world's answer
       legitimately holds none (G10): a cull that kept zero faces
       is the world saying nothing stands here, and the empty mesh
       settles without a draw. The model ask keeps its old
       contract: an empty mesh there is a smoke argument's mistake,
       and the backstop catches it. */
    if ((g_mesh_ns != NULL && g_mesh_ok) ||
        (g_world_dir != NULL && g_chunk_ok &&
         g_chunk_faces > 0)) {
        if (!mcme_viewport_drew(g_viewport) &&
            !mcme_viewport_gl_failed(g_viewport))
            return; /* a built mesh awaits its frame; the poll asks */
    }
    {
        /* One settle line per run (G19's guard): once this decides
           to quit, the line is printed and the exit code fixed —
           results still landing in this iteration would otherwise
           print a second line with newer state, and a gate reading
           two settles is a gate reading nothing. */
        static gboolean settled;

        if (settled) return;
        settled = TRUE;
    }

    g_exit_code = 0;
    if (!g_pack_ok) {
        g_exit_code = 1;
    } else if (g_mesh_ns != NULL) {
        if (!g_mesh_ok) g_exit_code = 1;
        else if (mcme_viewport_gl_failed(g_viewport)) g_exit_code = 2;
        else if (!mcme_viewport_drew(g_viewport)) g_exit_code = 1;
    }
    if (g_world_dir != NULL) {
        /* A landed prune's re-ask refusing is the world's own new
           answer, not the smoke's failure: the chunk the cell
           stood in is gone, and the refusal says so (G18). The
           ask's clauses speak only for runs that asked — a
           stats-only world run has no answer at the cell to
           judge (G21) — while the frame's GL is judged for any
           world run, asked or not. */
        if (g_ask_seen && !g_chunk_ok &&
            !(g_prune_seen && g_prune_ok))
            g_exit_code = 1;
        else if (mcme_viewport_gl_failed(g_viewport)) g_exit_code = 2;
        else if (g_ask_seen && g_chunk_ok && g_chunk_faces > 0 &&
                 !mcme_viewport_drew(g_viewport)) g_exit_code = 1;
    }
    /* The creating writer's own verdict (G19): a refused convert
       fails the smoke whatever the world's ask then answers. */
    if (g_convert_seen && !g_convert_ok) g_exit_code = 1;
    /* The reader's own verdict (G21): a refused count fails the
       smoke on its words — every door owns its settle clause. */
    if (g_stats_seen && !g_stats_ok) g_exit_code = 1;
    {
        float cyaw = 0, cpitch = 0, cdist = 0;
        long pick[3];
        char picks[32];

        mcme_viewport_camera(g_viewport, &cyaw, &cpitch, &cdist);
        /* G14's tap, answered without a hand: the widget's own
           center pixel down the frame's matrix — the same road a
           click calls, its cell in world coordinates, or the
           honest none (nothing staged, nothing under the point). */
        if (pick_world((double)gtk_widget_get_width(g_viewport) / 2.0,
                       (double)gtk_widget_get_height(g_viewport) / 2.0,
                       pick))
            snprintf(picks, sizeof picks, "%ld,%ld,%ld",
                     pick[0], pick[1], pick[2]);
        else
            snprintf(picks, sizeof picks, "none");
        fprintf(stderr,
                "mcgui: smoke: pack=%s models=%zu blocks=%zu mesh=%s "
                "ask=%s world=%s spec=%s faces=%zu/%zu cells=%zu "
                "drew=%d tex=%d gl_failed=%d cam=%.2f,%.2f,%.2f "
                "pick=%s fill=%s prune=%s convert=%s stats=%s\n",
                g_pack_ok ? "ok" : "refused",
                g_models_seen,
                g_blocks_seen,
                g_mesh_ns == NULL ? "not-asked"
                                  : (g_mesh_ok ? "ok" : "refused"),
                g_mesh_ns == NULL ? "none"
                                  : (g_mesh_by_block ? "block"
                                                     : "model"),
                !g_ask_seen ? "not-asked"
                            : (g_chunk_ok ? "ok" : "refused"),
                g_chunk_ok && g_spec_seen != NULL ? g_spec_seen : "-",
                g_chunk_faces,
                g_chunk_faces_total,
                g_chunk_cells,
                mcme_viewport_drew(g_viewport),
                mcme_viewport_textured(g_viewport),
                mcme_viewport_gl_failed(g_viewport),
                cyaw, cpitch, cdist, picks,
                g_fill_seen ? (g_fill_ok ? "ok" : "refused") : "none",
                g_prune_seen ? (g_prune_ok ? "ok" : "refused")
                             : "none",
                g_convert_seen ? (g_convert_ok ? "ok" : "refused")
                               : "none",
                g_stats_seen ? (g_stats_ok ? "ok" : "refused")
                             : "none");
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
        /* The frame now carries the picker's mesh: no world stands
           behind it, so nothing to pick — the stage goes with the
           picture (G14). A refused or stale mesh leaves the world's
           answer on screen, and its stage with it. */
        g_free(g_pick_verts);
        g_free(g_pick_idx);
        g_pick_verts = NULL;
        g_pick_idx = NULL;
        g_pick_nverts = 0;
        g_pick_nidx = 0;
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

/* The world's own block, asked once per pack session (G9) — and
 * since G13 re-askable from the bar, so exactly one ask rides the
 * road at a time: the answer's landing clears the lane, and a Go
 * pressed while one was up takes the next lane instead of racing
 * it (the pool runs workers side by side). Its answer is a mesh
 * like any other: it carries the mesh generation — open_pack
 * bumps that counter on every pack switch and every pick bumps it
 * too, so a world answer still on the road when either lands falls
 * whole (D52, one counter at the viewport's door) — while the
 * pending ask posts with the generation that stands by then. */
static void queue_chunk_ask(void);
static void queue_fill(void);  /* the lane's writer (G15) */
static void start_fill(void);
static void start_prune(void);
static void start_convert(void);
static void start_stats(void);    /* the reader behind them (G21)   */

static void on_chunk_result(void *userdata, McmeChunkResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);

    g_tasks--; /* a post is a post: the count follows workers */
    g_chunk_flight = false;
    if (gen != g_mesh_gen) {
        mcme_chunkresult_free(result); /* the lane clears anyway */
    } else {
        char txt[256];

        g_chunk_settled = true;
        g_chunk_ok = result->ok;
        if (result->ok) {
            g_free(g_spec_seen);
            g_spec_seen = g_strdup(result->spec);
            g_chunk_faces = result->faces;
            g_chunk_faces_total = result->faces_total;
            g_chunk_cells = result->cells;
            /* G14 keeps its own stage of the world's geometry: the
               same stream the frame uploads, the page withheld (the
               pick reads positions alone), staged here while the
               result still holds the mesh — set_mesh takes the
               result after. A refusal or a stale answer leaves the
               stage as the viewport stands (they leave that too). */
            g_free(g_pick_verts);
            g_free(g_pick_idx);
            g_pick_verts = NULL;
            g_pick_idx = NULL;
            g_pick_nverts = 0;
            g_pick_nidx = 0;
            g_pick_section = g_section;
            mcme_viewport_stage(&result->mesh->mesh, NULL, NULL,
                                &g_pick_verts, &g_pick_idx,
                                &g_pick_nverts, &g_pick_nidx);
            mcme_viewport_set_mesh(g_viewport, result->mesh); /* takes it */
            result->mesh = NULL;
            snprintf(txt, sizeof txt, "%s — %zu/%zu faces, %zu cells",
                     g_spec_seen ? g_spec_seen : "?",
                     g_chunk_faces, g_chunk_faces_total, g_chunk_cells);
            gtk_label_set_text(GTK_LABEL(g_world_label), txt);
        } else {
            gtk_label_set_text(GTK_LABEL(g_world_label),
                               result->text ? result->text : "(no memory)");
        }
        mcme_chunkresult_free(result);
    }
    if (g_fill_pending) {
        g_fill_pending = false;
        start_fill(); /* the writer's turn: its answer re-asks */
    } else if (g_prune_pending) {
        g_prune_pending = false;
        start_prune(); /* the writers drain fill-first (G18) */
    } else if (g_convert_pending) {
        g_convert_pending = false;
        start_convert(); /* last writer: it reads what they did (G19) */
    } else if (g_chunk_pending) {
        g_chunk_pending = false;
        queue_chunk_ask(); /* the landed answer steps aside for the Go */
    } else if (g_stats_pending) {
        g_stats_pending = false;
        start_stats(); /* the reader waits behind the traffic (G21) */
    }
    maybe_smoke_quit();
}

static void start_chunk_ask(void)
{
    g_ask_seen = true; /* an ask ever runs: the settle's world=
                          and the exit's world clauses speak for
                          it from here (G21) */
    g_chunk_settled = false;
    g_chunk_ok = false;
    g_chunk_faces = 0;
    g_chunk_faces_total = 0;
    g_chunk_cells = 0;
    if (mcme_chunktask_start(g_wire, g_world_dir, g_pack_dir,
                             g_world_dim, g_wx, g_wy, g_wz,
                             g_section,
                             on_chunk_result,
                             GINT_TO_POINTER(g_mesh_gen))) {
        g_tasks++;
        g_chunk_flight = true;
    } else {
        g_chunk_settled = true;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "chunk task: no memory");
    }
}

/* The ask through one lane (G13): the flags' startup chain and the
 * bar's Go both land here. One ask rides the road at a time — a Go
 * pressed mid-flight waits for the landed answer instead of racing
 * it (the pool runs workers side by side) — and a Go before the
 * pack stands waits too: the pack's answer starts the ask exactly
 * where the flags' chain always started it. */
static void queue_chunk_ask(void)
{
    if (g_world_dir == NULL) return;
    /* No cell ever given: no question stands — the flags' startup
       chain may arrive here on a world run that asked for none
       (G21's --stats relaxes --at), and the bar's default numbers
       are display, not intent. The hand's roads (Go, the cell
       doors, the world dialog's own world_go) all mark the cell
       given before they queue. */
    if (!g_at_seen) return;
    if (g_chunk_flight) {
        g_chunk_pending = true;
        return;
    }
    if (!g_pack_settled || !g_pack_ok || g_pack_dir == NULL)
        return;
    start_chunk_ask();
}

/* The writer's answer (G15): a landed fill re-asks — the label
 * shows the world's NEW answer, never the write's claim, so the
 * proof is the changed answer. A refusal stands on its own words
 * (nothing changed; the ask would only overwrite them), and the
 * write's own truth rides the settle line whatever moved
 * underneath — it is world-state, not pack-state — while the
 * message keeps the generation discipline (D52). */
static void on_fill_result(void *userdata, McmeFillResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);
    bool stale = gen != g_mesh_gen;
    bool ok = result->ok;

    g_tasks--; /* a post is a post: the count follows workers */
    g_chunk_flight = false;
    g_fill_seen = true;
    g_fill_ok = ok;
    if (!ok && !stale) {
        g_chunk_settled = true;
        g_chunk_ok = false;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           result->text ? result->text : "(no memory)");
    }
    mcme_fillresult_free(result);
    if (g_fill_pending) {
        g_fill_pending = false;
        start_fill(); /* the latest cell wins the road */
    } else if (g_prune_pending) {
        g_prune_pending = false;
        start_prune(); /* the writers drain before the ask (G18) */
    } else if (g_convert_pending) {
        g_convert_pending = false;
        start_convert(); /* last writer: it reads what they did (G19) */
    } else if (g_chunk_pending || ok) {
        g_chunk_pending = false;
        queue_chunk_ask(); /* the landed fill's own proof (G15) */
    } else if (g_stats_pending) {
        g_stats_pending = false;
        start_stats(); /* the reader waits behind the traffic (G21) */
    }
    maybe_smoke_quit();
}

/* One writer on the lane (G15): the bar's cell and block, the
 * worker's own copy of them at post (G13's rule). The lane is
 * busy or the answer comes back settled the same way the ask's
 * does. */
static void start_fill(void)
{
    g_chunk_settled = false;
    g_chunk_ok = false;
    if (mcme_filltask_start(g_wire, g_world_dir, g_world_dim,
                            g_wx, g_wy, g_wz,
                            gtk_editable_get_text(
                                GTK_EDITABLE(g_block_entry)),
                            on_fill_result,
                            GINT_TO_POINTER(g_mesh_gen))) {
        g_tasks++;
        g_chunk_flight = true;
    } else {
        g_chunk_settled = true;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "fill task: no memory");
    }
}

/* The fill's door (G15): the bar's cell and block through the
 * same lane the ask rides — one store user ever, so a fill
 * pressed mid-ask waits its turn and a Go pressed mid-fill is not
 * lost (the landed fill re-asks anyway). No world, nothing to
 * write: the refusal lands in the world's own label. */
static void queue_fill(void)
{
    g_at_seen = true; /* a cell door: the bar's cell is the question (G21) */
    if (g_world_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "no world open — nothing to fill");
        return;
    }
    if (g_chunk_flight) {
        g_fill_pending = true;
        return;
    }
    start_fill();
}

static void on_fill_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    queue_fill();
}

static void on_prune_result(void *userdata, McmePruneResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);
    bool stale = gen != g_mesh_gen;
    bool ok = result->ok;

    g_tasks--; /* a post is a post: the count follows workers */
    g_chunk_flight = false;
    g_prune_seen = true;
    g_prune_ok = ok;
    if (!ok && !stale) {
        g_chunk_settled = true;
        g_chunk_ok = false;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           result->text ? result->text : "(no memory)");
    } else if (ok && !stale) {
        char txt[160];

        /* The report's own words, in the unit that world counts in
           (prune.h): Bedrock tombstones keys, Java punches region
           files, and an empty selection wrote nothing — its own
           contract, said plain. The re-ask that follows replaces
           the label with the world's new answer: the refusal that
           says the chunk is gone — the delete proving itself the
           way the fill's write did, by being read back (D62,
           inverted: here the read that fails is the proof). */
        if (result->chunks == 0)
            snprintf(txt, sizeof txt,
                     "nothing stored in that chunk — nothing pruned");
        else if (result->keys > 0)
            snprintf(txt, sizeof txt, "pruned %ld chunk%s (%ld keys)",
                     (long)result->chunks,
                     result->chunks == 1 ? "" : "s",
                     (long)result->keys);
        else
            snprintf(txt, sizeof txt,
                     "pruned %ld chunk%s (%ld region file%s)",
                     (long)result->chunks,
                     result->chunks == 1 ? "" : "s",
                     (long)result->region_files,
                     result->region_files == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(g_world_label), txt);
    }
    mcme_pruneresult_free(result);
    if (g_fill_pending) {
        g_fill_pending = false;
        start_fill(); /* the writers drain fill-first (G18) */
    } else if (g_prune_pending) {
        g_prune_pending = false;
        start_prune(); /* the latest cell wins the road */
    } else if (g_convert_pending) {
        g_convert_pending = false;
        start_convert(); /* last writer: it reads what they did (G19) */
    } else if (g_chunk_pending || ok) {
        g_chunk_pending = false;
        queue_chunk_ask(); /* the landed prune's own proof: the world
                              says the chunk is gone */
    } else if (g_stats_pending) {
        g_stats_pending = false;
        start_stats(); /* the reader waits behind the traffic (G21) */
    }
    maybe_smoke_quit();
}

/* The destructive writer takes the lane the fill holds (G18): one
 * store user ever, the bar's cell's chunk column, the worker's own
 * copy of them at post (G13's rule). Y never rides: a chunk is all
 * of its Y (D13). */
static void start_prune(void)
{
    g_chunk_settled = false;
    g_chunk_ok = false;
    if (mcme_prunetask_start(g_wire, g_world_dir, g_world_dim,
                             g_wx, g_wz,
                             on_prune_result,
                             GINT_TO_POINTER(g_mesh_gen))) {
        g_tasks++;
        g_chunk_flight = true;
    } else {
        g_chunk_settled = true;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "prune task: no memory");
    }
}

/* The prune's door (G18): the bar's cell's chunk column, through
 * the same lane the ask rides — one store user ever, so a prune
 * pressed mid-ask waits its turn and a Go or Fill pressed mid-
 * prune is not lost (the landed prune re-asks anyway). No world,
 * nothing to delete: the refusal lands in the world's own label.
 * The bar's button asks first — a pruned column is the game's own
 * data gone, unfilled-back — while the --prune flag skips the
 * dialog because the typed verb is its own confirmation, the CLI's
 * `chunk prune` never asking either (D65). */
static void queue_prune(void)
{
    g_at_seen = true; /* a cell door: the bar's cell is the question (G21) */
    if (g_world_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "no world open — nothing to prune");
        return;
    }
    if (g_chunk_flight) {
        g_prune_pending = true;
        return;
    }
    start_prune();
}

/* G18: the hand's confirmation, which the smoke never presses. The
 * response is the button's index — the docs' own contract for
 * choose_finish (read at docs.gtk.org, not the guessed
 * GTK_RESPONSE_* constants): Delete stands at index 0, Escape's
 * cancel at index 1, and any other answer is no. */
static void on_prune_alert_done(GObject *source, GAsyncResult *res,
                                gpointer user_data)
{
    GError *err = NULL;
    int response = gtk_alert_dialog_choose_finish(
        GTK_ALERT_DIALOG(source), res, &err);

    (void)user_data;
    if (err != NULL) g_error_free(err);
    if (response == 0) queue_prune();
}

static void on_prune_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(user_data);
    GtkAlertDialog *alert;
    const char *buttons[] = { "Delete", "Cancel", NULL };
    char detail[320];

    (void)btn;
    if (g_world_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "no world open — nothing to prune");
        return;
    }
    snprintf(detail, sizeof detail,
             "Every block of the column's Y is deleted from the "
             "world at %s — the chunk the game saves, gone. This "
             "cannot be undone.", g_world_dir);
    /* The column the tapped cell stands in, floor division by 16
       the chunk verbs' own rule (D13) — the number the deletion
       names, not the cell the bar holds. */
    alert = gtk_alert_dialog_new("Delete the chunk at (%ld, %ld)?",
                                 (long)(g_wx >> 4), (long)(g_wz >> 4));
    gtk_alert_dialog_set_detail(alert, detail);
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);   /* Escape */
    gtk_alert_dialog_set_default_button(alert, 1); /* Return is never
                                                       the destructive
                                                       one */
    gtk_alert_dialog_choose(alert, win, NULL, on_prune_alert_done,
                            NULL);
    g_object_unref(alert);
}

static void on_convert_result(void *userdata, McmeConvertResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);
    bool stale = gen != g_mesh_gen;
    bool ok = result->ok;

    g_tasks--; /* a post is a post: the count follows workers */
    g_chunk_flight = false;
    g_convert_seen = true;
    g_convert_ok = ok;
    if (!ok && !stale) {
        g_chunk_settled = true;
        g_chunk_ok = false;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           result->text ? result->text : "(no memory)");
    } else if (ok && !stale) {
        char drops[600];
        char txt[900];

        /* The report's words in the shell's own spelling: the
           headline, then mcme_convert_drops_text — the sentence
           both clients print (G19), so the window cannot drift from
           the CLI's line. This writer's answer STANDS: convert
           writes a new directory beside the world and never
           touches it, so the open world's answer at the cell is
           unchanged and D62's re-ask has nothing new to say (a Go
           pressed meanwhile still gets its turn in the drain). */
        snprintf(txt, sizeof txt, "converted to %s: %zu chunk%s, "
                 "%zu block%s", result->out_dir,
                 result->report.chunks,
                 result->report.chunks == 1 ? "" : "s",
                 result->report.blocks,
                 result->report.blocks == 1 ? "" : "s");
        if (mcme_convert_drops_text(&result->report, drops,
                                    sizeof drops) > 0) {
            size_t at = strlen(txt);

            snprintf(txt + at, sizeof txt - at, "; %s", drops);
        }
        gtk_label_set_text(GTK_LABEL(g_world_label), txt);
    }
    mcme_convertresult_free(result);
    if (g_fill_pending) {
        g_fill_pending = false;
        start_fill(); /* the writers drain fill-first (G19) */
    } else if (g_prune_pending) {
        g_prune_pending = false;
        start_prune();
    } else if (g_convert_pending) {
        g_convert_pending = false;
        start_convert(); /* the latest world wins the road */
    } else if (g_chunk_pending) {
        g_chunk_pending = false;
        /* Only a pressed Go re-asks after this writer: it changed
           nothing in the open world, so the label keeps the
           report's own words. */
        queue_chunk_ask();
    } else if (g_stats_pending) {
        g_stats_pending = false;
        start_stats(); /* the reader waits behind the traffic (G21) */
    }
    maybe_smoke_quit();
}

/* The creating writer takes the lane the others hold (G19): one
 * world user ever — convert READS the input whole, so it rides the
 * same exclusivity as the writes — the worker's own copy of the
 * directory at post (G13's rule). */
static void start_convert(void)
{
    g_chunk_settled = false;
    g_chunk_ok = false;
    if (mcme_converttask_start(g_wire, g_world_dir,
                               on_convert_result,
                               GINT_TO_POINTER(g_mesh_gen))) {
        g_tasks++;
        g_chunk_flight = true;
    } else {
        g_chunk_settled = true;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "convert task: no memory");
    }
}

/* The convert's door (G19): the open world to Java beside itself,
 * through the same lane. No dialog (D66): this writer never
 * touches its input and refuses to overwrite anything, so nothing
 * asks back — and the name it writes is spoken in the answer, not
 * chosen. No world, nothing to convert: the refusal lands in the
 * world's own label. */
static void queue_convert(void)
{
    if (g_world_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "no world open — nothing to convert");
        return;
    }
    if (g_chunk_flight) {
        g_convert_pending = true;
        return;
    }
    start_convert();
}

static void on_convert_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    queue_convert(); /* creates, never destroys: no asking back */
}

static void on_stats_result(void *userdata, McmeStatsResult *result)
{
    int gen = GPOINTER_TO_INT(userdata);
    bool stale = gen != g_mesh_gen;
    bool ok = result->ok;

    g_tasks--; /* a post is a post: the count follows workers */
    g_chunk_flight = false;
    g_stats_seen = true;
    g_stats_ok = ok;
    /* The reader's answer is final: it changed nothing, so nothing
       re-asks after it (the convert's rule, held wider) — it
       settles the wait itself, because a stats-only run has no
       ask behind it to settle instead. The ask's own verdict under
       it is left alone: world= speaks for the ask, stats= for
       this door. */
    g_chunk_settled = true;
    if (!ok && !stale) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           result->text ? result->text : "(no memory)");
    } else if (ok && !stale) {
        char txt[600];
        size_t at;

        /* The counts' own sentence: identity first, then the
           numbers — bedrock's store walked key by key, its
           dimensions spelled through mcme_dimension_name (G20's
           vocabulary, the number bare when the name doesn't know
           it); java's regions from the probe that counted them.
           Both spellings are the CLI's inventory printing its own
           rows, one engine two faces. */
        if (result->name != NULL)
            snprintf(txt, sizeof txt, "%s — ", result->name);
        else
            txt[0] = '\0';
        at = strlen(txt);
        if (result->kind == MCME_WORLD_JAVA) {
            snprintf(txt + at, sizeof txt - at,
                     "java: %zu chunk%s in %zu region%s",
                     result->chunks, result->chunks == 1 ? "" : "s",
                     result->region_files,
                     result->region_files == 1 ? "" : "s");
        } else {
            snprintf(txt + at, sizeof txt - at,
                     "bedrock: %zu chunk%s in %zu key%s",
                     result->chunks, result->chunks == 1 ? "" : "s",
                     result->keys, result->keys == 1 ? "" : "s");
        }
        /* The rows spell either engine's dimensions (G22: java's
           trees answer the same question as bedrock's keys) — the
           sentence's one shape for both: identity, then what each
           dimension holds, spelled through mcme_dimension_name with
           the number bare where the name doesn't know it. */
        at = strlen(txt);
        {
            size_t rows_at = at;

            for (size_t i = 0; i < result->dims &&
                              at + 40 < sizeof txt; ++i) {
                const char *dim = mcme_dimension_name(
                    result->by_dim[i].dimension);
                char dimtxt[32];

                if (dim != NULL)
                    snprintf(dimtxt, sizeof dimtxt, "%s", dim);
                else
                    snprintf(dimtxt, sizeof dimtxt, "%d",
                             (int)result->by_dim[i].dimension);
                snprintf(txt + at, sizeof txt - at, "%s%s %zu",
                         i == 0 ? " (" : ", ", dimtxt,
                         result->by_dim[i].chunks);
                at = strlen(txt);
            }
            if (at > rows_at && at + 2 < sizeof txt)
                snprintf(txt + at, sizeof txt - at, ")");
        }
        gtk_label_set_text(GTK_LABEL(g_world_label), txt);
    }
    mcme_statsresult_free(result);
    if (g_fill_pending) {
        g_fill_pending = false;
        start_fill(); /* the writers drain fill-first (G18) */
    } else if (g_prune_pending) {
        g_prune_pending = false;
        start_prune();
    } else if (g_convert_pending) {
        g_convert_pending = false;
        start_convert();
    } else if (g_chunk_pending) {
        g_chunk_pending = false;
        /* Only a pressed Go re-asks after this reader: nothing
           changed in the open world, so the label keeps the
           count's own sentence. */
        queue_chunk_ask();
    } else if (g_stats_pending) {
        g_stats_pending = false;
        start_stats(); /* the latest count wins the road */
    }
    maybe_smoke_quit();
}

/* The count takes the lane the traffic holds (G21): the walk
 * OPENS the store and reads every key, so it rides the same
 * exclusivity as the writes and the ask — one world user ever,
 * side by side otherwise (the pool runs workers concurrently).
 * g_chunk_ok is left standing: the ask's verdict, not this door's
 * — stats= carries this door's own. */
static void start_stats(void)
{
    g_chunk_settled = false;
    if (mcme_statstask_start(g_wire, g_world_dir,
                             on_stats_result,
                             GINT_TO_POINTER(g_mesh_gen))) {
        g_tasks++;
        g_chunk_flight = true;
    } else {
        g_chunk_settled = true;
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "stats task: no memory");
    }
}

/* The count's door (G21): the open world, counted. No cell — this
 * door reads the world, not a place in it — no dialog (it breaks
 * nothing, so nothing asks back: the readers' family under D66),
 * and no world, nothing to count: the refusal lands in the
 * world's own label. */
static void queue_stats(void)
{
    if (g_world_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "no world open — nothing to count");
        return;
    }
    if (g_chunk_flight) {
        g_stats_pending = true;
        return;
    }
    start_stats();
}

static void on_stats_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    queue_stats(); /* reads, never writes: no asking back */
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
    /* The world's block rides this session's answer too: asked when
       the pack landed (its mesh needs the pack's indices), settled
       same-shaped as the mesh when no ask stands (G9) — and since
       G13 through the same lane as the bar, which may already hold
       one in flight (its answer lands stale and clears the road
       for the chain's ask). A refused pack answers the world with
       the same words. */
    if (result->ok && g_world_dir != NULL) {
        queue_chunk_ask();
    } else {
        g_chunk_settled = true;
        if (g_world_dir != NULL)
            gtk_label_set_text(GTK_LABEL(g_world_label),
                               "the pack refused — no mesh for the world");
    }
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

/* The bar's one press (G13): the entries through the shared
 * validation, then the same lane the flags' chain rides. The
 * refusal lands in the world's own label and nothing posts; a pack
 * not yet standing is not a refusal — the chain starts the ask
 * when the pack's answer lands, with exactly these coordinates. */
static void world_go(void)
{
    long x, y, z;
    int32_t dim;
    const char *why = NULL;

    if (!mcme_worldbar_ask(g_world_dir,
                           gtk_editable_get_text(GTK_EDITABLE(g_wx_entry)),
                           gtk_editable_get_text(GTK_EDITABLE(g_wy_entry)),
                           gtk_editable_get_text(GTK_EDITABLE(g_wz_entry)),
                           gtk_editable_get_text(GTK_EDITABLE(g_dim_entry)),
                           &x, &y, &z, &dim, &why)) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           why ? why : "the bar has no answer");
        return;
    }
    g_wx = x;
    g_wy = y;
    g_wz = z;
    g_world_dim = dim;
    g_at_seen = true; /* the bar's cell parsed and standing: the
                         hand has given a question (G21) */
    g_section = gtk_check_button_get_active(
        GTK_CHECK_BUTTON(g_section_check));
    if (g_pack_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "open a pack first — the world's faces need it");
        return;
    }
    if (!g_pack_settled) {
        gtk_label_set_text(GTK_LABEL(g_world_label), "reading the pack…");
        return;
    }
    if (!g_pack_ok) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "the pack refused — no mesh for the world");
        return;
    }
    gtk_label_set_text(GTK_LABEL(g_world_label), "asking…");
    queue_chunk_ask();
}

/* GtkFileDialog's answer for the WORLD (G13), on the loop thread:
 * the chosen directory lands where --world lands it, and the bar's
 * own press follows — the entries as they stand, one road. */
static void on_world_folder_picked(GObject *source, GAsyncResult *res,
                                   gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(dialog, res, &err);

    (void)user_data;
    if (file != NULL) {
        char *path = g_file_get_path(file);

        if (path != NULL) {
            g_free(g_world_dir);
            g_world_dir = path; /* the dialog's answer: owned (G13) */
            world_go();
        }
        g_object_unref(file);
    }
    g_clear_error(&err);
    g_object_unref(dialog);
}

static void on_world_clicked(GtkButton *btn, gpointer user_data)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();

    (void)btn;
    gtk_file_dialog_set_title(dialog, "Open a world directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(user_data), NULL,
                                  on_world_folder_picked, NULL);
}

static void on_go_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    world_go();
}

/* The tap's road, both doors (G14): a widget pixel through the
 * frame's own matrix — the orbit read back and re-made, the very
 * width/height the render passes to mcme_camera_mvp, viewport.c's
 * pair — onto the staged world mesh, answered as the cell's WORLD
 * coordinates: the ask's own origin (the sub-chunk's floor, the
 * block's corner — G12's tint base, same arithmetic) plus the
 * local cell under the face. False when no answer stands: no
 * world mesh staged, not laid out, no camera, or nothing hit. */
static bool pick_world(double px, double py, long out[3])
{
    float mvp[16], orig[3], dir[3];
    float yaw = 0, pitch = 0, dist = 0;
    McmeCamera cam;
    long cell[3];
    long base_x, base_y, base_z;
    int w, h;

    if (g_pick_verts == NULL || g_pick_idx == NULL) return false;
    w = gtk_widget_get_width(g_viewport);
    h = gtk_widget_get_height(g_viewport);
    if (w <= 0 || h <= 0) return false;
    if (!mcme_viewport_camera(g_viewport, &yaw, &pitch, &dist))
        return false;
    mcme_camera_default(&cam);
    cam.yaw = yaw;
    cam.pitch = pitch;
    cam.dist = dist; /* last_x/last_y are the drag's bookkeeping
                        alone (camera.h) — the matrix reads three */
    mcme_camera_mvp(&cam, (float)w / (float)h, mvp);
    if (!mcme_pick_ray(mvp, px, py, w, h, orig, dir)) return false;
    if (!mcme_pick_cell(g_pick_verts, g_pick_nverts, g_pick_idx,
                        g_pick_nidx, orig, dir,
                        g_pick_section ? 16 : 1, cell))
        return false;
    if (g_pick_section) {
        base_x = (g_wx >> 4) * 16; /* the floor the ask stands on */
        base_y = (g_wy >> 4) * 16;
        base_z = (g_wz >> 4) * 16;
    } else {
        base_x = g_wx;
        base_y = g_wy;
        base_z = g_wz;
    }
    out[0] = base_x + cell[0];
    out[1] = base_y + cell[1];
    out[2] = base_z + cell[2];
    return true;
}

/* The entries show the coordinates the ask will ride — the flags'
 * own words at startup (G13), and since G14 whatever point you
 * touched. */
static void set_bar_coords(long x, long y, long z)
{
    char txt[32];

    snprintf(txt, sizeof txt, "%ld", x);
    gtk_editable_set_text(GTK_EDITABLE(g_wx_entry), txt);
    snprintf(txt, sizeof txt, "%ld", y);
    gtk_editable_set_text(GTK_EDITABLE(g_wy_entry), txt);
    snprintf(txt, sizeof txt, "%ld", z);
    gtk_editable_set_text(GTK_EDITABLE(g_wz_entry), txt);
}

/* The window's answer to a point (G14): the cell's world
 * coordinates become the bar's own question — the entries show
 * what you touched and the lane asks it (G13), the label reports
 * either the way there or what stood under the point. */
static void pick_at(double px, double py)
{
    long cell[3];

    if (g_world_dir == NULL) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           "no world open — nothing to pick");
        return;
    }
    if (!pick_world(px, py, cell)) {
        gtk_label_set_text(GTK_LABEL(g_world_label),
                           g_pick_verts == NULL
                               ? "the picker's mesh — no world behind it"
                               : "no face answers that point");
        return;
    }
    set_bar_coords(cell[0], cell[1], cell[2]);
    world_go();
}

/* The gesture's road (G14): down records where, up asks — and only
 * when up is still down there: a drag's down belongs to the camera
 * (G8's road) and must not re-ask the world. Four pixels is the
 * gap between a point and a push. */
static void on_pick_pressed(GtkGestureClick *gesture, int n_press,
                            double x, double y, gpointer user_data)
{
    (void)gesture;
    (void)n_press;
    (void)user_data;
    g_press_x = x;
    g_press_y = y;
}

static void on_pick_released(GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer user_data)
{
    (void)gesture;
    (void)user_data;
    if (n_press != 1) return; /* the second of a double-tap: one ask */
    if (fabs(x - g_press_x) > 4.0 || fabs(y - g_press_y) > 4.0)
        return; /* a drag: the camera took it (G8) */
    pick_at(g_press_x, g_press_y);
}

static void on_activate(GtkApplication *app, gpointer user)
{
    GtkWidget *win = gtk_application_window_new(app);
    GtkWidget *bar = gtk_header_bar_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *open = gtk_button_new_with_label("Open…");
    GtkWidget *world = gtk_button_new_with_label("World…");
    GtkWidget *wrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *go = gtk_button_new_with_label("Go");
    GtkWidget *fill = gtk_button_new_with_label("Fill");
    GtkWidget *prune = gtk_button_new_with_label("Prune");
    GtkWidget *convert = gtk_button_new_with_label("Convert");
    GtkWidget *stats = gtk_button_new_with_label("Stats");
    char init[32];

    (void)user;
    g_viewport = mcme_viewport_new();
    gtk_widget_set_vexpand(g_viewport, TRUE);
    g_mesh_label = gtk_label_new(g_mesh_ns ? "meshing…" : "");
    g_pack_label = gtk_label_new((g_pack_arg || g_open_dir)
                                 ? "reading the pack…"
                                 : "the shell is up — no world open");
    /* The bar's answer lands here (G13): "spec — kept/total faces,
       cells" on a real answer, the refusal's own words under it. */
    g_world_label = gtk_label_new("");
    g_wx_entry = gtk_entry_new();
    g_wy_entry = gtk_entry_new();
    g_wz_entry = gtk_entry_new();
    g_dim_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_wx_entry), "x");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_wy_entry), "y");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_wz_entry), "z");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_dim_entry), "dim");
    g_block_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_block_entry), "block");
    set_bar_coords(g_wx, g_wy, g_wz);
    snprintf(init, sizeof init, "%d", (int)g_world_dim);
    gtk_editable_set_text(GTK_EDITABLE(g_dim_entry), init);
    /* --fill's answer lands in the widget (G15), the flags' own
       words shown where the button reads them — one road. */
    if (g_fill_arg != NULL)
        gtk_editable_set_text(GTK_EDITABLE(g_block_entry), g_fill_arg);
    g_section_check = gtk_check_button_new_with_label("section");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_section_check),
                                g_section);
    gtk_box_append(GTK_BOX(wrow), g_wx_entry);
    gtk_box_append(GTK_BOX(wrow), g_wy_entry);
    gtk_box_append(GTK_BOX(wrow), g_wz_entry);
    gtk_box_append(GTK_BOX(wrow), g_dim_entry);
    gtk_box_append(GTK_BOX(wrow), g_section_check);
    gtk_box_append(GTK_BOX(wrow), go);
    gtk_box_append(GTK_BOX(wrow), g_block_entry);
    gtk_box_append(GTK_BOX(wrow), fill);
    gtk_box_append(GTK_BOX(wrow), prune);
    gtk_box_append(GTK_BOX(wrow), convert);
    gtk_box_append(GTK_BOX(wrow), stats);
    g_models = gtk_string_list_new(NULL);
    g_picker = gtk_drop_down_new(G_LIST_MODEL(g_models), NULL);
    g_blocks = gtk_string_list_new(NULL);
    g_block_picker = gtk_drop_down_new(G_LIST_MODEL(g_blocks), NULL);
    g_signal_connect(g_picker, "notify::selected",
                     G_CALLBACK(on_model_picked), NULL);
    g_signal_connect(g_block_picker, "notify::selected",
                     G_CALLBACK(on_block_picked), NULL);
    {
        /* G14: the picture's own tap — the gesture rides the same
           widget the camera's drag gestures do (its own group, so
           both see the press; the 4-pixel gate keeps a push with
           the camera from re-asking the world). */
        GtkGesture *tap = gtk_gesture_click_new();

        gtk_widget_add_controller(g_viewport,
                                  GTK_EVENT_CONTROLLER(tap));
        g_signal_connect(tap, "pressed",
                         G_CALLBACK(on_pick_pressed), NULL);
        g_signal_connect(tap, "released",
                         G_CALLBACK(on_pick_released), NULL);
    }

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(bar),
                                    gtk_label_new("mcme"));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), open);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), world);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_clicked), win);
    g_signal_connect(world, "clicked", G_CALLBACK(on_world_clicked), win);
    g_signal_connect(go, "clicked", G_CALLBACK(on_go_clicked), win);
    g_signal_connect(fill, "clicked", G_CALLBACK(on_fill_clicked), win);
    /* The destructive button asks back (D65): the click opens the
       alert, and only its Delete answer reaches queue_prune. */
    g_signal_connect(prune, "clicked", G_CALLBACK(on_prune_clicked),
                     win);
    /* The creating writer names its pattern before the press and its
       exact directory in the answer (D66): no dialog, no picker. */
    gtk_widget_set_tooltip_text(
        convert, "write this world as Java in <name>_java beside it");
    g_signal_connect(convert, "clicked",
                     G_CALLBACK(on_convert_clicked), win);
    /* The reader names its question before the press (G21):
       nothing asks back, and the count arrives in the label. */
    gtk_widget_set_tooltip_text(
        stats, "count this world's chunks, keys and dimensions");
    g_signal_connect(stats, "clicked",
                     G_CALLBACK(on_stats_clicked), win);
    gtk_window_set_titlebar(GTK_WINDOW(win), bar);
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    gtk_widget_set_halign(g_pack_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(g_pack_label, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(g_world_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(g_world_label, GTK_ALIGN_CENTER);
    {
        /* No gtk_label_set_monospace in this GTK4; the Pango way. */
        PangoAttrList *attrs = pango_attr_list_new();

        pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(g_pack_label), attrs);
        pango_attr_list_unref(attrs);
    }
    /* G16: the twin takes its seat — both rows of the pack's
       answer side by side, the block row wired the whole time
       (G7), now reachable. */
    gtk_box_append(GTK_BOX(prow), g_picker);
    gtk_box_append(GTK_BOX(prow), g_block_picker);
    gtk_box_append(GTK_BOX(box), prow);
    gtk_box_append(GTK_BOX(box), wrow);
    gtk_box_append(GTK_BOX(box), g_viewport);
    gtk_box_append(GTK_BOX(box), g_mesh_label);
    gtk_box_append(GTK_BOX(box), g_pack_label);
    gtk_box_append(GTK_BOX(box), g_world_label);
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

    /* G15's chain: --fill queues the writer where the flags' chain
       always stood — after the pack session took its generation (a
       fill answered under the old one would fall whole), and
       through the same lane the pack's ask rides: the ask lands as
       the fill's own proof, the world's new answer. */
    if (g_fill_arg != NULL)
        queue_fill();

    /* G18's: --prune queues the destructive writer the same way —
       the typed verb is its own confirmation (the dialog belongs to
       the button alone), and the lane holds it like the fill. */
    if (g_prune_arg)
        queue_prune();

    /* G19's: --convert queues last of the writers — it reads the
       world whole, so behind any fill or prune the same line asked
       for. */
    if (g_convert_arg)
        queue_convert();

    /* G21's: --stats queues the reader behind every writer — the
       walk opens the store, so the lane holds it like the writes
       and the ask, and its sentence lands under whatever the
       traffic answered last. */
    if (g_stats_arg)
        queue_stats();

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

/* A coordinate as a whole block position — the CLI's parse_coord
   shape, verbless (G9), and since G13 the bar's parser too: one
   shape in worldbar.c, selftest-pinned for both doors. */
static bool parse_long(const char *text, long *out)
{
    return mcme_worldbar_coord(text, out);
}

int main(int argc, char **argv)
{
    GtkApplication *app;
    const char *model = NULL;
    const char *pos[2];
    int npos = 0;
    bool at_seen = false, dim_seen = false;
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
        } else if (strcmp(argv[i], "--world") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mcgui: --world wants a directory\n");
                return 1;
            }
            g_world_dir = g_strdup(argv[++i]); /* owned (G13) */
        } else if (strcmp(argv[i], "--at") == 0) {
            if (i + 3 >= argc) {
                fprintf(stderr, "mcgui: --at wants three "
                                "coordinates\n");
                return 1;
            }
            if (!parse_long(argv[++i], &g_wx) ||
                !parse_long(argv[++i], &g_wy) ||
                !parse_long(argv[++i], &g_wz)) {
                fprintf(stderr, "mcgui: --at wants whole "
                                "coordinates\n");
                return 1;
            }
            at_seen = true;
            g_at_seen = true; /* the flags gave the question (G21) */
        } else if (strcmp(argv[i], "--dimension") == 0) {
            /* G20: the bar's names at the flag too — overworld,
               nether, end, or a number, the one vocabulary core
               owns; the refusal is the core's own words under
               the flag's name. */
            if (i + 1 >= argc) {
                fprintf(stderr, "mcgui: --dimension wants "
                                "overworld, nether, end, or a "
                                "number\n");
                return 1;
            }
            if (!mcme_dimension_parse(argv[i + 1], &g_world_dim)) {
                fprintf(stderr, "mcgui: --dimension: %s\n",
                        mcme_last_error());
                return 1;
            }
            dim_seen = true;
            i++;
        } else if (strcmp(argv[i], "--section") == 0) {
            /* G11: the whole sub-chunk the coordinates stand in,
               not the one block — the same road, one cell wider. */
            g_section = true;
        } else if (strcmp(argv[i], "--fill") == 0) {
            /* G15: the bar's Fill without the bar — the block spec
               the writer rides, shown in the entry the button
               reads. */
            if (i + 1 >= argc) {
                fprintf(stderr, "mcgui: --fill wants a block\n");
                return 1;
            }
            g_fill_arg = argv[++i];
        } else if (strcmp(argv[i], "--prune") == 0) {
            /* G18: the bar's Prune without the bar — the cell's
               whole chunk column, deleted. No argument: the cell
               is --at's, the same one --fill writes. */
            g_prune_arg = TRUE;
        } else if (strcmp(argv[i], "--convert") == 0) {
            /* G19: the bar's Convert without the bar — the open
               world to Java, written beside itself as <dir>_java.
               No argument: the input is --world's, the output is
               the derived name the answer spells. */
            g_convert_arg = TRUE;
        } else if (strcmp(argv[i], "--stats") == 0) {
            /* G21: the bar's Stats without the bar — count what
               the world holds, the CLI's info/chunks engine read
               through the door. No argument: the counts are the
               world's own; no cell either — this door reads the
               world, not a place in it. */
            g_stats_arg = TRUE;
        } else if (strcmp(argv[i], "--block") == 0) {
            /* G16: the picker's other row without the bar — the
               block the chain meshes through the blockstate
               index (mcme_mesh_for_block, D33). */
            if (i + 1 >= argc) {
                fprintf(stderr, "mcgui: --block wants ns:block\n");
                return 1;
            }
            g_block_arg = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: mcgui [--selftest|--smoke] "
                            "[--open dir] [--world dir --at x y z "
                            "[--dimension n] [--section] "
                            "[--fill block] [--prune] [--convert] "
                            "[--stats]] "
                            "[--block ns:block] "
                            "[pack-dir [ns:model]]\n");
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
    /* The world ask (G9): one question in three flags — alone they
       are a typo, together they need a pack to mesh the block with.
       Since G21 --stats is the exception: it needs no cell, so a
       world run may stand on the count alone — unless a cell
       writer came along, which writes a place and asks for it. */
    if (at_seen && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --at wants --world\n");
        return 1;
    }
    if (g_world_dir != NULL && !at_seen &&
        (g_fill_arg != NULL || g_prune_arg || g_convert_arg ||
         !g_stats_arg)) {
        fprintf(stderr, "mcgui: --world wants --at x y z\n");
        return 1;
    }
    if (dim_seen && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --dimension belongs to --world\n");
        return 1;
    }
    if (g_section && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --section belongs to --world\n");
        return 1;
    }
    if (g_fill_arg != NULL && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --fill wants --world\n");
        return 1;
    }
    if (g_prune_arg && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --prune wants --world\n");
        return 1;
    }
    if (g_prune_arg && g_fill_arg != NULL) {
        fprintf(stderr, "mcgui: --fill and --prune are one cell — "
                        "give one\n");
        return 1;
    }
    if (g_convert_arg && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --convert wants --world\n");
        return 1;
    }
    if (g_stats_arg && g_world_dir == NULL) {
        fprintf(stderr, "mcgui: --stats wants --world\n");
        return 1;
    }
    /* --fill/--prune, --convert and --stats compose deliberately
       (G19/G21): the writers drain fill, prune, then convert — the
       biggest copy reads what the smaller ones did — and the count
       waits behind them all, so "fill this cell, then convert, then
       count the world" is one run, not two questions. */
    if (g_world_dir != NULL && g_open_dir == NULL && npos == 0) {
        fprintf(stderr, "mcgui: a world block needs a pack to mesh "
                        "it\n");
        return 1;
    }
    if (model != NULL && g_block_arg != NULL) {
        fprintf(stderr, "mcgui: the model positional and --block are "
                        "one question — give one\n");
        return 1;
    }
    if (g_block_arg != NULL) {
        /* G16: the other row's split — a block is ns:name, walked
           through the blockstate index instead of the model
           path. */
        if (!split_model(g_block_arg, &g_mesh_ns, &g_mesh_path)) {
            fprintf(stderr, "mcgui: --block is ns:block\n");
            return 1;
        }
        g_mesh_by_block = true;
    } else if (model != NULL) {
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
    g_free(g_world_dir);
    g_free(g_mesh_ns);
    g_free(g_mesh_path);
    g_free(g_spec_seen);
    g_free(g_pick_verts);
    g_free(g_pick_idx);
    return g_exit_code ? g_exit_code : rc;
}
