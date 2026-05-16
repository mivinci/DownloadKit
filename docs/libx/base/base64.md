# base64.h — Base64 Encoding and Decoding (RFC 4648)

## Introduction

`base64.h` provides functions to encode and decode data in Base64 (RFC 4648).
The encoder uses the standard alphabet (`A-Z`, `a-z`, `0-9`, `+`, `/`)
with `=` padding. The decoder is tolerant: it accepts both the standard and
URL-safe (`-`, `_`) alphabets, and tolerates input with or without padding.

The API mirrors `hex.h`: the caller supplies the output buffer and its size
via a pointer; on success the pointer is updated to the actual byte count.

## Design Philosophy

1. **Standard + URL-Safe Decode** — Real-world Base64 data appears in both
   standard form (`+/`, with `=` padding) and URL-safe form (`-_`, typically
   unpadded). The decoder accepts all common variants.

2. **Padded Output on Encode** — `xBase64Encode` always emits `=` padding,
   producing output that is guaranteed to be decodable by strict RFC 4648
   implementations.

3. **Unpadded Input Accepted** — Many JSON/web APIs omit padding to save two
   bytes. `xBase64Decode` accepts input whose length is not a multiple of 4,
   as long as the length modulo 4 is 0, 2, or 3.

4. **Defensive NULL Handling** — Consistent with `hex.h`, all functions
   return `-1` for NULL pointers or invalid `src == NULL && src_len > 0`.

5. **Macro-Based Size Hints** — `XBASE64_ENCODE_MAXLEN(n)` and
   `XBASE64_DECODE_MAXLEN(n)` give tight upper bounds for buffer allocation
   without calling the function first.

## API Reference

### Macros

| Macro | Expansion | Description |
| --- | --- | --- |
| `XBASE64_ENCODE_MAXLEN(n)` | `(size_t)(((n) + 2) / 3 * 4 + 1)` | Maximum encoded string length (including NUL terminator) for `n` input bytes |
| `XBASE64_DECODE_MAXLEN(n)` | `(size_t)((n) * 3 / 4 + 2)` | Safe upper bound for decoded byte length for `n` encoded characters |

### Functions

| Function | Signature | Returns |
| --- | --- | --- |
| `xBase64Encode` | `int xBase64Encode(const uint8_t *src, size_t src_len, char *dst, size_t *dst_len)` | `0` on success, `-1` on error |
| `xBase64Decode` | `int xBase64Decode(const char *src, size_t src_len, uint8_t *dst, size_t *dst_len)` | `0` on success, `-1` on error |

#### `xBase64Encode`

Encodes `src[0..src_len-1]` into a NUL-terminated Base64 string written to
`dst`. The output always includes `=` padding. On success, `*dst_len` is set
to the length of the encoded string (excluding the NUL terminator).

**Error cases:** `dst == NULL`, `dst_len == NULL`,
`*dst_len < ((src_len + 2) / 3) * 4 + 1`, `src == NULL && src_len > 0`.

#### `xBase64Decode`

Decodes the Base64 string `src[0..src_len-1]` into binary written to `dst`.
`src` may use standard or URL-safe alphabet, with or without `=` padding.
On success, `*dst_len` is set to the number of decoded bytes.

**Error cases:** `src == NULL`, `dst == NULL`, `dst_len == NULL`,
`src_len % 4 == 1` (ambiguous padding), non-Base64 characters before
padding, `*` padding appears in a non-terminal position, `*dst_len` too small.

## Usage Examples

### Basic Encode and Decode

```c
#include <x/base/base64.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Encode */
    uint8_t data[] = "Hello!";
    char   buf[XBASE64_ENCODE_MAXLEN(6)];
    size_t len = sizeof(buf);

    xBase64Encode(data, 6, buf, &len);
    printf("Encoded: %s\n", buf);  /* "SGVsbG8h" */

    /* Decode */
    uint8_t out[XBASE64_DECODE_MAXLEN(8)];
    size_t  out_len = sizeof(out);
    xBase64Decode("SGVsbG8h", 8, out, &out_len);
    /* out == "Hello!", out_len == 6 */
    return 0;
}
```

### Decoding URL-Safe Base64

```c
uint8_t out[256];
size_t  out_len = sizeof(out);
/* URL-safe alphabet: - instead of +, _ instead of / */
xBase64Decode("AP__", 4, out, &out_len);
/* Decodes the same as "AP//" → {0x00, 0xFF, 0xFF} */
```

### Decoding Unpadded Input

```c
uint8_t out[256];
size_t  out_len = sizeof(out);
/* No padding — accepted as long as length mod 4 is 0, 2, or 3. */
xBase64Decode("SGVsbG8", 7, out, &out_len);
/* Same result as "SGVsbG8h==", out_len == 6 */
```

### Buffer Sizing Before Encoding

```c
size_t need = XBASE64_ENCODE_MAXLEN(payload_len);
char   *buf = malloc(need);  /* need includes space for NUL */
```

## Use Cases

1. **Binary Data in JSON** — JSON has no binary string type. Base64 is the
   standard encoding for embedding binary blobs (certificates, signatures,
   images) in JSON payloads.

2. **HTTP Basic Authentication** — The `Authorization: Basic ...` header
   carries credentials as Base64-encoded `username:password`.

3. **Data URIs** — `data:` URIs (e.g., inline images in HTML/CSS) encode
   the payload in Base64.

4. **TLS Certificates and Keys** — PEM files are Base64-encoded DER
   certificates wrapped in `-----BEGIN/END-----` markers.

5. **Cryptographic Signatures** — Many signing protocols (JWS, PASETO) use
   URL-safe Base64 to encode signatures in JSON-friendly strings.

## Best Practices

- **Always use `XBASE64_ENCODE_MAXLEN` for buffer allocation.** Guessing
  `src_len * 2` is incorrect — the actual expansion is `(n+2)/3*4`.

- **Pass `strlen(src)` as `src_len`, not `strlen(src)+1`.**
  The NUL terminator is not part of the encoded data.

- **Validate `src_len % 4 != 1` if you are reading from an untrusted source.**
  A length that is 1 modulo 4 is never valid for Base64 and indicates
  corrupted or malicious input.

- **Prefer padded encode + tolerant decode.** Encode with padding (the
  default) for maximum compatibility; decode tolerantly (as `xBase64Decode`
  does) to accept real-world data.

- **URL-safe vs. standard is only a decode concern.** The encoder always
  uses the standard alphabet. If you need URL-safe output, replace `+` with
  `-` and `/` with `_` after encoding — or just decode both forms on input.

## Comparison with Other Libraries

| Feature | xbase base64.h | OpenSSL `EVP_Encode` | mbedTLS `mbedtls_base64` | glib `g_base64` |
| --- | --- | --- | --- | --- |
| **Alphabet (encode)** | Standard (`+/`) | Standard | Standard | Standard |
| **Alphabet (decode)** | Standard + URL-safe | Standard only | Standard only | Standard only |
| **Padding (encode)** | Yes (`=`) | Yes | Yes | Yes |
| **Padding (decode)** | Optional | Required | Required | Optional |
| **NUL Terminator** | Yes | No | No | No (separate len) |
| **Buffer Sizing** | `XBASE64_ENCODE_MAXLEN` macro | Caller computes | Caller computes | Caller computes |
| **Dependency** | None | OpenSSL | mbedTLS | glib |
| **NULL Safety** | Explicit checks | Crash | Crash | Crash |
