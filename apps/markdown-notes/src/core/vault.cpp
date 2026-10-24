/*
 * SPDX-License-Identifier: MIT
 */
#include "vault.hpp"

#include "fsutil.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <unordered_map>

namespace mdnotes {

namespace {

struct Walker {
    VaultIndex &ix;
    const WalkLimits &lim;

    /* Walks folder `fi` (index into ix.folders); returns false once the note cap is reached. */
    bool walk(int fi)
    {
        const std::string rel = ix.folders[static_cast<std::size_t>(fi)].rel;
        const int depth = ix.folders[static_cast<std::size_t>(fi)].depth;
        std::vector<DirEntry> entries;
        if (!list_dir(ix.abs_path(rel), entries)) {
            if (fi == 0) ix.ok = false;
            else ++ix.unreadable;
            return true;
        }
        std::vector<std::string> subdirs;
        for (const DirEntry &e : entries) {
            if (e.name.empty() || e.name[0] == '.') continue;
            if (e.is_link) {
                ++ix.skipped_links;
                continue;
            }
            if (e.is_dir) {
                subdirs.push_back(e.name);
                continue;
            }
            if (!e.is_reg || !is_note_file(e.name)) continue;
            if (static_cast<int>(ix.notes.size()) >= lim.max_notes) {
                ix.capped_notes = true;
                return false;
            }
            NoteEntry n;
            n.rel = rel.empty() ? e.name : rel + "/" + e.name;
            n.name = strip_md(e.name);
            n.folder = rel;
            n.folded = fold_case(n.name);
            n.mtime_ns = e.mtime_ns;
            n.size = e.size;
            ix.notes.push_back(std::move(n));
            ++ix.folders[static_cast<std::size_t>(fi)].notes;
        }
        for (const std::string &d : subdirs) {
            if (depth + 1 > lim.max_depth) {
                ix.capped_depth = true;
                continue;
            }
            FolderEntry f;
            f.rel = rel.empty() ? d : rel + "/" + d;
            f.name = d;
            f.depth = depth + 1;
            ix.folders.push_back(f);
            ++ix.folders[static_cast<std::size_t>(fi)].folders;
            if (!walk(static_cast<int>(ix.folders.size()) - 1)) return false;
        }
        return true;
    }
};

} // namespace

int VaultIndex::find_note(const std::string &rel) const
{
    for (std::size_t i = 0; i < notes.size(); ++i)
        if (notes[i].rel == rel) return static_cast<int>(i);
    return -1;
}

int VaultIndex::find_folder(const std::string &rel) const
{
    for (std::size_t i = 0; i < folders.size(); ++i)
        if (folders[i].rel == rel) return static_cast<int>(i);
    return -1;
}

VaultIndex walk_vault(const std::string &root, const WalkLimits &limits)
{
    VaultIndex ix;
    ix.root = root;
    while (ix.root.size() > 1 && ix.root.back() == '/') ix.root.pop_back();
    FolderEntry top;
    ix.folders.push_back(top);
    const FileStat fs = stat_path(ix.root);
    if (!fs.exists || !fs.is_dir) return ix;
    ix.ok = true;
    Walker w{ix, limits};
    w.walk(0);
    if (!ix.ok) return ix;
    /* totals: every note counts in its folder and in each folder above it */
    std::unordered_map<std::string, std::size_t> by_rel;
    for (std::size_t i = 0; i < ix.folders.size(); ++i) by_rel.emplace(ix.folders[i].rel, i);
    for (const NoteEntry &n : ix.notes) {
        std::string f = n.folder;
        for (;;) {
            const auto it = by_rel.find(f);
            if (it != by_rel.end()) ++ix.folders[it->second].total;
            if (f.empty()) break;
            const std::size_t p = f.rfind('/');
            f = p == std::string::npos ? std::string() : f.substr(0, p);
        }
    }
    return ix;
}

FolderListing list_folder(const VaultIndex &ix, const std::string &rel)
{
    FolderListing l;
    for (std::size_t i = 0; i < ix.folders.size(); ++i) {
        const FolderEntry &f = ix.folders[i];
        if (f.rel.empty() || f.rel == rel) continue;
        const std::size_t p = f.rel.rfind('/');
        const std::string parent = p == std::string::npos ? std::string() : f.rel.substr(0, p);
        if (parent == rel) l.folders.push_back(static_cast<int>(i));
    }
    std::stable_sort(l.folders.begin(), l.folders.end(), [&](int a, int b) {
        return fold_case(ix.folders[static_cast<std::size_t>(a)].name) < fold_case(ix.folders[static_cast<std::size_t>(b)].name);
    });
    for (std::size_t i = 0; i < ix.notes.size(); ++i)
        if (ix.notes[i].folder == rel) l.notes.push_back(static_cast<int>(i));
    std::stable_sort(l.notes.begin(), l.notes.end(), [&](int a, int b) {
        const NoteEntry &x = ix.notes[static_cast<std::size_t>(a)], &y = ix.notes[static_cast<std::size_t>(b)];
        if (x.mtime_ns != y.mtime_ns) return x.mtime_ns > y.mtime_ns;
        return x.folded < y.folded;
    });
    return l;
}

std::vector<int> recent_notes(const VaultIndex &ix, std::size_t n)
{
    std::vector<int> v(ix.notes.size());
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int>(i);
    const auto newer = [&](int a, int b) {
        const NoteEntry &x = ix.notes[static_cast<std::size_t>(a)], &y = ix.notes[static_cast<std::size_t>(b)];
        if (x.mtime_ns != y.mtime_ns) return x.mtime_ns > y.mtime_ns;
        return x.rel < y.rel;
    };
    if (v.size() > n) {
        std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n), v.end(), newer);
        v.resize(n);
    } else {
        std::sort(v.begin(), v.end(), newer);
    }
    return v;
}

} // namespace mdnotes
