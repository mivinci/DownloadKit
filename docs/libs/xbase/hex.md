# hex.h — Hex (Base16) Encoding and Decoding

## Introduction

`hex.h` provides functions to encode and decode data in hexadecimal (base16)
representation. Each byte is encoded as two ASCII hex digits (`0-9`, `a-f`);
decoding accepts both upper- and lower-case input. The API follows the
`xBase64Encode` / `xBase64Decode` convention: the caller supplies the output
buffer and its size via a pointer; on success the pointer is updated to the
actual number of bytes written.

## Design Philosophy

1. **Lower-Case Output** — Encoded output always uses lower-case hex digits
   (`a-f`), matching the convention used by most hash displays and debug logs.

2. **NUL-Terminated Strings** — `xHexEncode` writes a trailing `'\0'` so the
   result can be used directly as a C string without additional copying.

3. **Lenient Decoding** — `xHexDecode` accepts both `A-F` and `a-f`, making
   it safe to decode hex strings from external sources without case normalization.

4. **Defensive NULL Handling** — All functions return `-1` when passed NULL
   pointers or when `src == NULL && src_len > 0`, preventing silent undefined
   behaviour.

5. **Macro-Based Size Hints** — `XHEX_ENCODE_MAXLEN(n)` and
   `XHEX_DECODE_MAXLEN(n)` let callers allocate buffers with exact upper bounds
   without calling the encode/decode function first.

## API Reference

### Macros

| Macro | Expansion | Description |
| --- | --- | --- |
| `XHEX_ENCODE_MAXLEN(n)` | `(size_t)((n) * 2 + 1)` | Maximum encoded string length (including NUL terminator) for `n` input bytes |
| `XHEX_DECODE_MAXLEN(n)` | `(size_t)((n) / 2)` | Maximum decoded byte length for `n` hex characters |

### Functions

| Function | Signature | Returns |
| --- | --- | --- |
| `xHexEncode` | `int xHexEncode(const uint8_t *src, size_t src_len, char *dst, size_t *dst_len)` | `0` on success, `-1` on error |
| `xHexDecode` | `int xHexDecode(const char *src, size_t src_len, uint8_t *dst, size_t *dst_len)` | `0` on success, `-1` on error |

#### `xHexEncode`

Encodes `src[0..src_len-1]` into a NUL-terminated hex string written to
`dst`. On success, `*dst_len` is set to the length of the encoded string
(excluding the NUL terminator).

**Error cases:** `dst == NULL`, `dst_len == NULL`, `*dst_len < src_len * 2 + 1`,
`src == NULL && src_len > 0`.

#### `xHexDecode`

Decodes the hex string `src[0..src_len-1]` into binary written to `dst`.
`src_len` must be even. On success, `*dst_len` is set to the number of decoded
bytes.

**Error cases:** `src == NULL`, `dst == NULL`, `dst_len == NULL`,
`src_len` is odd, `*dst_len` too small, `src` contains non-hex characters.

## Usage Examples

### Basic Encode and Decode

```c
#include <xbase/hex.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Encode */
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char   buf[XHEX_ENCODE_MAXLEN(4)];
    size_t len = sizeof(buf);

    xHexEncode(data, 4, buf, &len);
    printf("Encoded: %s\n", buf);  /* "deadbeef" */

    /* Decode */
    uint8_t out[2];
    size_t  out_len = sizeof(out);
    xHexDecode("AB", 2, out, &out_len);
    /* out == {0xAB}, out_len == 1 */
    return 0;
}
```

### Computing an Encoded Length Without Encoding

```c
size_t need = XHEX_ENCODE_MAXLEN(payload_len);
char   *buf = malloc(need);
```

### Decoding Mixed-Case Input

```c
uint8_t out[16];
size_t  out_len = sizeof(out);
/* Accepts upper-case, lower-case, or mixed. */
xHexDecode("DEADbeef1234", 12, out, &out_len);
```

## Use Cases

1. **Binary Data in JSON** — Many JSON APIs require binary blobs to be
   hex-encoded. `xHexEncode` produces a NUL-terminated string safe for direct
   inclusion as a JSON string value.

2. **Debug / Log Output** — Hex encoding is the standard way to render
   non-printable binary data in log messages and error reports.

3. **Hash Digests** — SHA-1, SHA-256, and other hash outputs are
   conventionally displayed as hex strings. Encode the raw digest bytes with
   `xHexEncode` for display.

4. **TLS Certificate Fingerprints** — Certificate pinning often uses hex
   fingerprints. Decode a user-provided hex fingerprint with `xHexDecode`
   before comparing against the raw certificate hash.

## Best Practices

- **Always check the return value.** Both functions return `-1` on error;
  ignoring the return value can silently leave `dst` partially written.

- **Use the `XHEX_*_MAXLEN` macros for buffer sizing.** They are pure
  compile-time expressions — no function call overhead.

- **Pass `strlen(src)` as `src_len` for NUL-terminated strings.**
  `xHexDecode("deadbeef", strlen("deadbeef"), ...)` is the idiomatic form.

- **Even-length input is required for decode.** If you are reading hex from an
  external source, validate `src_len % 2 == 0` before calling `xHexDecode`.

## Comparison with Other Libraries

| Feature | xbase hex.h | OpenSSL `BN_bn2hex` | mbedTLS `mbedtls_hex` | Linux `bin2hex` (kernel) |
| --- | --- | --- | --- | --- |
| **Output Case** | Lower (`a-f`) | Upper (`A-F`) | Configurable | Lower |
| **NUL Terminator** | Yes | Yes (caller must `OPENSSL_free`) | Yes | No (separate len) |
| **Decode Case** | Both | N/A | Both | Both |
| **NULL Safety** | Explicit checks | Crash | Crash | Crash |
| **Buffer Sizing** | `XHEX_ENCODE_MAXLEN` macro | `BN_hex2bn` allocates | Caller computes | Caller computes |
| **Dependency** | None | OpenSSL | mbedTLS | Linux kernel |
