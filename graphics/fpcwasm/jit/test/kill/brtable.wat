;; A state machine that never ends: br_table back to a loop
(module
  (memory (export "memory") 1)
  (func (export "_start") (local i32)
    (loop $o
      (block $b2 (block $b1 (block $b0
        (br_table $b0 $b1 $b2 (i32.and (local.get 0) (i32.const 3))))
        (local.set 0 (i32.add (local.get 0) (i32.const 1)))
        (br $o))
        (local.set 0 (i32.add (local.get 0) (i32.const 2)))
        (br $o))
      (local.set 0 (i32.add (local.get 0) (i32.const 3)))
      (br $o))))
