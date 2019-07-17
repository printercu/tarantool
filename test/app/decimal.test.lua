decimal = require('decimal')
test_run = require('test_run').new()
ffi = require('ffi')

-- check various constructors
decimal.new('1234.5678')
decimal.new('1e6')
decimal.new('-6.234612e2')
-- check (u)int16/32/64_t
decimal.new(2ULL ^ 63)
decimal.new(123456789123456789ULL)
decimal.new(-123456789123456789LL)
decimal.new(ffi.new('uint8_t', 231))
decimal.new(ffi.new('int8_t', -113))
decimal.new(ffi.new('uint16_t', 65535))
decimal.new(ffi.new('int16_t', -31263))
decimal.new(ffi.new('uint32_t', 4123123123))
decimal.new(ffi.new('int32_t', -2123123123))
decimal.new(ffi.new('float', 128.5))
decimal.new(ffi.new('double', 128.5))

decimal.new(1)
decimal.new(-1)
decimal.new(2^64)
decimal.new(2^(-20))

-- incorrect constructions
decimal.new(box.NULL)
decimal.new(ffi.new('float', 1 / 0))
decimal.new(ffi.new('double', 1 / 0))
decimal.new(1 / 0)
decimal.new({1, 2, 3})
decimal.new()
decimal.new('inf')
decimal.new('NaN')
decimal.new('not a valid number')

a = decimal.new('10')
a
b = decimal.new('0.1')
b
a + b
a - b
a * b
a / b
a ^ b
b ^ a
-a + -b == -(a + b)
a
b

a < b
b < a
a <= b
b <= a
a > b
b > a
a >= b
b >= a
a == b
a ~= b
a
b

decimal.sqrt(a)
decimal.ln(a)
decimal.log10(a)
decimal.exp(a)
a == decimal.ln(decimal.exp(a))
a == decimal.sqrt(a ^ 2)
a == decimal.log10('10' ^ a)
a == decimal.abs(-a)
a + -a == 0
a

a = decimal.new('1.1234567891234567891234567891234567891')
a
decimal.precision(a)
decimal.scale(a)
decimal.round(a, 37) == a
a
a = decimal.round(a, 36)
decimal.precision(a)
decimal.scale(a)
decimal.round(a, 100) == a
-- noop
decimal.round(a, -5) == a
decimal.round(a, 7)
decimal.round(a, 3)
decimal.round(a, 0)
a

decimal.ln(0)
decimal.ln(-1)
decimal.ln(1)
decimal.log10(0)
decimal.log10(-1)
decimal.log10(1)
decimal.exp(88)
decimal.exp(87)
decimal.sqrt(-5)
decimal.sqrt(5)

-- various incorrect operands
decimal.round(a)
decimal.round(1, 2)
decimal.scale(1.234)
decimal.precision(1.234)
decimal.scale()
decimal.precision()
decimal.abs()

a = decimal.new('1e19')
a * '1e19'
a ^ 2
a ^ 1.9
a * '1e18'
a = decimal.new(string.rep('9', 38))
decimal.precision(a)
a + 1
a + '0.9'
a + '0.5'
a + '0.4'
a / 0.5
1 / decimal.new('0')

a = decimal.new('-13')
a ^ 2
-- fractional powers are allowed only for positive numbers
a ^ 2.5

-- check correct rounding when scale = 0
decimal.round(decimal.new(0.9), 0)
decimal.round(decimal.new(9.9), 0)
decimal.round(decimal.new(99.9), 0)
decimal.round(decimal.new(99.4), 0)
