/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: the Open page's ranking (report 037 section 4, "Open or create"). The query is cut at spaces;
 * every word must match the note's name (case-insensitive, utf8.hpp folding) as a substring or, failing that, as a
 * subsequence; a word that matches only the folder path counts for less. Higher scores for an exact name, a prefix,
 * matches at word starts and runs of consecutive letters; ties go to the shorter name, then the newer note, then the
 * path. "exact" tells the UI there is no need for a Create row.
 */
#pragma once

#include "vault.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace mdnotes {

struct Match {
    int note = -1;        /* index into VaultIndex::notes */
    int score = 0;
    bool exact = false;   /* the name equals the whole query, ignoring case and surrounding spaces */
};

/* The score of one folded word against a folded text, -1 when it does not match. */
int fuzzy_word_score(const std::string &word, const std::string &text);

/* At most `limit` matches, best first. An empty query: no matches. */
std::vector<Match> rank_notes(const VaultIndex &index, const std::string &query, std::size_t limit = 50);
bool has_exact(const std::vector<Match> &matches);

} // namespace mdnotes
