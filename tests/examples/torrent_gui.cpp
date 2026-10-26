#include "tests/examples/torrent_engine.hpp"
#include "tests/examples/torrent_manifest.hpp"

#include <gtk/gtk.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace zeromesh::examples;

// ─────────────────────────────────────────────────────────────────────────────
// GUI State & Context
// ─────────────────────────────────────────────────────────────────────────────

struct AppContext {
    std::filesystem::path current_dir;
    uint16_t dht_port{8982};
    uint16_t chunk_port{8984};
    std::unique_ptr<TorrentEngine> engine;

    // GTK Widgets
    GtkWidget* window{nullptr};
    GtkWidget* status_label{nullptr};
    GtkWidget* peers_label{nullptr};
    GtkWidget* ports_label{nullptr};

    // Export Tab
    GtkWidget* export_dir_label{nullptr};
    GtkWidget* export_status_label{nullptr};
    GtkWidget* export_progress_bar{nullptr};
    GtkWidget* magnet_entry{nullptr};
    GtkWidget* copy_magnet_btn{nullptr};
    GtkWidget* export_btn{nullptr};
    GtkWidget* seeded_files_view{nullptr};
    GtkTextBuffer* seeded_files_buf{nullptr};

    // Download Tab
    GtkWidget* import_magnet_entry{nullptr};
    GtkWidget* download_btn{nullptr};
    GtkWidget* dest_dir_label{nullptr};
    GtkWidget* download_progress_bar{nullptr};
    GtkWidget* download_status_label{nullptr};
    GtkWidget* download_log_view{nullptr};
    GtkTextBuffer* download_log_buf{nullptr};

    // Telemetry Tab
    GtkWidget* node_id_label{nullptr};
    GtkWidget* seeded_bytes_label{nullptr};
    GtkWidget* downloaded_bytes_label{nullptr};
    GtkWidget* telemetry_log_view{nullptr};
    GtkTextBuffer* telemetry_log_buf{nullptr};
};

static AppContext g_app;

// ─────────────────────────────────────────────────────────────────────────────
// UI Helpers (Marshaled onto the GTK Main Thread via g_idle_add)
// ─────────────────────────────────────────────────────────────────────────────

struct ProgressPayload {
    float fraction;
    std::string text;
    uint64_t bytes_done;
    uint64_t total_bytes;
    bool is_download;
};

static gboolean OnProgressIdle(gpointer data) {
    auto* payload = static_cast<ProgressPayload*>(data);
    if (payload->is_download) {
        if (g_app.download_progress_bar) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_app.download_progress_bar), payload->fraction);
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << (payload->fraction * 100.0f) << "% ("
               << (payload->bytes_done / 1024) << " / " << (payload->total_bytes / 1024) << " KB)";
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.download_progress_bar), ss.str().c_str());
        }
        if (g_app.download_status_label) {
            gtk_label_set_text(GTK_LABEL(g_app.download_status_label), payload->text.c_str());
        }
    } else {
        if (g_app.export_progress_bar) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_app.export_progress_bar), payload->fraction);
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << (payload->fraction * 100.0f) << "%";
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.export_progress_bar), ss.str().c_str());
        }
        if (g_app.export_status_label) {
            gtk_label_set_text(GTK_LABEL(g_app.export_status_label), payload->text.c_str());
        }
    }
    delete payload;
    return G_SOURCE_REMOVE;
}

struct LogPayload {
    GtkTextBuffer* buffer;
    std::string message;
};

static gboolean OnAppendLogIdle(gpointer data) {
    auto* p = static_cast<LogPayload*>(data);
    if (p->buffer) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(p->buffer, &end);
        std::string line = p->message + "\n";
        gtk_text_buffer_insert(p->buffer, &end, line.c_str(), -1);
    }
    delete p;
    return G_SOURCE_REMOVE;
}

static void AppendLog(GtkTextBuffer* buf, const std::string& msg) {
    auto* p = new LogPayload{buf, msg};
    g_idle_add(OnAppendLogIdle, p);
}

// Periodic Telemetry Timer
static gboolean OnTelemetryTimer(gpointer) {
    if (!g_app.engine) return G_SOURCE_CONTINUE;

    auto stats = g_app.engine->GetStats();
    if (g_app.peers_label) {
        std::string pt = "Active Mesh Peers: " + std::to_string(stats.active_peers);
        gtk_label_set_text(GTK_LABEL(g_app.peers_label), pt.c_str());
    }
    if (g_app.seeded_bytes_label) {
        std::string sb = "Total Seeded: " + std::to_string(stats.total_bytes_seeded / 1024) + " KB";
        gtk_label_set_text(GTK_LABEL(g_app.seeded_bytes_label), sb.c_str());
    }
    if (g_app.downloaded_bytes_label) {
        std::string db = "Total Downloaded: " + std::to_string(stats.total_bytes_downloaded / 1024) + " KB";
        gtk_label_set_text(GTK_LABEL(g_app.downloaded_bytes_label), db.c_str());
    }
    return G_SOURCE_CONTINUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Action Handlers
// ─────────────────────────────────────────────────────────────────────────────

static void OnExportClicked(GtkWidget*, gpointer) {
    gtk_widget_set_sensitive(g_app.export_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_app.export_status_label), "Scanning directory & chunking files...");

    std::thread([dir = g_app.current_dir]() {
        try {
            AppendLog(g_app.seeded_files_buf, "[zm-torrent] Scanning directory: " + dir.string());

            auto [manifest, info_hash] = g_app.engine->ExportDirectory(
                dir, 256 * 1024,
                [](float frac, const std::string& status, uint64_t done, uint64_t total) {
                    auto* p = new ProgressPayload{frac, status, done, total, false};
                    g_idle_add(OnProgressIdle, p);
                });

            std::string magnet = TorrentManifest::MakeMagnetUri(info_hash, manifest.name, manifest.total_bytes);

            // Update Magnet Entry on GTK thread
            struct FinishExportPayload {
                std::string magnet;
                std::string info_hash;
                size_t file_count;
                uint64_t total_bytes;
            };

            auto* fin = new FinishExportPayload{magnet, info_hash, manifest.files.size(), manifest.total_bytes};
            g_idle_add([](gpointer d) -> gboolean {
                auto* f = static_cast<FinishExportPayload*>(d);
                gtk_editable_set_text(GTK_EDITABLE(g_app.magnet_entry), f->magnet.c_str());
                gtk_widget_set_sensitive(g_app.copy_magnet_btn, TRUE);
                gtk_widget_set_sensitive(g_app.export_btn, TRUE);

                std::ostringstream ss;
                ss << "Seeding Active! (" << f->file_count << " files, " << (f->total_bytes / 1024) << " KB)";
                gtk_label_set_text(GTK_LABEL(g_app.export_status_label), ss.str().c_str());

                AppendLog(g_app.seeded_files_buf, "✓ Published InfoHash: " + f->info_hash);
                AppendLog(g_app.seeded_files_buf, "✓ Magnet Link Ready: " + f->magnet);
                AppendLog(g_app.seeded_files_buf, "✓ DHT ANNOUNCE broadcasted to mesh swarm. Seeding chunks...");

                delete f;
                return G_SOURCE_REMOVE;
            }, fin);

        } catch (const std::exception& ex) {
            AppendLog(g_app.seeded_files_buf, std::string("Error during export: ") + ex.what());
            g_idle_add([](gpointer) -> gboolean {
                gtk_widget_set_sensitive(g_app.export_btn, TRUE);
                gtk_label_set_text(GTK_LABEL(g_app.export_status_label), "Export failed. Check logs.");
                return G_SOURCE_REMOVE;
            }, nullptr);
        }
    }).detach();
}

static void OnCopyMagnetClicked(GtkWidget*, gpointer) {
    const char* text = gtk_editable_get_text(GTK_EDITABLE(g_app.magnet_entry));
    if (text && std::strlen(text) > 0) {
        GdkDisplay* display = gdk_display_get_default();
        if (display) {
            GdkClipboard* clipboard = gdk_display_get_clipboard(display);
            gdk_clipboard_set_text(clipboard, text);
            gtk_label_set_text(GTK_LABEL(g_app.export_status_label), "✓ Magnet Link Copied to Clipboard!");
        }
    }
}

static void OnDownloadClicked(GtkWidget*, gpointer) {
    const char* raw_input = gtk_editable_get_text(GTK_EDITABLE(g_app.import_magnet_entry));
    if (!raw_input || std::strlen(raw_input) == 0) {
        gtk_label_set_text(GTK_LABEL(g_app.download_status_label), "Please paste a Magnet URI or 64-char InfoHash.");
        return;
    }

    auto hash_opt = TorrentManifest::ExtractHash(raw_input);
    if (!hash_opt) {
        gtk_label_set_text(GTK_LABEL(g_app.download_status_label), "Invalid Magnet URI or InfoHash.");
        return;
    }

    std::string info_hash = *hash_opt;
    gtk_widget_set_sensitive(g_app.download_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_app.download_status_label), "Resolving manifest in DHT swarm...");
    AppendLog(g_app.download_log_buf, "[zm-torrent] Resolving InfoHash: " + info_hash);

    std::thread([info_hash, dest_dir = g_app.current_dir]() {
        bool ok = g_app.engine->DownloadTorrent(
            info_hash, dest_dir,
            [](float frac, const std::string& status, uint64_t done, uint64_t total) {
                auto* p = new ProgressPayload{frac, status, done, total, true};
                g_idle_add(OnProgressIdle, p);
                AppendLog(g_app.download_log_buf, status);
            });

        g_idle_add([](gpointer d) -> gboolean {
            bool success = static_cast<bool>(reinterpret_cast<uintptr_t>(d));
            gtk_widget_set_sensitive(g_app.download_btn, TRUE);
            if (success) {
                gtk_label_set_text(GTK_LABEL(g_app.download_status_label),
                                   "✓ Download Finished! Files verified & now seeding to mesh.");
                AppendLog(g_app.download_log_buf, "✓ Directory completely reconstructed in current working path.");
                AppendLog(g_app.download_log_buf, "✓ Re-announced all chunks to DHT. Swarm health +1 seeder.");
            } else {
                gtk_label_set_text(GTK_LABEL(g_app.download_status_label),
                                   "Download failed or timed out. Ensure seeding peer is running.");
            }
            return G_SOURCE_REMOVE;
        }, reinterpret_cast<gpointer>(static_cast<uintptr_t>(ok)));
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// GTK4 CSS Styling
// ─────────────────────────────────────────────────────────────────────────────

static void ApplyModernCss() {
    const char* css = R"(
        window {
            background-color: #1e1e2e;
            color: #cdd6f4;
            font-family: 'Inter', 'Ubuntu', 'DejaVu Sans', sans-serif;
            font-size: 14px;
        }

        .header-box {
            background: linear-gradient(135deg, #24273a, #181926);
            border-bottom: 2px solid #89b4fa;
            padding: 16px;
        }

        .title-label {
            font-size: 20px;
            font-weight: 800;
            color: #89b4fa;
        }

        .subtitle-label {
            font-size: 13px;
            color: #a6adc8;
        }

        .card {
            background-color: #24273a;
            border-radius: 10px;
            border: 1px solid #363a4f;
            padding: 14px;
            margin: 8px;
        }

        .card-title {
            font-size: 15px;
            font-weight: bold;
            color: #b4befe;
            margin-bottom: 6px;
        }

        button.action-btn {
            background: linear-gradient(135deg, #89b4fa, #74c7ec);
            color: #11111b;
            font-weight: bold;
            border-radius: 8px;
            padding: 10px 18px;
            border: none;
        }

        button.action-btn:hover {
            background: linear-gradient(135deg, #b4befe, #89dceb);
        }

        button.action-btn:disabled {
            background: #45475a;
            color: #6c7086;
        }

        button.copy-btn {
            background-color: #313244;
            color: #cdd6f4;
            border-radius: 6px;
            border: 1px solid #45475a;
            padding: 6px 12px;
        }

        button.copy-btn:hover {
            background-color: #45475a;
            color: #f5e0dc;
        }

        entry {
            background-color: #181926;
            color: #cdd6f4;
            border: 1px solid #45475a;
            border-radius: 8px;
            padding: 8px;
            font-family: monospace;
        }

        entry:focus {
            border-color: #89b4fa;
        }

        progressbar progress {
            background: linear-gradient(90deg, #89b4fa, #a6e3a1);
            border-radius: 6px;
        }

        progressbar trough {
            background-color: #181926;
            border-radius: 6px;
            min-height: 14px;
        }

        textview text {
            background-color: #11111b;
            color: #a6e3a1;
            font-family: monospace;
            font-size: 12px;
            padding: 8px;
        }

        notebook header {
            background-color: #181926;
            border-bottom: 1px solid #363a4f;
        }

        notebook tab {
            padding: 8px 16px;
            font-weight: 600;
            color: #a6adc8;
        }

        notebook tab:checked {
            color: #89b4fa;
            border-bottom: 2px solid #89b4fa;
        }
    )";

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    GdkDisplay* display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI Layout Construction
// ─────────────────────────────────────────────────────────────────────────────

static GtkWidget* BuildHeaderBar() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box, "header-box");

    GtkWidget* title = gtk_label_new("⚡ ZeroMesh BitTorrent P2P Directory Swarm");
    gtk_widget_add_css_class(title, "title-label");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);

    std::string sub = "Working Directory: " + g_app.current_dir.string();
    GtkWidget* subtitle = gtk_label_new(sub.c_str());
    gtk_widget_add_css_class(subtitle, "subtitle-label");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_box_append(GTK_BOX(box), subtitle);

    GtkWidget* h_stats = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_app.peers_label = gtk_label_new("Active Mesh Peers: 0");
    gtk_label_set_xalign(GTK_LABEL(g_app.peers_label), 0.0f);
    gtk_box_append(GTK_BOX(h_stats), g_app.peers_label);

    std::string ports_text = "DHT UDP: " + std::to_string(g_app.dht_port) + " | TCP Chunk Port: " + std::to_string(g_app.chunk_port);
    g_app.ports_label = gtk_label_new(ports_text.c_str());
    gtk_label_set_xalign(GTK_LABEL(g_app.ports_label), 0.0f);
    gtk_box_append(GTK_BOX(h_stats), g_app.ports_label);

    gtk_box_append(GTK_BOX(box), h_stats);
    return box;
}

static GtkWidget* BuildExportTab() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);

    // Card 1: Directory Export Action
    GtkWidget* card1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card1, "card");

    GtkWidget* c1_title = gtk_label_new("Export & Seed Current Directory");
    gtk_widget_add_css_class(c1_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(c1_title), 0.0f);
    gtk_box_append(GTK_BOX(card1), c1_title);

    std::string dir_info = "Folder to share: " + g_app.current_dir.string();
    g_app.export_dir_label = gtk_label_new(dir_info.c_str());
    gtk_label_set_xalign(GTK_LABEL(g_app.export_dir_label), 0.0f);
    gtk_box_append(GTK_BOX(card1), g_app.export_dir_label);

    g_app.export_btn = gtk_button_new_with_label("🚀 Create Magnet & Start Seeding");
    gtk_widget_add_css_class(g_app.export_btn, "action-btn");
    g_signal_connect(g_app.export_btn, "clicked", G_CALLBACK(OnExportClicked), nullptr);
    gtk_box_append(GTK_BOX(card1), g_app.export_btn);

    g_app.export_progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g_app.export_progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.export_progress_bar), "Ready to export");
    gtk_box_append(GTK_BOX(card1), g_app.export_progress_bar);

    g_app.export_status_label = gtk_label_new("Click 'Create Magnet' to chunk directory files and announce to DHT swarm.");
    gtk_label_set_xalign(GTK_LABEL(g_app.export_status_label), 0.0f);
    gtk_box_append(GTK_BOX(card1), g_app.export_status_label);

    gtk_box_append(GTK_BOX(root), card1);

    // Card 2: Generated Magnet Link
    GtkWidget* card2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card2, "card");

    GtkWidget* c2_title = gtk_label_new("Swarm Magnet URI (Share with other computers)");
    gtk_widget_add_css_class(c2_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(c2_title), 0.0f);
    gtk_box_append(GTK_BOX(card2), c2_title);

    GtkWidget* magnet_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_app.magnet_entry = gtk_entry_new();
    gtk_widget_set_hexpand(g_app.magnet_entry, TRUE);
    gtk_editable_set_editable(GTK_EDITABLE(g_app.magnet_entry), FALSE);
    gtk_box_append(GTK_BOX(magnet_box), g_app.magnet_entry);

    g_app.copy_magnet_btn = gtk_button_new_with_label("📋 Copy Magnet");
    gtk_widget_add_css_class(g_app.copy_magnet_btn, "copy-btn");
    gtk_widget_set_sensitive(g_app.copy_magnet_btn, FALSE);
    g_signal_connect(g_app.copy_magnet_btn, "clicked", G_CALLBACK(OnCopyMagnetClicked), nullptr);
    gtk_box_append(GTK_BOX(magnet_box), g_app.copy_magnet_btn);

    gtk_box_append(GTK_BOX(card2), magnet_box);
    gtk_box_append(GTK_BOX(root), card2);

    // Card 3: Seeding Activity Log
    GtkWidget* card3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card3, "card");
    gtk_widget_set_vexpand(card3, TRUE);

    GtkWidget* c3_title = gtk_label_new("Seeding Activity & Chunks");
    gtk_widget_add_css_class(c3_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(c3_title), 0.0f);
    gtk_box_append(GTK_BOX(card3), c3_title);

    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    g_app.seeded_files_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app.seeded_files_view), FALSE);
    g_app.seeded_files_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.seeded_files_view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), g_app.seeded_files_view);
    gtk_box_append(GTK_BOX(card3), sw);

    gtk_box_append(GTK_BOX(root), card3);
    return root;
}

static GtkWidget* BuildDownloadTab() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);

    // Card 1: Magnet Import & Start
    GtkWidget* card1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card1, "card");

    GtkWidget* c1_title = gtk_label_new("Import Torrent / Directory from Magnet");
    gtk_widget_add_css_class(c1_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(c1_title), 0.0f);
    gtk_box_append(GTK_BOX(card1), c1_title);

    GtkWidget* input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_app.import_magnet_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.import_magnet_entry),
                                   "Paste magnet:?xt=urn:zm:... or 64-char InfoHash here");
    gtk_widget_set_hexpand(g_app.import_magnet_entry, TRUE);
    gtk_box_append(GTK_BOX(input_box), g_app.import_magnet_entry);

    g_app.download_btn = gtk_button_new_with_label("⬇ Fetch & Reconstruct");
    gtk_widget_add_css_class(g_app.download_btn, "action-btn");
    g_signal_connect(g_app.download_btn, "clicked", G_CALLBACK(OnDownloadClicked), nullptr);
    gtk_box_append(GTK_BOX(input_box), g_app.download_btn);

    gtk_box_append(GTK_BOX(card1), input_box);

    std::string target_msg = "Download target: " + g_app.current_dir.string() + "/<torrent_name>";
    g_app.dest_dir_label = gtk_label_new(target_msg.c_str());
    gtk_label_set_xalign(GTK_LABEL(g_app.dest_dir_label), 0.0f);
    gtk_box_append(GTK_BOX(card1), g_app.dest_dir_label);

    g_app.download_progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g_app.download_progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.download_progress_bar), "Idle");
    gtk_box_append(GTK_BOX(card1), g_app.download_progress_bar);

    g_app.download_status_label = gtk_label_new("Awaiting Magnet URI...");
    gtk_label_set_xalign(GTK_LABEL(g_app.download_status_label), 0.0f);
    gtk_box_append(GTK_BOX(card1), g_app.download_status_label);

    gtk_box_append(GTK_BOX(root), card1);

    // Card 2: Live Download Stream Log
    GtkWidget* card2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card2, "card");
    gtk_widget_set_vexpand(card2, TRUE);

    GtkWidget* c2_title = gtk_label_new("Live Download & Piece Verification Log");
    gtk_widget_add_css_class(c2_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(c2_title), 0.0f);
    gtk_box_append(GTK_BOX(card2), c2_title);

    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    g_app.download_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app.download_log_view), FALSE);
    g_app.download_log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.download_log_view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), g_app.download_log_view);
    gtk_box_append(GTK_BOX(card2), sw);

    gtk_box_append(GTK_BOX(root), card2);
    return root;
}

static GtkWidget* BuildTelemetryTab() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "card");

    GtkWidget* title = gtk_label_new("Kademlia DHT & P2P Swarm Diagnostics");
    gtk_widget_add_css_class(title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(card), title);

    auto stats = g_app.engine->GetStats();
    std::string nid = "Local Node ID: " + stats.self_node_id;
    g_app.node_id_label = gtk_label_new(nid.c_str());
    gtk_label_set_xalign(GTK_LABEL(g_app.node_id_label), 0.0f);
    gtk_box_append(GTK_BOX(card), g_app.node_id_label);

    g_app.seeded_bytes_label = gtk_label_new("Total Seeded: 0 KB");
    gtk_label_set_xalign(GTK_LABEL(g_app.seeded_bytes_label), 0.0f);
    gtk_box_append(GTK_BOX(card), g_app.seeded_bytes_label);

    g_app.downloaded_bytes_label = gtk_label_new("Total Downloaded: 0 KB");
    gtk_label_set_xalign(GTK_LABEL(g_app.downloaded_bytes_label), 0.0f);
    gtk_box_append(GTK_BOX(card), g_app.downloaded_bytes_label);

    gtk_box_append(GTK_BOX(root), card);
    return root;
}

// ─────────────────────────────────────────────────────────────────────────────
// Application Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

static void OnAppActivate(GtkApplication* app, gpointer) {
    ApplyModernCss();

    g_app.window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_app.window), "ZeroMesh P2P Directory Torrent Swarm");
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 780, 580);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // 1. Header
    gtk_box_append(GTK_BOX(main_vbox), BuildHeaderBar());

    // 2. Notebook (Tabs)
    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), BuildExportTab(), gtk_label_new("📤 Export & Seed"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), BuildDownloadTab(), gtk_label_new("📥 Download & Reconstruct"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), BuildTelemetryTab(), gtk_label_new("🌐 Mesh Telemetry"));

    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), notebook);

    gtk_window_set_child(GTK_WINDOW(g_app.window), main_vbox);
    gtk_window_present(GTK_WINDOW(g_app.window));

    // Start periodic 1-second telemetry timer
    g_timeout_add_seconds(1, OnTelemetryTimer, nullptr);
}

int main(int argc, char** argv) {
    // Determine working directory
    g_app.current_dir = std::filesystem::current_path();

    // Parse optional arguments: --dir <path> --dht-port <port> --chunk-port <port>
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            g_app.current_dir = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--dht-port" && i + 1 < argc) {
            g_app.dht_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--chunk-port" && i + 1 < argc) {
            g_app.chunk_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    std::cout << "[zm-torrent-gui] Starting ZeroMesh Torrent App\n"
              << "  Directory:  " << g_app.current_dir << "\n"
              << "  DHT Port:   " << g_app.dht_port << "\n"
              << "  Chunk Port: " << g_app.chunk_port << "\n"
              << std::endl;

    // Storage for CAS and ChunkCache under ~/.zeromesh/torrent_<port>
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".");
    auto storage = home / ".zeromesh" / ("torrent_" + std::to_string(g_app.dht_port));
    g_app.engine = std::make_unique<TorrentEngine>(storage, g_app.dht_port, g_app.chunk_port);
    g_app.engine->Start();

    GtkApplication* app = gtk_application_new("org.zeromesh.torrent", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(OnAppActivate), nullptr);

    int status = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);

    if (g_app.engine) {
        g_app.engine->Stop();
    }
    return status;
}
