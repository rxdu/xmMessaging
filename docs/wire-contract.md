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
5. The **standard metric schema** (§7) and the **introspection segment** (§8).

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

**Clock rule (R8).** `publish_stamp` and `origin_stamp` are **per-host monotonic** (`CLOCK_MONOTONIC`, shared with xmBase telemetry — one timeline by construction). Same-host readers MAY compute ages and enforce deadlines from them directly. A cross-host reader MUST treat differences against its own clock as **advisory** unless the domain declares a synchronized-clock domain (PTP/NTP) in its `Domain` configuration; that declaration is recorded in the domain's introspection home segment (§8) and in introspection output, so post-hoc analysis knows what the numbers meant. The `ClockDomain` declaration's encoding lives with the introspection segment layout. **TBD (P1b).**

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

Every backend resource a domain creates — topics/services, shm segments, introspection segments — MUST be namespaced by the domain's **isolation key**, so two domains on one host share nothing and cross-domain visibility is only ever explicit (composition-scale contract, design.md). The key is a string satisfying the `segment` grammar above. Default derivation is from the effective user plus the configured domain name; the exact derivation (separator, hashing/truncation for length-limited backend namespaces) is **TBD (P1)** and will be specified here, since a foreign participant must reproduce it to land in the same domain.

### 6.3 Per-backend mapping of the five QoS knobs

The in-process reach defines the reference semantics (design.md); each backend column states how the portable contract is realized, or names the divergence recorded in the R3 support matrix. Backend columns are structural placeholders until the backend is integrated.

| Knob | Portable contract | iceoryx2 **(TBD, P1)** | POSIX-shm fallback **(TBD, P1)** | Zenoh **(TBD, P2)** |
|---|---|---|---|---|
| History `latest-only` | LatestMailbox: overwrite, newest-or-nothing, never torn | subscriber buffer depth 1, overwrite-on-full | writer-progress-only seqlock slot | keep newest sample per key |
| History `queue<N>` | bounded FIFO, depth from QoS | buffer depth N | shared ring, depth N | TBD |
| Reliability `best-effort` | overflow drops, always counted | TBD | drop + counter | TBD |
| Reliability `reliable` | overflow back-pressures via explicit status | TBD | later or declared divergence (`Supports()` = no) | TBD |
| Deadline | consumer-side staleness bound from envelope stamps (§2, R8 rule) | portable-layer, from envelope | portable-layer, from envelope | portable-layer; cross-host advisory unless ClockDomain declared |
| Loan | zero-copy publish for conforming payloads | native loan | construct in shared mapping | divergence: copy + serialize |
| Ownership `exclusive`/`shared` | second `Advertise` refused / last-writer-wins by publish stamp | TBD | TBD | TBD |

How a topic name plus domain key maps to the backend's native resource name (iceoryx2 service name, Zenoh key expression, shm object path) is part of this section and is **TBD per backend (P1/P2)** — a foreign participant needs it to rendezvous, so each backend's integration is not complete until its column and naming rule land here.

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
| `deadline_miss_count` | counter | 1 | takes/polls observing the deadline passed |
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

## 8. Introspection segment

Placeholder — the byte layout is designed and specified at **P1b** (**TBD**), alongside the POSIX-shm backend, with which it shares its substrate. What is already normative:

- The segment is **named and versioned**: its name is derived from the domain isolation key (§6.2), and its first bytes carry a layout version that readers MUST check before interpreting anything else, with the same refuse-unknown rule as the envelope version byte.
- It is readable by **any** process with no cooperation from the observed application (R5, M10-A1) — attaching and reading MUST be invisible to the observed processes' latency profile (M10-A4).
- **Torn-read safety**: a reader never blocks and never observes a half-written record; the layout must make torn reads detectable and retryable (versioned/seqlock-style reads), since readers take no locks (M10-A5).
- **Crash-of-writer safety**: a writer dying mid-update leaves a segment that readers can still traverse safely — a skippable in-progress record, never a poisoned lock. Lifecycle is daemonless; the kernel reclaims mappings when the last mapper exits.
- **Single-writer-per-slot**: every slot in the segment has exactly one writing process/endpoint at any time; there is no cross-process mutual exclusion on the data path.
- The domain's declared **ClockDomain** (R8) is recorded in the segment's home area, so external tooling and post-hoc analysis know whether cross-host stamps were synchronized. Encoding TBD with the layout.

## Open items (consolidated)

| Item | Section | Resolves |
|---|---|---|
| Network serialization format (inter-host payload encoding) | §1 | P2 |
| ClockDomain declaration encoding | §2, §8 | P1b |
| Maximum topic length | §6.1 | P1 |
| Domain isolation key derivation algorithm | §6.2 | P1 |
| iceoryx2 / POSIX-shm QoS mapping columns + resource naming | §6.3 | P1 |
| Zenoh QoS mapping column + key-expression mapping | §6.3 | P2 |
| Introspection segment byte layout | §8 | P1b |
