// Shader preparation (first launch): the game's shader variants are translated, compiled and built into pipelines
// before the game starts, so a first visit anywhere has no compile hitch or pop-in. The approach follows
// SuperMarioWonderRecomp's ahead-of-time shaders (github.com/BelmanteGu/SuperMarioWonderRecomp).
//
// What ships is only a list of variant records (runtime/nvn/shader_variants.txt): pipeline / compute state in NVN terms
// (formats, vertex layout, blend state, ...) plus each shader's identity as a one-way hash of its code bytes. No
// shader code ships in any form: the code is found by that hash in the player's own game files (the BNSH shader
// containers in the romfs' shader archives, effects and UI archives, and in the main module's data), and each backend
// builds the variants on the player's machine through the same code its draws use. The objects are kept for the game,
// which finds them by content at first use (code addresses differ from run to run). Anything the list misses still
// compiles at first use (asynchronously, as before) and is logged as a first visit.
//
// The start screen runs it once the game files are set up or updated (runtime/ui/launcher.mm: a progress bar and
// Skip), and on every later start in the background (warm system shader caches: well under a second here); a PLAY
// before that is done shows the same bar. One run at a time. Later
// (phase 2): a Metal binary archive and periodic Vulkan pipeline cache saves (renderer_prepare_end), and a tag that
// invalidates them when the translator changes.
//
// LARECOMP_RECORD_KEYS=<file> (dev): every program / pipeline / compute variant the game builds is appended to <file>
// (one line each, deduplicated), e.g. during a shader tour (LARECOMP_SHADER_TOUR, runtime/ui/shader_tour.mm).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SR_FLIP_Y = 1, SR_DEPTH_MINUS_ONE = 2, SR_FLIP_X = 4 };

typedef struct {
    char kind;               // 'G' graphics pipeline, 'C' compute program
    uint64_t vs, fs;         // code hashes (shader_code_hash); compute: vs
    uint8_t flags;           // SR_*: lower-left window origin, NVN depth mode [-1, 1], mirrored composite
    uint8_t nattrib, nstream, ncolor;
    uint32_t attrib[16];     // NVN vertex attribute state: format | stream << 8 | offset << 13
    uint32_t stream[16][2];  // vertex stream stride, divisor
    uint32_t color[8];       // render target texture formats (0: no target)
    uint32_t cview[8];       // render target view formats (0: the texture's own)
    uint32_t depth;          // depth / stencil target format (0: none)
    uint32_t samples;        // samples of the targets (1: single-sampled)
    uint32_t channel_mask;   // color write masks, 4 bits per target
    uint8_t blend_on;        // blending enabled, 1 bit per target
    uint8_t blend[8][8];     // NVN blend state per target (zero where blending is off)
    uint8_t a2c;             // alpha to coverage
    uint8_t topo;            // primitive class: 0 points, 1 lines, 2 triangles
    uint8_t poly_fill;       // filled polygons (0: lines)
    uint32_t local[3];       // compute: local size
    uint32_t fstencil, fsint;  // fragment texture slots sampled as stencil / unsigned, signed integer (ShaderKey's slots)
} ShaderRec;

// The record's text form (one line, no newline) and back
void shader_rec_format(const ShaderRec *r, char *out, size_t n);
bool shader_rec_parse(const char *line, ShaderRec *r);

// One-way hash of a shader's code bytes: what identifies a shader in the records
uint64_t shader_code_hash(const uint8_t *code, size_t n);
struct NvnShaderStage;
uint64_t shader_stage_hash(const struct NvnShaderStage *s);  // its code up to the end of its constants; 0: unknown
uint8_t shader_topo_class(uint32_t mode);                     // NVN draw mode -> ShaderRec.topo

// Stale-program detector. The in-process program and compute lookups are keyed by guest code address; a hit is not
// otherwise checked against the code. Each program keeps a fingerprint of the code it was built from (its full hash,
// taken once, plus its length and a few sampled words); every bind compares only the length and the sampled words.
// A mismatch logs "[shader] stale program: ..." (16 lines at most) and stops checking that fingerprint. Detection
// only: the program returned is unchanged.
typedef struct ShaderFp {
    uint64_t hash;   // shader_stage_hash when taken
    uint32_t len;    // bytes hashed (code up to the end of its constants)
    uint32_t w[4];   // sampled words
    uint8_t state;   // 0 not taken, 1 checking, 2 off (no constants to bound the code, or already reported)
} ShaderFp;
// Takes the fingerprint on first use, compares it after that. stage: "vs", "fs", "cs" (for the log)
void shader_fp_check(ShaderFp *fp, const struct NvnShaderStage *s, const char *stage);

// (LARECOMP_RECORD_KEYS, the player's own list) note a variant the game builds; cheap when recording is off
bool shader_rec_recording(void);
void shader_rec_note(const ShaderRec *r);

// ---- the player's own list (runtime/nvn/shader_seen.c): cache/shader_seen.txt in the data folder, in the shipped
// list's format. Every variant the game builds that the shipped list doesn't have is added (queued by
// shader_rec_note, written by a background thread in batches and at exit; at most 20000 records), and the next
// preparation does them after the shipped list ("[shader] prepared N + M seen before"), so a shader met once is
// ready from the second run on. LARECOMP_SHADER_SEEN=0 turns it off (nothing recorded, nothing prepared from it).
bool shader_seen_on(void);
void shader_seen_note(const ShaderRec *r);
// The code of the shaders the game builds, for a game whose shaders the preparation can't find in its files (Pokémon
// Z-A: inside its packed archive, arc/data.trpfs): kept in SHADER_CODE_FILE, in this player's data folder and made from
// their own game files - never shipped. Each shader once: a ShaderCodeHdr, then its bytes (code + constants). The
// preparation indexes the file as it does the game's shader archives. hash: shader_stage_hash(s). Queued, written by
// the seen list's writer thread.
#define SHADER_CODE_FILE "cache/shader_code.bin"
typedef struct { uint64_t hash; uint32_t size, const_offset, const_size, pad; } ShaderCodeHdr;
void shader_seen_code(uint64_t hash, const struct NvnShaderStage *s);
bool shader_seen_shipped(const char *line);  // the record (its text form) is in the shipped list
// (preparation) the file's text (NULL: none) and the game id in its header (0: none)
char *shader_seen_read(uint64_t *file_game);
// (preparation) game: the id of the game files' shaders; keep (not NULL: the file was of another game): the records
// whose shaders this game has, to replace the file's records with
void shader_seen_prep_done(uint64_t game, const char *keep);
const char *shader_prep_shipped_list(void);  // the shipped list's text

// First visits: a program / pipeline / compute program the game had to build itself (preparation didn't cover it).
// what: "program", "pipeline", "compute"; a, b: its shaders' hashes; id: what it is (logged once per process)
void shader_first_visit(const char *what, uint64_t a, uint64_t b, uint64_t id);

// ---- preparation
// Shader code found in the game files for a record's hash
typedef struct {
    const uint8_t *code;     // the stage's code (graphics: with the 0x30-byte NVN header and the program header)
    uint32_t size;           // bytes up to the end of its constants
    const uint8_t *control;  // its control section (NULL: not needed - the record holds the compute local size)
    uint32_t const_offset, const_size;
} ShaderBlob;

// Backend (runtime/nvn/renderer.m, runtime/nvn/vulkan/exec.c): build what the record describes through the draw
// path's own code and keep it for the game. Called on one thread, while no game runs; builds may finish later
// (renderer_prepare_pending). false: not built (the backend can't, or the translation failed).
bool renderer_prepare_begin(void);
bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vs, const ShaderBlob *fs);
int renderer_prepare_pending(void);
void renderer_prepare_end(void);  // all builds done (e.g. keep the pipeline cache)

// Prepares every record of the shipped list (LARECOMP_SHADER_LIST=<file>: that list instead) from the game files in
// romfs (the extracted romfs folder), then the player's own list (shader_seen_*). progress(done, total) is called now
// and then; *cancel (any thread) stops it early (builds already started finish in the background). Logs
// "[shader] prepared N + M seen before, not found K". Returns false
// when it couldn't start (no list, no GPU device).
bool shader_prep_run(const char *romfs, void (*progress)(int done, int total), const volatile int *cancel);
// (LARECOMP_SHADER_PREP=1, tests) the same at game start, before the game's first frame: runtime/nvn/objects.c
void shader_prep_boot(const char *romfs);

// ---- the shader caches' tag (Metal binary archive, Vulkan pipeline cache): this build, the translator, the prepared
// objects' format and the GPU / driver; a cache with another tag is discarded (rebuilt as the game runs)
#define SHADER_CACHE_VERSION 1  // (bump when what goes into the caches changes meaning)
void shader_cache_tag(char *out, size_t n, const char *gpu);       // gpu: device name, driver / OS version
bool shader_cache_tag_ok(const char *tag_path, const char *tag);   // the cache's tag file says tag
void shader_cache_tag_write(const char *tag_path, const char *tag);  // (written atomically)
bool shader_cache_write(const char *path, const void *data, size_t n);  // temp file + rename: never half-written
