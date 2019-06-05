decimal = require('decimal')
test_run = require('test_run').new()

-- check various constructors
decimal.tonumber('1234.5678')
decimal.tonumber('1e6')
decimal.tonumber('-6.234612e2')
decimal.tonumber(tonumber64(2^63))
decimal.tonumber(12345678ULL)
decimal.tonumber(-12345678LL)
decimal.tonumber(1)
decimal.tonumber(-1)
decimal.tonumber(2^64)
decimal.tonumber(2^(-20))

a = decimal.tonumber('100')
a:log10()
a:ln()
-- overflow, e^100 > 10^38
a:exp()
-- e^87 < 10^38, no overflow, but rounds
-- to 0 digits after the decimal point.
decimal.tonumber(87):exp()
a:sqrt()
a
a = decimal.tonumber('-123.456')
a:round(2)
a:round(1)
a:round(0)
a:abs()
a:tostring()
-a
a / 10
a * 5
a + 17
a - 0.0001
a ^ 2
a:abs() ^ 0.5
a:abs() ^ 0.5 == a:abs():sqrt()
a - 2 < a - 1
a + 1e-10 > a
a <= a
a >= a
a == a

decimal.tostring(a)
a:tostring()
decimal.abs(a)
decimal.minus(a)
decimal.round(a, 2)
