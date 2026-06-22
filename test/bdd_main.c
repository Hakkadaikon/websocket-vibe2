// Runner for the generated BDD test. feature_to_c.py emits run_bdd() with CHECK
// assertions; this TU supplies ws/ws.h, the internal action helpers, and the
// freestanding CHECK harness, then #includes the generated body so it is one
// translation unit (ws_test_failures is a single counter).
//
// Build: clang ... {{srcs}} test/bdd_main.c -DBDD_GENERATED='"<path>"' -o build/bdd
#include "../src/ws_internal.h"
#include "ws/ws.h"

#include "harness.h"

#ifndef BDD_GENERATED
#error "define BDD_GENERATED to the feature_to_c.py output path"
#endif
#include BDD_GENERATED

void run_tests(void) {
    run_bdd();
}
