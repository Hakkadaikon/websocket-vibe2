// Test entry point. Cases are split by subject under test/cases/ and #include'd
// here as one translation unit, so the freestanding harness (CHECK, _start) and
// ws_test_failures stay a single counter. To add a subject: drop a cases/<x>.c
// with its test_* functions, #include it below, and call them from run_tests.
#include "../src/ws_internal.h"
#include "ws/ws.h"

#include "harness.h"

// The cases are deliberately #include'd as source (one TU; see file header),
// in run_tests order. SortIncludes is off project-wide so this order sticks.
// NOLINTBEGIN(bugprone-suspicious-include) — intentional single-TU composition.
#include "cases/mem.c"
#include "cases/mask.c"
#include "cases/frame.c"
#include "cases/sha1.c"
#include "cases/base64.c"
#include "cases/handshake.c"
#include "cases/utf8.c"
#include "cases/close_code.c"
#include "cases/lifecycle.c"
#include "cases/driver.c"
// NOLINTEND(bugprone-suspicious-include)

void run_tests(void) {
    test_mem();
    test_close_forbidden();
    test_close_protocol();
    test_close_app();
    test_mask();
    test_classify_data();
    test_classify_control();
    test_classify_reserved();
    test_parse_lengths();
    test_parse_errors();
    test_build_header();
    test_sha1();
    test_base64();
    test_handshake();
    test_utf8_reject();
    test_utf8_accept();
    test_utf8_boundary();
    test_lc_init();
    test_lc_handshake();
    test_lc_send_close();
    test_lc_recv_close();
    test_lc_frag();
    test_drv_single_text();
    test_drv_incremental();
    test_drv_fragmented();
    test_drv_ping();
    test_drv_close();
    test_drv_interleave();
    test_drv_stray_cont();
    test_drv_too_big();
    test_drv_unmasked_server();
    test_drv_illegal_state();
    test_drv_bad_utf8();
    test_drv_send_message();
    test_drv_send_close();
}
