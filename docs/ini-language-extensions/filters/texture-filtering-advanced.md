# Texture Filtering - Advanced

This page explains how Filter matching compares texture fingerprints and how `match_confidence` controls the allowed difference. It is an advanced companion to the [Texture Filtering](texture-filtering.md) usage guide.

## 1. What Is dHash?

A **difference hash (dHash)** is a compact fingerprint of an image or texture. Rather than comparing every pixel, it records a pattern of local changes in sampled image values. The result is a 64-bit value that can be compared quickly with another dHash.

The Filter system uses dHash to find texture resources that are visually similar even when their underlying bytes or resource hashes differ. It is a similarity signal, not a unique identifier: different textures can produce the same or similar dHashes.

The DLL chooses the mip level used to create the fingerprint. By default. it **prefers a 256×256 mip when one is available**; otherwise, it hashes the best-quality mip available. This gives the comparison a consistent image size when the preferred mip exists, while still allowing textures without that mip to be processed.

### Computation and Performance

The dHash is computed on the GPU by a dedicated compute shader. The resulting 64-bit hash is then transferred back to the CPU through a synchronous readback and cached for the lifetime of the resource. Because the hash is cached, the readback is normally a one-time cost per resource rather than work repeated for every draw that uses it.

A typical end-to-end dHash operation takes around 0.5 ms, with most of that time spent transferring the result from the GPU to the CPU rather than running the shader itself. In common modding scenarios, this overhead is generally not noticeable. It could become more significant if hundreds of distinct modded textures need to be hashed in the same frame, since each new resource may require its own readback.

Asynchronous readback could reduce the synchronization cost, but the current implementation uses synchronous readback.

## 2. What Is Hamming Distance?

The **Hamming distance** between two bit strings is the number of bit positions at which they differ. Since a dHash contains 64 bits, the Hamming distance between two dHashes is an integer from 0 to 64:

- `0`: all 64 bits are identical.
- `1`: one bit differs.
- `6`: six bits differ.
- `64`: every bit differs.

The distance is calculated by XOR-ing the two hashes and counting the number of set bits in the result. This count is also called the *population count* or *popcount*.

For example:

```text
reference:  8cb4796369a42c84
candidate:  8cb4796369a42cbb
            ----------------
XOR:        000000000000003f
```

`0x3f` contains six set bits, so the Hamming distance is **6**.

The calculation is:

$$
D(H_r,H_c)=\mathrm{popcount}(H_r\oplus H_c)
$$

where:

- $H_r$ is the Filter's reference dHash;
- $H_c$ is the candidate texture's dHash;
- $\oplus$ is bitwise XOR;
- `popcount` counts the set bits in the XOR result.

A smaller distance means the fingerprints have more bits in common.

## 3. Hamming Distance as a Similarity Score

Because a dHash contains 64 bits, its Hamming distance can be converted into a normalized bit-similarity score:

$$
S=1-\frac{D}{64}
$$

Here, $D$ is the Hamming distance and $S$ is the fraction of matching bits. Expressed as a percentage, multiply $S$ by 100.

| Hamming distance | Matching bits | Normalized similarity |
|---:|---:|---:|
| 0 | 64 / 64 | 100% |
| 1 | 63 / 64 | 98.4375% |
| 3 | 61 / 64 | 95.3125% |
| 6 | 58 / 64 | 90.625% |
| 8 | 56 / 64 | 87.5% |
| 16 | 48 / 64 | 75% |
| 32 | 32 / 64 | 50% |
| 64 | 0 / 64 | 0% |

Each bit accounts for $1/64$, or **1.5625 percentage points**, of the score. Consequently, the score is discrete: it has only 65 possible values, from 0% to 100% in steps of 1.5625 percentage points.

This percentage describes **how many hash bits agree**. It is not a measurement of pixel equality, perceived visual similarity, or the probability that two textures represent the same material. Two textures can look similar but have a noticeable Hamming distance, and different textures can coincidentally have similar hashes.

## 4. How `match_confidence` Becomes a Distance Limit and How to Tune It

`match_confidence` sets the minimum normalized dHash similarity required for a candidate to qualify. The DLL converts it into an integer maximum Hamming distance:

$$
D_{\max}=\lfloor(1-C)\times64\rfloor
$$

where:

- $C$ is `match_confidence`, expressed as a value from 0 to 1;
- `64` is the number of bits in the dHash;
- `trunc` discards the fractional part;
- $D_{\max}$ is the maximum number of differing bits allowed.

A candidate qualifies when its Hamming distance is less than or equal to that limit:

$$
D(H_r,H_c)\leq D_{\max}
$$

The comparison is inclusive: a candidate at the limit is accepted, while a candidate one bit beyond it is rejected.

### Example: `match_confidence = 0.90`

```ini
[FilterTexture]
match_dhash = 8cb4796369a42c84
match_confidence = 0.90
```

The configured confidence becomes:

$$
D_{\max}=\lfloor(1-0.90)\times64\rfloor
        =\lfloor6.4\rfloor
        =6
$$

The candidate may therefore differ from the reference by at most six bits:

- Distance **6**: accepted.
- Distance **7**: rejected.

A distance of 6 means 58 of 64 bits match, which is a normalized similarity of 90.625%. A distance of 7 means 57 of 64 bits match, or approximately 89.0625%, so it falls below the configured threshold.

### Confidence Is Quantized

The conversion produces an integer distance limit, not a fractional one. This means that small changes to `match_confidence` may have no effect until the formula crosses a whole-bit boundary. The effective threshold can therefore be slightly stricter than the configured percentage suggests.

| Configured confidence | Distance before truncation | Maximum differing bits | Lowest accepted bit similarity |
|---:|---:|---:|---:|
| `0.75` | 16.00 | 16 | 75.0000% |
| `0.80` | 12.80 | 12 | 81.2500% |
| `0.85` | 9.60 | 9 | 85.9375% |
| `0.90` | 6.40 | 6 | 90.6250% |
| `0.95` | 3.20 | 3 | 95.3125% |
| `0.98` | 1.28 | 1 | 98.4375% |
| `0.99` | 0.64 | 0 | 100.0000% |

A few practical consequences:

- The default `0.75` allows up to 16 differing bits.
- `0.90` allows up to 6 differing bits, not 6.4.
- A confidence greater than `0.984375` produces a zero-bit limit. The candidate dHash must then match the reference exactly.
- `1.0` also requires an exact 64-bit dHash match.

### Practical Tuning

Start with the default confidence of `0.75`. Adjust it according to the kind of mismatch you observe:

- **Unwanted similar textures are being selected:** increase confidence to make the maximum distance smaller. For example, move from `0.75` (16 bits) to `0.90` (6 bits), or to `0.95` (3 bits).
- **The intended texture is not being selected:** first verify the reference dHash and the texture/mip variant used to generate it. If those are correct, lower confidence to allow a larger distance.
- **Wet/dry or material-state variants are confused:** try a stricter threshold, because these textures may share a broadly similar pattern but differ in enough sampled comparisons to distinguish them.
- **You need a predictable adjustment:** think in terms of the resulting bit budget rather than changing the decimal value by tiny increments. Each one-bit reduction raises the required bit similarity by 1.5625 percentage points.

Before relaxing the threshold, check that `match_dhash` was copied from the intended texture and that it represents the mip variant the DLL will use for hashing. Add `match_format` when the format is known and should be required. Lowering confidence increases tolerance but also increases the risk of false positives; raising it reduces false positives but may reject legitimate variants.

### What `match_confidence` Does Not Mean

`match_confidence` is a deterministic comparison threshold. It is not:

- a statistical probability that the candidate is the correct texture;
- a machine-learning confidence score;
- a percentage of pixels that are identical;
- a direct measurement of color difference;
- a guarantee that two textures depict the same object or material.

It controls the maximum Hamming distance between two 64-bit dHashes. The distance is the measurement; `match_confidence` determines how much difference is allowed.
