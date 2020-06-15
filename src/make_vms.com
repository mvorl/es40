$ SET NOVERIFY
$!
$! ES40 Emulator
$! Copyright (C) 2007-2008 by the ES40 Emulator Project
$!
$! This file was created by make_vms.sh. Please refer to that file
$! for more information.
$!
$ SAY = "WRITE SYS$OUTPUT"
$!
$! DETERMINE ES40 SRC ROOT PATH IN UNIX-STYLE SYNTAX
$!
$ DFLT = F$STRING("/" + F$ENVIRONMENT("DEFAULT"))
$ DLEN = F$LENGTH("''DFLT'")
$!
$ loop_dot:
$   DD = F$LOCATE(".",DFLT)
$   IF DD .EQ. DLEN
$   THEN
$     GOTO loop_dot_end
$   ENDIF
$   DFLT[DD,1]:="/"
$ GOTO loop_dot
$ loop_dot_end:
$!
$ DD = F$LOCATE(":[",DFLT)
$ IF DD .NE. DLEN
$ THEN
$   DFLT[DD,2]:="/"
$ ENDIF
$!
$ DD = F$LOCATE("]",DFLT)
$ IF DD .NE. DLEN
$ THEN
$   DFLT[DD,1]:=""
$ ENDIF
$!
$ DD = F$LOCATE("$",DFLT)
$ IF DD .NE. DLEN
$ THEN
$   DFLT=F$STRING(F$EXTRACT(0,DD,DFLT) + "\$" + F$EXTRACT(DD+1,DLEN-DD,DFLT))
$ ENDIF
$!
$ ES40_ROOT = F$EDIT(DFLT,"COLLAPSE")
$!
$! Determine if X11 support is available...
$!
$ CREATE X11TEST.CPP
$ DECK
#include <X11/Xlib.h>

void x() { XOpenDisplay(NULL); }
$ EOD
$ SET NOON
$ CXX X11TEST.CPP /OBJECT=X11TEST.OBJ
$ IF $STATUS
$ THEN
$   SAY "Have found X11 support"
$   X11_DEF=",HAVE_X11"
$   X11_LIB=",SYS$LIBRARY:DECWINDOWS/LIB"
$ ELSE
$   SAY "Have not found X11 support"
$   X11_DEF=""
$   X11_LIB=""
$ ENDIF
$ DELETE X11TEST.CPP;
$ DELETE X11TEST.OBJ;
$ SET ON
$!
$!
$! Compile sources for es40
$!
$! Compile with the following defines: ES40,__USE_STD_IOSTREAM
$!
$ SAY "Compiling es40..."
