// libws trace points — the debug logging aspect, kept out of the main logic.
//
// The core scatters only one-line WS_TRACE_* calls at its join points (event
// emission, lifecycle transitions, I/O steps). What a trace *means* — its
// formatting and its destination — lives in src/trace.c, not in the logic.
//
// Default builds: WS_DEBUG is undefined, every macro expands to ((void)0), so
// the trace points vanish entirely (zero code, zero cost). Debug builds pass
// -DWS_DEBUG and link src/trace.c, which writes one line per point to stderr.
// Toggle with `just debug` (see the justfile).
#ifndef WS_TRACE_H
#define WS_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef WS_DEBUG

// Implemented in src/trace.c (the aspect). Linked only into debug builds.
void ws_trace_event(int ev_type, size_t len, uint16_t close_code);
void ws_trace_state(int from, int to, const char *why);
void ws_trace_io(const char *what, int fd, long detail);

#define WS_TRACE_EVENT(ev_type, len, close_code) ws_trace_event((ev_type), (len), (close_code))
#define WS_TRACE_STATE(from, to, why) ws_trace_state((from), (to), (why))
#define WS_TRACE_IO(what, fd, detail) ws_trace_io((what), (fd), (detail))

#else

// Non-debug: the aspect is woven out. The casts consume each argument so a
// variable a call site only kept for tracing (e.g. a saved old state) does not
// warn as unused, yet no code is emitted (the comma-expr folds to (void)0).
#define WS_TRACE_EVENT(ev_type, len, close_code) ((void)(ev_type), (void)(len), (void)(close_code))
#define WS_TRACE_STATE(from, to, why) ((void)(from), (void)(to), (void)(why))
#define WS_TRACE_IO(what, fd, detail) ((void)(what), (void)(fd), (void)(detail))

#endif

#endif
