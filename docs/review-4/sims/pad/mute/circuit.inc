* the mute as built (boards/lite-main/kicad/gen/root.py, lite-main.net): p5 = +5V, lout/rout = ES8388 LOUT2/ROUT2
* R80/R81 carry a tolerance factor TOLHI (>1 raises the trip: R80 high, R81 low); the bench sets TOLHI, BFQ, VTO, KP
QD p5 p5 res Q3904
C90 res 0 22u
R80 p5 det {120k*TOLHI}
R81 det 0 {22k/TOLHI}
QDET mute det 0 Q3904
R82 res mute 47k
X13 subl mute ms N7002 vto={VTO} kp={KP}
X15 subr mute ms N7002 vto={VTO} kp={KP}
X14 0 mute ms N7002 vto={VTO} kp={KP}
C47 lout acl 10u
R36 acl subl 100
R37 subl 0 47k
RSUBL subl 0 10k
C48 rout acr 10u
R38 acr subr 100
R39 subr 0 47k
RSUBR subr 0 10k
