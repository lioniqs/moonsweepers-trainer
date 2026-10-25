#pragma once

#include <set>
#include <string>
#include <vector>

#include "game.hpp"

namespace ms {

// A place the user marked by hand: a name and a unit direction from the Moon's centre.
struct Mark {
    std::string name;
    Vec3 dir;
};

// User data kept between runs in %APPDATA%\MoonSweepersTrainer:
//   marks.txt   - one mark per line: name <TAB> x <TAB> y <TAB> z
//   visited.txt - keys of places the user ticked as visited by hand, one per line
struct Storage {
    std::vector<Mark> marks;
    std::set<std::string> visited;

    void load();
    bool save() const;

    // Folder the files live in (for display).
    std::wstring folder() const;
};

}  // namespace ms
