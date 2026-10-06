;; A loop of calls that never ends, its body a leaf with a loop of its own
(module
  (memory (export "memory") 1)
  (func $f (param i32) (result i32)
    (local i32)
    (loop $l
      (local.set 1 (i32.add (local.get 1) (local.get 0)))
      (br_if $l (i32.lt_u (local.get 1) (i32.const 100))))
    (local.get 1))
  (func (export "_start") (local i32)
    (loop $o
      (local.set 0 (call $f (i32.add (local.get 0) (i32.const 1))))
      (br_if $o (i32.ne (local.get 0) (i32.const -1))))))
