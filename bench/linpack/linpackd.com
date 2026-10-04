$! linpackd.com -- build and run the double-precision LINPACK benchmark with DEC C.
$!
$! IEEE T-float is the normal double for cross-host comparison. /FLOAT=G_FLOAT
$! instead exercises the VAX floating-point path (helper-based in the JIT).
$!
$ set verify
$ cc /float=ieee_float /ieee_mode=fast /optimize linpackd.c
$ link linpackd
$ run linpackd
$ set noverify
$ exit
