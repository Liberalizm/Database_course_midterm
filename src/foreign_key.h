// foreign_key.h - FOREIGN KEY constraint definitions, shared by the parser and the catalog.
#pragma once

#include <cstdint>
#include <string>

namespace minidb {

// What happens to referencing rows when the referenced row is deleted, or its
// key changes. NO ACTION and RESTRICT both reject the statement (both are
// checked against the state at the end of the statement).
enum class FkAction : uint8_t { NO_ACTION = 0, RESTRICT = 1, CASCADE = 2, SET_NULL = 3, SET_DEFAULT = 4 };

inline const char* FkActionName(FkAction a) {
    switch (a) {
        case FkAction::NO_ACTION: return "NO ACTION";
        case FkAction::RESTRICT: return "RESTRICT";
        case FkAction::CASCADE: return "CASCADE";
        case FkAction::SET_NULL: return "SET NULL";
        case FkAction::SET_DEFAULT: return "SET DEFAULT";
    }
    return "?";
}

// table(column) REFERENCES ref_table(ref_column). Single-column only. The
// referenced column must carry a unique index (PRIMARY KEY or UNIQUE), which
// both guarantees that a key identifies one row and makes the lookup cheap.
// A NULL in the referencing column satisfies the constraint.
struct ForeignKey {
    std::string name;
    std::string table, column;          // the referencing ("child") side
    std::string ref_table, ref_column;  // the referenced ("parent") side; empty column = its primary key
    FkAction on_delete = FkAction::NO_ACTION;
    FkAction on_update = FkAction::NO_ACTION;

    // "FOREIGN KEY (col) REFERENCES t(c) ON DELETE CASCADE" (actions only when not NO ACTION).
    std::string ToString() const {
        std::string s = "FOREIGN KEY (" + column + ") REFERENCES " + ref_table + "(" + ref_column + ")";
        if (on_delete != FkAction::NO_ACTION) s += std::string(" ON DELETE ") + FkActionName(on_delete);
        if (on_update != FkAction::NO_ACTION) s += std::string(" ON UPDATE ") + FkActionName(on_update);
        return s;
    }
};

}  // namespace minidb
