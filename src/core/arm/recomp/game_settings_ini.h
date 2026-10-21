// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

// game_settings.ini layout: the option table, where each key lives, and the text transforms (new file, one-time
// migration of an older layout, adding keys a newer build knows, setting one value). Pure string functions with no
// emulator dependencies, so a small harness can run them on a copy of a real ini.
namespace Core::GameSettings::Ini {

enum class Kind {
    Menu, // always written as a real line; listed in the game menu's Settings tab
    Doc,  // written as "; key = default" unless the file sets it (then a real line)
    Advanced, // written like Doc (never pinned as a real line, so defaults that depend on other keys stay); listed in the
              // game menu's Settings tab only while [Menu] show_advanced (Advanced mode) is on
    Dead, // never written; still read from an older file and kept as found (a real line) while the file has it
};

struct Row {
    const char* section; // where the key lives in the current layout
    const char* key;
    const char* value;   // default; nullptr = no literal default (the comment says what happens)
    const char* comment; // "; ..." lines, each ending in \n
    Kind kind;
    const char* legacy;  // the section older files had it in ("Debug"), or nullptr
};

const std::vector<Row>& Rows();
const Row* FindRow(std::string_view key);

/// The section a key effectively lives in: an older file's [Debug] key that moved maps to its new section, so
/// readers and writers treat [Debug] nvn_fsr and [Upscaler] nvn_fsr as the same key. Anything else: the section.
std::string Canonical(std::string_view section, std::string_view key);

/// The file was written in the current layout (has the layout marker in its header).
bool IsCurrentLayout(const std::string& text);

/// Rebuilds a file in the current layout from any older text ("" = a new file with the defaults). Every value the
/// old file set is kept (the effective one: the first occurrence wins, as the reader does); ignored later
/// duplicates are kept as comments. notes: what was moved / dropped / kept, for the log.
std::string Rebuild(const std::string& text, std::vector<std::string>* notes);

/// Adds the keys of the table a current-layout file lacks, each inside its own section (no new duplicate
/// sections). Returns the text unchanged when nothing is missing.
std::string AddMissing(const std::string& text, std::vector<std::string>* added);

/// Sets [section] key = value: replaces the effective line (or its "; key = default" line), else adds it to
/// the key's section.
std::string SetKey(const std::string& text, const std::string& section, const std::string& key,
                   const std::string& value);

/// [section] key's raw value (first occurrence, inline "; comment" removed); present = found at all.
std::string Lookup(const std::string& text, const std::string& section, const std::string& key, bool* present);

} // namespace Core::GameSettings::Ini
