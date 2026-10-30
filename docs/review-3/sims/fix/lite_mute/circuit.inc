* the mute as built in root.py (R3-AUDIO-2 fix); nodes: p5 (+5V), lout/rout (ES8388 LOUT2/ROUT2)
QD p5 p5 res Q3904
C90 res 0 22u IC=0
R80 p5 det 120k
R81 det 0 22k
QDET mute det 0 Q3904
R82 res mute 47k
X13 subl mute ms N7002 vto={VTO}
X15 subr mute ms N7002 vto={VTO}
X14 0 mute ms N7002 vto={VTO}
C47 lout acl 10u
R36 acl subl 100
R37 subl 0 47k
RSUBL subl 0 10k
C48 rout acr 10u
R38 acr subr 100
R39 subr 0 47k
RSUBR subr 0 10k
