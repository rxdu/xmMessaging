# xmMessaging Wire-Contract Specification

- Spec version: **0.0.0 (draft)** — pre-freeze; anything may change until 1.0.0
- Status: skeleton authored at P0a; TBD items are marked inline and resolve no later than the phase named next to them
- Governing requirements: R6 (schema hash), R8 (clocks), R10 (multi-language by contract), R11 (standard metric schema) in [design.md](design.md)
- Conformance scenarios: M11, M12 in [scenarios.md](scenarios.md)

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are to be interpreted as described in RFC 2119.

## 1. Scope & versioning

This document is the language-neutral, byte-level specification of every portable contract xmMessaging puts on the wire. Per R10, a process outside the family — any language — participates in an xmMessaging system through its backend's *native* bindings (iceoryx2, Zenoh) plus this spec, with zero xmMessaging code. The C++ library in this repository is one implementation of this spec, not its definition; where the two disagree, the spec wins and the disagreement is a bug in one of them (recorded like an API delta, per M12-A4).

The spec covers five artifact groups:

1. The **envelope layout** (§2) — the fixed-size header every message carries.
2. The **payload layout rules** (§3) — what a cross-language payload type is allowed to look like.
3. The **schema-hash algorithm** (§4, vectors in §5) — the R6 type-identity gate, computable without a C++ compiler.
4. **Topic naming and per-backend QoS mappings** (§6).
5. The **standard metric schema** (§7) and the **introspection surface** (§8).

The **network serialization format** for the inter-host reach (xmBase serialization of payloads over Zenoh) is in scope for this spec but its section is deferred to P2, when the Zenoh backend is integrated. **TBD (P2).**

**Versioning.** The spec itself is semantically versioned, independent of the library version. Rules:

- **Major**: any change that alters bytes an existing reader could misinterpret — envelope field moves, canonicalization changes, a different hash function (including an upgrade to a keyed or cryptographic hash), payload rule relaxations. The envelope version byte (§2) tracks the spec major version.
- **Minor**: additions that consume only reserved space or add new, optional artifacts (new metric instruments, new QoS mapping columns). Existing readers remain correct.
- **Patch**: clarifications with no byte-level effect.

A reader MUST reject (refuse the match, with an explicit status) any envelope whose version byte it does not implement. Pre-1.0.0, no compatibility is promised between drafts; the version byte is 0 throughout the draft series.

## 2. Envelope layout

Every message on every reach carries this header. It is a **fixed-size (64-byte), fixed-offset, little-endian** byte structure — documented bytes, not a shared C++ struct. All multi-byte fields are **little-endian**. All reserved bytes MUST be written as zero and MUST be ignored on read (they are minor-version expansion space).

| Offset | Size | Field | Type | Semantics |
|---|---|---|---|---|
| 0x00 | 1 | `envelope_version` | u8 | Spec major version of this envelope. **0** for this draft. Readers MUST refuse unknown values. |
| 0x01 | 7 | `reserved0` | u8[7] | MUST be zero. |
| 0x08 | 8 | `trace_id_hi` | u64 | Telemetry context, byte 0–7 of the xmBase `Inject` envelope. |
| 0x10 | 8 | `trace_id_lo` | u64 | Telemetry context, byte 8–15. |
| 0x18 | 8 | `span_id` | u64 | Telemetry context, byte 16–23. |
| 0x20 | 8 | `publish_stamp` | u64 | Publisher's `CLOCK_MONOTONIC` at publish, in **nanoseconds**. |
| 0x28 | 8 | `origin_stamp` | u64 | Lineage origin (R11): monotonic ns of the *oldest consumed input's* origin; equals `publish_stamp` on a first-hop publish. |
| 0x30 | 2 | `hop_count` | u16 | Lineage hop count: 0 on a first-hop publish; a derived publish writes (max upstream hop_count) + 1. Saturates at 65535 (MUST NOT wrap). |
| 0x32 | 14 | `reserved1` | u8[14] | MUST be zero. |

Total envelope size: **64 bytes** (one cache line on the tested baselines). The payload follows immediately at offset 0x40 with no gap; payload alignment requirements are the backend's concern, not the envelope's.

**Telemetry context (offsets 0x08–0x1F).** These 24 bytes are exactly the xmBase telemetry `Inject`/`Extract` byte envelope (`xmbase/telemetry/context.hpp`, `kContextWireSize = 24`, layout `trace.hi | trace.lo | span.value`). An all-zero trace id (both `trace_id_hi` and `trace_id_lo` zero) means "no active trace" and MUST be preserved as zero, not invented. *Note:* xmBase `Inject` writes host byte order; this spec fixes little-endian. On the family's tested baselines (x86_64, aarch64 Linux) these coincide byte-for-byte; a big-endian port would require xmBase to fix its `Inject` order first. **Recorded as a known coupling, not a TBD.**

**Clock rule (R8).** `publish_stamp` and `origin_stamp` are **per-host monotonic** (`CLOCK_MONOTONIC`, shared with xmBase telemetry — one timeline by construction). Same-host readers MAY compute ages and enforce deadlines from them directly. A cross-host reader MUST treat differences against its own clock as **advisory** unless the domain declares a synchronized-clock domain (PTP/NTP) in its `Domain` configuration; that declaration is recorded in the domain's introspection surface (§8) and in introspection output, so post-hoc analysis knows what the numbers meant. The `ClockDomain` declaration's encoding lands with the first cross-host backend (§8.5). **TBD (P2).**

## 3. Payload layout rules

These rules define what a payload type MUST look like to cross a process or language boundary (inter-process and inter-host reaches, and any topic a foreign participant joins). The in-process reach accepts richer C++ types; those types are outside this spec by definition.

A conforming payload type:

- MUST be **standard-layout** and **fixed-size**: every field at a compile-time-constant offset, total size a compile-time constant. No variable-length tails.
- MUST be **explicitly padded**: the declared fields (including explicitly declared padding fields) tile the struct completely — there are **no implicit padding bytes** anywhere, including trailing padding and inside nested structs. Implicit padding has unspecified content and would poison both the schema hash's meaning and zero-copy reads from another language. The C++ library asserts this at wiring time (M12-A5); other implementations SHOULD verify it against the canonical description (§4): the field extents, sorted by offset, must exactly tile `[0, size)`.
- MUST encode all multi-byte scalars **little-endian**.

**Permitted field types** (and nothing else):

- Fixed-width integers: `u8 u16 u32 u64 i8 i16 i32 i64`.
- IEEE-754 binary floating point: `f32 f64`.
- Nested structs that themselves conform to these rules.
- Fixed-length arrays (including multi-dimensional, row-major) of any permitted type.

**Forbidden** (non-exhaustive, but these are the classic offenders):

- Pointers, references, handles of any kind — meaningless across a process boundary.
- `bool`, unless the schema declares it as `u8` with values 0/1 (a `bool`'s object representation is not portable).
- Bit-fields (allocation order is implementation-defined).
- Enums, unless declared in the schema as their fixed-width underlying integer type.
- Unions, virtual anything, non-trivial constructors/destructors on the wire path, and any implicit padding as above.

Padding fields are real fields: they participate in the canonical description and the hash (§4), they MUST be written as zero by publishers, and readers MUST ignore their content. The recommended (not required) naming convention is `_pad0`, `_pad1`, … of type `u8[N]`.

## 4. Schema-hash algorithm (R6)

Endpoint matching compares a 64-bit **schema hash** of the payload's wire layout. Two endpoints match only if their hashes are equal; a mismatch is refused with a distinct status and is visible in introspection (M11-A1/A3). The hash is defined over the **canonical type description string** below, so any language can compute it from a struct definition alone — no C++ compiler, no repo code (M12-A2).

### 4.1 Hash function: FNV-1a, 64-bit

The hash is **FNV-1a 64-bit** over the ASCII bytes of the canonical description:

```
hash = 0xCBF29CE484222325                  # offset basis
for each byte b of the canonical description (in order):
    hash = hash XOR b
    hash = (hash * 0x100000001B3) mod 2^64  # FNV prime
```

**Why FNV-1a 64 and not xxHash64:** the M12 acceptance bar is that a foreign process implements this spec from the document alone — FNV-1a is five lines in any language, has zero dependencies, operates on bytes (so it is endian-free by construction), and has well-published reference vectors (`fnv1a64("") = 0xCBF29CE484222325`). xxHash64 is faster, but speed is irrelevant here: the hash is computed once per endpoint type at build/wiring time, never on the message path, over strings of a few hundred bytes. Determinism across compilers, standard libraries, and languages is the only quality that matters. **This is skew detection, not security** (R6/R9): the threat is accidental rebuild skew, not an adversary crafting collisions — a 64-bit non-cryptographic hash is proportionate. If a future threat model requires a keyed or cryptographic hash, that is a spec **major version bump** (§1), signaled by the envelope version byte.

The hash value is carried and compared as a `u64`. When rendered as text (introspection, logs, this spec), it MUST be written as `0x` followed by exactly 16 uppercase hexadecimal digits.

### 4.2 Canonical type description

The canonical description of a payload type is an ASCII string built from these rules. Every rule is normative; there is no whitespace flexibility anywhere.

1. **Lines.** The string is a sequence of lines, each terminated by a single LF (0x0A) — including the last line. No CR, no trailing spaces, no blank lines, no other whitespace than what these rules produce (which is none).
2. **Header line.** The first line is `size:<N>` where `<N>` is the total payload size in bytes, decimal, no leading zeros, no sign.
3. **Field lines.** One line per **leaf field**, in **declaration order**, of the form `name:type:offset:size` — four fields joined by single colons, no spaces. `offset` is the field's byte offset from the start of the payload (absolute, not parent-relative), `size` its total byte size, both decimal with no leading zeros (`0` is written `0`).
4. **Names participate.** `name` is the declared field name, case-sensitive, and MUST match `[A-Za-z_][A-Za-z0-9_]*`. *Decision:* field names are part of the hash. The D14/M11 focus is layout, and offsets alone already catch reorders — but names additionally catch **semantic swaps** (two `f64` fields exchanging meanings at unchanged offsets, e.g. `lat`/`lon` swapped in a refactor), which are exactly the silent-reinterpretation incidents R6 exists to prevent. Trade-off, stated: a pure rename with identical layout and identical meaning is refused too, forcing a coordinated rebuild where byte-compatibility technically held. Accepted — a field rename usually *is* a semantic event, and the failure mode of the alternative (silently matching renamed fields) is the worse one.
5. **Type vocabulary.** `type` is drawn from exactly: `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64`, optionally followed by array suffixes (rule 6). Language mapping:

| Spec | C++ | Python (`struct`) | Rust | Go |
|---|---|---|---|---|
| `u8` | `std::uint8_t` (also `bool`-as-u8, `enum : uint8_t`) | `B` | `u8` | `uint8` |
| `u16` | `std::uint16_t` | `H` | `u16` | `uint16` |
| `u32` | `std::uint32_t` | `I` | `u32` | `uint32` |
| `u64` | `std::uint64_t` | `Q` | `u64` | `uint64` |
| `i8` | `std::int8_t` | `b` | `i8` | `int8` |
| `i16` | `std::int16_t` | `h` | `i16` | `int16` |
| `i32` | `std::int32_t` | `i` | `i32` | `int32` |
| `i64` | `std::int64_t` | `q` | `i64` | `int64` |
| `f32` | `float` | `f` | `f32` | `float32` |
| `f64` | `double` | `d` | `f64` | `float64` |

   Enums canonicalize as their fixed underlying integer type; `bool` (where a schema admits it at all) canonicalizes as `u8`. No other spellings (`int`, `char`, `size_t`, …) ever appear in a canonical description.

6. **Fixed arrays of scalars.** An array field of a scalar type is **one** field line with type `T[N]` (e.g. `f32[3]`), `N` decimal with no leading zeros; multi-dimensional arrays append suffixes in row-major declaration order (`f32[3][4]`); `size` is the array's total byte size. Zero-length arrays are forbidden.
7. **Nested structs.** A nested struct field produces **no line of its own**; its leaf fields are expanded recursively, in the nested type's declaration order, with dotted path names (`pose.x`) and **absolute** offsets from the start of the outermost payload. Nested struct *type names* never appear — the hash is over wire layout, not type-system identity (R10), so the same bytes described through different type nesting hash identically only if the leaf names, offsets, sizes, and types all agree.
8. **Arrays of structs.** A fixed array of a nested struct type expands **per element**, each element's leaves in declaration order, with the element index in the path: `wp[0].x`, `wp[0].y`, `wp[1].x`, … Indices are decimal from 0, no leading zeros.
9. **Padding fields** are leaf fields like any other (typically `_padN:u8[K]:…`) and participate fully.
10. **Completeness check.** The field extents `[offset, offset+size)`, taken in declaration order, MUST be strictly increasing, non-overlapping, and exactly tile `[0, size)` from the header line. An implementation SHOULD verify this before hashing; a description that fails it is not a conforming payload (§3), independent of what it hashes to.

### 4.3 What the hash provably catches

| Skew | Caught by | Scenario |
|---|---|---|
| Field appended / removed (size change) | header `size:` line + field set | M11 case 1 |
| Fields reordered at identical total size | offsets in the field lines — automatic, no special casing | M11-A2 (the nasty one) |
| Type changed at same offset/size (`f64`→`u64`) | the `type` token | M11 case 3 |
| Semantic swap: names exchanged at unchanged layout | names participate (rule 4) | — (design decision above) |
| Identical layouts built separately, different compilers | hash identically — the description contains nothing build-specific | M11-A4 |

## 5. Conformance vectors

Normative worked examples. An implementation is conformant only if it reproduces every hash below **exactly**. Canonical strings are shown verbatim; every line, including the last, ends with LF (0x0A). (Cross-check for the hash function itself: `FNV-1a-64("") = 0xCBF29CE484222325`, `FNV-1a-64("a") = 0xAF63DC4C8601EC8C`, `FNV-1a-64("foobar") = 0x85944171F73967E8`.)

### V1 — baseline

```c
struct Pose2d {          // 24 bytes, no padding needed
  f64 x;                 // offset 0
  f64 y;                 // offset 8
  f64 theta;             // offset 16
};
```

Canonical description (43 bytes):

```
size:24
x:f64:0:8
y:f64:8:8
theta:f64:16:8
```

Hash: **`0xE0978597FA5660D4`**

### V2 — field reorder at identical size (M11-A2)

Same three fields, same `sizeof`, `y` and `theta` swapped in declaration order:

```c
struct Pose2d {          // still 24 bytes
  f64 x;                 // offset 0
  f64 theta;             // offset 8   <- was y
  f64 y;                 // offset 16  <- was theta
};
```

```
size:24
x:f64:0:8
theta:f64:8:8
y:f64:16:8
```

Hash: **`0x9EF4520D5E058D58`** ≠ V1. The offsets in the field lines make reorder detection automatic.

### V3 — type swap at same offset and size

`theta` changed from `f64` to `u64` (say, a raw encoder tick count) — identical offsets, identical sizes, identical names:

```
size:24
x:f64:0:8
y:f64:8:8
theta:u64:16:8
```

Hash: **`0x9444641EFF1DE2E1`** ≠ V1. The type token alone changes the hash.

### V4 — nested struct

```c
struct StampedPose2d {   // 32 bytes
  u64 stamp;             // offset 0
  Pose2d pose;           // offset 8 (V1 layout, expanded — no line of its own)
};
```

```
size:32
stamp:u64:0:8
pose.x:f64:8:8
pose.y:f64:16:8
pose.theta:f64:24:8
```

Hash: **`0x777F5FA85CE9182A`**. Note `Pose2d` as a name appears nowhere; offsets are absolute.

### V5 — fixed arrays

```c
struct ImuSample {       // 32 bytes
  u64 stamp;             // offset 0
  f32 accel[3];          // offset 8,  12 bytes
  f32 gyro[3];           // offset 20, 12 bytes
};
```

```
size:32
stamp:u64:0:8
accel:f32[3]:8:12
gyro:f32[3]:20:12
```

Hash: **`0x4F932BBE69B94D14`**

### V6 — explicit padding

```c
struct ModeStatus {      // 8 bytes; the compiler would have padded implicitly — we do it explicitly
  u8  mode;              // offset 0
  u8  _pad0[3];          // offset 1 — explicit, participates, publisher writes zero
  u32 code;              // offset 4
};
```

```
size:8
mode:u8:0:1
_pad0:u8[3]:1:3
code:u32:4:4
```

Hash: **`0x5F05885505F5CDDC`**

## 6. Topic & QoS conventions

### 6.1 Topic naming grammar

```
topic   = segment *( "." segment )
segment = 1*( %x61-7A / %x30-39 / "_" )   ; lowercase a-z, 0-9, underscore
```

- Topic names MUST match the grammar above (e.g. `m1.plan.head`). Uppercase, leading/trailing/double dots, and empty segments are all invalid; conforming implementations MUST refuse them at `Advertise`/`Subscribe`, not mangle them.
- The first segment `xm` is **reserved** for the library's own topics; applications MUST NOT use it.
- Maximum topic length: **TBD (P1)** — will be fixed when the introspection segment's per-topic slot size is fixed; expected on the order of 192 bytes.

### 6.2 Domain isolation key

Every backend resource a domain creates — topics/services, shm segments, introspection segments — MUST be namespaced by the domain's **isolation key**, so two domains on one host share nothing and cross-domain visibility is only ever explicit (composition-scale contract, design.md). **Resolved (P1b).** The key is dot-separated `segment`s (same grammar as topics), derived as:

```
key            = "u" euid-decimal "." sanitized-name
euid-decimal   = 1*DIGIT                ; getuid()/geteuid(), no leading zeros
sanitized-name = 1*( %x61-7A / %x30-39 / "_" )
```

- `sanitized-name` is the configured domain name (default `default` when none is configured) with uppercase ASCII folded to lowercase and every other byte outside `[a-z0-9_]` replaced by `_`.
- The **numeric euid** (not the login name) keeps the derivation deterministic and passwd-free; a foreign participant reproduces it from `getuid()` alone.
- Example: euid 1000, domain name `Nav-Stack` → key `u1000.nav_stack`.

Backend resource names prefix this key; the POSIX-shm mapping is normative in §6.4. When a backend's namespace length limit would be exceeded by a derived resource name, the over-long name degrades to the hashed form defined where that backend's naming is specified (POSIX shm: §6.4).

### 6.3 Per-backend mapping of the five QoS knobs

The in-process reach defines the reference semantics (design.md); each backend column states how the portable contract is realized, or names the divergence recorded in the R3 support matrix. Backend columns are structural placeholders until the backend is integrated.

| Knob | Portable contract | iceoryx2 **(TBD, P1)** | POSIX-shm fallback **(resolved, P1b)** | Zenoh **(TBD, P2)** |
|---|---|---|---|---|
| History `latest-only` | LatestMailbox: overwrite, newest-or-nothing, never torn | subscriber buffer depth 1, overwrite-on-full | writer-progress-only seqlock slot per subscriber + a master slot (§6.4) | keep newest sample per key |
| History `queue<N>` | bounded FIFO, depth from QoS | buffer depth N | per-subscriber shared SPSC ring, depth N ≤ 16 (the per-slot reservation, §6.4); N > 16 refused at wiring, never clamped | TBD |
| Reliability `best-effort` | overflow drops, always counted | TBD | drop-newest + shared per-subscriber drop counter (publisher-counted, subscriber-readable) | TBD |
| Reliability `reliable` | overflow back-pressures via explicit status | TBD | **declared divergence** (`Supports()` = no; `Advertise` refused) — cross-process all-or-nothing needs a pre-check spanning peer-owned rings, deferred | TBD |
| Deadline | consumer-side staleness bound from envelope stamps (§2, R8 rule) | portable-layer, from envelope | portable-layer, from envelope; same host ⇒ measured (never advisory) | portable-layer; cross-host advisory unless ClockDomain declared |
| Loan | zero-copy publish for conforming payloads | native loan | **declared divergence** (`Supports(kZeroCopyLoan)` = no): the `Loan` verb works but publication copies through the seqlock's atomic words — that copy is what makes reads crash-safe | divergence: copy + serialize |
| Ownership `exclusive`/`shared` | second `Advertise` refused / last-writer-wins by publish stamp | TBD | `exclusive` enforced via the segment's publisher-liveness slot (pid CAS + ESRCH reclaim, §6.4); `shared` is a **declared divergence** (`Supports()` = no; refused) — no robust cross-process writer serialization by design | TBD |
| Request/response | typed Client/Server with mandatory deadline | native (≥ 0.6) | **declared divergence** (`Supports(kRequestResponse)` = no; `Serve`/`Client` handles carry the unsupported-reach status) — later or never (design.md) | queryables |

How a topic name plus domain key maps to the backend's native resource name (iceoryx2 service name, Zenoh key expression) is part of this section and is **TBD per backend (P1/P2)** — a foreign participant needs it to rendezvous, so each backend's integration is not complete until its column and naming rule land here. The POSIX-shm rule is §6.4.

### 6.4 POSIX-shm resource naming & per-topic segment (resolved, P1b)

**Object name.** One named shared-memory object per topic:

```
/xmmsg.<isolation-key>.<sanitized-topic>
```

where `<isolation-key>` is §6.2 and `<sanitized-topic>` applies §6.2's sanitizer with `.` additionally preserved. If the name (excluding the leading `/`) exceeds **240 bytes**, the whole name degrades to `/xmmsg.h` + 16 **lowercase** hex digits of FNV-1a-64 (§4.1) over the over-long name — unreadable but collision-safe and reproducible from this spec.

**Creation & rendezvous.** `shm_open` with `O_CREAT|O_EXCL` decides one creator per name (order-independent wiring, whoever arrives first); attachers poll (bounded) for the segment to reach its declared size and for the header's init flag, then validate magic, layout version, envelope version, schema hash, payload size/alignment, and total size — a schema-hash mismatch is the R6 refusal, any other mismatch refuses attachment. `shm_open` (not `memfd_create`) is deliberate: independent processes must rendezvous **by name** with no common ancestor and no broker to pass fds (daemonless, M4).

**Segment contents.** A fixed header (identity, publisher liveness slot with pid + generation counter, the topic's ordinal counter — which survives publisher crashes, so a restarted publisher resumes the ordinal sequence — a cross-process futex word, and 16 subscriber slots carrying pid/state/history-shape plus the D9 counters as shared atomics), followed by the data plane: one master latest-only slot (the late-join warm-start source; it survives publisher death) and 16 per-subscriber regions (a seqlock slot **and** an SPSC ring reservation of 16 records each; tmpfs allocates pages on first touch, so unused reservation costs no RAM). The header layout version gates interpretation with the same refuse-unknown rule as the envelope version byte.

**Data-plane region carve (layout v3).** Every region start is 64-byte aligned. A latest-only **slot region** is one seqlock cell followed by a cursor word: cell byte 0 is the `u64` sequence word, cell byte 8 the `u64` write-index word (the ring cursor position of the value held — new at v3), and the record words follow from cell byte 16; the cell is padded up to the next 64-byte boundary, after which one `u64` write-count cursor word closes the slot carve (`slot_bytes = align64(16 + 8 * ceil(record_bytes / 8)) + 8`, and the region reserves `align64(slot_bytes)`). A **ring region** is one control block (`u64` capacity; head and tail on separate 64-byte lines; 192 bytes total) followed by 16 plain record cells of `record_bytes` each, the whole region padded to 64. The master slot region sits at `align64(header)`; per-subscriber region *i* (slot region then ring region) follows at `master_offset + slot_region + i * (slot_region + ring_region)`. These offsets are a pure function of `payload_size` and the constants above, so every attacher computes them identically (v2 lacked the write-index and cursor words; a v2 reader would compute wrong offsets, hence the version bump).

**Segment record layout — recorded divergence (P1b).** The segment's mail records carry the §2 envelope *fields* in a segment-local 48-byte packed form (context 24 B @0, publish stamp i64 @24, origin stamp i64 @32, hop count u32 @40, flags u32 @44) followed by a u64 transport ordinal and the payload — not the §2 64-byte frame (no version byte / reserved regions inside each record; the header carries the envelope version once per segment). Both P1b participants are this library, and the header's version fields gate skew. Aligning per-record bytes to the §2 frame (or publishing this record layout as normative) is REQUIRED before a foreign participant (M12) targets this backend; tracked in Open items.

**Lifecycle.** Segments are **never unlinked by the library** ("last detacher unlinks" is racy, and unlinking would destroy the warm-start value whose survival is the point). They are bounded by the domain's topic set, namespaced by the isolation key, and reclaimed explicitly: `xmmsg clean` (R5 CLI, shipped with the P1b introspection follow-up) unlinks segments whose publisher and all subscribers are dead by pid probe — dry-run by default, `--yes` to unlink; `rm /dev/shm/xmmsg.<key>.*` remains the CLI-less manual path.

**Schema-hash forms (library conformance note).** The C++ library computes the §4 canonical hash for payload types opted in via its `XMMSG_DESCRIBE` field-description macro (validated against the §5 vectors in-tree). Types that do not opt in fall back to an interim hash over the implementation's mangled type name + size + alignment, prefixed `xmmsg-interim-schema:` so the two forms can never collide. **Stated divergence:** the interim form is not computable by a foreign language and does not catch a same-name/same-size field reorder (M11-A2); payloads crossing a process boundary SHOULD be `XMMSG_DESCRIBE`d.

## 7. Standard metric schema (R11)

Every endpoint emits this instrument set through the xmBase telemetry API unconditionally — zero application code, no opt-in, no opt-out (M13-A4). This table is the **normative list**: names, types, units, and labels are part of the wire contract, not an implementation detail.

Common labels on every instrument: `topic` (string, §6.1 grammar), `endpoint_id` (string, unique within domain), `pid` (integer), `reach` (string: `in_process` | `inter_process` | `inter_host`).

**Per publisher** — prefix `messaging.pub.<topic>.`:

| Instrument | Type | Unit | Meaning |
|---|---|---|---|
| `publish_count` | counter | 1 | successful publishes |
| `refused_count` | counter | 1 | reliable-mode would-block refusals (explicit back-pressure) |
| `bytes` | counter | bytes | payload bytes published (envelope excluded) |

**Per subscriber** — prefix `messaging.sub.<topic>.`:

| Instrument | Type | Unit | Meaning |
|---|---|---|---|
| `take_count` | counter | 1 | successful takes |
| `drop_count` | counter | 1 | best-effort overflow drops (never silent) |
| `overwrite_count` | counter | 1 | latest-only values overwritten unread |
| `take_age_us` | histogram | microseconds | age of value at take (`take time − publish_stamp`) |
| `deadline_miss_count` | counter | 1 | Fresh→Stale transitions — one event per miss (D3), not per-take observations (which would scale with poll rate) |
| `queue_depth` | gauge | 1 | current queued values (`queue<N>` history) |

**Per hop** — emitted only where both ends share a clock (same host, or a declared ClockDomain — R8):

| Instrument | Type | Unit | Labels (additional) | Meaning |
|---|---|---|---|---|
| `messaging.hop.<topic>.hop_latency_us` | histogram | microseconds | `pub_endpoint_id`, `sub_endpoint_id` | `take time − publish_stamp` across the hop |

**Per domain** — prefix `messaging.domain.` (labels: domain name, `pid`):

| Instrument | Type | Unit | Meaning |
|---|---|---|---|
| `endpoint_count` | gauge | 1 | live endpoints in this process's view of the domain |
| `match_count` | gauge | 1 | currently matched endpoint pairs |

Histogram bucket boundaries are chosen by the telemetry binding, not this spec; the instrument names, types, units, and label keys above are normative. Adding instruments is a spec **minor** version change; renaming or retyping one is **major**.

## 8. Introspection surface (POSIX shm — resolved, P1b introspection follow-up)

**Decision.** There is no separate introspection segment on the POSIX-shm backend: the per-topic transport segment header (§6.4) **is** the introspection surface — transport and introspection share the substrate by design (design.md R5), so the counters an observer reads are the very atomics the transport writes, and reconciliation with the endpoints' own `messaging.*` telemetry is exact by construction (M10-A2). This section is normative for **segment layout version 3**; an external observer implements it from this document alone. The reference consumers are `detail/introspect_reader.hpp` and the `xmmsg` CLI (`list` / `stat` / `watch` / `clean`), which ships with the library (R5). The CLI is live-state only — history is the telemetry plane's job, offline.

### 8.1 Discovery

Discovery is a **directory scan** of the backend namespace (`/dev/shm` on Linux) against the §6.4 name grammar — daemonless: there is no registry process to ask and no query verb.

- A name `xmmsg.<isolation-key>.<topic>` parses unambiguously: after the prefix, the first **two** dot-separated segments are the isolation key (§6.2 — the domain-name sanitizer never emits `.`), the remainder is the topic. The over-long fallback form `xmmsg.h` + 16 lowercase hex digits carries no parseable key/topic (§6.4) and is reported by object name.
- A name match is a **candidate only**. Readers MUST validate per §8.2 before interpreting a single byte past the header's version fields; a foreign file that happens to match the glob MUST be skipped, never crashed on.

### 8.2 Observer contract & validation

- Observers MUST attach **read-only** (`shm_open` `O_RDONLY`, `mmap` `PROT_READ`). Every field of this protocol is readable without write access, so a conforming observer is *physically unable* to perturb transport state — observer invisibility (M10-A4) is enforced by the kernel, not by politeness. (Participants — the library's own wiring paths — do write: slot claims, the refusal record. Those are not observers.)
- Validation order (normative): (1) object size ≥ header size, else **not ready**; (2) `init_state` == 1 (acquire load), else **not ready** — before the creator's release store the identity fields are meaningless zeros; (3) `magic`, else **foreign**; (4) `layout_version` and `envelope_version` known, else **refuse** (the envelope version byte's refuse-unknown rule); (5) header `total_size` == actual object size, else **foreign**.
- Identity fields (offsets 0–63) are **init-once**: written by the creator before `init_state`'s release store, plain reads afterwards. All mutable fields are lock-free atomics: load each with a relaxed (liveness/ordering-insensitive) or acquire (`init_state`, `pub_pid`, `refusal_count`) load. Counters are monotonic and individually exact; the *set* is not mutually snapshotted — there is no cross-field consistency guarantee and MUST NOT need to be one.
- Liveness is a `kill(pid, 0)` probe (`ESRCH` ⇒ dead). The recycled-pid limit of §6.4 applies to observers identically: a recycled pid reads as alive; the consequence is a conservative diagnosis, never corruption.
- **Torn-read / crash-of-writer safety** (M10-A5): the only multi-word read is the master-slot envelope (§8.4), protected by the writer-progress-only seqlock — a torn copy cannot validate, a writer SIGKILLed mid-store leaves a permanently odd sequence that the bounded retry budget detects. An observer never blocks, never takes a lock, and never spins unbounded.
- **Single-writer-per-slot** holds for every transport field. The one stated exception is the refusal record (§8.3): a last-writer-wins advisory slot written by *refused attachers* on the wiring path — each field is a single atomic (untorn by construction), `refusal_count` is the reliable monotonic signal, the `refused_*` fields identify the most recent offender.

### 8.3 Header layout (layout version 3)

Little-endian, natural alignment; 1032 bytes total. "a" marks lock-free atomics. Offsets are normative for `layout_version` 3. Version history: v1 lacked the refusal record; v2 added it; v3 keeps the header **byte-identical to v2** but changes the data plane (§6.4 region carve / §8.4 cell layout gained the write-index and cursor words), so every region offset and the total segment size differ — readers refuse any version they do not implement, in both directions.

| Offset | Size | Field | Semantics |
|---|---|---|---|
| 0 | 8 | `magic` | `0x31455347534D4D58` ("XMMSGSE1") |
| 8 | 4 | `layout_version` | **3** — refuse-unknown |
| 12 | 4 | `envelope_version` | §2 version carried once per segment |
| 16 | 8 | `schema_hash` | the topic's established R6 identity (§4) |
| 24 | 8 | `payload_size` | bytes |
| 32 | 8 | `payload_align` | bytes |
| 40 | 8 | `total_size` | full segment size — validated by every attacher and observer |
| 48 | 4 | `max_subscribers` | 16 at v3 |
| 52 | 4 | `ring_capacity` | 16 at v3 |
| 56 | 4 | `creator_history_kind` | creator's declared QoS: 0 latest-only, 1 queue |
| 60 | 4 | `creator_queue_depth` | declared depth (queue kind) |
| 64 | 4a | `init_state` | creation barrier: 1 (release) = data plane ready |
| 68 | 4a | `futex_word` | cross-process wake seam (unused by observers) |
| 72 | 8a | `accepted_ordinal` | topic ordinal author; survives publisher crashes |
| 80 | 4a | `pub_pid` | publisher liveness slot; 0 = none |
| 84 | 4a | `pub_epoch` | publisher generation counter (M4-A4) |
| 88 | 8a | `pub_publish_count` | cumulative accepted publishes (all generations) |
| 96 | 8a | `pub_bytes` | cumulative payload bytes |
| 104 | 8a | `refusal_count` | R6 refusals recorded on this topic; 0 = never |
| 112 | 8a | `refused_schema_hash` | most recent refused endpoint's hash (M11-A3) |
| 120 | 8a | `refused_payload_size` | its payload size |
| 128 | 4a | `refused_pid` | its pid |
| 132 | 4 | reserved | zero |
| 136 | 896 | `sub_slots[16]` | 56 bytes each, layout below |

Per-subscriber slot (56 bytes, offsets relative to the slot):

| Offset | Size | Field | Semantics |
|---|---|---|---|
| +0 | 4a | `state` | 0 free, 1 claimed (mid-init), 2 active |
| +4 | 4a | `pid` | owner, for liveness |
| +8 | 4a | `history_kind` | 0 latest-only, 1 queue |
| +12 | 4a | `queue_depth` | declared depth (queue kind) |
| +16 | 8a | `last_consumed_ordinal` | `UINT64_MAX` = join baseline unset (D6) |
| +24 | 8a | `take_count` | §7 `take_count` |
| +32 | 8a | `drop_count` | §7 `drop_count` (publisher-counted, per-subscriber) |
| +40 | 8a | `overwrite_count` | §7 `overwrite_count` |
| +48 | 8a | `deadline_miss_count` | §7 `deadline_miss_count` |

### 8.4 Last-publish age: the master-slot read

The master latest-only slot (the §6.4 warm-start slot) doubles as the last-publish record. It sits at offset `align64(1032)` = **1088** and is a v3 seqlock cell (§6.4 region carve): the `u64` sequence word at cell byte 0, the `u64` write-index word at cell byte 8 (new at v3 — the ring cursor position of the held value; a depth-1 observer may ignore it, sequence validation alone rejects torn copies), then `N` record words from cell byte 16, `N = (48 + 8 + payload_size + 7) / 8` (envelope + ordinal + payload, §6.4 record layout). An observer needs only the **prefix**: record words 0–5 are the 48-byte envelope (publish stamp `i64` at record byte 24 = record word 3; origin stamp at byte 32 = record word 4), record word 6 is the transport ordinal. Reading a prefix under the seqlock is exactly as torn-proof as reading everything — validation is on the sequence, not the byte count. (The slot's write-count cursor word after the padded cell participates in region sizing only; the observer protocol does not read it.)

Normative read protocol (bounded seqlock read; same memory-ordering pairs as the transport's own reader; `record_words` starts at cell byte 16):

```
for attempt in 0 ..= retry_budget:            # budget REQUIRED; 4096 recommended
    s1 = load(seq, acquire)
    if s1 == 0:            -> EMPTY           # never written (or crash-repaired)
    if s1 is odd: continue                    # write in progress — retry
    copy record_words[0..6] (relaxed loads)
    fence(acquire)
    if load(seq, relaxed) == s1: -> VALUE     # validated, untorn
-> STALLED                                    # budget exhausted
```

- `EMPTY` and `STALLED` are honest answers, not errors: `STALLED` means the writer died mid-store (M4/M10-A5) or the read raced `retry_budget` consecutive overwrites — either way the observer reports "last publish unknown" and MUST NOT spin further or block.
- **Age** = observer's own `CLOCK_MONOTONIC` now − `publish_stamp`. Same host, one clock (R8): the age is *measured*, never advisory, and needs no cooperation to compute.

### 8.5 ClockDomain

The R8 ClockDomain declaration does **not** live on this backend: POSIX shm is same-host by construction — one `CLOCK_MONOTONIC`, nothing to declare. The declaration's introspection-visible encoding lands with the first cross-host backend (Zenoh, P2). **TBD (P2).**

## Open items (consolidated)

| Item | Section | Resolves |
|---|---|---|
| Network serialization format (inter-host payload encoding) | §1 | P2 |
| ClockDomain declaration encoding | §2, §8.5 | P2 (POSIX shm is same-host/single-clock; the declaration lands with the first cross-host backend) |
| Maximum topic length | §6.1 | P1 (POSIX shm bounds the full object name at 240 bytes with a hashed fallback, §6.4) |
| ~~Domain isolation key derivation algorithm~~ | §6.2 | **resolved (P1b)** |
| iceoryx2 QoS mapping column + resource naming | §6.3 | P1 |
| ~~POSIX-shm QoS mapping column + resource naming~~ | §6.3, §6.4 | **resolved (P1b)** |
| POSIX-shm per-record §2 frame alignment (segment-local record layout is a recorded divergence) | §6.4 | before M12 targets this backend |
| Zenoh QoS mapping column + key-expression mapping | §6.3 | P2 |
| ~~Introspection segment byte layout~~ | §8 | **resolved (P1b introspection follow-up)** — no separate segment: §8 documents the per-topic header layout (v2) + the normative external read protocol |
