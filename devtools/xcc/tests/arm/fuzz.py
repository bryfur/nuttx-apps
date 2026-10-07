#!/usr/bin/env python3
"""fuzz.py SEED [N]: a random C program (no undefined behavior) printing
checksums of what it computes: loops, conditions, switches, narrow and
signed types, shifts, arrays, pointers, a state machine, small functions.
fuzz.sh compiles it with gcc (host) and xcc (ARM32, qemu-arm) and compares
the outputs: a difference is a miscompile (of either)."""
import random
import sys

TYPES = ['uint8_t', 'int8_t', 'uint16_t', 'int16_t', 'uint32_t', 'int32_t']
UNSIGNED = {'uint8_t', 'uint16_t', 'uint32_t'}


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.vars = []      # (name, type)
        self.arrays = []    # (name, type, length)
        self.funcs = []     # names of helpers (uint32_t f(uint32_t, uint32_t))
        self.n = 0
        self.depth = 0
        self.readonly = set()  # loop counters

    def name(self, p):
        self.n += 1
        return '%s%d' % (p, self.n)

    def const(self):
        r = self.r
        k = r.choice([0, 1, 2, 3, 7, 8, 15, 16, 31, 100, 127, 128, 255, 256, 1000, 32767,
                      32768, 65535, 65536, 0x7fffffff, 0x80000000, 0xffffffff,
                      r.randrange(0, 1 << 32), r.randrange(0, 300)])
        return '%uu' % k

    def var(self):
        return self.r.choice(self.vars)[0]

    # An expression of type uint32_t (every operand made unsigned: no
    # signed overflow)
    def expr(self, d=0):
        r = self.r
        if d > 3 or r.random() < 0.25:
            c = r.random()
            if c < 0.45:
                return '(uint32_t)' + self.var()
            if c < 0.6 and self.arrays:
                a, t, ln = r.choice(self.arrays)
                return '(uint32_t)%s[(%s) %% %du]' % (a, self.expr(d + 2), ln)
            return self.const()
        op = r.choice(['+', '-', '*', '&', '|', '^', '<<', '>>', '/', '%', 'cmp', 'cond', 'not',
                       'neg', 'cast', 'call', 'range', 'land', 'wide', 'sdiv', 'sar', 'bf',
                       'logic', 'mulhi', 'rot'])
        a, b = self.expr(d + 1), self.expr(d + 1)
        if op in ('<<', '>>'):
            return '((%s) %s ((%s) & 31u))' % (a, op, b)
        if op in ('/', '%'):
            return '((%s) %s ((%s) | 1u))' % (a, op, b)
        if op == 'cmp':
            t = r.choice(TYPES)
            return '(uint32_t)((%s)(%s) %s (%s)(%s))' % (t, a, r.choice(['<', '<=', '>', '>=', '==', '!=']), t, b)
        if op == 'cond':
            return '((%s) ? (%s) : (%s))' % (self.expr(d + 1), a, b)
        if op == 'not':
            return '(uint32_t)!(%s)' % a
        if op == 'neg':
            return '(0u - (%s))' % a
        if op == 'cast':
            return '(uint32_t)(%s)(%s)' % (r.choice(TYPES), a)
        if op == 'call':
            if not self.funcs:
                return '((%s) + (%s))' % (a, b)
            return '%s(%s, %s)' % (r.choice(self.funcs), a, b)
        if op == 'range':
            lo = r.randrange(0, 200)
            return '(uint32_t)((%s) >= %du && (%s) <= %du)' % (a, lo, a, lo + r.randrange(0, 60))
        if op == 'land':
            return '(uint32_t)(((%s) & 1u) %s ((%s) & 1u))' % (a, r.choice(['^', '|', '&']), b)
        if op == 'wide':
            o = r.choice(['*', '+', '-', '/', '%', '>>', '<<'])
            w = '(((uint64_t)(%s) << 32) | (uint64_t)(%s))' % (a, b)
            if o in ('/', '%'):
                return '(uint32_t)(%s %s ((uint64_t)(%s) | 1u))' % (w, o, self.expr(d + 1))
            if o in ('>>', '<<'):
                return '(uint32_t)(%s %s ((%s) & 63u))' % (w, o, self.expr(d + 1))
            return '(uint32_t)((%s %s (uint64_t)(%s)) >> %d)' % (w, o, self.expr(d + 1), r.randrange(0, 40))
        if op == 'sdiv':
            return '(uint32_t)((int32_t)(%s) %s ((int32_t)((%s) & 0x7fffu) | 1))' % (a, r.choice(['/', '%']), b)
        if op == 'sar':
            t = r.choice(['int32_t', 'int16_t', 'int8_t', 'int64_t'])
            return '(uint32_t)((%s)(%s) >> ((%s) & %du))' % (t, a, b, 7 if t == 'int8_t' else 15 if t == 'int16_t' else 31)
        if op == 'bf':
            return '(uint32_t)bf.%s' % r.choice(['a', 'b', 'c', 'd'])
        if op == 'logic':
            return '(uint32_t)((%s) %s (%s))' % (a, r.choice(['&&', '||']), b)
        if op == 'mulhi':
            s = r.choice(['uint64_t', 'int64_t'])
            return '(uint32_t)(((%s)(int32_t)(%s) * (%s)(int32_t)(%s)) >> 32)' % (s, a, s, b) if s == 'int64_t' else \
                   '(uint32_t)(((uint64_t)(%s) * (uint64_t)(%s)) >> 32)' % (a, b)
        if op == 'rot':
            k = r.randrange(1, 32)
            return '(((%s) << %d) | ((%s) >> %d))' % (a, k, a, 32 - k)
        return '((%s) %s (%s))' % (a, op, b)

    def assign(self):
        r = self.r
        cands = [vt for vt in self.vars if vt[0] not in self.readonly]
        if not cands:
            return 'acc += %s;' % self.expr()
        v, t = r.choice(cands)
        if self.arrays and r.random() < 0.25:
            a, at, ln = r.choice(self.arrays)
            return '%s[(%s) %% %du] = (%s)(%s);' % (a, self.expr(2), ln, at, self.expr())
        if r.random() < 0.1:
            return 'bf.%s = %s;' % (r.choice(['a', 'b', 'c', 'd']), self.expr())
        if t != 'int32_t' and r.random() < 0.15:
            # C's own compound assignments and ++/-- (on a narrow variable:
            # done in int, then wrapped to its type; nothing that overflows
            # an int or shifts a negative one)
            k = r.random()
            if k < 0.3:
                return '%s%s;' % (v, r.choice(['++', '--']))
            if k < 0.45:
                return '%s%s;' % (r.choice(['++', '--']), v)
            if k < 0.6 and t in UNSIGNED:
                return '%s <<= (%s) & 7u;' % (v, self.expr(2))
            ops = ['+=', '-='] + (['*='] if t in ('uint8_t', 'int8_t', 'int16_t', 'uint32_t') else [])
            return '%s %s (%s)(%s);' % (v, r.choice(ops), t, self.expr(2))
        op = r.choice(['=', '=', '+=', '^=', '|=', '&='])
        if op == '=':
            return '%s = (%s)(%s);' % (v, t, self.expr())
        return '%s = (%s)((uint32_t)%s %s (%s));' % (v, t, v, op[0], self.expr())

    def stmt(self, d=0):
        r = self.r
        c = r.random()
        if d > 2 or c < 0.45:
            return [self.assign()]
        if c < 0.6:
            out = ['if (%s) {' % self.expr()]
            out += self.block(d + 1)
            if r.random() < 0.5:
                out += ['} else {'] + self.block(d + 1)
            return out + ['}']
        if c < 0.68 and r.random() < 0.3:
            # a narrow counter that wraps around (at most 12 trips)
            i = self.name('i')
            t = r.choice(['uint8_t', 'uint16_t'])
            top = 255 if t == 'uint8_t' else 65535
            start, n = top - r.randrange(0, 6), r.randrange(1, 7)
            end = (start + n) % (top + 1)
            out = ['for (%s %s = %d; %s != %d; %s++) {' % (t, i, start, i, end, i)]
            self.vars.append((i, t))
            self.readonly.add(i)
            out += self.block(d + 1)
            self.vars.pop()
            self.readonly.discard(i)
            return out + ['}']
        if c < 0.75:
            i = self.name('i')
            t = r.choice(['uint8_t', 'uint16_t', 'int', 'uint32_t'])
            lo, n, step = r.randrange(0, 5), r.randrange(1, 40), r.choice([1, 1, 2, 3])
            out = ['for (%s %s = %d; %s < %d; %s += %d) {' % (t, i, lo, i, lo + n, i, step)]
            self.vars.append((i, t))
            self.readonly.add(i)
            out += self.block(d + 1)
            if r.random() < 0.3:
                out += ['if (%s) %s;' % (self.expr(2), r.choice(['break', 'continue']))] + self.block(d + 1)
            self.vars.pop()
            self.readonly.discard(i)
            return out + ['}']
        if c < 0.85:
            v = self.var()
            out = ['switch ((%s) %% 6u) {' % ('(uint32_t)' + v)]
            for k in range(r.randrange(1, 6)):
                out += ['case %du:' % k] + self.block(d + 1)
                if r.random() < 0.8:
                    out += ['break;']
            out += ['default:'] + self.block(d + 1) + ['break;', '}']
            return out
        if c < 0.93:
            # A small state machine over the array
            if not self.arrays:
                return [self.assign()]
            a, at, ln = r.choice(self.arrays)
            s, k = self.name('st'), self.name('k')
            out = ['{ int %s = 0; for (int %s = 0; %s < %d && %s != 3; %s++) {' % (s, k, k, ln, s, k),
                   'switch (%s) {' % s]
            for state in range(3):
                out += ['case %d:' % state,
                        'if ((uint32_t)%s[%s] %% 3u == %du) %s = %d; else if ((uint32_t)%s[%s] > %du) %s = %d;' % (
                            a, k, state, s, (state + 1) % 4, a, k, r.randrange(0, 200), s, r.randrange(0, 4)),
                        'break;']
            cands = [vt[0] for vt in self.vars if vt[0] not in self.readonly] or ['acc']
            out += ['default: break; }', '%s ^= (%s)%s;' % (r.choice(cands), 'uint32_t', s), '} }']
            return out
        # A while over a pointer
        if not self.arrays:
            return [self.assign()]
        a, at, ln = r.choice(self.arrays)
        p = self.name('p')
        return ['{ %s *%s = %s; while (%s < %s + %d) { %s += (uint32_t)*%s; %s++; } }' % (
            at, p, a, p, a, ln, self.var() if False else 'acc', p, p)]

    def block(self, d):
        out = []
        for _ in range(self.r.randrange(1, 4)):
            out += self.stmt(d)
        return out

    def program(self):
        r = self.r
        lines = ['#include <stdint.h>', '#include <stdio.h>', '',
                 'static uint32_t acc;',
                 'static struct { unsigned a : 3; signed b : 5; unsigned c : 12; signed d : 9; } bf;', '']
        for f in range(r.randrange(0, 3)):
            fn = self.name('f')
            self.vars = [('x', 'uint32_t'), ('y', 'uint32_t')]
            body = self.expr()
            lines += ['static uint32_t %s(uint32_t x, uint32_t y) { return %s; }' % (fn, body), '']
            self.funcs.append(fn)
        for k in range(r.randrange(1, 4)):
            a, t, ln = self.name('arr'), r.choice(TYPES), r.randrange(4, 40)
            self.arrays.append((a, t, ln))
            lines.append('static %s %s[%d];' % (t, a, ln))
        lines += ['', 'static void run(uint32_t seed) {']
        self.vars = []
        for k in range(r.randrange(2, 6)):
            v, t = self.name('v'), r.choice(TYPES)
            self.vars.append((v, t))
            lines.append('%s %s = (%s)(seed * %s);' % (t, v, t, self.const()))
        for a, t, ln in self.arrays:
            lines.append('for (int j = 0; j < %d; j++) %s[j] = (%s)(seed ^ (uint32_t)j * 2654435761u);' % (ln, a, t))
        for _ in range(r.randrange(3, 10)):
            lines += self.stmt()
        for v, t in self.vars:
            lines.append('acc = acc * 31u + (uint32_t)%s;' % v)
        lines.append('acc = acc * 31u + bf.a + (uint32_t)bf.b * 8u + bf.c * 256u + (uint32_t)bf.d * 65536u;')
        for a, t, ln in self.arrays:
            lines.append('for (int j = 0; j < %d; j++) acc = acc * 7u + (uint32_t)%s[j];' % (ln, a))
        lines += ['}', '', 'int main(void) {',
                  'for (uint32_t s = 0; s < 4; s++) { run(s * 77u + 3u); printf("%u\\n", (unsigned)acc); }',
                  'return 0;', '}']
        return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    seed = int(sys.argv[1])
    sys.stdout.write(Gen(seed).program())
