/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <string.h>   // For std::memset

#include "material.h"
#include "position.h"

// Polynomial material imbalance parameters.

static const int QuadraticOurs[][8] = {
  //            OUR PIECES
  // pair pawn knight bishop rook queen
  {1667                               }, // Bishop pair
  {  40,    2                         }, // Pawn
  {  32,  255,  -3                    }, // Knight      OUR PIECES
  {   0,  104,   4,    0              }, // Bishop
  { -26,   -2,  47,   105,  -149      }, // Rook
  {-185,   24, 122,   137,  -134,   0 }  // Queen
};

static const int QuadraticTheirs[][8] = {
  //           THEIR PIECES
  // pair pawn knight bishop rook queen
  {   0                               }, // Bishop pair
  {  36,    0                         }, // Pawn
  {   9,   63,   0                    }, // Knight      OUR PIECES
  {  59,   65,  42,     0             }, // Bishop
  {  46,   39,  24,   -24,    0       }, // Rook
  { 101,  100, -37,   141,  268,    0 }  // Queen
};

// Helper used to detect a given material distribution.
static int is_KXK(Pos *pos, int us)
{
  return  !more_than_one(pieces_c(us ^ 1))
        && pos_non_pawn_material(us) >= RookValueMg;
}

static int is_KBPsKs(Pos *pos, int us)
{
  return   pos_non_pawn_material(us) == BishopValueMg
        && pieces_cp(us, BISHOP)
        && pieces_cp(us, PAWN);
}

static int is_KQKRPs(Pos *pos, int us) {
  return  !piece_count(us, PAWN)
        && pos_non_pawn_material(us) == QueenValueMg
        && pieces_cp(us, QUEEN)
        && piece_count(us ^ 1, ROOK) == 1
        && pieces_cp(us ^ 1, PAWN);
}

// imbalance() calculates the imbalance by comparing the piece count of each
// piece type for both colors.
int imbalance(int us, int pieceCount[][8])
{
  int *pc_us = pieceCount[us];
  int *pc_them = pieceCount[us ^ 1];
  int bonus = 0;

  // Second-degree polynomial material imbalance by Tord Romstad
  for (int pt1 = 0; pt1 <= QUEEN; pt1++) {
    if (!pc_us[pt1])
      continue;

    int v = 0;

    for (int pt2 = 0; pt2 <= pt1; pt2++)
      v +=  QuadraticOurs[pt1][pt2] * pc_us[pt2]
          + QuadraticTheirs[pt1][pt2] * pc_them[pt2];

    bonus += pc_us[pt1] * v;
  }

  return bonus;
}

typedef int PieceCountType[2][8];

// material_probe() looks up the current position's material configuration
// in the material hash table. It returns a pointer to the MaterialEntry
// if the position is found. Otherwise a new Entry is computed and stored
// there, so we don't have to recompute all when the same material
// configuration occurs again.

MaterialEntry *material_probe(Pos *pos)
{
  Key key = pos_material_key();
//  MaterialEntry *e = &pos->materialTable[(key * 0xb44cede0e4473d6dULL) >> (64 - 13)];
  MaterialEntry *e = &pos->materialTable[key >> (64 - 13)];

  if (e->key == key)
      return e;

  memset(e, 0, sizeof(MaterialEntry));
  e->key = key;
  e->factor[WHITE] = e->factor[BLACK] = (uint8_t)SCALE_FACTOR_NORMAL;
  e->gamePhase = game_phase(pos);

  // Look for a specialized evaluation function.
  for (int i = 0; i < NUM_EVAL; i++) {
    struct EndgameFunc *ef = &endgame_funcs[i];
    for (int c = 0; c < 2; c++)
      if (ef->key[c] == key) {
	e->eval_func = ef->eg_func;
	e->eval_func_side = c;
	return e;
      }
  }

  for (int c = 0; c < 2; c++)
    if (is_KXK(pos, c)) {
      e->eval_func = &EvaluateKXK;
      e->eval_func_side = c;
      return e;
    }

  // Look for a specialized scaling function.
  for (int i = 0; i < NUM_SCALING; i++) {
    struct EndgameFunc *ef = &endgame_funcs[NUM_EVAL + i];
    for (int c = 0; c < 2; c++)
      if (ef->key[c] == key) {
        e->scal_func[c] = ef->eg_func;
        return e;
      }
  }

  // We did not find any specialized scaling function, so fall back on
  // generic ones that refer to more than one material distribution. Note
  // that in this case we do not return after setting the function.
  for (int c = 0; c < 2; c++) {
    if (is_KBPsKs(pos, c))
      e->scal_func[c] = &ScaleKBPsK;

    else if (is_KQKRPs(pos, c))
      e->scal_func[c] = &ScaleKQKRPs;
  }

  Value npm_w = pos_non_pawn_material(WHITE);
  Value npm_b = pos_non_pawn_material(BLACK);

  if (npm_w + npm_b == 0 && pieces_p(PAWN)) { // Only pawns on the board.
    if (!pieces_cp(BLACK, PAWN)) {
      assert(piece_count(WHITE, PAWN) >= 2);

      e->scal_func[WHITE] = &ScaleKPsK;
    }
    else if (!pieces_cp(WHITE, PAWN)) {
      assert(piece_count(BLACK, PAWN) >= 2);

      e->scal_func[BLACK] = &ScaleKPsK;
    }
    else if (popcount(pieces_p(PAWN)) == 2) { // Each side has one pawn.
      // This is a special case because we set scaling functions
      // for both colors instead of only one.
      e->scal_func[WHITE] = &ScaleKPKP;
      e->scal_func[BLACK] = &ScaleKPKP;
    }
  }

  // Zero or just one pawn makes it difficult to win, even with a small
  // material advantage. This catches some trivial draws like KK, KBK and
  // KNK and gives a drawish scale factor for cases such as KRKBP and
  // KmmKm (except for KBBKN).
  if (!piece_count(WHITE, PAWN) && npm_w - npm_b <= BishopValueMg)
    e->factor[WHITE] = (uint8_t)(npm_w <  RookValueMg   ? SCALE_FACTOR_DRAW :
                                 npm_b <= BishopValueMg ? 4 : 14);

  if (!piece_count(BLACK, PAWN) && npm_b - npm_w <= BishopValueMg)
    e->factor[BLACK] = (uint8_t)(npm_b <  RookValueMg   ? SCALE_FACTOR_DRAW :
                                 npm_w <= BishopValueMg ? 4 : 14);

  if (piece_count(WHITE, PAWN) == 1 && npm_w - npm_b <= BishopValueMg)
    e->factor[WHITE] = (uint8_t)SCALE_FACTOR_ONEPAWN;

  if (piece_count(BLACK, PAWN) == 1 && npm_b - npm_w <= BishopValueMg)
    e->factor[BLACK] = (uint8_t)SCALE_FACTOR_ONEPAWN;

  // Evaluate the material imbalance. We use PIECE_TYPE_NONE as a place
  // holder for the bishop pair "extended piece", which allows us to be
  // more flexible in defining bishop pair bonuses.
#define pc(c,p) piece_count_mk(c,p)
  int PieceCount[2][8] = {
    { pc(0, BISHOP) > 1, pc(0, PAWN), pc(0, KNIGHT),
      pc(0, BISHOP)    , pc(0, ROOK), pc(0, QUEEN) },
    { pc(1, BISHOP) > 1, pc(1, PAWN), pc(1, KNIGHT),
      pc(1, BISHOP)    , pc(1, ROOK), pc(1, QUEEN) }
  };
#undef pc
  e->value = (int16_t)((imbalance(WHITE, PieceCount) - imbalance(BLACK, PieceCount)) / 16);

  return e;
}

