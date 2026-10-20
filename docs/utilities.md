# Utility contracts

`include/lsm/status.hpp` and `include/lsm/slice.hpp` are the M1 public headers.
The DB interface is reserved for M2; M1 does not expose a partially implemented DB.

- `Status` defaults to success and owns an error code and message on failure.
  Messages may contain binary bytes. Returning an error does not throw an exception.
  String allocation still follows the standard library's allocation behavior.
- `Slice` is an alias for `std::string_view`, with caller-managed storage lifetime.
  Comparisons use unsigned byte order. Prefix consumption leaves input unchanged
  on failure.
- `Crc32c` uses the reflected Castagnoli polynomial, with initial and final
  complements. The empty string's CRC is zero. `ExtendCrc32c` accepts a previously
  finalized CRC. Return values are numeric; future file formats must serialize
  them explicitly. RFC 3720 Appendix B.4 lists the same values as little-endian
  octets.
- `Hash64` returns the low 64 bits of MurmurHash3 x64_128. It accepts a 32-bit seed,
  loads bytes in little-endian order, and supports unaligned input. Its algorithm
  is independent of host byte order. It is for hash-based lookup and bloom filters;
  CRC32C supplies integrity checks.
- Varints encode unsigned 32-bit and 64-bit integers in base 128, low group first.
  Writers append to the destination. Readers consume exactly one canonical
  encoding and allow trailing bytes. Truncated, overflowing, unterminated, and
  redundant encodings fail without modifying either the input view or output value.

The CRC test retains a bit-at-a-time reference. Varint tests retain division-based
encoding and checked, reverse-order decoding references. Randomized tests take
`--seed=N`, print it, and include the iteration in failure diagnostics.

The hash fixture uses the independent `mmh3==5.2.0` implementation. Recreate it
with `python scripts/hash_vectors.py`, or verify it without writing using
`python scripts/hash_vectors.py --check`. It covers all short tail lengths,
multiple full blocks, binary inputs, and seeds 0, 42 and 4294967295. C++ tests
also check each fixture at eight byte alignments.

References:

- [RFC 3720, Appendix B.4](https://www.rfc-editor.org/rfc/rfc3720#appendix-B.4)
- [MurmurHash3 reference implementation](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp)
- [mmh3 package](https://pypi.org/project/mmh3/5.2.0/)
