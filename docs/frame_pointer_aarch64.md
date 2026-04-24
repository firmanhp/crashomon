# `-fno-omit-frame-pointer` on AArch64

How frame pointer walking behaves on AArch64 and how it differs from x86-64. Everything in
[stack_walking_accuracy.md](stack_walking_accuracy.md) applies to AArch64 unless noted
otherwise here.

---

## Frame record layout

[AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst) defines a
standard *frame record*: a pair of 64-bit values stored at the top of each function's stack
frame.

| Offset from x29 | Content |
|---|---|
| `[x29 + 0]` | Caller's x29 (previous frame pointer) |
| `[x29 + 8]` | Caller's x30 (return address / link register) |

On x86-64, the return address is pushed implicitly by the `call` instruction and is therefore
separate from the frame pointer chain. On AArch64, both are stored together explicitly by the
callee's prologue. The chain is otherwise traversed the same way: follow x29 to get the
previous frame record.

---

## Prologue and epilogue sequence

A non-leaf function with `-fno-omit-frame-pointer` opens with:

```asm
stp  x29, x30, [sp, #-N]!   ; save caller's fp and lr, allocate frame
mov  x29, sp                  ; establish frame pointer for this function
```

The epilogue:

```asm
ldp  x29, x30, [sp], #N     ; restore fp and lr, deallocate frame
ret                           ; branch to x30
```

The prologue window opens after `stp` completes (frame record is written, sp is updated, but
x29 still holds the caller's frame pointer) and closes when `mov x29, sp` completes — a
one-instruction-wide window. Red Hat's 2024 analysis measured crashes falling in this
window at
[**6.0% of samples on AArch64**](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding)
— slightly higher than the 5.2% measured on x86-64.

---

## Leaf functions

A leaf function (one that makes no calls) does not need to save x30 because `bl`/`blr` have
not overwritten it. With `-fno-omit-frame-pointer`, the compiler still establishes a frame
record so that the chain is complete. Without the flag, leaf functions often skip the entire
prologue and leave x29 undisturbed (still holding the caller's frame pointer).

In the no-flag case, this creates two sub-cases:

- **x29 not clobbered:** x29 still holds the caller's frame pointer. Frame pointer walking
  recovers the caller and everything above it; the leaf function itself appears only as
  frame 0 from the crash register state. This is the best-case behavior for unflagged leaf
  functions.
- **x29 clobbered (used as a GP register):** x29 holds garbage. The chain breaks at frame 0.
  Scan takes over immediately.

With `-fno-omit-frame-pointer`, leaf functions always take the first sub-case — the chain
cannot break inside a leaf — at the cost of two extra instructions and 16 bytes of stack.

---

## Proportional register pressure cost

AArch64 has 31 general-purpose registers (x0–x30). Reserving x29 as the frame pointer leaves
30 for allocation. Compare to x86-64's 16 GP registers minus 1 = 15. Losing one register is
proportionally less costly on AArch64 (~3.2%) than on x86-64 (~6.25%). Workloads that
saturate the integer register file — tight numerical loops, hand-vectorised code — may still
see a modest spill-rate increase, but the practical impact is smaller.

---

## Signal frame behavior — no improvement over x86-64

The signal frame limitation is strictly worse on AArch64 than on glibc x86-64, and
`-fno-omit-frame-pointer` does not help:

- **Frame pointer walking:** the kernel constructs a synthetic frame record embedded in the
  signal frame (separate from the `ucontext_t` register save area), sets its `fp` field to 0
  as a deliberate chain terminator, and points x29 at it. The pre-signal x29 is preserved in
  `uc_mcontext.regs[29]` but is unreachable by frame pointer walking. The chain halts at the
  signal frame regardless of whether the handler or interrupted code used frame pointers.
- **CFI (on-host):** DWARF cannot simultaneously recover both PC and x30 (LR) through
  `ucontext_t` on AArch64
  ([MaskRay, 2022](https://maskray.me/blog/2022-04-10-unwinding-through-signal-handler)).
  CFI-based unwinding also halts at the signal frame on any libc.

Unlike glibc x86-64 — where the vDSO provides CFI that lets on-host analysis cross the signal
frame — there is no equivalent mechanism for AArch64. Both analyses terminate at the signal
frame on AArch64 regardless of libc, compiler flags, or whether `.sym` files are present.

---

## Summary

| Property | AArch64 | x86-64 |
|---|---|---|
| Frame record stores return address | Yes — at `[x29+8]` | No — implicit from `call` |
| Prologue window error rate | 6.0% ([Red Hat, 2024](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding)) | 5.2% |
| Register pressure cost of reserving frame pointer reg | ~3.2% of GP registers | ~6.25% of GP registers |
| Leaf functions without flag: chain survives if x29 not clobbered | Yes | Yes — same behavior: `rbp` holds caller's fp if not clobbered |
| Signal frame crossable by frame pointer walking | No | No |
| Signal frame crossable by CFI (on-host) | No (any libc) | Yes (glibc x86-64 only) |

Enabling `-fno-omit-frame-pointer` globally across an AArch64 sysroot is recommended for the
same reasons as on x86-64 and carries a proportionally lower register pressure cost. The
flag is the minimum condition for on-target tombstones to produce a meaningful call chain.
It does not address the signal frame limitation, which is structural on AArch64.
