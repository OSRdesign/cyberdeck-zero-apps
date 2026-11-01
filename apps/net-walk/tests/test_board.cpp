/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk unit tests: the board's rules on hand-made boards (rotation, kinds, neighbours and wrap, the power flowing
 * from the server, the win check by state: other layouts than the stored one win, loose ends and islands do not).
 */
#include "test_util.hpp"

#include <sstream>

using namespace netwalk;

Board board_from_text(const std::vector<std::string> &rows, bool wrap, int server_x, int server_y)
{
    std::vector<std::uint8_t> masks;
    int w = 0;
    for (const std::string &row : rows) {
        std::istringstream in(row);
        std::string tok;
        int n = 0;
        while (in >> tok) {
            std::uint8_t m = 0;
            for (char c : tok) {
                if (c == 'N') m |= kN;
                else if (c == 'E') m |= kE;
                else if (c == 'S') m |= kS;
                else if (c == 'W') m |= kW;
            }
            masks.push_back(m);
            ++n;
        }
        w = n;
    }
    const int h = static_cast<int>(rows.size());
    return Board::from_masks(w, h, wrap, server_y * w + server_x, masks);
}

namespace {

const std::vector<std::string> kRing = {
    "ES EWS WS",
    "NES NESW NSW",
    "NE NEW NW",
};

int edges_total(const Board &b)
{
    int ends = 0;
    for (int i = 0; i < b.size(); ++i) ends += popcount4(b.mask(i));
    return ends / 2;
}

void test_rotation_and_kinds()
{
    CHECK(rot_cw(kN) == kE && rot_cw(kE) == kS && rot_cw(kS) == kW && rot_cw(kW) == kN);
    CHECK(rot_ccw(kN) == kW && rot_ccw(kE) == kN);
    for (int m = 0; m < 16; ++m) {
        CHECK(rot_ccw(rot_cw(static_cast<std::uint8_t>(m))) == m);
        CHECK(rot_cw(rot_cw(rot_cw(rot_cw(static_cast<std::uint8_t>(m))))) == m);
        CHECK(popcount4(rot_cw(static_cast<std::uint8_t>(m))) == popcount4(static_cast<std::uint8_t>(m)));
    }
    CHECK(opposite(kN) == kS && opposite(kE) == kW);
    CHECK(kind_of(0) == Kind::None);
    CHECK(kind_of(kN) == Kind::End && kind_of(kW) == Kind::End);
    CHECK(kind_of(kN | kS) == Kind::Straight && kind_of(kE | kW) == Kind::Straight);
    CHECK(kind_of(kN | kE) == Kind::Corner && kind_of(kW | kN) == Kind::Corner && kind_of(kS | kE) == Kind::Corner);
    CHECK(kind_of(kN | kE | kS) == Kind::Tee && kind_of(kE | kS | kW) == Kind::Tee);
    CHECK(kind_of(15) == Kind::Cross);
    /* turning never changes the type; a cross turns into itself and says so */
    Board b = board_from_text({"N NE NES", "E NS NESW", "W EW NEW"}, false, 1, 1);
    for (int i = 0; i < b.size(); ++i) {
        const Kind k = kind_of(b.mask(i));
        const bool changed = b.rotate(i, true);
        CHECK(kind_of(b.mask(i)) == k);
        CHECK(changed == (k != Kind::Cross));
        b.rotate(i, false);
        b.rotate(i, false);
        CHECK(kind_of(b.mask(i)) == k);
    }
}

void test_neighbors()
{
    const Board flat = board_from_text({"- - -", "- - -", "- - -"}, false, 1, 1);
    CHECK(flat.neighbor(0, kN) == -1 && flat.neighbor(0, kW) == -1);
    CHECK(flat.neighbor(0, kE) == 1 && flat.neighbor(0, kS) == 3);
    CHECK(flat.neighbor(8, kE) == -1 && flat.neighbor(8, kS) == -1);
    CHECK(flat.neighbor(4, kN) == 1 && flat.neighbor(4, kS) == 7 && flat.neighbor(4, kW) == 3 && flat.neighbor(4, kE) == 5);
    const Board torus = board_from_text({"- - -", "- - -", "- - -"}, true, 1, 1);
    CHECK(torus.neighbor(0, kN) == 6 && torus.neighbor(0, kW) == 2);
    CHECK(torus.neighbor(8, kE) == 6 && torus.neighbor(8, kS) == 2);
    CHECK(torus.neighbor(2, kE) == 0 && torus.neighbor(6, kS) == 0);
    for (int i = 0; i < 9; ++i) /* a step and its opposite come back */
        for (std::uint8_t d : {kN, kE, kS, kW}) CHECK(torus.neighbor(torus.neighbor(i, d), opposite(d)) == i);
}

void test_power()
{
    /* a line, server at the left end */
    Board line = board_from_text({"E EW EW EW W"}, false, 0, 0);
    CHECK(line.powered_count() == 5 && line.is_won());
    /* a tile turned away cuts the rest off, and its neighbour keeps a loose end */
    line.rotate(2, true); /* EW -> NS */
    const std::vector<std::uint8_t> p = line.powered();
    CHECK(p[0] && p[1] && !p[2] && !p[3] && !p[4]);
    CHECK(line.powered_count() == 2 && !line.is_won());
    CHECK(line.has_loose_end(1) && line.has_loose_end(3));
    line.rotate(2, false);
    CHECK(line.is_won());
    /* power does not pass a cable end that nobody answers (both point past each other) */
    const Board gap = board_from_text({"E W W E"}, false, 0, 0);
    CHECK(gap.powered_count() == 2 && !gap.is_won());
    /* an island of two joined tiles the server cannot reach */
    const Board island = board_from_text({"E W - E W"}, false, 0, 0);
    CHECK(island.powered_count() == 2 && !island.is_won());
    /* wrap: the power crosses the edge; without wrap the same tiles are not a network */
    const Board ring3w = board_from_text({"EW W E"}, true, 0, 0);
    CHECK(ring3w.powered_count() == 3 && ring3w.is_won());
    const Board ring3f = board_from_text({"EW W E"}, false, 0, 0);
    CHECK(ring3f.powered_count() == 2 && !ring3f.is_won());
    /* the same on the other axis, and the corner meeting both edges */
    const Board col3w = board_from_text({"NS", "N", "S"}, true, 0, 0);
    CHECK(col3w.powered_count() == 3 && col3w.is_won());
    /* power flows through a tee and a cross into every branch */
    const Board tree = board_from_text({"- S -", "E NESW W", "- N -"}, false, 1, 1);
    CHECK(tree.powered_count() == 5 && !tree.is_won()); /* the empty corners are not part of the network */
}

void test_win_by_state()
{
    /* a ring around the server: a loop, valid, a win (loops are allowed) */
    Board ring = board_from_text(kRing, false, 1, 1);
    CHECK(ring.powered_count() == 9 && ring.is_won());
    CHECK(edges_total(ring) == 12); /* 8 on the ring + 4 spokes: not a tree */
    /* turn one corner: the ring opens, loose ends appear */
    for (int i = 0; i < ring.size(); ++i) {
        Board t = ring;
        if (t.rotate(i, true)) CHECK(!t.is_won());
    }
    /* every tile powered but a cable end points off the board: not a win */
    const Board loose = board_from_text({"ESN EWS WS", "NES NESW NSW", "NE NEW NW"}, false, 1, 1);
    CHECK(loose.powered_count() == 9 && !loose.is_won() && loose.has_loose_end(0));
    /* every tile powered but a cable end faces a tile that does not answer: not a win */
    const Board unanswered = board_from_text({"ES EWS WS", "NES NESW NSW", "NE NEW N"}, false, 1, 1);
    CHECK(!unanswered.is_won());
    /* a tile with no cable at all is never powered */
    const Board hole = board_from_text({"ES EWS WS", "NES NESW NSW", "NE NEW -"}, false, 1, 1);
    CHECK(!hole.is_won());

    /* the stored solution is not what wins: a generated board put in a different valid layout is a win */
    Board gen = Board::generate(3, 3, false, 12345);
    CHECK(gen.valid() && !gen.is_won());
    for (int i = 0; i < 9; ++i) gen.set_mask(i, ring.mask(i));
    CHECK(gen.is_won()); /* the ring is not the stored tree (12 cables against 8), yet it wins */
    int differing = 0;
    for (int i = 0; i < 9; ++i) differing += gen.mask(i) != gen.solved_mask(i) ? 1 : 0;
    CHECK(differing > 0);
    /* and the stored solution on a changed board does not win by itself being stored */
    Board rotated = Board::generate(3, 3, false, 777);
    for (int i = 0; i < 9; ++i) rotated.solve_tile(i);
    CHECK(rotated.is_won());
    for (int i = 0; i < 9; ++i)
        if (rotated.kind(i) == Kind::Straight) {
            rotated.rotate(i, true);
            CHECK(!rotated.is_won());
            break;
        }

    /* a second hand-made layout, a spanning tree (8 cables) with corners and ends round a cross, wins too */
    const Board tree2 = board_from_text({"E SW S", "ES NESW NW", "N NE W"}, false, 1, 1);
    CHECK(tree2.is_won() && edges_total(tree2) == 8);
}

} // namespace

void test_board_group()
{
    test_rotation_and_kinds();
    test_neighbors();
    test_power();
    test_win_by_state();
}
