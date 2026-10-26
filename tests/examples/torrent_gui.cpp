#include "tests/examples/torrent_engine.hpp"
#include "tests/examples/torrent_manifest.hpp"

#include <gtk/gtk.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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
// Logging Subsystem
// ─────────────────────────────────────────────────────────────────────────────

enum class LogLevel {
    Info,
    Success,
    Warn,
    Error
};

struct LogEntry {
    std::string timestamp;
    LogLevel level;
    std::string component;
    std::string message;
};

class GuiLogger {
public:
    static GuiLogger& Instance() {
        static GuiLogger instance;
        return instance;
    }

    void Init(const std::filesystem::path& log_file_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_file_path_ = log_file_path;
        std::filesystem::create_directories(log_file_path.parent_path());
        log_file_.open(log_file_path, std::ios::app);
        if (log_file_) {
            log_file_ << "\n=== ZeroMesh Torrent Swarm GUI Session Started: "
                      << CurrentIsoTimestamp() << " ===\n" << std::flush;
        }
    }

    void Log(LogLevel level, const std::string& component, const std::string& msg);

    [[nodiscard]] uint32_t GetErrorCount() const { return error_count_.load(); }
    [[nodiscard]] uint32_t GetWarnCount() const { return warn_count_.load(); }
    [[nodiscard]] std::filesystem::path GetLogFilePath() const { return log_file_path_; }

    void ClearMemoryLogs() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        error_count_.store(0);
        warn_count_.store(0);
    }

    std::vector<LogEntry> GetEntries() {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    GuiLogger() = default;

    static std::string CurrentTimeStr() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S");
        return ss.str();
    }

    static std::string CurrentIsoTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    std::mutex mutex_;
    std::filesystem::path log_file_path_;
    std::ofstream log_file_;
    std::vector<LogEntry> entries_;
    std::atomic<uint32_t> error_count_{0};
    std::atomic<uint32_t> warn_count_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// GUI Context
// ─────────────────────────────────────────────────────────────────────────────

struct AppContext {
    std::filesystem::path current_dir;
    uint16_t dht_port{8982};
    uint16_t chunk_port{8984};
    std::unique_ptr<TorrentEngine> engine;

    // Window & Navigation
    GtkWidget* window{nullptr};
    GtkWidget* notebook{nullptr};

    // Header & Alert Banner
    GtkWidget* status_label{nullptr};
    GtkWidget* peers_label{nullptr};
    GtkWidget* ports_label{nullptr};
    GtkWidget* error_badge_label{nullptr};
    GtkWidget* error_banner_box{nullptr};
    GtkWidget* error_banner_label{nullptr};

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

    // System & Error Logs Tab
    GtkWidget* log_tab_label{nullptr};
    GtkWidget* system_log_view{nullptr};
    GtkTextBuffer* system_log_buf{nullptr};
    GtkWidget* log_filter_combo{nullptr};
    GtkWidget* log_search_entry{nullptr};
    GtkWidget* clear_logs_btn{nullptr};
    GtkWidget* copy_logs_btn{nullptr};
    GtkWidget* open_log_file_btn{nullptr};
    int current_log_filter{0}; // 0 = All, 1 = Errors Only, 2 = Warnings & Errors
};

static AppContext g_app;

// ─────────────────────────────────────────────────────────────────────────────
// UI Dispatch Helpers
// ─────────────────────────────────────────────────────────────────────────────

struct LogDispatchPayload {
    LogEntry entry;
};

static void AppendFormattedLogToBuffer(GtkTextBuffer* buf, const LogEntry& entry) {
    if (!buf) return;

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);

    // 1. Timestamp in muted gray
    std::string time_str = "[" + entry.timestamp + "] ";
    gtk_text_buffer_insert_with_tags_by_name(buf, &end, time_str.c_str(), -1, "time_tag", NULL);

    // 2. Level tag in colored badge
    const char* tag_name = "info_tag";
    const char* level_text = "[INFO] ";
    switch (entry.level) {
        case LogLevel::Info:
            tag_name = "info_tag";
            level_text = "[INFO] ";
            break;
        case LogLevel::Success:
            tag_name = "success_tag";
            level_text = "[OK]   ";
            break;
        case LogLevel::Warn:
            tag_name = "warn_tag";
            level_text = "[WARN] ";
            break;
        case LogLevel::Error:
            tag_name = "error_tag";
            level_text = "[ERR]  ";
            break;
    }
    gtk_text_buffer_insert_with_tags_by_name(buf, &end, level_text, -1, tag_name, NULL);

    // 3. Component in lavender
    if (!entry.component.empty()) {
        std::string comp = "[" + entry.component + "] ";
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, comp.c_str(), -1, "comp_tag", NULL);
    }

    // 4. Message text
    std::string msg = entry.message + "\n";
    if (entry.level == LogLevel::Error) {
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, msg.c_str(), -1, "error_msg_tag", NULL);
    } else {
        gtk_text_buffer_insert(buf, &end, msg.c_str(), -1);
    }
}

static void ShowErrorBanner(const std::string& err_msg) {
    if (g_app.error_banner_box && g_app.error_banner_label) {
        std::string banner_text = "🚨 " + err_msg;
        gtk_label_set_text(GTK_LABEL(g_app.error_banner_label), banner_text.c_str());
        gtk_widget_set_visible(g_app.error_banner_box, TRUE);
    }
}

static void HideErrorBanner() {
    if (g_app.error_banner_box) {
        gtk_widget_set_visible(g_app.error_banner_box, FALSE);
    }
}

static gboolean OnLogDispatchIdle(gpointer data) {
    auto* p = static_cast<LogDispatchPayload*>(data);
    const auto& entry = p->entry;

    // Check filter criteria for the system log tab
    bool show_in_system_log = true;
    if (g_app.current_log_filter == 1 && entry.level != LogLevel::Error) {
        show_in_system_log = false;
    } else if (g_app.current_log_filter == 2 && entry.level != LogLevel::Error && entry.level != LogLevel::Warn) {
        show_in_system_log = false;
    }

    if (show_in_system_log && g_app.system_log_buf) {
        AppendFormattedLogToBuffer(g_app.system_log_buf, entry);
    }

    // Also forward to contextual buffers
    if (entry.component == "EXPORT" && g_app.seeded_files_buf) {
        AppendFormattedLogToBuffer(g_app.seeded_files_buf, entry);
    } else if (entry.component == "DOWNLOAD" && g_app.download_log_buf) {
        AppendFormattedLogToBuffer(g_app.download_log_buf, entry);
    }

    // Update error badge in header
    uint32_t errs = GuiLogger::Instance().GetErrorCount();
    uint32_t warns = GuiLogger::Instance().GetWarnCount();
    if (g_app.error_badge_label) {
        if (errs > 0) {
            std::string text = "🚨 " + std::to_string(errs) + " Error" + (errs > 1 ? "s" : "");
            if (warns > 0) text += " | ⚠️ " + std::to_string(warns);
            gtk_label_set_text(GTK_LABEL(g_app.error_badge_label), text.c_str());
            gtk_widget_set_visible(g_app.error_badge_label, TRUE);
        } else if (warns > 0) {
            std::string text = "⚠️ " + std::to_string(warns) + " Warning" + (warns > 1 ? "s" : "");
            gtk_label_set_text(GTK_LABEL(g_app.error_badge_label), text.c_str());
            gtk_widget_set_visible(g_app.error_badge_label, TRUE);
        } else {
            gtk_label_set_text(GTK_LABEL(g_app.error_badge_label), "✓ 0 Errors");
            gtk_widget_set_visible(g_app.error_badge_label, TRUE);
        }
    }

    // Update Logs Tab label counter
    if (g_app.log_tab_label) {
        if (errs > 0) {
            std::string t = "📋 Logs (" + std::to_string(errs) + " Err)";
            gtk_label_set_text(GTK_LABEL(g_app.log_tab_label), t.c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(g_app.log_tab_label), "📋 System Logs");
        }
    }

    // If an error occurred, show the prominent error banner
    if (entry.level == LogLevel::Error) {
        ShowErrorBanner(entry.message);
    }

    delete p;
    return G_SOURCE_REMOVE;
}

void GuiLogger::Log(LogLevel level, const std::string& component, const std::string& msg) {
    LogEntry entry{CurrentTimeStr(), level, component, msg};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level == LogLevel::Error) error_count_++;
        if (level == LogLevel::Warn) warn_count_++;
        entries_.push_back(entry);

        // Persistent file write
        if (log_file_) {
            const char* tag = "INFO";
            if (level == LogLevel::Success) tag = "OK";
            if (level == LogLevel::Warn) tag = "WARN";
            if (level == LogLevel::Error) tag = "ERROR";

            log_file_ << "[" << CurrentIsoTimestamp() << "] [" << tag << "] ["
                      << component << "] " << msg << "\n" << std::flush;
        }
    }

    // Dispatch to GTK main thread
    auto* payload = new LogDispatchPayload{std::move(entry)};
    g_idle_add(OnLogDispatchIdle, payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// Progress UI Dispatch
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
    HideErrorBanner();
    gtk_widget_set_sensitive(g_app.export_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_app.export_status_label), "Scanning directory & chunking files...");

    GuiLogger::Instance().Log(LogLevel::Info, "EXPORT",
                              "Initiating directory export on path: " + g_app.current_dir.string());

    std::thread([dir = g_app.current_dir]() {
        try {
            auto [manifest, info_hash] = g_app.engine->ExportDirectory(
                dir, 256 * 1024,
                [](float frac, const std::string& status, uint64_t done, uint64_t total) {
                    auto* p = new ProgressPayload{frac, status, done, total, false};
                    g_idle_add(OnProgressIdle, p);
                });

            std::string lan_ip = GetLocalLanIp();
            std::string magnet = TorrentManifest::MakeMagnetUri(
                info_hash, manifest.name, manifest.total_bytes,
                lan_ip, g_app.dht_port, g_app.chunk_port);

            GuiLogger::Instance().Log(LogLevel::Success, "EXPORT",
                                      "Export succeeded. InfoHash: " + info_hash + " (" +
                                      std::to_string(manifest.files.size()) + " files, " +
                                      std::to_string(manifest.total_bytes / 1024) + " KB)");
            if (!lan_ip.empty() && lan_ip != "127.0.0.1") {
                GuiLogger::Instance().Log(LogLevel::Info, "NETWORK",
                                          "Bound seeder bootstrap endpoint: " + lan_ip + ":" +
                                          std::to_string(g_app.dht_port) + " (chunk TCP :" +
                                          std::to_string(g_app.chunk_port) + ")");
            }

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

                delete f;
                return G_SOURCE_REMOVE;
            }, fin);

        } catch (const std::exception& ex) {
            std::string err = std::string("Export failed: ") + ex.what();
            GuiLogger::Instance().Log(LogLevel::Error, "EXPORT", err);

            g_idle_add([](gpointer) -> gboolean {
                gtk_widget_set_sensitive(g_app.export_btn, TRUE);
                gtk_label_set_text(GTK_LABEL(g_app.export_status_label), "Export failed. See Error Logs.");
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
            GuiLogger::Instance().Log(LogLevel::Info, "EXPORT", "Magnet URI copied to system clipboard");
        }
    }
}

static void OnDownloadClicked(GtkWidget*, gpointer) {
    HideErrorBanner();
    const char* raw_input = gtk_editable_get_text(GTK_EDITABLE(g_app.import_magnet_entry));
    if (!raw_input || std::strlen(raw_input) == 0) {
        std::string err = "Please paste a Magnet URI or 64-char InfoHash before clicking Download.";
        gtk_label_set_text(GTK_LABEL(g_app.download_status_label), err.c_str());
        GuiLogger::Instance().Log(LogLevel::Warn, "DOWNLOAD", err);
        return;
    }

    auto meta_opt = TorrentManifest::ExtractMagnetMetadata(raw_input);
    if (!meta_opt || meta_opt->info_hash.empty()) {
        std::string err = "Invalid Magnet URI or InfoHash: input must contain a valid 64-character SHA-256 hash";
        gtk_label_set_text(GTK_LABEL(g_app.download_status_label), err.c_str());
        GuiLogger::Instance().Log(LogLevel::Error, "DOWNLOAD", err);
        return;
    }

    auto meta = *meta_opt;
    std::string info_hash = meta.info_hash;
    gtk_widget_set_sensitive(g_app.download_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_app.download_status_label), "Resolving manifest in DHT swarm...");

    GuiLogger::Instance().Log(LogLevel::Info, "DOWNLOAD", "Resolving torrent InfoHash: " + info_hash);
    if (!meta.peer_ip.empty()) {
        GuiLogger::Instance().Log(LogLevel::Info, "NETWORK",
                                  "Discovered bootstrap seeder endpoint in Magnet: " +
                                  meta.peer_ip + ":" + std::to_string(meta.dht_port) +
                                  " (chunk TCP :" + std::to_string(meta.chunk_port) + ")");
    }

    std::thread([meta, dest_dir = g_app.current_dir]() {
        auto res = g_app.engine->DownloadTorrent(
            meta.info_hash, dest_dir,
            [](float frac, const std::string& status, uint64_t done, uint64_t total) {
                auto* p = new ProgressPayload{frac, status, done, total, true};
                g_idle_add(OnProgressIdle, p);
            },
            meta.peer_ip, meta.dht_port, meta.chunk_port);

        if (res.success) {
            GuiLogger::Instance().Log(LogLevel::Success, "DOWNLOAD",
                                      "Torrent download completed successfully (" +
                                      std::to_string(res.bytes_downloaded / 1024) + " KB). Now seeding to mesh.");
        } else {
            GuiLogger::Instance().Log(LogLevel::Error, "DOWNLOAD", res.error_message);
        }

        struct FinishDownloadPayload {
            bool success;
            std::string error_message;
        };

        auto* fin = new FinishDownloadPayload{res.success, res.error_message};
        g_idle_add([](gpointer d) -> gboolean {
            auto* f = static_cast<FinishDownloadPayload*>(d);
            gtk_widget_set_sensitive(g_app.download_btn, TRUE);
            if (f->success) {
                gtk_label_set_text(GTK_LABEL(g_app.download_status_label),
                                   "✓ Download Finished! Files verified & now seeding to mesh.");
            } else {
                gtk_label_set_text(GTK_LABEL(g_app.download_status_label),
                                   ("Download failed: " + f->error_message).c_str());
            }
            delete f;
            return G_SOURCE_REMOVE;
        }, fin);
    }).detach();
}


static void OnDismissErrorBannerClicked(GtkWidget*, gpointer) {
    HideErrorBanner();
}

static void OnViewLogsFromBannerClicked(GtkWidget*, gpointer) {
    HideErrorBanner();
    if (g_app.notebook) {
        // Switch to the Logs tab (page 3)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_app.notebook), 3);
    }
}

static void OnClearLogsClicked(GtkWidget*, gpointer) {
    GuiLogger::Instance().ClearMemoryLogs();
    if (g_app.system_log_buf) {
        gtk_text_buffer_set_text(g_app.system_log_buf, "", 0);
    }
    if (g_app.error_badge_label) {
        gtk_label_set_text(GTK_LABEL(g_app.error_badge_label), "✓ 0 Errors");
    }
    if (g_app.log_tab_label) {
        gtk_label_set_text(GTK_LABEL(g_app.log_tab_label), "📋 System Logs");
    }
    HideErrorBanner();
}

static void OnCopyAllLogsClicked(GtkWidget*, gpointer) {
    if (!g_app.system_log_buf) return;

    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(g_app.system_log_buf, &start);
    gtk_text_buffer_get_end_iter(g_app.system_log_buf, &end);
    char* text = gtk_text_buffer_get_text(g_app.system_log_buf, &start, &end, FALSE);

    if (text) {
        GdkDisplay* display = gdk_display_get_default();
        if (display) {
            GdkClipboard* cb = gdk_display_get_clipboard(display);
            gdk_clipboard_set_text(cb, text);
            GuiLogger::Instance().Log(LogLevel::Info, "SYSTEM", "Entire log buffer copied to clipboard");
        }
        g_free(text);
    }
}

static void OnLogFilterChanged(GObject* object, GParamSpec*, gpointer) {
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    g_app.current_log_filter = static_cast<int>(selected);

    // Refresh view from GuiLogger memory buffer
    if (g_app.system_log_buf) {
        gtk_text_buffer_set_text(g_app.system_log_buf, "", 0);
        auto entries = GuiLogger::Instance().GetEntries();
        for (const auto& e : entries) {
            if (g_app.current_log_filter == 1 && e.level != LogLevel::Error) continue;
            if (g_app.current_log_filter == 2 && e.level != LogLevel::Error && e.level != LogLevel::Warn) continue;
            AppendFormattedLogToBuffer(g_app.system_log_buf, e);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GTK4 CSS Styling with Tag Colors & Alert Banners
// ─────────────────────────────────────────────────────────────────────────────

static void SetupTextBufferTags(GtkTextBuffer* buf) {
    gtk_text_buffer_create_tag(buf, "time_tag",
                               "foreground", "#6c7086",
                               "font", "Monospace 11",
                               NULL);
    gtk_text_buffer_create_tag(buf, "comp_tag",
                               "foreground", "#b4befe",
                               "weight", PANGO_WEIGHT_BOLD,
                               "font", "Monospace 11",
                               NULL);
    gtk_text_buffer_create_tag(buf, "info_tag",
                               "foreground", "#89dceb",
                               "weight", PANGO_WEIGHT_BOLD,
                               "font", "Monospace 11",
                               NULL);
    gtk_text_buffer_create_tag(buf, "success_tag",
                               "foreground", "#a6e3a1",
                               "weight", PANGO_WEIGHT_BOLD,
                               "font", "Monospace 11",
                               NULL);
    gtk_text_buffer_create_tag(buf, "warn_tag",
                               "foreground", "#f9e2af",
                               "weight", PANGO_WEIGHT_BOLD,
                               "font", "Monospace 11",
                               NULL);
    gtk_text_buffer_create_tag(buf, "error_tag",
                               "foreground", "#f38ba8",
                               "weight", PANGO_WEIGHT_BOLD,
                               "font", "Monospace 11",
                               NULL);
    gtk_text_buffer_create_tag(buf, "error_msg_tag",
                               "foreground", "#f38ba8",
                               "font", "Monospace 11",
                               NULL);
}

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
            padding: 14px 18px;
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

        .error-badge {
            background-color: #313244;
            color: #f38ba8;
            font-weight: bold;
            font-size: 12px;
            padding: 4px 10px;
            border-radius: 12px;
            border: 1px solid #f38ba8;
        }

        .error-banner {
            background: linear-gradient(90deg, #451b24, #311822);
            border-left: 5px solid #f38ba8;
            border-bottom: 1px solid #f38ba8;
            padding: 10px 16px;
        }

        .error-banner-label {
            color: #f5c2e7;
            font-weight: bold;
            font-size: 13px;
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

        button.tool-btn {
            background-color: #313244;
            color: #cdd6f4;
            border-radius: 6px;
            border: 1px solid #45475a;
            padding: 6px 12px;
        }

        button.tool-btn:hover {
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
            color: #cdd6f4;
            font-family: 'JetBrains Mono', 'Fira Code', 'DejaVu Sans Mono', monospace;
            font-size: 12px;
            padding: 10px;
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

    GtkWidget* top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* title = gtk_label_new("⚡ ZeroMesh BitTorrent P2P Directory Swarm");
    gtk_widget_add_css_class(title, "title-label");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(top_row), title);

    g_app.error_badge_label = gtk_label_new("✓ 0 Errors");
    gtk_widget_add_css_class(g_app.error_badge_label, "error-badge");
    gtk_box_append(GTK_BOX(top_row), g_app.error_badge_label);

    gtk_box_append(GTK_BOX(box), top_row);

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

    // Error Alert Banner (Hidden by default, revealed on error)
    g_app.error_banner_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(g_app.error_banner_box, "error-banner");
    gtk_widget_set_visible(g_app.error_banner_box, FALSE);

    g_app.error_banner_label = gtk_label_new("No active errors.");
    gtk_widget_add_css_class(g_app.error_banner_label, "error-banner-label");
    gtk_label_set_xalign(GTK_LABEL(g_app.error_banner_label), 0.0f);
    gtk_widget_set_hexpand(g_app.error_banner_label, TRUE);
    gtk_box_append(GTK_BOX(g_app.error_banner_box), g_app.error_banner_label);

    GtkWidget* view_logs_btn = gtk_button_new_with_label("🔍 Inspect Logs");
    gtk_widget_add_css_class(view_logs_btn, "tool-btn");
    g_signal_connect(view_logs_btn, "clicked", G_CALLBACK(OnViewLogsFromBannerClicked), nullptr);
    gtk_box_append(GTK_BOX(g_app.error_banner_box), view_logs_btn);

    GtkWidget* dismiss_banner_btn = gtk_button_new_with_label("✕ Dismiss");
    gtk_widget_add_css_class(dismiss_banner_btn, "tool-btn");
    g_signal_connect(dismiss_banner_btn, "clicked", G_CALLBACK(OnDismissErrorBannerClicked), nullptr);
    gtk_box_append(GTK_BOX(g_app.error_banner_box), dismiss_banner_btn);

    gtk_box_append(GTK_BOX(box), g_app.error_banner_box);
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
    gtk_widget_add_css_class(g_app.copy_magnet_btn, "tool-btn");
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
    SetupTextBufferTags(g_app.seeded_files_buf);
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

    std::string target_msg = "Download destination: " + g_app.current_dir.string() + "/<torrent_name>";
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

    GtkWidget* c2_title = gtk_label_new("Download & Verification Progress");
    gtk_widget_add_css_class(c2_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(c2_title), 0.0f);
    gtk_box_append(GTK_BOX(card2), c2_title);

    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    g_app.download_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app.download_log_view), FALSE);
    g_app.download_log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.download_log_view));
    SetupTextBufferTags(g_app.download_log_buf);
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

    std::string log_file_info = "Error Log Path: " + GuiLogger::Instance().GetLogFilePath().string();
    GtkWidget* path_lbl = gtk_label_new(log_file_info.c_str());
    gtk_label_set_xalign(GTK_LABEL(path_lbl), 0.0f);
    gtk_box_append(GTK_BOX(card), path_lbl);

    gtk_box_append(GTK_BOX(root), card);
    return root;
}

static GtkWidget* BuildSystemLogsTab() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);

    // Filter and Tool Bar
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(toolbar, "card");

    GtkWidget* filter_label = gtk_label_new("Filter:");
    gtk_box_append(GTK_BOX(toolbar), filter_label);

    const char* filter_items[] = {"All Logs (Verbose)", "🚨 Errors Only", "⚠️ Warnings & Errors", NULL};
    g_app.log_filter_combo = gtk_drop_down_new_from_strings(filter_items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_app.log_filter_combo), 0);
    g_signal_connect(g_app.log_filter_combo, "notify::selected", G_CALLBACK(OnLogFilterChanged), nullptr);
    gtk_box_append(GTK_BOX(toolbar), g_app.log_filter_combo);

    GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sep, TRUE);
    gtk_box_append(GTK_BOX(toolbar), sep);

    g_app.copy_logs_btn = gtk_button_new_with_label("📋 Copy All");
    gtk_widget_add_css_class(g_app.copy_logs_btn, "tool-btn");
    g_signal_connect(g_app.copy_logs_btn, "clicked", G_CALLBACK(OnCopyAllLogsClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), g_app.copy_logs_btn);

    g_app.clear_logs_btn = gtk_button_new_with_label("🗑️ Clear View");
    gtk_widget_add_css_class(g_app.clear_logs_btn, "tool-btn");
    g_signal_connect(g_app.clear_logs_btn, "clicked", G_CALLBACK(OnClearLogsClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), g_app.clear_logs_btn);

    gtk_box_append(GTK_BOX(root), toolbar);

    // Scrolled Console View
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_vexpand(card, TRUE);

    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);

    g_app.system_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app.system_log_view), FALSE);
    g_app.system_log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.system_log_view));
    SetupTextBufferTags(g_app.system_log_buf);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), g_app.system_log_view);
    gtk_box_append(GTK_BOX(card), sw);
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
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 820, 620);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // 1. Header with Error Alert Bar
    gtk_box_append(GTK_BOX(main_vbox), BuildHeaderBar());

    // 2. Notebook (Tabs)
    g_app.notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(g_app.notebook), BuildExportTab(), gtk_label_new("📤 Export & Seed"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_app.notebook), BuildDownloadTab(), gtk_label_new("📥 Download & Reconstruct"));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_app.notebook), BuildTelemetryTab(), gtk_label_new("🌐 Mesh Telemetry"));

    g_app.log_tab_label = gtk_label_new("📋 System Logs");
    gtk_notebook_append_page(GTK_NOTEBOOK(g_app.notebook), BuildSystemLogsTab(), g_app.log_tab_label);

    gtk_widget_set_vexpand(g_app.notebook, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), g_app.notebook);

    gtk_window_set_child(GTK_WINDOW(g_app.window), main_vbox);
    gtk_window_present(GTK_WINDOW(g_app.window));

    // Initial system greeting in log
    GuiLogger::Instance().Log(LogLevel::Info, "SYSTEM",
                              "ZeroMesh Torrent GUI ready. Active directory: " + g_app.current_dir.string());
    GuiLogger::Instance().Log(LogLevel::Info, "NETWORK",
                              "Kademlia DHT listening on UDP :" + std::to_string(g_app.dht_port) +
                              ", TCP Chunk Server on :" + std::to_string(g_app.chunk_port));

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

    // Storage for CAS, ChunkCache, and Error Logs under ~/.zeromesh/torrent_<port>
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".");
    auto storage = home / ".zeromesh" / ("torrent_" + std::to_string(g_app.dht_port));
    std::filesystem::create_directories(storage);

    // Initialize GuiLogger to write persistent log file
    auto log_file = storage / "gui_errors.log";
    GuiLogger::Instance().Init(log_file);

    std::cout << "[zm-torrent-gui] Starting ZeroMesh Torrent App\n"
              << "  Directory:  " << g_app.current_dir << "\n"
              << "  DHT Port:   " << g_app.dht_port << "\n"
              << "  Chunk Port: " << g_app.chunk_port << "\n"
              << "  Log File:   " << log_file << "\n"
              << std::endl;

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
