;; apps/graphics/fpcwasm/jit/test/wast/jit.wast
;;
;; SPDX-License-Identifier: Apache-2.0
;;
;; The JIT's own cases (make spec runs them with the spec tests): what
;; fuzzing found, and the code paths the spec tests do not reach.

;; A result computed into the local a local.tee takes it to, while an
;; older reference to that local is still on the stack: its operand (here
;; the constant) must not be overwritten by that reference's value first
;; (fuzzing, seed 90915)

(module
  (func $g (result f64) f64.const 1)
  (func (export "tee-loop") (param f32) (result i32)
    (local f32 i32)
    loop (result i32)
      local.get 0
      f32.const 0x1.8535aep+56
      f32.nearest
      local.tee 0
      local.tee 1
      i32.trunc_f32_u
      local.set 2
      drop
      call $g
      drop
      local.get 2
      return
    end)
  (func (export "tee-i32") (param i32) (result i32)
    local.get 0
    local.get 0
    i32.const 1000
    i32.add
    local.tee 0
    i32.sub)
)
(assert_trap (invoke "tee-loop" (f32.const 1)) "integer overflow")
(assert_return (invoke "tee-i32" (i32.const 5)) (i32.const -1000))

;; Reference locals start at null (fuzzing, seed 20459)

(module
  (func (export "null-local") (result i32)
    (local externref funcref)
    local.get 0
    ref.is_null
    local.get 1
    ref.is_null
    i32.and)
)
(assert_return (invoke "null-local") (i32.const 1))

;; Division by constants: no checks where they cannot fail, shifts for
;; powers of two

(module
  (func (export "div_s") (param i32) (result i32 i32 i32 i32 i32 i32)
    (i32.div_s (local.get 0) (i32.const 1))
    (i32.div_s (local.get 0) (i32.const 2))
    (i32.div_s (local.get 0) (i32.const 8))
    (i32.div_s (local.get 0) (i32.const 0x40000000))
    (i32.div_s (local.get 0) (i32.const 1000))
    (i32.div_s (local.get 0) (i32.const -7)))
  (func (export "div_s_m1") (param i32) (result i32)
    (i32.div_s (local.get 0) (i32.const -1)))
  (func (export "rem_s") (param i32) (result i32 i32 i32 i32 i32)
    (i32.rem_s (local.get 0) (i32.const 1))
    (i32.rem_s (local.get 0) (i32.const 8))
    (i32.rem_s (local.get 0) (i32.const -1))
    (i32.rem_s (local.get 0) (i32.const 0x80000000))
    (i32.rem_s (local.get 0) (i32.const 1000)))
  (func (export "div_u") (param i32) (result i32 i32 i32 i32 i32)
    (i32.div_u (local.get 0) (i32.const 1))
    (i32.div_u (local.get 0) (i32.const 0x100000))
    (i32.div_u (local.get 0) (i32.const 0x80000000))
    (i32.div_u (local.get 0) (i32.const -1))
    (i32.div_u (local.get 0) (i32.const 3)))
  (func (export "rem_u") (param i32) (result i32 i32 i32 i32)
    (i32.rem_u (local.get 0) (i32.const 2))
    (i32.rem_u (local.get 0) (i32.const 0x100000))
    (i32.rem_u (local.get 0) (i32.const 0x80000000))
    (i32.rem_u (local.get 0) (i32.const 1000)))
)
(assert_return (invoke "div_s" (i32.const -7))
  (i32.const -7) (i32.const -3) (i32.const 0) (i32.const 0) (i32.const 0)
  (i32.const 1))
(assert_return (invoke "div_s" (i32.const 0x80000000))
  (i32.const 0x80000000) (i32.const 0xc0000000) (i32.const 0xf0000000)
  (i32.const -2) (i32.const -2147483) (i32.const 306783378))
(assert_trap (invoke "div_s_m1" (i32.const 0x80000000)) "integer overflow")
(assert_return (invoke "div_s_m1" (i32.const 5)) (i32.const -5))
(assert_return (invoke "rem_s" (i32.const -1001))
  (i32.const 0) (i32.const -1) (i32.const 0) (i32.const -1001)
  (i32.const -1))
(assert_return (invoke "rem_s" (i32.const 0x80000000))
  (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)
  (i32.const -648))
(assert_return (invoke "div_u" (i32.const -1))
  (i32.const -1) (i32.const 0xfff) (i32.const 1) (i32.const 1)
  (i32.const 0x55555555))
(assert_return (invoke "rem_u" (i32.const -1))
  (i32.const 1) (i32.const 0xfffff) (i32.const 0x7fffffff) (i32.const 295))

;; Bounds checks a local's register has passed already, and the exact
;; checks of 1 and 2 bytes at the memory's end

(module
  (memory 1)
  (func (export "twice") (param i32) (result i32)
    local.get 0
    i32.load offset=4
    i32.load8_u
    local.get 0
    i32.load
    i32.add)
  (func (export "end8") (param i32) (result i32)
    local.get 0
    i32.load8_u)
  (func (export "end16") (param i32) (result i32)
    local.get 0
    i32.load16_u offset=2)
  (func (export "after-set") (param i32) (result i32)
    local.get 0
    i32.load
    drop
    i32.const 65535
    local.set 0
    local.get 0
    i32.load)
)
(assert_return (invoke "twice" (i32.const 0)) (i32.const 0))
(assert_trap (invoke "twice" (i32.const 65532)) "out of bounds memory access")
(assert_return (invoke "end8" (i32.const 65535)) (i32.const 0))
(assert_trap (invoke "end8" (i32.const 65536)) "out of bounds memory access")
(assert_return (invoke "end16" (i32.const 65532)) (i32.const 0))
(assert_trap (invoke "end16" (i32.const 65533)) "out of bounds memory access")
(assert_trap (invoke "after-set" (i32.const 0)) "out of bounds memory access")

;; The exact check of a 1 or 2-byte access in the memory's last 3 bytes,
;; out of line, with the address in lr (the fourth temporary of a leaf):
;; the memory's size must not be loaded into that same register

(module
  (memory 1)
  (data (i32.const 65532) "\01\02\03\04")
  (func (export "lr-cold8") (param i32) (result i32)
    (i32.add (local.get 0) (i32.const 1))
    (i32.add (local.get 0) (i32.const 2))
    (i32.add (local.get 0) (i32.const 3))
    (i32.add (local.get 0) (i32.const 65532))
    i32.load8_u
    i32.add
    i32.add
    i32.add)
  (func (export "lr-cold16") (param i32) (result i32)
    (i32.add (local.get 0) (i32.const 1))
    (i32.add (local.get 0) (i32.const 2))
    (i32.add (local.get 0) (i32.const 3))
    (i32.add (local.get 0) (i32.const 65532))
    i32.load16_u
    i32.add
    i32.add
    i32.add)
)
(assert_return (invoke "lr-cold8" (i32.const 3)) (i32.const 19))
(assert_return (invoke "lr-cold16" (i32.const 2)) (i32.const 1039))
(assert_trap (invoke "lr-cold16" (i32.const 3)) "out of bounds memory access")

;; i32.and, or, xor, sub only tested (i32.eqz, br_if, if, select after
;; them): tst, orrs, teq, cmp; constants that are immediates and that are
;; not, on either side

(module
  (func (export "and-brif") (param i32 i32) (result i32)
    (block (br_if 0 (i32.and (local.get 0) (local.get 1))) (return (i32.const 0)))
    (i32.const 1))
  (func (export "and-eqz-brif") (param i32) (result i32)
    (block (br_if 0 (i32.eqz (i32.and (local.get 0) (i32.const 0x12345))))
           (return (i32.const 0)))
    (i32.const 1))
  (func (export "and-k-if") (param i32) (result i32)
    (if (result i32) (i32.and (i32.const 0xff00) (local.get 0))
      (then (i32.const 7)) (else (i32.const 8))))
  (func (export "xor-select") (param i32 i32) (result i32)
    (select (i32.const 10) (i32.const 20) (i32.xor (local.get 0) (local.get 1))))
  (func (export "xor-eqz") (param i32) (result i32)
    (i32.eqz (i32.xor (local.get 0) (i32.const -559038737))))
  (func (export "or-eqz-select") (param i32 i32) (result i32)
    (select (i32.const 3) (i32.const 4)
      (i32.eqz (i32.or (local.get 0) (local.get 1)))))
  (func (export "or-k-brif") (param i32) (result i32)
    (block (br_if 0 (i32.or (local.get 0) (i32.const 0))) (return (i32.const 0)))
    (i32.const 1))
  (func (export "sub-brif") (param i32) (result i32)
    (block (br_if 0 (i32.sub (i32.const 100) (local.get 0))) (return (i32.const 0)))
    (i32.const 1))
  (func (export "sub-eqz") (param i32) (result i32)
    (i32.eqz (i32.sub (local.get 0) (i32.const 0x7654321))))
)
(assert_return (invoke "and-brif" (i32.const 6) (i32.const 3)) (i32.const 1))
(assert_return (invoke "and-brif" (i32.const 4) (i32.const 3)) (i32.const 0))
(assert_return (invoke "and-eqz-brif" (i32.const 0x10000)) (i32.const 0))
(assert_return (invoke "and-eqz-brif" (i32.const 0x20000)) (i32.const 1))
(assert_return (invoke "and-k-if" (i32.const 0x100)) (i32.const 7))
(assert_return (invoke "and-k-if" (i32.const 0xff)) (i32.const 8))
(assert_return (invoke "xor-select" (i32.const 5) (i32.const 5)) (i32.const 20))
(assert_return (invoke "xor-select" (i32.const 5) (i32.const 6)) (i32.const 10))
(assert_return (invoke "xor-eqz" (i32.const 0xdeadbeef)) (i32.const 1))
(assert_return (invoke "xor-eqz" (i32.const 0xdeadbeee)) (i32.const 0))
(assert_return (invoke "or-eqz-select" (i32.const 0) (i32.const 0)) (i32.const 3))
(assert_return (invoke "or-eqz-select" (i32.const 0) (i32.const 1)) (i32.const 4))
(assert_return (invoke "or-k-brif" (i32.const 0)) (i32.const 0))
(assert_return (invoke "or-k-brif" (i32.const -1)) (i32.const 1))
(assert_return (invoke "sub-brif" (i32.const 100)) (i32.const 0))
(assert_return (invoke "sub-brif" (i32.const 99)) (i32.const 1))
(assert_return (invoke "sub-eqz" (i32.const 0x7654321)) (i32.const 1))
(assert_return (invoke "sub-eqz" (i32.const 0x7654320)) (i32.const 0))

;; A second access through a local's register, proven in bounds by the
;; first, at an offset over 4 KB that is no immediate (it once added ip to
;; itself: clang's static arrays, a local as the index; kernels.c)

(module
  (memory 2)
  (data (i32.const 68296) "\01\00\00\00\02\00\00\00\03\00\00\00\04\00\00\00")
  (func (export "big-off") (param i32) (result i32)
    (i32.add
      (i32.load offset=68300 (local.get 0))
      (i32.mul (i32.load offset=68296 (local.get 0)) (i32.const 10))))
  (func (export "big-off64") (param i32) (result i64)
    (i64.add
      (i64.load offset=68300 (local.get 0))
      (i64.load offset=68296 (local.get 0))))
)
(assert_return (invoke "big-off" (i32.const 0)) (i32.const 12))
(assert_return (invoke "big-off" (i32.const 4)) (i32.const 23))
(assert_return (invoke "big-off64" (i32.const 0)) (i64.const 0x0000000500000003))
(assert_trap (invoke "big-off" (i32.const 63236)) "out of bounds memory access")

;; Locals set at the function's top level before anything reads them get
;; no starting value; the others still start at 0 (or null), however
;; they are reached (a set inside a block may be skipped)

(module
  (func $g (param i32) (result i32) (local.get 0))
  (func (export "lfirst") (param i32) (result i32)
    (local i32 i32 i32 funcref)
    (local.set 1 (i32.const 5))
    (block
      (br_if 0 (local.get 0))
      (local.set 2 (i32.const 7)))
    (loop
      (local.set 3 (i32.add (local.get 3) (i32.const 1)))
      (br_if 0 (i32.lt_u (local.get 3) (i32.const 3))))
    (i32.add
      (i32.add (local.get 1) (local.get 2))
      (i32.add (call $g (local.get 3)) (ref.is_null (local.get 4)))))
)
(assert_return (invoke "lfirst" (i32.const 0)) (i32.const 16))
(assert_return (invoke "lfirst" (i32.const 1)) (i32.const 9))
