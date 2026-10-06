;; An outer loop that never ends around an inner one that does
(module
  (memory (export "memory") 1)
  (func (export "_start") (local i32 i32)
    (loop $o
      (local.set 1 (i32.const 0))
      (loop $i
        (i32.store (local.get 1) (local.get 0))
        (local.set 1 (i32.add (local.get 1) (i32.const 4)))
        (br_if $i (i32.lt_u (local.get 1) (i32.const 64))))
      (local.set 0 (i32.add (local.get 0) (i32.const 1)))
      (br $o))))
