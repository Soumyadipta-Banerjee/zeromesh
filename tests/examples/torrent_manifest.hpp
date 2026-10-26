#pragma once

#include "cas/store.hpp"
#include "dht/kademlia.hpp"
#include "vfs/chunk_fetcher.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::examples {

// ─────────────────────────────────────────────────────────────────────────────
// Torrent-like Manifest Structure
// ─────────────────────────────────────────────────────────────────────────────

struct TorrentFileInfo {
    std::string relative_path;
    uint64_t size_bytes{0};
    uint32_t mode{0644};
    std::vector<std::string> chunk_hashes;
};

struct TorrentManifest {
    std::string name;
    uint64_t total_bytes{0};
    size_t chunk_size_bytes{256 * 1024}; // 256 KB chunks
    uint64_t created_at_sec{0};
    std::vector<TorrentFileInfo> files;

    /**
     * @brief Serializes the manifest into a compact, human-readable JSON string.
     */
    [[nodiscard]] std::string Serialize() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"name\": \"" << EscapeJson(name) << "\",\n";
        ss << "  \"total_bytes\": " << total_bytes << ",\n";
        ss << "  \"chunk_size\": " << chunk_size_bytes << ",\n";
        ss << "  \"created_at\": " << created_at_sec << ",\n";
        ss << "  \"files\": [\n";
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            ss << "    {\n";
            ss << "      \"path\": \"" << EscapeJson(f.relative_path) << "\",\n";
            ss << "      \"size\": " << f.size_bytes << ",\n";
            ss << "      \"mode\": " << f.mode << ",\n";
            ss << "      \"chunks\": [";
            for (size_t c = 0; c < f.chunk_hashes.size(); ++c) {
                ss << "\"" << f.chunk_hashes[c] << "\"";
                if (c + 1 < f.chunk_hashes.size()) ss << ", ";
            }
            ss << "]\n";
            ss << "    }" << (i + 1 < files.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}\n";
        return ss.str();
    }

    /**
     * @brief Parses a manifest from a JSON string.
     */
    static std::optional<TorrentManifest> Deserialize(std::string_view json) {
        TorrentManifest m;
        // Simple, robust zero-dependency parser for manifest JSON
        auto find_str = [&](std::string_view key) -> std::optional<std::string> {
            std::string search = "\"" + std::string(key) + "\": \"";
            auto pos = json.find(search);
            if (pos == std::string_view::npos) return std::nullopt;
            pos += search.size();
            auto end = json.find("\"", pos);
            if (end == std::string_view::npos) return std::nullopt;
            return std::string(json.substr(pos, end - pos));
        };

        auto find_uint = [&](std::string_view key) -> std::optional<uint64_t> {
            std::string search = "\"" + std::string(key) + "\": ";
            auto pos = json.find(search);
            if (pos == std::string_view::npos) return std::nullopt;
            pos += search.size();
            size_t i = pos;
            while (i < json.size() && (std::isdigit(json[i]) || json[i] == ' ')) i++;
            try {
                return std::stoull(std::string(json.substr(pos, i - pos)));
            } catch (...) {
                return std::nullopt;
            }
        };

        auto name_opt = find_str("name");
        if (!name_opt) return std::nullopt;
        m.name = *name_opt;
        m.total_bytes = find_uint("total_bytes").value_or(0);
        m.chunk_size_bytes = static_cast<size_t>(find_uint("chunk_size").value_or(256 * 1024));
        m.created_at_sec = find_uint("created_at").value_or(0);

        // Parse files array
        auto files_pos = json.find("\"files\": [");
        if (files_pos == std::string_view::npos) return std::nullopt;
        auto current_pos = files_pos;

        while (true) {
            auto obj_start = json.find('{', current_pos);
            if (obj_start == std::string_view::npos) break;
            auto obj_end = json.find('}', obj_start);
            if (obj_end == std::string_view::npos) break;

            std::string_view file_block = json.substr(obj_start, obj_end - obj_start + 1);

            TorrentFileInfo fi;
            // Extract path
            std::string path_key = "\"path\": \"";
            auto p1 = file_block.find(path_key);
            if (p1 != std::string_view::npos) {
                p1 += path_key.size();
                auto p2 = file_block.find("\"", p1);
                if (p2 != std::string_view::npos) {
                    fi.relative_path = UnescapeJson(file_block.substr(p1, p2 - p1));
                }
            }

            // Extract size
            std::string size_key = "\"size\": ";
            auto s1 = file_block.find(size_key);
            if (s1 != std::string_view::npos) {
                s1 += size_key.size();
                size_t s2 = s1;
                while (s2 < file_block.size() && std::isdigit(file_block[s2])) s2++;
                try {
                    fi.size_bytes = std::stoull(std::string(file_block.substr(s1, s2 - s1)));
                } catch (...) {}
            }

            // Extract mode
            std::string mode_key = "\"mode\": ";
            auto m1 = file_block.find(mode_key);
            if (m1 != std::string_view::npos) {
                m1 += mode_key.size();
                size_t m2 = m1;
                while (m2 < file_block.size() && std::isdigit(file_block[m2])) m2++;
                try {
                    fi.mode = static_cast<uint32_t>(std::stoul(std::string(file_block.substr(m1, m2 - m1))));
                } catch (...) {}
            }

            // Extract chunk hashes
            std::string chunks_key = "\"chunks\": [";
            auto c1 = file_block.find(chunks_key);
            if (c1 != std::string_view::npos) {
                c1 += chunks_key.size();
                auto c2 = file_block.find(']', c1);
                if (c2 != std::string_view::npos) {
                    std::string_view chunks_str = file_block.substr(c1, c2 - c1);
                    size_t cp = 0;
                    while (cp < chunks_str.size()) {
                        auto q1 = chunks_str.find('"', cp);
                        if (q1 == std::string_view::npos) break;
                        auto q2 = chunks_str.find('"', q1 + 1);
                        if (q2 == std::string_view::npos) break;
                        fi.chunk_hashes.emplace_back(chunks_str.substr(q1 + 1, q2 - q1 - 1));
                        cp = q2 + 1;
                    }
                }
            }

            if (!fi.relative_path.empty()) {
                m.files.push_back(std::move(fi));
            }

            current_pos = obj_end + 1;
            // Check if we hit the closing bracket of files array
            auto next_bracket = json.find(']', current_pos);
            auto next_obj = json.find('{', current_pos);
            if (next_bracket != std::string_view::npos && (next_obj == std::string_view::npos || next_bracket < next_obj)) {
                break;
            }
        }

        return m;
    }

    /**
     * @brief Computes the SHA-256 hash of the manifest string (acts as the Torrent InfoHash).
     */
    static std::string ComputeInfoHash(const std::string& manifest_json) {
        auto d = cas::CasStore::ComputeDigest(manifest_json);
        return d.hash;
    }

    struct MagnetMetadata {
        std::string info_hash;
        std::string display_name;
        uint64_t total_bytes{0};
        std::string peer_ip;
        uint16_t dht_port{0};
        uint16_t chunk_port{0};
    };

    /**
     * @brief Generates a standard BitTorrent-like Magnet URI from the manifest info-hash,
     * optionally including bootstrap peer address and data ports.
     */
    static std::string MakeMagnetUri(const std::string& info_hash,
                                     const std::string& display_name,
                                     uint64_t total_bytes,
                                     const std::string& seeder_ip = "",
                                     uint16_t dht_port = 0,
                                     uint16_t chunk_port = 0) {
        std::ostringstream ss;
        ss << "magnet:?xt=urn:zm:" << info_hash
           << "&dn=" << UrlEncode(display_name)
           << "&xl=" << total_bytes;
        if (!seeder_ip.empty() && dht_port > 0) {
            ss << "&pe=" << seeder_ip << ":" << dht_port;
            if (chunk_port > 0) {
                ss << "&cp=" << chunk_port;
            }
        }
        return ss.str();
    }

    /**
     * @brief Extracts structured metadata and bootstrap peer info from a Magnet link or raw hash.
     */
    static std::optional<MagnetMetadata> ExtractMagnetMetadata(std::string_view input) {
        // Trim whitespace
        while (!input.empty() && std::isspace(input.front())) input.remove_prefix(1);
        while (!input.empty() && std::isspace(input.back())) input.remove_suffix(1);

        MagnetMetadata meta;

        // Check if raw 64-char hex
        if (input.size() == 64) {
            bool all_hex = true;
            for (char c : input) {
                if (!std::isxdigit(c)) { all_hex = false; break; }
            }
            if (all_hex) {
                meta.info_hash = std::string(input);
                return meta;
            }
        }

        // Find xt=urn:zm:<hash> or xt=urn:btih:<hash>
        std::string_view prefix = "urn:zm:";
        auto pos = input.find(prefix);
        if (pos == std::string_view::npos) {
            prefix = "urn:btih:";
            pos = input.find(prefix);
        }

        if (pos != std::string_view::npos) {
            auto hash_part = input.substr(pos + prefix.size(), 64);
            if (hash_part.size() == 64) {
                meta.info_hash = std::string(hash_part);
            }
        }

        if (meta.info_hash.empty()) {
            return std::nullopt;
        }

        // Extract dn=
        auto dn_pos = input.find("&dn=");
        if (dn_pos != std::string_view::npos) {
            dn_pos += 4;
            auto end = input.find('&', dn_pos);
            meta.display_name = std::string(input.substr(dn_pos, end == std::string_view::npos ? end : end - dn_pos));
        }

        // Extract xl=
        auto xl_pos = input.find("&xl=");
        if (xl_pos != std::string_view::npos) {
            xl_pos += 4;
            auto end = input.find('&', xl_pos);
            try {
                meta.total_bytes = std::stoull(std::string(input.substr(xl_pos, end == std::string_view::npos ? end : end - xl_pos)));
            } catch (...) {}
        }

        // Extract pe=<ip>:<port>
        auto pe_pos = input.find("&pe=");
        if (pe_pos != std::string_view::npos) {
            pe_pos += 4;
            auto end = input.find('&', pe_pos);
            std::string_view pe_str = input.substr(pe_pos, end == std::string_view::npos ? end : end - pe_pos);
            auto colon = pe_str.find(':');
            if (colon != std::string_view::npos) {
                meta.peer_ip = std::string(pe_str.substr(0, colon));
                try {
                    meta.dht_port = static_cast<uint16_t>(std::stoul(std::string(pe_str.substr(colon + 1))));
                } catch (...) {}
            } else {
                meta.peer_ip = std::string(pe_str);
            }
        }

        // Extract cp=<chunk_port>
        auto cp_pos = input.find("&cp=");
        if (cp_pos != std::string_view::npos) {
            cp_pos += 4;
            auto end = input.find('&', cp_pos);
            try {
                meta.chunk_port = static_cast<uint16_t>(std::stoul(std::string(input.substr(cp_pos, end == std::string_view::npos ? end : end - cp_pos))));
            } catch (...) {}
        }

        return meta;
    }

    /**
     * @brief Extracts the 64-character SHA-256 info-hash from a Magnet link or raw hash.
     */
    static std::optional<std::string> ExtractHash(std::string_view input) {
        auto meta = ExtractMagnetMetadata(input);
        if (meta) return meta->info_hash;
        return std::nullopt;
    }

private:
    static std::string EscapeJson(std::string_view s) {
        std::ostringstream ss;
        for (char c : s) {
            if (c == '"') ss << "\\\"";
            else if (c == '\\') ss << "\\\\";
            else if (c == '\n') ss << "\\n";
            else if (c == '\r') ss << "\\r";
            else if (c == '\t') ss << "\\t";
            else ss << c;
        }
        return ss.str();
    }

    static std::string UnescapeJson(std::string_view s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[i + 1]) {
                    case '"': out += '"'; i++; break;
                    case '\\': out += '\\'; i++; break;
                    case 'n': out += '\n'; i++; break;
                    case 'r': out += '\r'; i++; break;
                    case 't': out += '\t'; i++; break;
                    default: out += s[i]; break;
                }
            } else {
                out += s[i];
            }
        }
        return out;
    }

    static std::string UrlEncode(std::string_view s) {
        std::ostringstream ss;
        for (char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                ss << c;
            } else {
                ss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(c));
            }
        }
        return ss.str();
    }
};

} // namespace zeromesh::examples
