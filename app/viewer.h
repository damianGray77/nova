#pragma once

// Viewer: the analysis UI's panels (Phase 6, design.md 6.12; plan C1, C4-C8).
//
// Immediate mode: every frame() handles the input that arrived since the
// last one and redraws everything from the latest view frame, in one pass.
// The viewer never touches a Run: frames come in through take(), and what
// the user asks for (pause, a knob, a snapshot) goes out as commands that
// the shell applies between ticks, so watching changes nothing (7.1 D).
// It has no Win32 in it either, beyond drawing through a Canvas, so tests
// can drive it with made-up input and read back what it selected.
#include "canvas.h"
#include "nova/view.h"

namespace nova {

struct UiInput {
    i32 mx = -1, my = -1; // mouse position, pixels (-1: outside)
    bool down = false;    // left button held
    bool pressed = false; // ... went down since the last frame
    bool released = false;
    bool dbl = false;     // a double click
    i32 wheel = 0;        // notches, positive away from the user
    u32 keys[16] = {};    // virtual keys pressed (Win32 VK_ codes)
    u32 nkeys = 0;
    char chars[32] = {}; // characters typed
    u32 nchars = 0;
};

enum CmdKind : u32 { kCmdPause, kCmdResume, kCmdStep, kCmdSpeed, kCmdSnapshot, kCmdStop, kCmdKnob, kCmdWindow };

struct ViewCommand {
    u32 kind;
    u32 pad;
    double value; // speed (ticks per second, 0 = flat out), window index
    char key[24];
    char text[40]; // a knob's new value
};

// What the shell knows about the run that a frame doesn't carry.
struct RunStatus {
    bool live = true; // false: a finished run's files
    bool paused = false, finished = false;
    double speed = 0;   // the speed limit, ticks per second (0 = flat out)
    double tps = 0, ips = 0; // measured ticks and instructions per second
    u32 windows = 0, window = 0; // an archive's nodes.csv windows
    char note[200] = {};  // the latest message (a knob refused, a snapshot written)
    char title[120] = {}; // what is open
};

class Viewer {
  public:
    Viewer();

    // Takes a frame: its state replaces the current one (swapped in), and its
    // new chart samples, events and genebank saves join the logs.
    void take(ViewFrame& f);
    // Handles input, then draws everything into c (sized by the caller).
    void frame(const UiInput& in, Canvas& c, const RunStatus& st);
    // What the next frame should carry in detail.
    ViewRequest request() const;
    // Commands from the last frame(); the shell empties the list.
    Array<ViewCommand>& commands() { return cmds_; }

    // Picks a node, a body and a lineage to show if nothing is selected yet
    // (the busiest node, its largest lineage's first member there, and that
    // lineage's latest genotype).
    void auto_select();

    // Inspection and test hooks.
    u32 selected_node() const { return sel_node_; }
    u32 selected_body() const { return sel_body_; }
    u64 selected_lineage() const { return sel_lineage_; }
    usize selected_save() const { return sel_save_; }
    u32 color_by() const { return color_by_; }
    void set_color_by(u32 c) { color_by_ = c < kColorCount ? c : 0; }
    void select_node(u32 n);
    void select_save(usize i);
    const ChartLog& charts() const { return charts_; }
    usize save_count() const { return saves_.size(); }
    const GenomeEntry& save(usize i) const { return saves_[i]; }
    usize event_count() const { return events_.size(); }
    const ViewFrame& current() const { return cur_; }
    // Where node n was drawn in the last frame (false if off the map).
    bool node_screen(u32 n, i32* x, i32* y) const;
    // The genome browser's rows, in order, after its filter (test hook).
    usize browser_rows() const { return order_.size(); }
    void set_filter(const char* text);
    bool knob_editing() const { return edit_ != kNone; }
    // The mouse cursor the shell should show: 0 arrow, 1 west-east resize,
    // 2 north-south resize, 3 hand.
    u32 cursor() const { return cursor_; }

  private:
    // layout
    void layout(Canvas& c, const RunStatus& st);
    // panels
    void toolbar(const UiInput& in, Canvas& c, const RunStatus& st);
    void map_panel(const UiInput& in, Canvas& c);
    void charts_panel(const UiInput& in, Canvas& c);
    void inspector(const UiInput& in, Canvas& c);
    void browser(const UiInput& in, Canvas& c);
    void phylogeny(const UiInput& in, Canvas& c);
    void splitters(const UiInput& in, Canvas& c);
    void popup(const UiInput& in, Canvas& c, bool* consumed);
    void tooltip(Canvas& c);
    // widgets
    bool button(const UiInput& in, Canvas& c, const Rect& r, const char* label, bool on = false);
    void panel_box(Canvas& c, const Rect& r, const char* title, Rect* inner);
    void open_popup(u32 kind, u32 owner, const Rect& anchor);
    void command(u32 kind, double value, const char* key = "", const char* text = "");
    // the map transform
    double map_scale() const;
    void to_screen(float x, float y, i32* sx, i32* sy) const;
    void to_world(i32 sx, i32 sy, double* x, double* y) const;
    // helpers
    void sort_saves();
    void trim_logs();
    u64 richest_lineage() const;
    void set_tooltip(const char* text);
    u32 knob_index(const char* key) const;
    void apply_knob_text(u32 k);

    ViewFrame cur_;
    bool have_ = false;
    ChartLog charts_;
    Array<ViewEvent> events_;
    Array<char> event_text_;
    Array<GenomeEntry> saves_;
    Array<char> save_text_;
    usize saves_seen_ = 0; // every save taken, kept or not
    Array<u64> flash_tick_; // per node: tick of its latest collapse / recovery (+1, 0 = none)
    Array<u8> flash_kind_;
    u64 first_event_[kEvCount] = {}; // tick + 1 of each kind's first event

    // selection
    u32 sel_node_ = kNone, sel_body_ = kNone;
    u64 sel_serial_ = 0;
    u64 sel_lineage_ = 0;
    usize sel_save_ = ~static_cast<usize>(0);
    u32 strip_by_ = kStripOwner;
    u32 color_by_ = kColorBodies;
    bool size_by_bodies_ = true, show_cargo_ = true, show_edges_ = true;

    // layout (fractions, dragged by the splitters) and the panel rects
    float fx_ = 0.60f, fy_ = 0.60f, fb_ = 0.62f, fr_ = 0.56f;
    u32 drag_split_ = 0; // 0 none, 1 fx, 2 fy, 3 fb, 4 fr
    Rect bar_, map_, charts_r_, phylo_r_, insp_, browse_;
    Rect map_inner_;

    // the map view
    double zoom_ = 1.0, pan_x_ = 0.0, pan_y_ = 0.0;
    bool panning_ = false, pan_moved_ = false;
    i32 pan_mx_ = 0, pan_my_ = 0, press_x_ = 0, press_y_ = 0;
    Array<i32> node_sx_, node_sy_; // where each node was drawn
    u32 hover_node_ = kNone;

    // charts: up to 4 series per chart
    static constexpr u32 kCharts = 4, kSeries = 4;
    u32 series_[kCharts][kSeries];

    // lists
    i32 body_scroll_ = 0, browse_scroll_ = 0, phylo_scroll_ = 0, genome_scroll_ = 0;
    u32 sort_col_ = 3; // tick
    bool sort_desc_ = true, lineage_only_ = false;
    Array<u32> order_;   // saves_ in browser order
    bool order_dirty_ = true;
    u64 order_lineage_ = 0; // sel_lineage_ when order_ was built
    // The browser's filter: a length, a range "a-b", or a lineage "n.k".
    static constexpr u32 kEditFilter = 1000;
    char filt_[24] = {};
    u32 len_lo_ = 0, len_hi_ = 0xFFFFFFFFu;
    u64 filt_lineage_ = 0;
    void apply_filter();
    Array<PhyloNode> phylo_;
    u64 phylo_lineage_ = 0;
    usize phylo_saves_ = 0;

    // text editing (a knob's value)
    u32 edit_ = kNone;
    char edit_buf_[40] = {};
    u32 edit_len_ = 0;

    // the popup list
    u32 popup_ = 0, popup_owner_ = 0; // 0 none, 1 colour-by, 2 strip-by, 3 chart series
    Rect popup_r_;
    i32 popup_scroll_ = 0;

    char tip_[512] = {};
    i32 tip_x_ = 0, tip_y_ = 0;
    Array<ViewCommand> cmds_;
    i32 mx_ = -1, my_ = -1;
    u32 cursor_ = 0;
    bool auto_ = true;          // auto_select() still has work to do
    bool drag_slider_ = false;  // the archive's window slider
    Rect slider_;
    // A landscape (Phase 7): the land image and box, landmasses, the overview map.
    bool landscape_ = false;
    u32 land_w_ = 0, land_h_ = 0;
    Array<u32> land_img_;
    float box_[4] = {0, 0, 1, 1};
    Array<MassView> masses_;
    Rect minimap_;
    bool mm_drag_ = false;
    void fit_mass(u32 mass);

  public:
    // Test hooks: the map's zoom, and the overview map's place.
    double zoom() const { return zoom_; }
    const Rect& minimap() const { return minimap_; }
};

} // namespace nova
