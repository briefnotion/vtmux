// Pictures: the picture layer of the emulator. A Sixel picture is cut into tiles, one per character cell, and each cell
// of the screen (and of the scrollback) may hold one tile: which picture, and which piece of it. The layer is a second
// grid beside the cells, and it follows them: whatever the emulator does to cells (write a letter, erase, shift, scroll),
// it does here too, so a letter written over a cell wipes that cell's piece of the picture, and a picture scrolls with
// the text and into the scrollback.
//
// The pictures themselves are kept once, however many tiles point at them, within a budget of memory; when the budget is
// full, the pictures nothing points to any more go first, and then the oldest.
//
// Nothing here knows about Sixel or about drawing; the pictures are only held (as vtscreen Rasters, which this header
// does not need to see).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace vtscreen { struct Raster; }

namespace vtmux {

// One cell's piece of a picture. A picture of number 0 is no picture.
struct Tile {
    uint32_t image = 0;
    uint16_t col = 0, row = 0;                 // the piece: this many cells from the picture's left, and from its top
    bool empty() const { return image == 0; }
    bool operator==(const Tile& o) const { return image == o.image && col == o.col && row == o.row; }
    bool operator!=(const Tile& o) const { return !(*this == o); }
};

class Pictures {
public:
    static constexpr size_t DEFAULT_BUDGET = 64u << 20;        // bytes of pictures kept, for one console

    struct Image {
        std::shared_ptr<const vtscreen::Raster> raster;
        size_t bytes = 0;
    };

    Pictures() = default;

    // ---- the pictures
    void set_budget(size_t bytes) { budget_ = bytes; }
    size_t budget() const { return budget_; }
    size_t bytes() const { return bytes_; }                    // what the pictures held take
    size_t image_count() const { return images_.size(); }
    const Image* image(uint32_t id) const {
        const auto it = images_.find(id);
        return it == images_.end() ? nullptr : &it->second;
    }

    // Keep a picture of this many bytes; returns its number, or 0 if it can never fit the budget. Making room for it may
    // forget other pictures (their tiles are cleared).
    uint32_t add(std::shared_ptr<const vtscreen::Raster> raster, size_t bytes) {
        if (bytes > budget_ || !raster) return 0;
        const uint32_t id = next_id_++;
        Image im;
        im.raster = std::move(raster);
        im.bytes = bytes;
        images_.emplace(id, std::move(im));
        bytes_ += bytes;
        make_room(id);
        return id;
    }

    // Forget everything: all tiles of both screens and the history, and the pictures.
    void clear_all() {
        g_[0].clear(); g_[1].clear();
        for (std::vector<Tile>& line : history_) std::vector<Tile>().swap(line);      // (the lines stay: one for every line of scrollback)
        images_.clear();
        bytes_ = 0;
        swept_bytes_ = 0;
    }

    // ---- the grids (screen 0 is the main one, 1 the alternate one)
    void set_size(int rows, int cols) { rows_ = rows; cols_ = cols; g_[0].clear(); g_[1].clear(); }

    // A grid that was never written to is empty (no memory), and reads as no tiles.
    bool allocated(int screen) const { return !g_[screen].empty(); }
    Tile tile(int screen, int row, int col) const {
        const std::vector<Tile>& g = g_[screen];
        return g.empty() ? Tile() : g[(size_t)row * cols_ + col];
    }
    void put(int screen, int row, int col, Tile t) {
        std::vector<Tile>& g = g_[screen];
        if (g.empty()) g.assign((size_t)rows_ * cols_, Tile());
        g[(size_t)row * cols_ + col] = t;
    }
    // from <= col < to of this row has no pieces any more (the cells were written or erased)
    void clear(int screen, int row, int from, int to) {
        std::vector<Tile>& g = g_[screen];
        if (g.empty()) return;
        from = std::max(from, 0);
        to = std::min(to, cols_);
        for (int c = from; c < to; c++) g[(size_t)row * cols_ + c] = Tile();
    }
    void clear_screen(int screen) { std::vector<Tile>().swap(g_[screen]); }       // and gives the memory back

    // As open_gap and close_gap of the cells: the rest of the row moves right (or left) by n; the cells that come in are empty.
    void shift_right(int screen, int row, int col, int n) {
        std::vector<Tile>& g = g_[screen];
        if (g.empty()) return;
        Tile* r = &g[(size_t)row * cols_];
        for (int c = cols_ - 1; c >= col + n; c--) r[c] = r[c - n];
        for (int c = col; c < std::min(col + n, cols_); c++) r[c] = Tile();
    }
    void shift_left(int screen, int row, int col, int n) {
        std::vector<Tile>& g = g_[screen];
        if (g.empty()) return;
        Tile* r = &g[(size_t)row * cols_];
        for (int c = col; c + n < cols_; c++) r[c] = r[c + n];
        for (int c = cols_ - n; c < cols_; c++) r[c] = Tile();
    }
    // As scroll_up and scroll_down of the cells (rows top..bottom, n lines).
    void scroll_up(int screen, int top, int bottom, int n) {
        std::vector<Tile>& g = g_[screen];
        if (g.empty()) return;
        std::move(g.begin() + (size_t)(top + n) * cols_, g.begin() + (size_t)(bottom + 1) * cols_, g.begin() + (size_t)top * cols_);
        std::fill(g.begin() + (size_t)(bottom - n + 1) * cols_, g.begin() + (size_t)(bottom + 1) * cols_, Tile());
    }
    void scroll_down(int screen, int top, int bottom, int n) {
        std::vector<Tile>& g = g_[screen];
        if (g.empty()) return;
        std::move_backward(g.begin() + (size_t)top * cols_, g.begin() + (size_t)(bottom + 1 - n) * cols_, g.begin() + (size_t)(bottom + 1) * cols_);
        std::fill(g.begin() + (size_t)top * cols_, g.begin() + (size_t)(top + n) * cols_, Tile());
    }
    // The grids are now this size: the top left stays, the rest is cut or empty (as the cells).
    void reshape(int rows, int cols) {
        for (std::vector<Tile>& g : g_) {
            if (g.empty()) continue;
            std::vector<Tile> next((size_t)rows * cols, Tile());
            for (int r = 0; r < std::min(rows, rows_); r++)
                for (int c = 0; c < std::min(cols, cols_); c++) next[(size_t)r * cols + c] = g[(size_t)r * cols_ + c];
            g.swap(next);
        }
        rows_ = rows;
        cols_ = cols;
    }

    // ---- the history: one line of tiles for every line of scrollback, in the same order (a line without pieces is empty)
    size_t history_size() const { return history_.size(); }
    const std::vector<Tile>& history(size_t i) const { return history_[i]; }
    void push_history(int screen, int row) {                   // the line leaves the top of the screen
        std::vector<Tile> line;
        const std::vector<Tile>& g = g_[screen];
        if (!g.empty()) {
            const Tile* b = &g[(size_t)row * cols_];
            bool any = false;
            for (int c = 0; c < cols_ && !any; c++) any = !b[c].empty();
            if (any) line.assign(b, b + cols_);
        }
        history_.push_back(std::move(line));
    }
    void pop_history() { history_.pop_front(); }
    void clear_history() { history_.clear(); }

private:
    // After a picture is added: if there is too much, forget what nothing shows any more, then the oldest.
    void make_room(uint32_t keep) {
        if (bytes_ > budget_ || bytes_ > std::max(budget_ / 4, 2 * swept_bytes_)) sweep(keep);
        if (bytes_ <= budget_) return;
        std::unordered_set<uint32_t> dead;
        size_t freed = 0;
        for (const auto& kv : images_) {                       // the oldest have the smallest numbers
            if (bytes_ - freed <= budget_) break;
            if (kv.first == keep) continue;
            dead.insert(kv.first);
            freed += kv.second.bytes;
        }
        forget(dead);
        swept_bytes_ = bytes_;
    }

    // Forget the pictures no tile points to (except the one just added, which is not on the screen yet).
    void sweep(uint32_t keep) {
        std::unordered_set<uint32_t> used;
        used.insert(keep);
        auto scan = [&](const std::vector<Tile>& v) { for (const Tile& t : v) if (!t.empty()) used.insert(t.image); };
        scan(g_[0]);
        scan(g_[1]);
        for (const std::vector<Tile>& line : history_) scan(line);
        std::unordered_set<uint32_t> dead;
        for (const auto& kv : images_) if (!used.count(kv.first)) dead.insert(kv.first);
        forget(dead);
        swept_bytes_ = bytes_;
    }

    // Forget these pictures, and clear every tile that pointed to them.
    void forget(const std::unordered_set<uint32_t>& dead) {
        if (dead.empty()) return;
        auto wipe = [&](std::vector<Tile>& v) {
            bool all_empty = true;
            for (Tile& t : v) {
                if (!t.empty() && dead.count(t.image)) t = Tile();
                if (!t.empty()) all_empty = false;
            }
            return all_empty;
        };
        wipe(g_[0]);
        wipe(g_[1]);
        for (std::vector<Tile>& line : history_) if (!line.empty() && wipe(line)) std::vector<Tile>().swap(line);
        for (uint32_t id : dead) {
            const auto it = images_.find(id);
            if (it == images_.end()) continue;
            bytes_ -= it->second.bytes;
            images_.erase(it);
        }
    }

    int rows_ = 0, cols_ = 0;
    std::vector<Tile> g_[2];
    std::deque<std::vector<Tile>> history_;
    std::map<uint32_t, Image> images_;
    uint32_t next_id_ = 1;
    size_t bytes_ = 0, budget_ = DEFAULT_BUDGET;
    size_t swept_bytes_ = 0;                                   // what the pictures took after the last clean-up (so one is not done after every picture)
};

}  // namespace vtmux
