/*
 *  Program: pgn-extract: a Portable Game Notation (PGN) extractor.
 *  Copyright (C) 1994-2015 David Barnes
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 1, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  David Barnes may be contacted as D.J.Barnes@kent.ac.uk
 *  http://www.cs.kent.ac.uk/people/staff/djb/
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bool.h"
#include "mymalloc.h"
#include "defs.h"
#include "typedef.h"
#include "tokens.h"
#include "taglist.h"
#include "lex.h"
#include "map.h"
#include "decode.h"
#include "apply.h"

        /* Structures to hold the x,y displacements of the various
         * piece movements.
         */

/* Define a list of possible Knight moves. */
#define NUM_KNIGHT_MOVES 8
static short Knight_moves[2*NUM_KNIGHT_MOVES] =
        {  1, 2,
           1,-2,
           2, 1,
           2,-1,
          -1, 2,
          -1,-2,
          -2, 1,
          -2,-1,
        };

/* Define a list of possible Bishop moves. */
#define NUM_BISHOP_MOVES 4
static short Bishop_moves[2*NUM_BISHOP_MOVES] =
    {  1, 1,
       1,-1,
      -1, 1,
      -1,-1
    };

/* Define a list of possible Rook moves. */
#define NUM_ROOK_MOVES 4
static short Rook_moves[2*NUM_ROOK_MOVES] =
    {  1, 0,
       0, 1,
      -1, 0,
       0,-1
    };

/* Define a list of possible King moves. */
#define NUM_KING_MOVES 8
static short King_moves[2*NUM_KING_MOVES] =
    {  1, 0,
       1, 1,
       1,-1,
       0, 1,
       0,-1,
      -1, 0,
      -1, 1,
      -1,-1,
    };

/* Define a list of possible Queen moves. */
#define NUM_QUEEN_MOVES 8
static short Queen_moves[2*NUM_QUEEN_MOVES] =
    {  1, 0,
       1, 1,
       1,-1,
       0, 1,
       0,-1,
      -1, 0,
      -1, 1,
      -1,-1,
    };


        /* A table of hash values for square/piece/colour combinations.
         * When a piece is moved, the hash value is xor-ed into a
         * running description of the current board state.
         */
#define NUMBER_OF_PIECES 6
static HashCode HashTab[BOARDSIZE][BOARDSIZE][NUMBER_OF_PIECES][2];

        /* Prototypes for functions in this file. */

        /* Code to allocate and free MovePair structures.  New moves are
         * allocated from the move_pool, if it isn't empty.  Old moves
         * are freed to this pool.
         * This is used to avoid too much fragmentation of the heap.
         * Since the program will spend a lot of its life allocating and
         * deallocating move structures, we might as well hang on to them.
         */
/* Keep a pool of free move structures. */
static MovePair *move_pool = NULL;

static MovePair *
malloc_move(void)
{   MovePair *move;

    if(move_pool != NULL){
        move = move_pool;
        move_pool = move_pool->next;
    }
    else{
        move = (MovePair *)MallocOrDie(sizeof(MovePair));
    }
    move->next = NULL;
    return move;
}

        /* Append another move pair onto moves, and return the new list. */
static MovePair *
append_move_pair(Col from_col, Rank from_rank, Col to_col, Rank to_rank,MovePair *moves)
{   MovePair *move = malloc_move();

    move->from_rank = from_rank;
    move->from_col = from_col;
    move->to_rank = to_rank;
    move->to_col = to_col;
    move->next = moves;
    return move;
}

        /* Simply add the move to the free move pool. */
static void
free_move_pair(MovePair *move)
{
    move->next = move_pool;
    move_pool = move;
}

        /* Free a whole list of moves to the move pool. */
void
free_move_pair_list(MovePair *move_list)
{
    if(move_list != NULL){
        free_move_pair_list(move_list->next);
        free_move_pair(move_list);
    }
}

        /* Produce a hash value for each piece, square, colour combination.
         * This code is a modified version of that to be found in
         * Steven J. Edwards' SAN kit.
         */
#define SHIFT_LENGTH 7
void
init_hashtab(void)
{   Piece piece;
    Colour colour;
    Rank rank;
    Col col;
    static HashCode seed = 0;

    for(col = FIRSTCOL; col <= LASTCOL; col++){
      for(rank = FIRSTRANK; rank <= LASTRANK; rank++){
        for(piece = PAWN; piece <= KING; piece++){
          for(colour = BLACK; colour <= WHITE; colour++){
            HashCode code;

#if 1
            /* Try to use a wider range of the available values
             * in an attempt to avoid spurious hash matches.
             */
            seed = (seed * 1103515245L) + 123456789L;
#else
            /* This is the version used up to and including version 13.8. */
            seed = (seed * 1103515245L) + 12345L;
#endif
            code = (seed >> SHIFT_LENGTH);
            HashTab[col-FIRSTCOL][rank-FIRSTRANK][piece-PAWN][colour-BLACK] = code;
          }
        }
      }
    }
}

        /* Look up the hash value for this combination. */
HashCode
HashLookup(Col col, Rank rank, Piece piece, Colour colour)
{
    return HashTab[col-FIRSTCOL][rank-FIRSTRANK][piece-PAWN][colour-BLACK];
}

        /* Is the given piece of the named colour? */
static Boolean
piece_is_colour(Piece coloured_piece, Colour colour)
{   
    return EXTRACT_COLOUR(coloured_piece) == colour;
}

        /* Make the given move.  This is assumed to have been thoroughly
         * checked beforehand, and the from_ and to_ information to be
         * complete.  Update the board structure to reflect
         * the full set of changes implied by it.
         */
void
make_move(MoveClass class, Col from_col, Rank from_rank, Col to_col, Rank to_rank,
          Piece piece, Colour colour,Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    short from_r = RankConvert(from_rank);
    short from_c = ColConvert(from_col);
    /* Does this move involve a capture? */
    Boolean capture = FALSE;

    /* If a KING or ROOK is moved, this might affect castling rights. */
    if(piece == KING){
        if(colour == WHITE){
            board->WKingCol = to_col;
            board->WKingRank = to_rank;
            board->WKingCastle =
                    board->WQueenCastle = FALSE;
        }
        else{
            board->BKingCol = to_col;
            board->BKingRank = to_rank;
            board->BKingCastle =
                    board->BQueenCastle = FALSE;
        }
    }
    else if(piece == ROOK){
        /* Some castling rights may need disallowing. */
        if(colour == WHITE){
            if(from_rank == FIRSTRANK){
                if(from_col == FIRSTCOL){
                    board->WQueenCastle = FALSE;
                }
                else if(from_col == LASTCOL){
                    board->WKingCastle = FALSE;
                }
                else{
                    /* No change. */
                }
            }
        }
        else{
            if(from_rank == LASTRANK){
                if(from_col == FIRSTCOL){
                    board->BQueenCastle = FALSE;
                }
                else if(from_col == LASTCOL){
                    board->BKingCastle = FALSE;
                }
                else{
                    /* No change. */
                }
            }
        }
    }
    else{
        /* Castling not in question from the piece being moved,
         * but see below for checks on any captured piece.
         */
    }
    /* Check for en-passant rights resulting from this move. */
    if(piece != PAWN){
        /* The move cannot result in en-passant rights. */
        board->EnPassant = FALSE;
    }
    else{
        if(colour == WHITE){
            if((from_rank == '2') && (to_rank == '4')){
                /* This move permits an en-passant capture on the following move. */
                board->EnPassant = TRUE;
                board->ep_rank = to_rank-1;
                board->ep_col = to_col;
            }
            else if((board->EnPassant) && (board->ep_rank == to_rank) &&
                        (board->ep_col == to_col)){
                /* This is an ep capture. Remove the intermediate pawn. */
                board->board[RankConvert(to_rank)-1][ColConvert(to_col)] = EMPTY;
                board->hash_value ^= HashLookup(to_col,to_rank-1,PAWN,BLACK);
                board->EnPassant = FALSE;
            }
            else{
                board->EnPassant = FALSE;
            }
        }
        else{
            if((from_rank == '7') && (to_rank == '5')){
                board->EnPassant = TRUE;
                board->ep_rank = to_rank+1;
                board->ep_col = to_col;
            }
            else if((board->EnPassant) && (board->ep_rank == to_rank) &&
                        (board->ep_col == to_col)){
                /* This is an ep capture. Remove the intermediate pawn. */
                board->board[RankConvert(to_rank)+1][ColConvert(to_col)] = EMPTY;
                board->hash_value ^= HashLookup(to_col,to_rank+1,PAWN,WHITE);
                board->EnPassant = FALSE;
            }
            else{
                board->EnPassant = FALSE;
            }
        }
    }
    /* Clear the source square. */
    /* NB: Up to v17-18 there was an error here in the calculation of the hash
     * when the move class is PAWN_MOVE_WITH_PROMOTION.
     * The value of the promoted piece was removed, rather than the value
     * of the pawn because piece is the promoted piece rather than PAWN.
     */
    if(class == PAWN_MOVE_WITH_PROMOTION && piece != PAWN) {
	/* Remove the promoted pawn. */
	board->hash_value ^= HashLookup(from_col,from_rank,PAWN,colour);
    }
    else {
	board->hash_value ^= HashLookup(from_col,from_rank,piece,colour);
    }
    board->board[from_r][from_c] = EMPTY;
    if(board->board[to_r][to_c] != EMPTY){
        /* Delete the captured piece from the hash value. */
        Piece coloured_piece = board->board[to_r][to_c];
        Piece removed_piece;
        Colour removed_colour;

        removed_piece = EXTRACT_PIECE(coloured_piece);
        removed_colour = EXTRACT_COLOUR(coloured_piece);
        board->hash_value ^= HashLookup(to_col,to_rank,removed_piece,removed_colour);
        /* See whether the removed piece is a Rook, as this could
         * affect castling rights.
         */
        if(removed_piece == ROOK){
            if(removed_colour == WHITE){
                if(to_rank == FIRSTRANK){
                    if(to_col == FIRSTCOL){
                        board->WQueenCastle = FALSE;
                    }
                    else if(to_col == LASTCOL){
                        board->WKingCastle = FALSE;
                    }
                    else{
                        /* No change. */
                    }
                }
            }
            else{
                if(to_rank == LASTRANK){
                    if(to_col == FIRSTCOL){
                        board->BQueenCastle = FALSE;
                    }
                    else if(to_col == LASTCOL){
                        board->BKingCastle = FALSE;
                    }
                    else{
                        /* No change. */
                    }
                }
            }
        }
        capture = TRUE;
    }
    /* Deal with the half-move clock. */
    if(piece == PAWN || class == PAWN_MOVE_WITH_PROMOTION) {
        board->halfmove_clock = 0;
    }
    else if(capture) {
        board->halfmove_clock = 0;
    }
    else if(piece != ROOK) {
        board->halfmove_clock++;
    }
    else {
        /* Check whether this is the Rook being moved as part of a
         * two-stage castling move. If so, then don't update the
         * clock a second time.
         */
        if(class != KINGSIDE_CASTLE && class != QUEENSIDE_CASTLE) {
            board->halfmove_clock++;
        }
    }
    /* Place the piece at its destination. */
    board->board[to_r][to_c] = MAKE_COLOURED_PIECE(colour,piece);
    /* Insert the moved piece into the hash value. */
    board->hash_value ^= HashLookup(to_col,to_rank,piece,colour);
}

        /* Find pawn moves matching the to_ and from_ information.
         * Depending on the input form of the move, some of this will be
         * incomplete.  For instance: e4 supplies just the to_ information
         * whereas cb supplies some from_ and some to_.
         */
MovePair *
find_pawn_moves(Col from_col, Rank from_rank, Col to_col,Rank to_rank,
                Colour colour, const Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    short from_r = RankConvert(from_rank);
    short from_c = ColConvert(from_col);
    MovePair *move_list = NULL;
    MovePair *move = NULL;
    /* White pawn moves are offset by +1, Black by -1. */
    short offset = COLOUR_OFFSET(colour);
    Piece piece_to_move = MAKE_COLOURED_PIECE(colour,PAWN);

    if((to_col != 0) && (to_rank != 0)){
        /* We know the complete destination. */
        if(board->board[to_r][to_c] == EMPTY){
            /* Destination must be empty for this form. */
            if(board->board[to_r-offset][to_c] == piece_to_move){
                /* MovePair of one square. */
                move = append_move_pair(ToCol(to_c),ToRank(to_r-offset),
                                        to_col,to_rank,NULL);
            }
            else if((board->board[to_r-offset][to_c] == EMPTY) &&
                            (to_rank == (colour == WHITE?'4':'5'))){
                /* Special case of initial two square move. */
                if(board->board[to_r-2*offset][to_c] == piece_to_move){
                    move = append_move_pair(ToCol(to_c),ToRank(to_r-2*offset),
                                        to_col,to_rank,NULL);
                }
            }
            else if(board->EnPassant &&
                        (board->ep_rank == to_rank) &&
                        (board->ep_col == to_col)){
                /* Make sure that there is a valid pawn in position. */
                if(from_col != 0){
                    from_r = to_r-offset;
                    if(board->board[from_r][from_c] == piece_to_move){
                        move = append_move_pair(ToCol(from_c),ToRank(from_r),
                                        to_col,ToRank(to_r),NULL);
                    }
                }
            }
        }
        else if(piece_is_colour(board->board[to_r][to_c],OPPOSITE_COLOUR(colour))){
            /* Capture on the destination square. */
            if(from_col != 0){
                /* We know the from column. */
                from_r = to_r-offset;
                if(board->board[from_r][from_c] == piece_to_move){
                    move = append_move_pair(ToCol(from_c),ToRank(from_r),
                                        to_col,to_rank,NULL);
                }
            }
        }
        else{
            /* We have no move. */
            move = NULL;
        }
        move_list = move;
    }
    else if((from_col != 0) && (to_col != 0)){
        /* Should be a diagonal capture. */
        if(((from_col + 1) != to_col) && ((from_col - 1) != to_col)){
            /* Inconsistent information. */
        }
        else{
            if(from_rank != 0){
                /* We have complete source information. Check its
                 * veracity.
                 */
                to_r = from_r - offset;
                if(board->board[from_r][from_c] == piece_to_move){
                    Piece occupant = board->board[to_r][to_c];

                    if((occupant != EMPTY) &&
                            (piece_is_colour(occupant,OPPOSITE_COLOUR(colour)))){
                        move = append_move_pair(ToCol(from_c),ToRank(from_r),
                                        to_col,ToRank(to_r),NULL);
                    }
                    else if(board->EnPassant && (board->ep_rank == ToRank(to_r)) &&
                                    (board->ep_col == ToCol(to_c))){
                        move = append_move_pair(ToCol(from_c),ToRank(from_r),
                                        to_col,ToRank(to_r),NULL);
                    }
                }
            }
            else{
                /* We must search the from_col and to_col for appropriate
                 * combinations.  There may be more than one.
                 */
                short start_rank, end_rank;

                /* Work out from where to start and end. */
                if(colour == WHITE){
                    start_rank = RankConvert(FIRSTRANK+1);
                    end_rank = RankConvert(LASTRANK);
                }
                else{
                    start_rank = RankConvert(LASTRANK-1);
                    end_rank = RankConvert(FIRSTRANK);
                }
                for(from_r = start_rank; from_r != end_rank; from_r += offset){
                    to_r = from_r+offset;
                    if(board->board[from_r][from_c] == piece_to_move){
                        Piece occupant = board->board[to_r][to_c];

                        if((occupant != EMPTY) &&
                                (piece_is_colour(occupant,OPPOSITE_COLOUR(colour)))){
                            move_list = append_move_pair(ToCol(from_c),ToRank(from_r),
                                        to_col,ToRank(to_r),move_list);
                        }
                        else if(board->EnPassant && (board->ep_rank == ToRank(to_r)) &&
                                        (board->ep_col == ToCol(to_c))){
                            move_list = append_move_pair(ToCol(from_c),ToRank(from_r),
                                        to_col,ToRank(to_r),move_list);
                        }
                    }
                }
            }
        }
    }
    return move_list;
}

        /* Find knight moves to the given square. */
MovePair *
find_knight_moves(Col to_col,Rank to_rank, Colour colour, const Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    unsigned ix;
    MovePair *move_list = NULL;
    Piece target_piece =  MAKE_COLOURED_PIECE(colour,KNIGHT);

    /* Pick up pairs of offsets from to_r,to_c to look for a Knight of
     * the right colour.
     */
    for(ix = 0; ix < 2*NUM_KNIGHT_MOVES; ix += 2){
        short r = Knight_moves[ix]+to_r;
        short c = Knight_moves[ix+1]+to_c;

        if(board->board[r][c] == target_piece){
            move_list = append_move_pair(ToCol(c),ToRank(r),to_col,to_rank,move_list);
        }
    }
    return move_list;
}

        /* Find bishop moves to the given square. */
MovePair *
find_bishop_moves(Col to_col,Rank to_rank, Colour colour, const Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    unsigned ix;
    MovePair *move_list = NULL;
    Piece target_piece =  MAKE_COLOURED_PIECE(colour,BISHOP);

    /* Pick up pairs of offsets from to_r,to_c to look for a Bishop of
     * the right colour.
     */
    for(ix = 0; ix < 2*NUM_BISHOP_MOVES; ix += 2){
        short r = to_r, c = to_c;
        
        /* Work backwards from the destination to find a bishop of
         * the right colour.
         */
        do{
            r += Bishop_moves[ix];
            c += Bishop_moves[ix+1];
        } while(board->board[r][c] == EMPTY);

        if(board->board[r][c] == target_piece){
            move_list = append_move_pair(ToCol(c),ToRank(r),to_col,to_rank,move_list);
        }
    }
    return move_list;
}

        /* Find rook moves to the given square. */
MovePair *
find_rook_moves(Col to_col,Rank to_rank, Colour colour, const Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    unsigned ix;
    MovePair *move_list = NULL;
    Piece target_piece = MAKE_COLOURED_PIECE(colour,ROOK);

    /* Pick up pairs of offsets from to_r,to_c to look for a Rook of
     * the right colour.
     */
    for(ix = 0; ix < 2*NUM_ROOK_MOVES; ix += 2){
        short r = to_r, c = to_c;
        
        /* Work backwards from the destination to find a rook of
         * the right colour.
         */
        do{
            r += Rook_moves[ix];
            c += Rook_moves[ix+1];
        } while(board->board[r][c] == EMPTY);

        if(board->board[r][c] == target_piece){
            move_list = append_move_pair(ToCol(c),ToRank(r),to_col,to_rank,move_list);
        }
    }
    return move_list;
}

        /* Find queen moves to the given square. */
MovePair *
find_queen_moves(Col to_col,Rank to_rank, Colour colour, const Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    unsigned ix;
    MovePair *move_list = NULL;
    Piece target_piece = MAKE_COLOURED_PIECE(colour,QUEEN);

    /* Pick up pairs of offsets from to_r,to_c to look for a Knight of
     * the right colour.
     */
    for(ix = 0; ix < 2*NUM_QUEEN_MOVES; ix += 2){
        short r = to_r, c = to_c;
        
        /* Work backwards from the destination to find a queen of
         * the right colour.
         */
        do{
            r += Queen_moves[ix];
            c += Queen_moves[ix+1];
        } while(board->board[r][c] == EMPTY);

        if(board->board[r][c] == target_piece){
            move_list = append_move_pair(ToCol(c),ToRank(r),to_col,to_rank,move_list);
        }
    }
    return move_list;
}

        /* Find King moves to the given square. */
MovePair *
find_king_moves(Col to_col,Rank to_rank, Colour colour, const Board *board)
{   short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    unsigned ix;
    MovePair *move_list = NULL;
    Piece target_piece = MAKE_COLOURED_PIECE(colour,KING);

    /* Pick up pairs of offsets from to_r,to_c to look for a King of
     * the right colour.
     */
    for(ix = 0; ix < 2*NUM_KING_MOVES; ix += 2){
        short r = King_moves[ix]+to_r;
        short c = King_moves[ix+1]+to_c;
        
        if(board->board[r][c] == target_piece){
            move_list = append_move_pair(ToCol(c),ToRank(r),to_col,to_rank,move_list);
        }
    }
    return move_list;
}

        /* Return true if the king of the given colour is
         * in check on the board, FALSE otherwise.
         */
CheckStatus
king_is_in_check(const Board *board,Colour king_colour)
{   MovePair *replies = NULL;
    /* Assume that there is no check. */
    CheckStatus in_check = NOCHECK;
    Col king_col;
    Rank king_rank;
    Colour opponent_colour = OPPOSITE_COLOUR(king_colour);

    /* Find out where the king is now. */
    if(king_colour == WHITE){
        king_col = board->WKingCol;
        king_rank = board->WKingRank;
    }
    else{
        king_col = board->BKingCol;
        king_rank = board->BKingRank;
    }
    /* Try and find one move that leaves this king in check.
     * There is probably an optimal order for these tests, but I haven't
     * tried to find it.
     */
    if((king_col != LASTCOL) &&
                ((replies = find_pawn_moves(king_col+1,0,king_col,king_rank,
                                    opponent_colour,board)) != NULL)){
        /* King is in check from a pawn to its right. */
    }
    else if((king_col != FIRSTCOL) &&
                ((replies = find_pawn_moves(king_col-1,0,king_col,king_rank,
                                    opponent_colour,board)) != NULL)){
        /* King is in check from a pawn. to its left */
    }

    if(replies != NULL) {
        /* Already found a pawn move. */
    }
    else if((replies =
        find_knight_moves(king_col,king_rank,
                                    opponent_colour,board)) != NULL){
        /* King is in check from a knight. */
    }
    else if((replies =
        find_bishop_moves(king_col,king_rank,
                                    opponent_colour,board)) != NULL){
        /* King is in check from a bishop. */
    }
    else if((replies =
        find_rook_moves(king_col,king_rank,
                                    opponent_colour,board)) != NULL){
        /* King is in check from a rook. */
    }
    else if((replies =
        find_queen_moves(king_col,king_rank,
                                    opponent_colour,board)) != NULL){
        /* King is in check from a queen. */
    }
    else if((replies =
        find_king_moves(king_col,king_rank,
                                    opponent_colour,board)) != NULL){
        /* King is in check from a king. */
    }
    else{
        /* King is safe. */
    }
    if(replies != NULL){
       in_check = CHECK;
       free_move_pair_list(replies);
    }
    return in_check;
}

        /* possibles contains a list of possible moves of piece.
         * This function should exclude all of those moves of this piece
         * which leave its own king in check.  The list of remaining legal
         * moves is returned as result.
         * This function operates by looking for at least one reply by the
         * opponent that could capture the king of the given colour.
         * Only one such move needs to be found to invalidate one of the
         * possible moves.
         */
MovePair *
exclude_checks(Piece piece, Colour colour,MovePair *possibles, const Board *board)
{   /* As this function is not called recursively, it should be
     * safe to retain a single copy of the board on the stack without risking
     * overflow. This has been a problem in the past with the PC version.
     */
    Board copy_board;
    MovePair *valid_move_list = NULL;
    MovePair *move;

    /* For each possible move, make the move and see if it leaves the king
     * in check.
     */
    for(move = possibles; move != NULL; ){
        /* Take a copy of the board before playing this next move. */
        copy_board = *board;
        make_move(UNKNOWN_MOVE, move->from_col,move->from_rank,
                  move->to_col,move->to_rank,piece,colour,&copy_board);
        if(king_is_in_check(&copy_board,colour) != NOCHECK){
            MovePair *illegal_move = move;

            move = move->next;
            /* Free the illegal move. */
            free_move_pair(illegal_move);
        }
        else{
            /* King is safe and the move may be kept. */
            MovePair *legal_move = move;

            move = move->next;
            legal_move->next = valid_move_list;
            valid_move_list = legal_move;
        }
    }
    return valid_move_list;
}

    /* We must exclude the possibility of the king passing
     * through check, or castling out of it.
     */
static Boolean
exclude_castling_across_checks(MoveClass class, Colour colour, const Board *board)
{   Boolean Ok = TRUE;
    MovePair *move = malloc_move();
    Rank rank = (colour == WHITE)? FIRSTRANK : LASTRANK;
    Col to_col;

    if(class == KINGSIDE_CASTLE){
        /* Start where we are, because you can't castle out of check. */
        for(to_col = 'e'; (to_col <= 'g') && Ok; to_col++){
            move->from_col = 'e';
            move->from_rank = rank;
            move->to_col = to_col;
            move->to_rank = rank;
            move = exclude_checks(KING,colour,move,board);
            if(move == NULL){
                Ok = FALSE;
            }
        }
    }
    else{
        /* Start where we are, because you can't castle out of check. */
        for(to_col = 'e'; (to_col <= 'c') && Ok; to_col--){
            move->from_col = 'e';
            move->from_rank = rank;
            move->to_col = to_col;
            move->to_rank = rank;
            move = exclude_checks(KING,colour,move,board);
            if(move == NULL){
                Ok = FALSE;
            }
        }
    }
    if(move != NULL){
        free_move_pair(move);
    }
    return Ok;
}

        /* Possibles is a list of possible moves of piece.
         * Exclude all of those that either leave the king in check
         * or those excluded by non-null information in from_col or from_rank.
         */
static MovePair *
exclude_moves(Piece piece, Colour colour,Col from_col, Rank from_rank,
        MovePair *possibles, const Board *board)
{   MovePair *move_list = NULL;

    /* See if we have disambiguating from_ information. */
    if((from_col != 0) || (from_rank != 0)){
        MovePair *move, *temp;

        for(move = possibles; move != NULL; ){
            Boolean excluded = FALSE;

            if(from_col != 0){
                /* The from_col is specified. */
                if(move->from_col != from_col){
                    excluded = TRUE;
                }
            }
            if((from_rank != 0) && !excluded){
                if(move->from_rank != from_rank){
                    excluded = TRUE;
                }
            }
            temp = move;
            move = move->next;
            if(!excluded){
                /* Add it to the list of possibles. */
                temp->next = move_list;
                move_list = temp;
            }
            else{
                /* Discard it. */
                free_move_pair(temp);
            }
        }
    }
    else{
        /* Everything is still possible. */
        move_list = possibles;
    }
    if(move_list != NULL){
        move_list = exclude_checks(piece,colour,move_list,board);
    }
    return move_list;
}

        /* Make a pawn move.
         * En-passant information in the original move text is not currently used
         * to disambiguate pawn moves.  E.g. with Black pawns on c4 and c5 after
         * White move 1. d4 a reply 1... cdep will be rejected as ambiguous.
         */
static Boolean
pawn_move(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    /* Find the basic set of moves that match the move_details criteria. */
    MovePair *move_list;
    Boolean Ok = TRUE;

    /* Make sure that the col values are consistent with a pawn move. */
    if((from_col != '\0') && (to_col != '\0') &&
            /* Forward. */
            (from_col != to_col) &&
            /* Capture. */
            (from_col != (to_col+1)) && (from_col != (to_col-1))){
        /* Inconsistent. */
        Ok = FALSE;
    }
    else if((move_list = find_pawn_moves(from_col,from_rank,to_col,to_rank,
                colour,board)) == NULL){
        Ok = FALSE;
    }
    else{
        /* Exclude any moves that leave the king in check, or are disambiguate
         * by from_information.
         */
        move_list = exclude_moves(PAWN,colour,from_col,from_rank,move_list,board);
        if(move_list != NULL){
            if(move_list->next == NULL){
                /* Unambiguous move. Some pawn moves will have supplied
                 * incomplete destinations (e.g. cd as opposed to cxd4)
                 * so pick up both from_ and to_ information.
                 */
                move_details->from_col = move_list->from_col;
                move_details->from_rank = move_list->from_rank;
                move_details->to_col = move_list->to_col;
                move_details->to_rank = move_list->to_rank;
            }
            else{
                /* Ambiguous. */
                Ok = FALSE;
            }
            free_move_pair_list(move_list);
        }
        else{
            /* Excluded. */
            Ok = FALSE;
        }
    }
    return Ok;
}

        /* Make a pawn move that involves an explicit promotion to promoted_piece. */
static Boolean
promote(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    MovePair *move_list;
    Boolean Ok = FALSE;

    if(to_rank == '\0'){
        /* We can fill this in. */
        if(colour == WHITE){
            to_rank = LASTRANK;
        }
        else{
            to_rank = FIRSTRANK;
        }
    }
    /* Now check that to_rank makes sense for the given colour. */
    if(((colour == WHITE) && (to_rank != LASTRANK)) ||
            ((colour == BLACK) && (to_rank != FIRSTRANK))){
        fprintf(GlobalState.logfile,"Illegal pawn promotion to %c%c\n",to_col,to_rank);
    }
    else{
        move_list = find_pawn_moves(from_col,from_rank,to_col,to_rank,colour,board);
        if(move_list != NULL){
            if(move_list->next == NULL){
                /* Unambiguous move. Some pawn moves will have supplied
                 * incomplete destinations (e.g. cd as opposed to cxd8)
                 * so pick up both from_ and to_ information.
                 */
                move_details->from_col = move_list->from_col;
                move_details->from_rank = move_list->from_rank;
                move_details->to_col = move_list->to_col;
                move_details->to_rank = move_list->to_rank;
                Ok = TRUE;
            }
            else{
                fprintf(GlobalState.logfile,"Ambiguous pawn move to %c%c\n",to_col,to_rank);
            }
            free_move_pair_list(move_list);
        }
        else{
            fprintf(GlobalState.logfile,"Illegal pawn promotion to %c%c\n",to_col,to_rank);
        }
    }
    return Ok;
}

        /* Make a knight move, indicated by move_details.
         * This may result in further information being added to move_details.
         */
static Boolean
knight_move(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    MovePair *move_list = find_knight_moves(to_col,to_rank,colour,board);
    /* Assume everything will be ok. */
    Boolean Ok = TRUE;

    move_list = exclude_moves(KNIGHT,colour,from_col,from_rank,move_list,board);

    if(move_list == NULL){
        fprintf(GlobalState.logfile,"No knight move possible to %c%c.\n",to_col,to_rank);
        Ok = FALSE;
    }
    else if(move_list->next == NULL){
        /* Only one possible.  Check for legality. */
        Piece occupant = board->board[to_r][to_c];

        if((occupant == EMPTY) || piece_is_colour(occupant,OPPOSITE_COLOUR(colour))){
            move_details->from_col = move_list->from_col;
            move_details->from_rank = move_list->from_rank;
        }
        else{
            fprintf(GlobalState.logfile,"Knight destination square %c%c is illegal.\n",
                                to_col,to_rank);
            Ok = FALSE;
        }
        free_move_pair(move_list);
    }
    else{
        fprintf(GlobalState.logfile,"Ambiguous knight move to %c%c.\n",to_col,to_rank);
        free_move_pair_list(move_list);
        Ok = FALSE;
    }
    return Ok;
}

        /* Make a bishop move, indicated by move_details.
         * This may result in further information being added to move_details.
         */
static Boolean
bishop_move(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    MovePair *move_list = find_bishop_moves(to_col,to_rank,colour,board);
    /* Assume that it is ok. */
    Boolean Ok = TRUE;

    move_list = exclude_moves(BISHOP,colour,from_col,from_rank,move_list,board);

    if(move_list == NULL){
        fprintf(GlobalState.logfile,"No bishop move possible to %c%c.\n",to_col,to_rank);
        Ok = FALSE;
    }
    else if(move_list->next == NULL){
        /* Only one possible.  Check for legality. */
        Piece occupant = board->board[to_r][to_c];

        if((occupant == EMPTY) || piece_is_colour(occupant,OPPOSITE_COLOUR(colour))){
            move_details->from_col = move_list->from_col;
            move_details->from_rank = move_list->from_rank;
        }
        else{
            fprintf(GlobalState.logfile,"Bishop's destination square %c%c is illegal.\n",
                                to_col,to_rank);
            Ok = FALSE;
        }
        free_move_pair(move_list);
    }
    else{
        fprintf(GlobalState.logfile,"Ambiguous bishop move to %c%c.\n",to_col,to_rank);
        free_move_pair_list(move_list);
        Ok = FALSE;
    }
    return Ok;
}

        /* Make a rook move, indicated by move_details.
         * This may result in further information being added to move_details.
         */
static Boolean
rook_move(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    MovePair *move_list = find_rook_moves(to_col,to_rank,colour,board);
    /* Assume that it is ok. */
    Boolean Ok = TRUE;

    if(move_list == NULL){
        fprintf(GlobalState.logfile,"No rook move possible to %c%c.\n",to_col,to_rank);
        Ok = FALSE;
    }
    else{
        move_list = exclude_moves(ROOK,colour,from_col,from_rank,move_list,board);

        if(move_list == NULL){
            fprintf(GlobalState.logfile,"Indicated rook move is excluded.\n");
            Ok = FALSE;
        }
        else if(move_list->next == NULL){
            /* Only one possible.  Check for legality. */
            Piece occupant = board->board[to_r][to_c];

            if((occupant == EMPTY) || piece_is_colour(occupant,OPPOSITE_COLOUR(colour))){
                move_details->from_col = move_list->from_col;
                move_details->from_rank = move_list->from_rank;
            }
            else{
                fprintf(GlobalState.logfile,
                                    "Rook's destination square %c%c is illegal.\n",
                                    to_col,to_rank);
                Ok = FALSE;
            }
            free_move_pair(move_list);
        }
        else{
            fprintf(GlobalState.logfile,"Ambiguous rook move to %c%c.\n",to_col,to_rank);
            free_move_pair_list(move_list);
            Ok = FALSE;
        }
    }
    return Ok;
}

        /* Find a queen move indicated by move_details.
         * This may result in further information being added to move_details.
         */
static Boolean
queen_move(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    MovePair *move_list = find_queen_moves(to_col,to_rank,colour,board);
    /* Assume that it is ok. */
    Boolean Ok = TRUE;

    move_list = exclude_moves(QUEEN,colour,from_col,from_rank,move_list,board);

    if(move_list == NULL){
        fprintf(GlobalState.logfile,"No queen move possible to %c%c.\n",to_col,to_rank);
        Ok = FALSE;
    }
    else if(move_list->next == NULL){
        /* Only one possible.  Check for legality. */
        Piece occupant = board->board[to_r][to_c];

        if((occupant == EMPTY) || piece_is_colour(occupant,OPPOSITE_COLOUR(colour))){
            move_details->from_col = move_list->from_col;
            move_details->from_rank = move_list->from_rank;
        }
        else{
            fprintf(GlobalState.logfile,"Queen's destination square %c%c is illegal.\n",
                                        to_col,to_rank);
            Ok = FALSE;
        }
        free_move_pair(move_list);
    }
    else{
        fprintf(GlobalState.logfile,"Ambiguous queen move to %c%c.\n",to_col,to_rank);
        free_move_pair_list(move_list);
        Ok = FALSE;
    }
    return Ok;
}

        /* Can colour castle kingside? */
static Boolean
can_castle_kingside(Colour colour, const Board *board)
{   /* Assume failure. */
    Boolean Ok = FALSE;
    Rank king_rank;

    if(colour == WHITE){
        king_rank = FIRSTRANK;
        Ok = board->WKingCastle;
    }
    else{
        king_rank = LASTRANK;
        Ok = board->BKingCastle;
    }

    if(Ok){
        /* It is permitted. */
        short king_c = ColConvert('e');
        short king_r = RankConvert(king_rank);
        Piece coloured_king = MAKE_COLOURED_PIECE(colour,KING);
        Piece coloured_rook = MAKE_COLOURED_PIECE(colour,ROOK);

        if((board->board[king_r][king_c] == coloured_king) &&
                (board->board[king_r][king_c+1] == EMPTY) &&
                (board->board[king_r][king_c+2] == EMPTY) &&
                (board->board[king_r][king_c+3] == coloured_rook)){
            if(exclude_castling_across_checks(KINGSIDE_CASTLE,colour,board)){
                Ok = TRUE;
            }
            else{
                /* Can't castle across check. */
                Ok = FALSE;
            }
                
        }
        else{
            /* Kingside castling is blocked. */
            Ok = FALSE;
        }
    }
    else{
        /* Kingside castling is forbidden. */
    }
    return Ok;
}

        /* Can colour castle queenside? */
static Boolean
can_castle_queenside(Colour colour, const Board *board)
{   /* Assume failure. */
    Boolean Ok = FALSE;
    Rank king_rank;

    if(colour == WHITE){
        king_rank = FIRSTRANK;
        Ok = board->WQueenCastle;
    }
    else{
        king_rank = LASTRANK;
        Ok = board->BQueenCastle;
    }

    if(Ok){
        /* It is permitted. */
        short king_c = ColConvert('e');
        short king_r = RankConvert(king_rank);
        Piece coloured_king = MAKE_COLOURED_PIECE(colour,KING);
        Piece coloured_rook = MAKE_COLOURED_PIECE(colour,ROOK);

        if((board->board[king_r][king_c] == coloured_king) &&
                (board->board[king_r][king_c-1] == EMPTY) &&
                (board->board[king_r][king_c-2] == EMPTY) &&
                (board->board[king_r][king_c-3] == EMPTY) &&
                (board->board[king_r][king_c-4] == coloured_rook)){
            if(exclude_castling_across_checks(QUEENSIDE_CASTLE,colour,board)){
                Ok = TRUE;
            }
            else{
                /* Can't castle across check. */
                Ok = FALSE;
            }
                
        }
        else{
            /* Queenside castling is blocked. */
            Ok = FALSE;
        }
    }
    else{
        /* Queenside castling is forbidden. */
    }
    return Ok;
}

        /* Castle king side. */
static Boolean
kingside_castle(Move *move_details,Colour colour, Board *board)
{   Boolean Ok;

    if(can_castle_kingside(colour,board)){
        Rank rank = colour == WHITE?FIRSTRANK:LASTRANK;

        move_details->from_col = 'e';
        move_details->from_rank = rank;
        move_details->to_col = 'g';
        move_details->to_rank = rank;
        Ok = TRUE;
    }
    else{
        fprintf(GlobalState.logfile,"Kingside castling is forbidden to %s.\n",
                        colour == WHITE?"White":"Black");
        Ok = FALSE;
    }
    return Ok;
}

static Boolean
queenside_castle(Move *move_details, Colour colour, Board *board)
{   Boolean Ok;

    if(can_castle_queenside(colour,board)){
        Rank rank = colour == WHITE?FIRSTRANK:LASTRANK;

        move_details->from_col = 'e';
        move_details->from_rank = rank;
        move_details->to_col = 'c';
        move_details->to_rank = rank;
        Ok = TRUE;
    }
    else{
        fprintf(GlobalState.logfile,"Queenside castling is forbidden to %s.\n",
                        colour == WHITE?"White":"Black");
        Ok = FALSE;
    }
    return Ok;
}

        /* Move the king according to move_details.
         * This may result in further information being added to move_details.
         */
static Boolean
king_move(Move *move_details, Colour colour, Board *board)
{   Col from_col = move_details->from_col;
    Rank from_rank = move_details->from_rank;
    Col to_col = move_details->to_col;
    Rank to_rank = move_details->to_rank;
    short to_r = RankConvert(to_rank);
    short to_c = ColConvert(to_col);
    /* Find all possible king moves to the destination squares. */
    MovePair *move_list = find_king_moves(to_col,to_rank,colour,board);
    /* Assume that it is ok. */
    Boolean Ok = TRUE;

    /* Exclude disambiguated and illegal moves. */
    move_list = exclude_moves(KING,colour,from_col,from_rank,move_list,board);

    if(move_list == NULL){
        fprintf(GlobalState.logfile,"No king move possible to %c%c.\n",to_col,to_rank);
        Ok = FALSE;
    }
    else if(move_list->next == NULL){
        /* Only one possible.  Check for legality. */
        Piece occupant = board->board[to_r][to_c];

        if((occupant == EMPTY) || piece_is_colour(occupant,OPPOSITE_COLOUR(colour))){
            move_details->from_col = move_list->from_col;
            move_details->from_rank = move_list->from_rank;
        }
        else{
            fprintf(GlobalState.logfile,"King's destination square %c%c is illegal.\n",
                                to_col,to_rank);
            Ok = FALSE;
        }
        free_move_pair(move_list);
    }
    else{
        fprintf(GlobalState.logfile,
                        "Ambiguous king move possible to %c%c.\n",to_col,to_rank);
        fprintf(GlobalState.logfile,"Multiple kings?!\n");
        free_move_pair_list(move_list);
        Ok = FALSE;
    }
    return Ok;
}

        /* Try to complete the full set of move information for
         * move details.
         * In the process, several fields of move_details are modified
         * as accurate information is determined about the effect of the move.
         * The from_ and to_ fields are completed, class may be refined, and
         * captured_piece is filled in.  The purpose of this is to make it
         * possible to call apply_move with a full set of information on the
         * move.  In addition, once determine_move_details has done its job,
         * it should be possible to use the information to reconstruct the effect
         * of a move in reverse, if necessary.  This would be required by a display
         * program, for instance.
         * NB: this function does not determine whether or not the move gives check.
         */
Boolean
determine_move_details(Colour colour,Move *move_details, Board *board)
{   Boolean Ok = FALSE;

    if(move_details == NULL){
        /* Shouldn't happen. */
        fprintf(GlobalState.logfile,
                "Internal error: Empty move details in apply_move.\n");
    }
    else if(move_details->move[0] == '\0'){
        /* Shouldn't happen. */
        fprintf(GlobalState.logfile,
                "Internal error: Null move string in apply_move.\n");
    }
    else if(move_details->class == NULL_MOVE) {
        /* Non-standard PGN.
	 * Nothing more to be done.
	 */
	Ok = TRUE;
    }
    else{
        /* We have something -- normal case. */
        const unsigned char *move = move_details->move;
        MoveClass class = move_details->class;
        Boolean move_handled = FALSE;

        /* A new piece on promotion. */
        move_details->promoted_piece = EMPTY;

        /* Because the decoding process did not have the current board
         * position available, trap apparent pawn moves that maybe something
         * else.
         * At the moment, only do this when full positional information is
         * available.
         */
        if((class == PAWN_MOVE) &&
                (move_details->from_col != 0) &&
                       (move_details->from_rank != 0) &&
                       (move_details->to_col != 0) &&
                       (move_details->to_rank != 0)){
            /* Try to work out its details and handle it below. */
            move_details = decode_algebraic(move_details,board);
            /* Pick up the new class. */
            class = move_details->class;
        }

        /* Deal with apparent pawn moves first. */
        if((class == PAWN_MOVE) || (class == ENPASSANT_PAWN_MOVE) ||
                        (class == PAWN_MOVE_WITH_PROMOTION)){
            move_details->piece_to_move = PAWN;
            /* Fill in any promotional details. */
            if(class == PAWN_MOVE){
                /* Check for implicit promotion. */
                if(((move_details->to_rank == LASTRANK) && (colour == WHITE)) ||
                        ((move_details->to_rank == FIRSTRANK) && (colour == BLACK))){
                    /* Don't modify move_details->class as we are not
                     * adding the new piece to the move string.
                     */
                    class = PAWN_MOVE_WITH_PROMOTION;
                    /* Since the move string doesn't tell us, we have to
                     * assume a queen.
                     */
                    move_details->promoted_piece = QUEEN;
                }
            }
            else if(class == ENPASSANT_PAWN_MOVE){
                /* No promotion possible. */
            }
            else{
                /* Pick up the promoted_piece from the move string. */
                size_t last_char = strlen((const char *)move)-1;

                /* Skip any check character. */
                while(is_check(move[last_char])){
                    last_char--;
                }
                /* Pick up what kind of piece it is. */
                move_details->promoted_piece = is_piece(&move[last_char]);
                switch(move_details->promoted_piece){
                    case QUEEN: case ROOK: case BISHOP: case KNIGHT:
                        /* Ok. */
                        break;
                    default:
                        fprintf(GlobalState.logfile,
                                    "Unknown piece in promotion %s\n",move);
                        move_details->promoted_piece = EMPTY;
                        break;
                }
            }
            if((class == PAWN_MOVE) || (class == ENPASSANT_PAWN_MOVE)){
                /* Check out the details and confirm the move's class. */
                Ok = pawn_move(move_details,colour,board);
                /* See if we are dealing with and En Passant move. */
                if(Ok){
                    if((board->EnPassant) &&
                            (board->ep_rank == move_details->to_rank) &&
                            (board->ep_col == move_details->to_col)){
                        move_details->class = class = ENPASSANT_PAWN_MOVE;
                    }
                    else{
                        /* Just in case the original designation was incorrect. */
                        move_details->class = class = PAWN_MOVE;
                    }
                    move_handled = TRUE;
                }
            }
            else if (class == PAWN_MOVE_WITH_PROMOTION){
                /* Handle a move involving promotion. */
                Ok = promote(move_details,colour,board);
                move_handled = TRUE;
            }
            else{
                /* Shouldn't get here. */
            }
            if(!move_handled){
                /* We failed to find the move, for some reason. */
                /* See if it might be a Bishop move with a lower case 'b'. */
                if(move_details->move[0] == 'b'){
                    /* See if we can find an valid Bishop alternative for it. */
                    unsigned char alternative_move[MAX_MOVE_LEN+1];
                    Move *alternative;

                    strcpy((char *) alternative_move,
                           (const char *)move_details->move);
                    *alternative_move = 'B';
                    alternative = decode_move((unsigned char *) alternative_move);
                    if(alternative != NULL){
                        Ok = bishop_move(alternative,colour,board);
                        if(Ok){
                            /* Copy the relevant details. */
                            move_details->class = alternative->class;
                            move_details->from_col = alternative->from_col;
                            move_details->from_rank = alternative->from_rank;
                            move_details->to_col = alternative->to_col;
                            move_details->to_rank = alternative->to_rank;
                            move_details->piece_to_move =
                                        alternative->piece_to_move;
                            free_move_list(alternative);
                            move_handled = TRUE;
                        }
                    }
                }
            }
        }
        if(!move_handled){
          /* Pick up any moves not handled as pawn moves.
           * This includes algebraic moves that were originally assumed to
           * be pawn moves.
           */
          switch(class){
            case PAWN_MOVE:
            case ENPASSANT_PAWN_MOVE:
            case PAWN_MOVE_WITH_PROMOTION:
                /* No more tries left. */
                break;
            case PIECE_MOVE:
                switch(move_details->piece_to_move){
                    case KING:
                        Ok = king_move(move_details,colour,board);
                        break;
                    case QUEEN:
                        Ok = queen_move(move_details,colour,board);
                        break;
                    case ROOK:
                        Ok = rook_move(move_details,colour,board);
                        break;
                    case KNIGHT:
                        Ok = knight_move(move_details,colour,board);
                        break;
                    case BISHOP:
                        Ok = bishop_move(move_details,colour,board);
                        break;
                    default:
                        Ok = FALSE;
                        fprintf(GlobalState.logfile,"Unknown piece move %s\n",move);
                        break;
                }
                break;
            case KINGSIDE_CASTLE:
                move_details->piece_to_move = KING;
                Ok = kingside_castle(move_details,colour,board);
                break;
            case QUEENSIDE_CASTLE:
                move_details->piece_to_move = KING;
                Ok = queenside_castle(move_details,colour,board);
                break;
            case UNKNOWN_MOVE:
                Ok = FALSE;
                break;
            default:
                fprintf(GlobalState.logfile,
                        "Unknown move class in determine_move_details(%d).\n",
                                move_details->class);
                break;
          }
        }
        if(Ok){
            /* Fill in the remaining items in move_details. */
            short to_r = RankConvert(move_details->to_rank);
            short to_c = ColConvert(move_details->to_col);

            /* Keep track of any capture. */
            if(board->board[to_r][to_c] != EMPTY){
                move_details->captured_piece =
                                EXTRACT_PIECE(board->board[to_r][to_c]);
            }
            else if(move_details->class == ENPASSANT_PAWN_MOVE){
                move_details->captured_piece = PAWN;
            }
            else{
                move_details->captured_piece = EMPTY;
            }
        }
    }
    return Ok;
}

    /* Generate a list of moves of a king or knight from from_col,from_rank.
     * This does not include castling for the king, because it is used
     * by the code that looks for ways to escape from check for which
     * castling is illegal, of course.
     */
static MovePair *
GenerateSingleMoves(Colour colour,Piece piece,
                    const Board *board,Col from_col, Rank from_rank)
{   short from_r = RankConvert(from_rank);
    short from_c = ColConvert(from_col);
    unsigned ix;
    MovePair *moves = NULL;
    Colour target_colour = OPPOSITE_COLOUR(colour);
    unsigned num_directions = piece == KING?NUM_KING_MOVES:NUM_KNIGHT_MOVES;
    const short *Piece_moves = piece == KING?King_moves:Knight_moves;

    /* Pick up pairs of offsets from from_r,from_c to look for an
     * EMPTY square, or one containing a piece of OPPOSITE_COLOUR(colour).
     */
    for(ix = 0; ix < 2*num_directions; ix += 2){
        short r = Piece_moves[ix]+from_r;
        short c = Piece_moves[ix+1]+from_c;
        Piece occupant = board->board[r][c];
        Boolean Ok = FALSE;

        if(occupant == OFF){
            /* Not a valid move. */
        }
        else if(board->board[r][c] == EMPTY){
            Ok = TRUE;
        }
        else if(EXTRACT_COLOUR(occupant) == target_colour){
            Ok = TRUE;
        }
        else{
        }
        if(Ok){
            /* Fill in the details, and add it to the list. */
            moves = append_move_pair(from_col,from_rank,ToCol(c),ToRank(r),moves);
        }
    }
    if(moves != NULL){
        moves = exclude_checks(piece,colour,moves,board);
    }
    return moves;
}

        /* Generate a list of moves of a queen, rook or bishop from
         * from_col,from_rank.
         */
static MovePair *
GenerateMultipleMoves(Colour colour,Piece piece,
                const Board *board,Col from_col, Rank from_rank)
{   short from_r = RankConvert(from_rank);
    short from_c = ColConvert(from_col);
    unsigned ix;
    MovePair *moves = NULL;
    Colour target_colour = OPPOSITE_COLOUR(colour);
    unsigned num_directions = piece == QUEEN?NUM_QUEEN_MOVES:
                                piece == ROOK?NUM_ROOK_MOVES:NUM_BISHOP_MOVES;
    const short *Piece_moves = piece == QUEEN?Queen_moves:
                                piece == ROOK?Rook_moves:Bishop_moves;

    /* Pick up pairs of offsets from from_r,from_c to look for an
     * EMPTY square, or one containing a piece of OPPOSITE_COLOUR(colour).
     */
    for(ix = 0; ix < 2*num_directions; ix += 2){
        short r = Piece_moves[ix]+from_r;
        short c = Piece_moves[ix+1]+from_c;
        Piece occupant = board->board[r][c];

        /* Include EMPTY squares as possible moves. */
        while(occupant == EMPTY){
            /* Fill in the details, and add it to the list. */
            moves = append_move_pair(from_col,from_rank,ToCol(c),ToRank(r),moves);
            /* Move on to the next square in this direction. */
            r += Piece_moves[ix];
            c += Piece_moves[ix+1];
            occupant = board->board[r][c];
        }
        /* We have come up against an obstruction. */
        if(occupant == OFF){
            /* Not a valid move. */
        }
        else if(EXTRACT_COLOUR(occupant) == target_colour){
            moves = append_move_pair(from_col,from_rank,ToCol(c),ToRank(r),moves);
        }
        else{
            /* Should be a piece of our own colour. */
        }
    }
    if(moves != NULL){
        moves = exclude_checks(piece,colour,moves,board);
    }
    return moves;
}

        /* Generate a list of moves of a pawn from from_col,from_rank. */
static MovePair *
GeneratePawnMoves(Colour colour,const Board *board,Col from_col, Rank from_rank)
{   MovePair *moves = NULL;
    Piece piece = PAWN;
    Colour target_colour = OPPOSITE_COLOUR(colour);
    /* Determine the direction in which a pawn can move. */
    short offset = colour == WHITE? 1 : -1;
    short to_r, to_c = ColConvert(from_col);

    /* Try single step ahead. */
    to_r = RankConvert(from_rank)+offset;
    if(board->board[to_r][to_c] == EMPTY){
        /* Fill in the details, and add it to the list. */
        moves = append_move_pair(from_col,from_rank,ToCol(to_c),ToRank(to_r),moves);
        if(((colour == WHITE) && (from_rank == FIRSTRANK+1)) ||
               ((colour == BLACK) && (from_rank == LASTRANK-1))){
           /* Try two steps. */
           to_r = RankConvert(from_rank)+2*offset;
            if(board->board[to_r][to_c] == EMPTY){
                moves = append_move_pair(from_col,from_rank,ToCol(to_c),ToRank(to_r),moves);
            }
        }
    }
    /* Try to left. */
    to_r = RankConvert(from_rank)+offset;
    to_c = ColConvert(from_col)-1;
    if(board->board[to_r][to_c] == OFF){
    }
    else if(board->board[to_r][to_c] == EMPTY){
        if(board->EnPassant && (ToRank(to_r) == board->ep_rank) &&
                        (ToCol(to_c) == board->ep_col)){
            moves = append_move_pair(from_col,from_rank,ToCol(to_c),ToRank(to_r),moves);
        }
    }
    else if(EXTRACT_COLOUR(board->board[to_r][to_c]) == target_colour){
        moves = append_move_pair(from_col,from_rank,ToCol(to_c),ToRank(to_r),moves);
    }
    else{
    }

    /* Try to right. */
    to_r = RankConvert(from_rank)+offset;
    to_c = ColConvert(from_col)+1;
    if(board->board[to_r][to_c] == OFF){
    }
    else if(board->board[to_r][to_c] == EMPTY){
        if(board->EnPassant && (ToRank(to_r) == board->ep_rank) &&
                        (ToCol(to_c) == board->ep_col)){
            moves = append_move_pair(from_col,from_rank,ToCol(to_c),ToRank(to_r),moves);
        }
    }
    else if(EXTRACT_COLOUR(board->board[to_r][to_c]) == target_colour){
        moves = append_move_pair(from_col,from_rank,ToCol(to_c),ToRank(to_r),moves);
    }
    else{
    }

    if(moves != NULL){
        moves = exclude_checks(piece,colour,moves,board);
    }
    return moves;
}

        /* See whether the king of the given colour is in checkmate.
         * Assuming that the king is in check, generate all possible moves
         * for colour on board until at least one saving move is found.
         * Excepted from this are the castling moves (not legal whilst in check)
         * and underpromotions (inadequate in thwarting a check).
         */
Boolean
king_is_in_checkmate(Colour colour,Board *board)
{  Rank rank;
   Col col;
   MovePair *moves = NULL;
   Boolean in_checkmate = FALSE;

   /* Search the board for pieces of the right colour.
    * Keep going until we have exhausted all pieces, or until
    * we have found a saving move.
    */
   for(rank = LASTRANK; (rank >= FIRSTRANK) && (moves == NULL); rank--){
       for(col = FIRSTCOL; (col <= LASTCOL) && (moves == NULL); col++){
           short r = RankConvert(rank);
           short c = ColConvert(col);
           Piece occupant = board->board[r][c];

           if((occupant != EMPTY) && (colour == EXTRACT_COLOUR(occupant))){
               /* This square is occupied by a piece of the required colour. */
               Piece piece = EXTRACT_PIECE(occupant);

               switch(piece){
                   case KING:
                   case KNIGHT:
                       moves = GenerateSingleMoves(colour,piece,board,col,rank);
                       break;
                   case QUEEN:
                   case ROOK:
                   case BISHOP:
                       moves = GenerateMultipleMoves(colour,piece,board,col,rank);
                       break;
                   case PAWN:
                       moves = GeneratePawnMoves(colour,board,col,rank);
                       break;
                   default:
                       fprintf(GlobalState.logfile,
                           "Internal error: unknown piece %d in king_is_in_checkmate().\n",
                               piece);
                }
           }
       }
   }
   if(moves != NULL){
        /* No checkmate.  Free the move list. */
        free_move_pair_list(moves);
   }
   else{
       in_checkmate = TRUE;
   }
   return in_checkmate;
}

#if INCLUDE_UNUSED_FUNCTIONS

        /* Return an approximation of how many moves there are for
         * board->to_move on board.
         * This may be exact, but I haven't checked.
         * This is not currently used, but I found it useful at one
         * point for generating game statistics.
         */
static unsigned
approx_how_many_moves(Board *board)
{  Rank rank;
   Col col;
   Colour colour = board->to_move;
   unsigned num_moves = 0;

   /* Search the board for pieces of the right colour.
    * Keep going until we have exhausted all pieces, or until
    * we have found a saving move.
    */
   for(rank = LASTRANK; rank >= FIRSTRANK; rank--){
       for(col = FIRSTCOL; col <= LASTCOL; col++){
           short r = RankConvert(rank);
           short c = ColConvert(col);
           Piece occupant = board->board[r][c];
           MovePair *moves = NULL;

           if((occupant != EMPTY) && (colour == EXTRACT_COLOUR(occupant))){
               /* This square is occupied by a piece of the required colour. */
               Piece piece = EXTRACT_PIECE(occupant);

               switch(piece){
                   case KING:
                   case KNIGHT:
                       moves = GenerateSingleMoves(colour,piece,board,col,rank);
                       break;
                   case QUEEN:
                   case ROOK:
                   case BISHOP:
                       moves = GenerateMultipleMoves(colour,piece,board,col,rank);
                       break;
                   case PAWN:
                       moves = GeneratePawnMoves(colour,board,col,rank);
                       break;
                   default:
                       fprintf(GlobalState.logfile,
                           "Internal error: unknown piece %d in king_is_in_checkmate().\n",
                               piece);
                }
               if(moves != NULL){
                    /* At least one move. */
                    MovePair *m;
                    for(m = moves; m != NULL; m = m->next){
                        num_moves++;
                    }
                    /* Free the move list. */
                    free_move_pair_list(moves);
               }
           }
       }
   }
   return num_moves;
}
#endif

        /* Find all moves for colour board. */
MovePair *
find_all_moves(const Board *board, Colour colour)
{  Rank rank;
   Col col;
   /* All moves for colour. */
   MovePair *all_moves = NULL;

   /* Pick up each piece of the required colour. */
   for(rank = LASTRANK; rank >= FIRSTRANK; rank--){
       short r = RankConvert(rank);
       for(col = FIRSTCOL; col <= LASTCOL; col++){
           short c = ColConvert(col);
           Piece occupant = board->board[r][c];

           if((occupant != EMPTY) && (colour == EXTRACT_COLOUR(occupant))){
               /* This square is occupied by a piece of the required colour. */
               Piece piece = EXTRACT_PIECE(occupant);
                   /* List of moves for this piece. */
                   MovePair *moves = NULL;

               switch(piece){
                   case KING:
                       moves = GenerateSingleMoves(colour,piece,board,col,rank);
                       /* Add any castling, as this is not covered
                        * by GenerateSingleMoves.
                        */
                       if(can_castle_kingside(colour,board)){
                           MovePair *m = (MovePair *)
                                   MallocOrDie(sizeof(MovePair));
                           m->from_col = 'e';
                           m->from_rank = rank;
                           m->to_col = 'g';
                           m->to_rank = rank;
                           /* Prepend. */
                           m->next = moves;
                           moves = m;
                       }
                       if(can_castle_queenside(colour,board)){
                           MovePair *m = (MovePair *)
                                   MallocOrDie(sizeof(MovePair));
                           m->from_col = 'e';
                           m->from_rank = rank;
                           m->to_col = 'c';
                           m->to_rank = rank;
                           /* Prepend. */
                           m->next = moves;
                           moves = m;
                       }
                       break;
                   case KNIGHT:
                       moves = GenerateSingleMoves(colour,piece,board,col,rank);
                       break;
                   case QUEEN:
                   case ROOK:
                   case BISHOP:
                       moves = GenerateMultipleMoves(colour,piece,board,col,rank);
                       break;
                   case PAWN:
                       moves = GeneratePawnMoves(colour,board,col,rank);
                       break;
                   default:
                       fprintf(GlobalState.logfile,
                           "Internal error: unknown piece %d in king_is_in_checkmate().\n",
                               piece);
                }
               if(moves != NULL){
                    /* At least one move.
                     * Append what we have so far to this list.
                     * Find the last one.
                     */
                    MovePair *m;
                    for(m = moves; m->next != NULL; m = m->next){
                    }
                    m->next = all_moves;
                    all_moves = moves;
               }
           }
       }
   }
   return all_moves;
}

