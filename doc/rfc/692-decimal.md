# RFC: Decimal type in tarantool

* **Status**: In progress
* **Start date**: 02-04-2019
* **Authors**: Serge Petrenko @sergepetrenko \<sergepetrenko@tarantool.org\>
* **Issues**: \[#692\](https://github.com/tarantool/tarantool/issues/692)

## Summary

Decimal type suits financial calculations best, so it would be nice to have it
on board of tarantool.
In order to implement it we may use one of the exising libraries with decimal
calculations, and adapt it to fixed-point calculations, if needed.
The proposal is to use a decNumber library, which implements floating-point
decimal type, and tune it for fixed-point calculations as needed.

A value of decimal type has two properties: precision and scale.
Precision indicates how many decimal digits the number consists of, and scale
indicates how many digits are there after the decimal point. So, precision is
always greater or equal to scale.
Decimal is a signed type, sign is stored separately of the number value itself.

Let's adapt Decimal(precision, scale) notion for the value types for this memo.

Decimal(p, s) is suitable for holding values from
```
-9...9,9...9
       <-s->
 <----p---->
```
to
```
to +9...9,9...9
          <-s->
    <----p---->
```
Our decimal type will be a wrapper to the decNumber type from the decNumber
library. All the calculations on decimals will be carried out by the decNumber
code, our wrapper methods will just ensure correct precision and scale for the
numbers after each operation as described below. They will also hide decContext,
which defines the precision of operations and rounding algorithms, from the user.

The maximum value for both precision and scale is 38.
This value is used in Oracle Database and Microsoft SQL Server, so, let's adapt
the same one.
Also, the default value for precision will be 5, and the default value for scale
will be 0. These values will be used if no other values are provided.

### Arithmetic operations.

When doing calculations on decimals with certain precision and scale, the result
should have different precision and scale (generally greater, than the ones of
the operands). Otherwise some data may be lost in calculations. So, let's define
result precision and scale for the operations. Each operation will return a new
Decimal value, with precision and scale defined below.
(One of the operands may also be modified in case `X = X + Y`, for example).

In this paragraph, operands are `a ~ Decimal(p1, s1)` and `b ~ Decimal(p2, s2)`,
and c is the result of operation. `c ~ Decimal(p, s)`, where `p` and `s` depend on the
operation type.

Addition and subtraction
```
	c = a ± b
	c ~ Decimal(p, s)
	p = max(s1, s2) + max(p1 - s1, p2 - s2) + 1
	s = max(s1, s2)
```
Multiplication
```
	c = a * b
	c ~ Decimal(p, s);
	p = p1 + p2 + 1
	s = s1 + s2
```
Division
```
	c = a / b
	c ~ Decimal(p, s)
	p = p1 + p2 + s2 + 1
	s = s1 + p2 + 1
```
The publicly visible methods will be
```c
struct decimal *
decimal_add(struct decimal *c, struct decimal *a, struct decimal *b);

struct decimal *
decimal_sub(struct decimal *c, struct decimal *a, struct decimal *b);

struct decimal *
decimal_mul(struct decimal *c, struct decimal *a, struct decimal *b);

struct decimal *
decimal_div(struct decimal *c, struct decimal *a, struct decimal *b);
```

We may also introduce some mathematical functions, if we need them.
But it is an open question which precision and scale to set for `pow`, `sqrt`,
`log10`, `ln`.
decNumber has the aforementioned methods, so adding them is just the matter of
aggreeing on correct precision and scale and writing the wrappers.


Whenever result `precision` or `scale` get bigger than `38`, the result is
rounded until `precision = 38`, if possible. Rounding is performed only on the
fractional part of the number, so, if `precision > 38` and `scale < precision - 38`
then after rounding the number will still have excess precision, and there will
be no way to represent it in 38 digits, so an error will be raised.

### MsgPack extension

In order to pass decimals around and store them in tuples we have to extend
msgpack to store decimals.
Luckily, msgPack has a pre-defined `EXT_8` type, which can be used for
user-defined types. `EXT_8` is followed by two unsigned bytes, representing the
length of the following data and its type.
We haven't used any extention types yet, so let's adopt `type=1` for Decimal.
The proposed MsgPack encoding will look like this:
```
                       0                                            LENGTH - 1
+------+--------+------+-----------+-------+========================+
| 0xd7 | LENGTH | 0x01 | PRECISION | SCALE | Decimal representation |
+------+--------+------+-----------+-------+========================+
 MP_EXT  MP_INT  MP_INT   MP_INT    MP_INT           MP_BIN
```
Decimal representation is the chosen library format. For example, for decNumber
library, it is a sequence of bytes, containing sign, number of currently used
digits, exponent (since decNumber implements floating-point decimal), and the
coefficient itself in either Binary Coded Decimal (2 decimal digits per 1 byte)
or Densely Packed Decimal (3 decimal digits per 10 bits).
We can choose either BCD or DPD format, it still has to be decided.
