;; A leaf function's loop that never ends (jitrun run -k: interrupted)
(module
  (memory (export "memory") 1)
  (func $spin (param i32) (result i32)
    (loop $l
      (local.set 0 (i32.add (local.get 0) (i32.const 1)))
      (br $l))
    (local.get 0))
  (func (export "_start") (drop (call $spin (i32.const 0)))))
