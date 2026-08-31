// Cosmic Desk — single-active-transfer receiver for the client-to-host file
// upload feature (plan FILE_TRANSFER_PLAN.md, "Host side"). A client POSTs a
// file in bounded chunks to Sunshine's /cosmic/file/* routes
// (host/sunshine/src/nvhttp.cpp); this module owns the on-disk state for
// exactly one such transfer at a time and writes the finished file to the
// console user's Downloads\CosmicDesk\.
//
// This header is deliberately free of vendored Sunshine includes so
// nvhttp.cpp can include it without dragging the host's dependency graph into
// the app; this bridge needs none (same pattern as clipboard.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cosmic::filerecv {

// Largest POST body accepted by a single chunk() call. Chunks larger than
// this are rejected outright rather than partially applied.
inline constexpr std::size_t kMaxChunkBytes = 8 * 1024 * 1024;

// A transfer with no chunk() or begin() activity for longer than this is
// considered abandoned by tick() and its partial file is deleted.
inline constexpr std::uint64_t kIdleTimeoutMs = 90000;

// Outcome of begin(). NoSpace means declared_size exceeds free space on the
// destination volume; BadRequest covers an unusable name (sanitize_filename
// refused it, or 999 collision suffixes were exhausted).
enum class BeginResult { Ok, Busy, NoSpace, BadRequest, IoError };

// Outcome of chunk(). UnknownId also covers "no transfer is active at all".
// BadOffset means offset is not exactly the number of bytes received so far,
// or this chunk would push received bytes past declared_size -- chunks must
// arrive strictly sequentially.
enum class ChunkResult { Ok, UnknownId, BadOffset, TooLarge, IoError };

// Outcome of end(). SizeMismatch means fewer or more bytes were received
// than declared_size promised at begin() time.
enum class EndResult { Ok, UnknownId, SizeMismatch, IoError };

// Host-side opt-out, wired to the share_files setting (src/main.cpp,
// src/hostglue/host.cpp). Defaults to true. Disabling also aborts any
// transfer in progress, mirroring cosmic::clipboard::set_enabled's
// clear_waiters() call. Thread-safe.
void set_enabled(bool enabled);
bool enabled();

// Starts a new transfer, called from a Sunshine HTTP handler thread for
// POST /cosmic/file/begin. raw_name is the client-supplied file name (not yet
// sanitized -- this function does that); declared_size is the total byte
// count the client promises to send. On BeginResult::Ok, out_id receives a
// 16-lowercase-hex-char transfer id the client must echo on every chunk()/
// end()/abort() call for this transfer; out_id is left untouched on any other
// result. Only one transfer may be active at a time (see BeginResult::Busy).
// Resolves and creates the destination directory on first use. Thread-safe;
// never throws.
BeginResult begin(const std::string &raw_name, std::uint64_t declared_size, std::string &out_id);

// Appends one chunk to the active transfer, called from a Sunshine HTTP
// handler thread for POST /cosmic/file/chunk. data/len is the raw chunk
// body; len must not exceed kMaxChunkBytes. On any failure that is not
// UnknownId/BadOffset/TooLarge (i.e. IoError), the transfer is aborted and
// its partial file deleted -- the caller does not need to also call abort().
// Thread-safe; never throws.
ChunkResult chunk(const std::string &id, std::uint64_t offset, const char *data, std::size_t len);

// Finalizes the active transfer, called from a Sunshine HTTP handler thread
// for POST /cosmic/file/end. Verifies the byte count, flushes and closes the
// partial file, and atomically renames it into place. The active transfer is
// cleared on every outcome except UnknownId (there was nothing to clear);
// every non-Ok outcome also deletes the partial file. Thread-safe; never
// throws.
EndResult end(const std::string &id);

// Cancels the active transfer if its id matches, called from a Sunshine HTTP
// handler thread for POST /cosmic/file/abort. Closes and deletes the partial
// file. Returns whether a matching transfer was actually active. Thread-safe;
// never throws.
bool abort(const std::string &id);

// Cancels the active transfer unconditionally, if any: closes and deletes
// its partial file. Called when file sharing is toggled off, on stream
// teardown, and on host shutdown (same triggers as the clipboard owner
// reset), so no partial file or open handle outlives the session that
// started it. Thread-safe; never throws.
void abort_all();

// Called once per frame from the app main thread (src/main.cpp, same seam as
// cosmic::clipboard::tick()) to abort a transfer that has been idle longer
// than kIdleTimeoutMs. Thread-safe; never throws.
void tick();

// Reduces an attacker-controlled client file name to a safe basename to
// write under the destination directory. Applies, in order: strip any
// leading path (drop everything up to the last '/' or '\'); replace control
// characters and the characters <>:"/\|?* with '_' (':' in particular is the
// NTFS alternate-data-stream separator); strip trailing '.' and ' '; an
// empty (or all-'.') result becomes "file"; cap to 200 bytes without
// splitting a UTF-8 code point, preserving a short trailing extension when
// there is one; finally, refuse reserved Windows device names (CON, PRN,
// AUX, NUL, COM1-9, LPT1-9, case-insensitive, with or without an extension)
// by returning "".
//
// The empty string has exactly one meaning from this function: "refuse this
// upload" (the device-name case). It is distinct from the empty-input case
// above, which is mapped to "file" rather than refused. Every other return
// value is a safe, non-empty basename. Thread-safe (pure function); never
// throws.
std::string sanitize_filename(const std::string &raw);

}  // namespace cosmic::filerecv
