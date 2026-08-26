#ifndef QUERY_FOLD_H
#define QUERY_FOLD_H

#include "qarena.h"
#include "ast.h"

/* Replace every constant subtree of the statement with its already-computed
   literal value, so the executor never re-evaluates it per row. Expressions
   whose evaluation fails (e.g. bad function arguments) are left untouched to
   preserve error semantics. */
void fold_constants(SelectStmt *stmt, QArena *arena);

#endif