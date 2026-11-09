#pragma once

// SetupPage: the setup page (docs/config-plan.md C2-C5): every setting, set
// before a run begins, then Start. A run's settings are fixed from its start
// (C3, Damian), so during a run, for a finished run and for a snapshot the
// page is read-only and shows what the run started with.
//
// Driven entirely by the settings registry (nova/keys.h): its groups down the
// left, the selected group's settings on the right, one row each -- label,
// control, default, a mark if changed from the loaded file, a reset button --
// with the control chosen by the setting's kind (a checkbox for a switch, a
// slider and a number field for a whole number or a number, held to its range
// and logarithmic where the registry says, a drop-down for a choice, a text
// field for text and paths). Hidden settings aren't shown; one whose "needs"
// switch is off is greyed out but keeps its value. A search box looks through
// names, labels and help; "changed only" lists what differs from the file.
//
// The check (C4, nova/check.h) runs after every change: problems show next to
// their settings, computed defaults show the value the run will use, and the
// right-hand panel has the memory and disk the world will need and, for a
// landscape, what it will be. Start stays off while a problem is open.
//
// Files (C5): Load opens any .cfg (the presets listed for one click); Save as
// writes the settings that differ from the defaults, grouped and commented;
// Start writes every setting, defaults included, to the run's folder as
// run.cfg (run_cfg_text) and asks the shell to start it, in the window or
// headless. The page only produces configuration files: the shell starts
// nova's own run from run.cfg, so a run started here is exactly `nova run
// run.cfg` (C6).
//
// Immediate mode like the viewer, and no Win32: the shell gives it input and
// a Canvas, and the lists it can't make itself (the presets, the run
// folders); tests drive it with made-up input.
#include "canvas.h"
#include "nova/check.h"
#include "nova/str.h"
#include "viewer.h" // UiInput

namespace nova {

enum SetupMode : u32 {
    kSetupEdit,     // before a run: editable
    kSetupLive,     // a run in the window: what it started with
    kSetupArchive,  // a finished run (nova view --run): its replay log's config
    kSetupSnapshot, // a snapshot continued: its config
};

// What the page asks the shell for.
enum SetupReqKind : u32 {
    kReqStartWindow,   // write run.cfg to `folder` and start it here, watched
    kReqStartHeadless, // ... and start `nova run run.cfg` in its own console, with a stop file
    kReqBack,          // back to the run's panels
    kReqStopHeadless,  // create the headless run's stop file
};

struct SetupRequest {
    u32 kind;
    char folder[260];
};

class SetupPage {
  public:
    SetupPage();

    // --- what is shown ----------------------------------------------------------
    // Loads a configuration (a .cfg file's text) as the page's settings and as
    // "the loaded file" that marks and resets compare with. `source` names it.
    // False (with why) if the text doesn't parse.
    bool load_text(Str text, const char* source, TextBuf* err);
    bool load_file(const char* path, TextBuf* err);
    // Every setting at its default (no file).
    void load_defaults(const char* source);
    // kSetupEdit makes the page editable; the others are read-only, `title`
    // naming the run.
    void set_mode(u32 mode, const char* title);
    u32 mode() const { return mode_; }
    bool read_only() const { return mode_ != kSetupEdit; }
    const char* source() const { return source_; }
    // A line under the title (read-only modes: an old log's knob records, ...).
    void set_info(const char* text);

    // --- what the shell provides ---------------------------------------------------
    // The presets for one-click Load and the run folders that exist, one path
    // a line.
    void set_presets(Str lines);
    void set_run_folders(Str lines);
    // The front end's own reads (app's read_run_keys), for the check.
    void set_front_end(FrontEndReader r) {
        front_end_ = r;
        dirty_ = true;
    }
    // Whether the check resolves the budgets and checks the fit against this
    // machine (on by default; tests may turn it off).
    void set_machine_check(bool on) {
        machine_ = on;
        dirty_ = true;
    }
    // A message in the bar (a file saved, a start refused, ...).
    void set_note(const char* text, bool bad = false);
    // A new map: world.seed (the map's seed) to a fresh number, so the check
    // generates the world's map again and the page draws it (the "new map"
    // buttons). Nothing while read-only.
    void new_map();
    // A headless run started from here, with its folder ("" none).
    void set_headless(const char* folder);

    // --- each frame -------------------------------------------------------------
    void frame(const UiInput& in, Canvas& c);
    Array<SetupRequest>& requests() { return reqs_; }
    // The mouse cursor: 0 arrow, 3 hand, 4 text.
    u32 cursor() const { return cursor_; }
    bool editing() const { return edit_ != kEditNone; } // a text field has the keyboard
    // The last frame changed something (rows drawn before it are stale), or a
    // change awaits its check: draw another frame.
    bool wants_frame() const { return dirty_ || redraw_; }

    // --- the settings (registry index k, key_at order) -------------------------------
    usize key_index(Str name) const; // or kNoKey
    static constexpr usize kNoKey = ~static_cast<usize>(0);
    bool is_set(usize k) const { return cur_[k].set != 0; }
    Str value(usize k) const; // as set ("" if not set)
    // What the control shows: the value set, else the value the run will use
    // (a computed default worked out), else the default.
    Str shown(usize k);
    // A value as typed: checked against the setting's kind, a number held to
    // its range. False (with why) if it can't be a value of this setting.
    bool set_text(usize k, Str text, TextBuf* why);
    void unset(usize k); // its default (a computed one worked out by the run)
    void reset(usize k); // back to the loaded file's value
    bool changed(usize k) const; // from the loaded file
    bool greyed(usize k);        // its needs switch is off
    usize changed_count() const;

    // --- files ------------------------------------------------------------------
    // The settings set, as config text (the check's input).
    void config_text(TextBuf& out) const;
    // Save as: the settings that differ from the defaults, grouped and
    // commented like the presets.
    void save_text(TextBuf& out) const;
    bool save_file(const char* path, TextBuf* err);
    // Start: every setting, defaults included (computed ones as the run will
    // work them out), grouped; with outputs_in_folder(), output paths moved
    // into `folder`. Call after check() is clean.
    void run_cfg_text(const char* folder, TextBuf& out);

    // --- the check and Start -------------------------------------------------------
    const SettingsCheck& check(); // up to date
    usize problem_count();
    // Editable, no problem open, a folder that holds no run yet.
    bool can_start();
    const char* folder() const { return folder_; }
    void set_folder(const char* f);
    void default_folder(); // runs\<name>, the first that holds no run.cfg
    bool outputs_in_folder() const { return outputs_here_; }
    void set_outputs_in_folder(bool on) { outputs_here_ = on; }
    // Why Start is off ("" if it is on).
    const char* start_blocked();

    // --- test hooks -------------------------------------------------------------
    void set_search(const char* text);
    void set_changed_only(bool on);
    void select_group(u32 g);
    bool select_group_named(Str name); // ignoring case; false if no such group
    void show_first_problem();         // its group, scrolled to its row (if there is one)
    u32 selected_group() const { return group_; }
    // The rows of the last frame, in order: registry indices (kNoKey for a
    // group's heading).
    usize row_count() const { return rows_.size(); }
    usize row_key(usize i) const { return rows_[i].key; }
    // Where a setting's row and its control were drawn in the last frame
    // (false if not on screen).
    bool row_rect(usize k, Rect* r) const;
    bool control_rect(usize k, Rect* r) const;
    const Rect& start_button(bool headless) const { return headless ? start_headless_ : start_window_; }
    // The landscape's picture and its "new map" button in the last frame
    // (w = 0: not drawn).
    const Rect& map_rect() const { return map_r_; }
    const Rect& new_map_button() const { return new_map_r_; }
    i32 scroll() const { return scroll_; }

  private:
    struct Val {
        u32 off, len;
        u8 set;
        u8 pad[3];
    };
    struct Row {
        usize key; // kNoKey: a group's heading
        u32 group;
        i32 y, h;  // in the list's content
    };
    enum Edit : u32 { kEditNone, kEditSearch, kEditValue, kEditFolder, kEditSave, kEditLoad };
    enum Popup : u32 { kPopNone, kPopChoice, kPopLoad, kPopSave, kPopFolder };

    // layout and panels
    void layout(Canvas& c);
    void bar(const UiInput& in, Canvas& c);
    void groups(const UiInput& in, Canvas& c);
    void list(const UiInput& in, Canvas& c);
    void check_panel(const UiInput& in, Canvas& c);
    void popup(const UiInput& in, Canvas& c, bool* consumed);
    void tooltip(Canvas& c);
    void build_rows(Canvas& c);
    void draw_row(const UiInput& in, Canvas& c, const Row& r, const Rect& rr);
    // widgets
    bool button(const UiInput& in, Canvas& c, const Rect& r, const char* label, bool on = false, bool enabled = true);
    bool checkbox(const UiInput& in, Canvas& c, const Rect& r, bool on, bool enabled, const char* label);
    void slider(const UiInput& in, Canvas& c, usize k, const Rect& track, bool enabled);
    // A text field: shows `text`, or the edit buffer while it has the keyboard;
    // a click gives it the keyboard with `text` to edit.
    void field(const UiInput& in, Canvas& c, const Rect& r, u32 kind, usize key, Str text, bool enabled, u32 color,
               const char* hint = "");
    void panel_box(Canvas& c, const Rect& r, const char* title, Rect* inner);
    void begin_edit(u32 kind, usize key, Str text);
    void commit_edit();
    void type_keys(const UiInput& in);
    void open_popup(u32 kind, usize key, const Rect& anchor);
    void set_tip(const char* text);
    // model helpers
    void store(Array<Val>& vals, usize k, Str v);
    Str file_value(usize k) const;
    Str effective(usize k); // the value the run reads (set, else computed / default)
    bool needs_met(usize k, u32 depth);
    bool matches(usize k) const;
    void make_folder_name();
    void suggest_save_path();
    void jump_to(usize k);

    // the settings: per registry entry, the current value and the file's
    Array<Val> cur_, file_;
    Array<char> text_;    // every value's text
    Array<char> unknown_; // keys in the file the registry doesn't know: "key = value\n" lines
    char source_[260] = {};
    char title_[200] = {};
    char info_[240] = {};
    u32 mode_ = kSetupEdit;

    // the check
    SettingsCheck check_;
    bool dirty_ = true, machine_ = true, redraw_ = false;
    FrontEndReader front_end_ = nullptr;

    // Start and files
    char folder_[260] = {};
    bool outputs_here_ = true;
    char headless_[260] = {};
    char presets_[4096] = {};
    char folders_[4096] = {};
    char save_path_[260] = {};
    char load_path_[260] = {};
    char note_[240] = {};
    bool note_bad_ = false;
    Array<SetupRequest> reqs_;

    // the list
    u32 group_ = 0;
    char search_[128] = {};
    bool changed_only_ = false;
    Array<Row> rows_;
    i32 content_h_ = 0, scroll_ = 0;
    ScrollBar list_bar_, check_bar_, popup_bar_; // the list's, the check's and the popup's (widgets.h)
    usize jump_ = kNoKey;   // scroll this setting into view next frame
    Array<Rect> row_r_, ctl_r_; // per key, the last frame's (w = 0: not drawn)

    // a slider being dragged
    usize drag_ = kNoKey;
    double drag_lo_ = 0, drag_hi_ = 1, drag_a_ = 0; // its span, fixed for the drag
    bool drag_log_ = false;
    char drag_text_[48] = {};

    // text editing
    u32 edit_ = kEditNone;
    usize edit_key_ = kNoKey;
    char edit_buf_[2048] = {};
    u32 edit_len_ = 0;
    TextBuf edit_orig_; // the text it showed when the edit began
    Rect edit_r_;
    bool edit_bad_ = false;

    // the popup
    u32 popup_ = kPopNone;
    usize popup_key_ = kNoKey;
    Rect popup_r_;
    i32 popup_scroll_ = 0;

    // A read-only row whose setting is in force: its controls take nothing but
    // read clearly (a greyed-out one is fainter).
    bool ro_row_ = false;
    u32 off_col() const;

    // the landscape's picture: its cells coloured, for the check's landscape land_gen
    Array<u32> map_img_;
    u32 map_gen_ = 0;
    Rect map_r_, new_map_r_;
    u64 new_maps_ = 0; // new_map's count (mixed into its seeds)

    // layout
    Rect bar_, groups_r_, list_r_, rows_r_, check_r_, start_window_, start_headless_;
    i32 check_scroll_ = 0, check_h_ = 0;
    char tip_[1024] = {};
    i32 mx_ = -1, my_ = -1, tip_x_ = 0, tip_y_ = 0;
    u32 cursor_ = 0;
};

} // namespace nova
