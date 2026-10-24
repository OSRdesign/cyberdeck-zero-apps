/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: ticking a task box (report 037 section 3: Ctrl+Enter in the editor, a tap in the preview). Only
 * the one byte between '[' and ']' changes: ' ' becomes 'x', 'x' or 'X' becomes ' '; every other byte of the text stays.
 */
#pragma once

#include <cstddef>
#include <string>

namespace mdnotes {

/* The box whose mark is at `offset` (md4c's task_mark_offset, markdown.hpp Block::task_offset). False, text untouched,
 * when the bytes there are not "[ ]", "[x]" or "[X]". */
bool toggle_task_at(std::string &text, std::size_t offset);

/* The task item on the line that holds byte `pos` (the editor's cursor): an indent (spaces, tabs, the tab glyph),
 * block-quote marks ">", a list marker ("-", "*", "+", "1." or "1)") and a space, then "[ ]" / "[x]" / "[X]" followed
 * by a space, a tab or the end of the line. mark_offset (optional) gets the offset of the mark. False when the line is
 * not a task item. */
bool toggle_task_on_line(std::string &text, std::size_t pos, std::size_t *mark_offset = nullptr);

/* The offset of the task mark on the line holding `pos`, or std::string::npos (the same rule, nothing changed). */
std::size_t task_mark_on_line(const std::string &text, std::size_t pos);

} // namespace mdnotes
